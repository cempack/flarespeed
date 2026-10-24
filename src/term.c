#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static bool write_all(term_t *t, const char *p, size_t n)
{
	while (n > 0 && !t->broken) {
		ssize_t w = write(t->out_fd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* Someone left the terminal non-blocking: wait for
				 * room instead of spinning. */
				struct pollfd pfd = {t->out_fd, POLLOUT, 0};
				if (poll(&pfd, 1, -1) >= 0 || errno == EINTR)
					continue;
			}
			t->broken = true; /* the terminal went away */
			return false;
		}
		p += w;
		n -= (size_t)w;
	}
	return !t->broken;
}

static void flush(term_t *t)
{
	if (t->out.len)
		write_all(t, t->out.data, t->out.len);
	buf_clear(&t->out);
}

/* x/term's MakeRaw, as Bubble Tea applies it to the input terminal. */
static bool make_raw(term_t *t)
{
	if (tcgetattr(t->in_fd, &t->saved) != 0)
		return false;
	struct termios raw = t->saved;
	raw.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	raw.c_oflag &= ~(tcflag_t)OPOST;
	raw.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	raw.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
	raw.c_cflag |= CS8;
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(t->in_fd, TCSANOW, &raw) != 0)
		return false;
	t->raw = true;
	return true;
}

void term_update_size(term_t *t)
{
	struct winsize ws;
	int w = 0, h = 0;
	if (ioctl(t->out_fd, TIOCGWINSZ, &ws) == 0 || ioctl(t->in_fd, TIOCGWINSZ, &ws) == 0) {
		w = ws.ws_col;
		h = ws.ws_row;
	}
	if (w != t->width || h != t->height)
		t->redraw_all = true;
	t->width = w;
	t->height = h;
}

/* Bubble Tea asks about synchronized output only where the answer can be
 * trusted. */
static bool should_query_sync(void)
{
	const char *term = getenv("TERM"), *prog = getenv("TERM_PROGRAM");
	bool ssh = getenv("SSH_TTY") != NULL, wt = getenv("WT_SESSION") != NULL;
	if (!term)
		term = "";
	return (!prog && !ssh) || wt || (prog && !strstr(prog, "Apple") && !ssh) || strstr(term, "ghostty") ||
	       strstr(term, "wezterm") || strstr(term, "alacritty") || strstr(term, "kitty") || strstr(term, "rio");
}

bool term_open(term_t *t, profile_t profile)
{
	memset(t, 0, sizeof(*t));
	buf_init(&t->out);
	t->out_fd = STDOUT_FILENO;
	t->profile = profile;
	t->in_fd = STDIN_FILENO;
	if (!isatty(t->in_fd)) {
		/* Like Bubble Tea, read keys from the terminal itself when stdin
		 * is redirected. */
		t->in_fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (t->in_fd < 0)
			return false;
		t->close_in = true;
	}
	if (!make_raw(t)) {
		int saved = errno;
		if (t->close_in)
			close(t->in_fd);
		errno = saved;
		return false;
	}
	term_update_size(t);

	/* Hide the cursor, turn on bracketed paste (so pasted text is not
	 * mistaken for keys), then ask for the background color, maybe
	 * synchronized output support, and finally the device attributes,
	 * which every terminal answers and so marks the end of the replies. */
	buf_puts(&t->out, "\x1b[?25l\x1b[?2004h\x1b]11;?\x07");
	if (should_query_sync())
		buf_puts(&t->out, "\x1b[?2026$p");
	buf_puts(&t->out, "\x1b[c");
	flush(t);
	return true;
}

ssize_t term_read(term_t *t, char *buf, size_t n)
{
	ssize_t r;
	do {
		r = read(t->in_fd, buf, n);
	} while (r < 0 && errno == EINTR);
	return r;
}

static void forget_lines(term_t *t)
{
	for (int i = 0; i < t->nlines; i++)
		free(t->lines[i]);
	t->nlines = 0;
}

static void put_progress(term_t *t, term_progress_t p)
{
	if (p.state == t->progress.state && p.value == t->progress.value)
		return;
	switch (p.state) {
	case TERM_PROGRESS_NONE:
		buf_puts(&t->out, "\x1b]9;4;0\x07");
		break;
	case TERM_PROGRESS_NORMAL:
		buf_printf(&t->out, "\x1b]9;4;1;%d\x07", p.value);
		break;
	case TERM_PROGRESS_ERROR:
		buf_printf(&t->out, "\x1b]9;4;2;%d\x07", p.value);
		break;
	}
	t->progress = p;
}

void term_render(term_t *t, const canvas_t *c, term_progress_t progress)
{
	if (t->broken)
		return;
	/* Without a known width, assume a classic 80 columns so lines never
	 * wrap behind our back. */
	int width = t->width > 0 ? t->width : 80;
	int first = 0, n = c->n;
	/* A frame taller than the screen loses its top lines. */
	if (t->height > 0 && n > t->height)
		first = n - t->height;
	n -= first;

	char **lines = xcalloc((size_t)(n ? n : 1), sizeof(char *));
	int *widths = xcalloc((size_t)(n ? n : 1), sizeof(int));
	buf_t line;
	buf_init(&line);
	for (int i = 0; i < n; i++) {
		buf_clear(&line);
		widths[i] = canvas_render_line(c, first + i, width, t->profile, &line);
		lines[i] = xstrdup(line.data ? line.data : "");
	}
	buf_free(&line);

	if (t->sync)
		buf_puts(&t->out, "\x1b[?2026h");
	if (t->drawn) {
		if (t->nlines > 1)
			buf_printf(&t->out, "\x1b[%dA", t->nlines - 1);
		buf_putc(&t->out, '\r');
		if (t->redraw_all)
			buf_puts(&t->out, "\x1b[J");
	}
	for (int i = 0; i < n; i++) {
		if (i > 0)
			buf_puts(&t->out, "\r\n");
		bool same = !t->redraw_all && i < t->nlines && strcmp(t->lines[i], lines[i]) == 0;
		if (same)
			continue;
		buf_puts(&t->out, lines[i]);
		/* Clearing the rest of a full-width line would erase its last
		 * character while the cursor waits to wrap. */
		if (widths[i] < width)
			buf_puts(&t->out, "\x1b[K");
	}
	if (t->drawn && !t->redraw_all && n < t->nlines)
		buf_puts(&t->out, "\x1b[B\r\x1b[J\x1b[A");
	if (t->sync)
		buf_puts(&t->out, "\x1b[?2026l");
	put_progress(t, progress);
	flush(t);

	forget_lines(t);
	free(t->lines);
	free(t->line_widths);
	t->lines = lines;
	t->line_widths = widths;
	t->nlines = n;
	t->lines_cap = n;
	t->drawn = true;
	t->redraw_all = false;
}

void term_restore(term_t *t)
{
	if (t->raw) {
		tcsetattr(t->in_fd, TCSANOW, &t->saved);
		t->raw = false;
	}
}

void term_close(term_t *t)
{
	if (t->drawn)
		buf_puts(&t->out, "\r\n");
	if (t->progress.state != TERM_PROGRESS_NONE)
		buf_puts(&t->out, "\x1b]9;4;0\x07");
	buf_puts(&t->out, "\x1b[?2004l\x1b[?25h");
	flush(t);
	term_restore(t);
	if (t->close_in)
		close(t->in_fd);
	forget_lines(t);
	free(t->lines);
	free(t->line_widths);
	buf_free(&t->out);
}
