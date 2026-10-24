/* Ports colorprofile.Detect from charmbracelet/colorprofile (MIT,
 * Charmbracelet, Inc.) and terminfo decoding from xo/terminfo (MIT, Anmol
 * Sethi). See THIRD-PARTY-NOTICES. */
#include "profile.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

extern char **environ;

bool env_bool(const char *v)
{
	static const char *const truthy[] = {"1", "t", "T", "TRUE", "true", "True"};
	if (!v)
		return false;
	for (size_t i = 0; i < ARRAY_LEN(truthy); i++)
		if (strcmp(v, truthy[i]) == 0)
			return true;
	return false;
}

static bool has_prefix(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool has_suffix(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && strcmp(s + n - m, suffix) == 0;
}

static bool color_term(env_lookup_fn env)
{
	const char *v = env("COLORTERM");
	if (!v)
		return false;
	char lower[16];
	size_t n = strlen(v);
	if (n >= sizeof(lower))
		return false;
	for (size_t i = 0; i <= n; i++)
		lower[i] = (char)(v[i] >= 'A' && v[i] <= 'Z' ? v[i] + 32 : v[i]);
	return !strcmp(lower, "truecolor") || !strcmp(lower, "24bit") || !strcmp(lower, "yes") ||
	       !strcmp(lower, "true");
}

/* colorprofile's envColorProfile. */
static profile_t env_color_profile(env_lookup_fn env)
{
	static const char *const truecolor_terms[] = {"alacritty", "contour", "foot", "ghostty",
						      "kitty",     "rio",     "st",   "wezterm"};
	const char *term = env("TERM");
	profile_t p = (!term || !*term || !strcmp(term, "dumb")) ? PROFILE_NOTTY : PROFILE_ANSI;
	const char *t = term ? term : "";

	for (size_t i = 0; i < ARRAY_LEN(truecolor_terms); i++)
		if (strstr(t, truecolor_terms[i]))
			return PROFILE_TRUECOLOR;
	if (has_prefix(t, "tmux") || has_prefix(t, "screen")) {
		if (p < PROFILE_ANSI256)
			p = PROFILE_ANSI256;
	} else if (has_prefix(t, "xterm")) {
		if (p < PROFILE_ANSI)
			p = PROFILE_ANSI;
	}

	const char *wt = env("WT_SESSION");
	if (wt && *wt)
		return PROFILE_TRUECOLOR;
	if (env_bool(env("GOOGLE_CLOUD_SHELL")))
		return PROFILE_TRUECOLOR;
	if (color_term(env) && !has_prefix(t, "screen") && !has_prefix(t, "tmux"))
		return PROFILE_TRUECOLOR;
	if (has_suffix(t, "256color") && p < PROFILE_ANSI256)
		p = PROFILE_ANSI256;
	if (has_suffix(t, "direct"))
		return PROFILE_TRUECOLOR;
	return p;
}

profile_t profile_from_env(bool isatty, env_lookup_fn env)
{
	const char *term = env("TERM");
	bool dumb = !term || !strcmp(term, "dumb");
	profile_t envp = env_color_profile(env);
	profile_t p = (!isatty || dumb) ? PROFILE_NOTTY : envp;

	if (env_bool(env("NO_COLOR")) && isatty) {
		if (p > PROFILE_ASCII)
			p = PROFILE_ASCII;
		return p;
	}
	if (env_bool(env("CLICOLOR_FORCE"))) {
		if (p < PROFILE_ANSI)
			p = PROFILE_ANSI;
		if (envp > p)
			p = envp;
		return p;
	}
	if (env_bool(env("CLICOLOR")) && isatty && !dumb && p < PROFILE_ANSI)
		p = PROFILE_ANSI;
	return p;
}

/* A bounds-checked reader over a terminfo file, mirroring xo/terminfo's
 * decoder: every integer read is followed by alignment to an even offset. */
typedef struct {
	const unsigned char *buf;
	size_t n, pos;
} reader_t;

static bool read_bytes(reader_t *r, size_t n, const unsigned char **out)
{
	if (n > r->n - r->pos)
		return false;
	*out = r->buf + r->pos;
	r->pos += n;
	return true;
}

static void align(reader_t *r)
{
	r->pos += r->pos % 2;
}

static bool read_i16s(reader_t *r, size_t n, int *out)
{
	const unsigned char *p;
	if (n > SIZE_MAX / 2 || !read_bytes(r, n * 2, &p))
		return false;
	align(r);
	for (size_t i = 0; i < n; i++)
		out[i] = (int16_t)(uint16_t)(p[2 * i] | (p[2 * i + 1] << 8));
	return true;
}

/* Skips n integers of the given byte width, then aligns. */
static bool skip_ints(reader_t *r, size_t n, size_t width)
{
	const unsigned char *p;
	if (n > SIZE_MAX / width || !read_bytes(r, n * width, &p))
		return false;
	align(r);
	return true;
}

static long find_null(const unsigned char *buf, size_t len, long start)
{
	for (long i = start; i >= 0 && (size_t)i < len; i++)
		if (buf[i] == 0)
			return i;
	return -1;
}

int terminfo_declares_truecolor(const unsigned char *data, size_t len)
{
	enum { MAGIC = 0432, MAGIC_EXTENDED = 01036, MAX_FILE = 32768 };
	if (len >= MAX_FILE)
		return -1;
	reader_t r = {data, len, 0};

	int h[6];
	if (!read_i16s(&r, 6, h))
		return -1;
	size_t num_width;
	if (h[0] == MAGIC)
		num_width = 2;
	else if (h[0] == MAGIC_EXTENDED)
		num_width = 4;
	else
		return -1;
	int names = h[1], nbools = h[2], nnums = h[3], nstrs = h[4], table = h[5];
	/* Negative sizes would crash xo/terminfo; they are simply invalid. */
	if (names < 0 || nbools < 0 || nnums < 0 || nstrs < 0 || table < 0)
		return -1;
	if (nbools > 44 || nnums > 39 || nstrs > 414)
		return -1;
	long cap_len = names + nbools + (names + nbools) % 2 + nnums * 2 + nstrs * 2 + table;
	if ((long)(r.n - r.pos) < cap_len)
		return -1;

	const unsigned char *p;
	if (!read_bytes(&r, (size_t)names, &p) || find_null(p, (size_t)names, 0) < 0)
		return -1;
	if (!skip_ints(&r, (size_t)nbools, 1) || !skip_ints(&r, (size_t)nnums, num_width))
		return -1;

	int *offsets = xcalloc((size_t)nstrs + 1, sizeof(int));
	const unsigned char *strtab;
	bool ok = read_i16s(&r, (size_t)nstrs, offsets) && read_bytes(&r, (size_t)table, &strtab);
	if (ok) {
		align(&r);
		for (int i = 0; i < nstrs && ok; i++)
			if (offsets[i] >= 0 && find_null(strtab, (size_t)table, offsets[i]) < 0)
				ok = false;
	}
	free(offsets);
	if (!ok)
		return -1;
	if (r.pos >= r.n)
		return 0; /* no extended section */

	int eh[5];
	if (!read_i16s(&r, 5, eh))
		return -1;
	int ebools = eh[0], enums = eh[1], estrs = eh[2], eoffsets = eh[3], etable = eh[4];
	if (ebools < 0 || enums < 0 || estrs < 0 || eoffsets < 0 || etable < 0)
		return -1;
	if (ebools + enums + estrs * 2 != eoffsets)
		return -1;
	long ext_len = ebools + ebools % 2 + enums * (long)num_width + eoffsets * 2L + etable;
	if ((long)(r.n - r.pos) != ext_len)
		return -1;

	if (!skip_ints(&r, (size_t)ebools, 1) || !skip_ints(&r, (size_t)enums, num_width))
		return -1;
	int *idx = xcalloc((size_t)eoffsets + 1, sizeof(int));
	const unsigned char *edata = NULL;
	int result = -1;
	if (!read_i16s(&r, (size_t)eoffsets, idx) || !read_bytes(&r, (size_t)etable, &edata) || r.pos != r.n)
		goto done;

	/* The table holds the string values first, then every capability
	 * name; name offsets count from just past the last value read. */
	size_t base = 0;
	for (int i = 0; i < estrs; i++) {
		if (idx[i] < 0)
			continue;
		long end = find_null(edata, (size_t)etable, idx[i]);
		if (end < 0)
			goto done;
		base = (size_t)end + 1;
	}
	const unsigned char *names_data = edata + base;
	size_t names_len = (size_t)etable - base;

	result = 0;
	const int *name_idx = idx + estrs;
	for (int i = 0; i < ebools + enums + estrs; i++) {
		if (name_idx[i] < 0)
			continue;
		long end = find_null(names_data, names_len, name_idx[i]);
		if (end < 0) {
			result = -1;
			goto done;
		}
		const char *name = (const char *)names_data + name_idx[i];
		if (i < ebools && (!strcmp(name, "Tc") || !strcmp(name, "RGB")))
			result = 1;
	}
done:
	free(idx);
	return result;
}

/* Reads a whole file of at most limit bytes; NULL on error. */
static unsigned char *read_file(const char *path, size_t limit, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	buf_t b;
	buf_init(&b);
	char chunk[4096];
	for (;;) {
		ssize_t n = read(fd, chunk, sizeof(chunk));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			buf_free(&b);
			close(fd);
			return NULL;
		}
		if (n == 0)
			break;
		buf_append(&b, chunk, (size_t)n);
		if (b.len > limit)
			break;
	}
	close(fd);
	*len = b.len;
	return (unsigned char *)buf_take(&b);
}

/* colorprofile's Terminfo: the terminfo database, searched the way
 * xo/terminfo does, can upgrade ANSI to truecolor. */
static profile_t terminfo_profile(const char *term, env_lookup_fn env)
{
	if (!*term || !strcmp(term, "dumb"))
		return PROFILE_NOTTY;

	struct passwd *pw = getpwuid(getuid());
	if (!pw || !pw->pw_dir)
		return PROFILE_ANSI;

	const char *dirs[64];
	size_t ndirs = 0;
	char *home_dir = xasprintf("%s/.terminfo", pw->pw_dir);
	char *dirs_copy = NULL;
	const char *v = env("TERMINFO");
	if (v && *v)
		dirs[ndirs++] = v;
	dirs[ndirs++] = home_dir;
	v = env("TERMINFO_DIRS");
	if (v && *v) {
		dirs_copy = xstrdup(v);
		for (char *s = dirs_copy;; s++) {
			char *colon = strchr(s, ':');
			if (ndirs < ARRAY_LEN(dirs) - 3)
				dirs[ndirs++] = s;
			if (!colon)
				break;
			*colon = '\0';
			s = colon;
		}
	}
	dirs[ndirs++] = "/etc/terminfo";
	dirs[ndirs++] = "/lib/terminfo";
	dirs[ndirs++] = "/usr/share/terminfo";

	profile_t p = PROFILE_ANSI;
	for (size_t i = 0; i < ndirs; i++) {
		/* path.Join drops an empty directory, giving a relative path. */
		const char *sep = *dirs[i] ? "/" : "";
		char *paths[2] = {xasprintf("%s%s%c/%s", dirs[i], sep, term[0], term),
				  xasprintf("%s%s%x/%s", dirs[i], sep, (unsigned char)term[0], term)};
		unsigned char *data = NULL;
		size_t len = 0;
		for (int j = 0; j < 2 && !data; j++)
			data = read_file(paths[j], 65536, &len);
		free(paths[0]);
		free(paths[1]);
		if (!data)
			continue;
		/* A malformed entry ends the search, as in xo/terminfo. */
		if (terminfo_declares_truecolor(data, len) == 1)
			p = PROFILE_TRUECOLOR;
		free(data);
		break;
	}
	free(home_dir);
	free(dirs_copy);
	return p;
}

/* colorprofile's tmux check: inside tmux, `tmux info` reports whether the
 * outer terminal handles truecolor. */
static profile_t tmux_profile(env_lookup_fn env)
{
	const char *tmux = env("TMUX");
	if (!tmux || !*tmux)
		return PROFILE_NOTTY;

	int fds[2];
	if (pipe(fds) != 0)
		return PROFILE_ANSI256;

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addclose(&fa, fds[0]);
	posix_spawn_file_actions_addclose(&fa, fds[1]);
	char *argv[] = {(char *)"tmux", (char *)"info", NULL};
	pid_t pid;
	int rc = posix_spawnp(&pid, "tmux", &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if (rc != 0) {
		close(fds[0]);
		return PROFILE_ANSI256;
	}

	buf_t out;
	buf_init(&out);
	char chunk[4096];
	for (;;) {
		ssize_t n = read(fds[0], chunk, sizeof(chunk));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (out.len < (4u << 20))
			buf_append(&out, chunk, (size_t)n);
	}
	close(fds[0]);
	int status = 0;
	pid_t waited;
	while ((waited = waitpid(pid, &status, 0)) < 0 && errno == EINTR)
		;

	profile_t p = PROFILE_ANSI256;
	if (waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 && out.data) {
		for (char *line = out.data; line && *line;) {
			char *nl = strchr(line, '\n');
			if (nl)
				*nl = '\0';
			if ((strstr(line, "Tc") || strstr(line, "RGB")) && strstr(line, "true")) {
				p = PROFILE_TRUECOLOR;
				break;
			}
			line = nl ? nl + 1 : NULL;
		}
	}
	buf_free(&out);
	return p;
}

profile_t profile_detect(bool output_is_tty, env_lookup_fn env)
{
	bool isatty = env_bool(env("TTY_FORCE")) || output_is_tty;
	const char *term = env("TERM");
	bool dumb = !term || !strcmp(term, "dumb");
	profile_t envp = profile_from_env(isatty, env);
	if (envp == PROFILE_TRUECOLOR || env_bool(env("NO_COLOR")))
		return envp;
	if (isatty && !dumb) {
		profile_t p = envp, tip = terminfo_profile(term, env), tmuxp = tmux_profile(env);
		if (tip > p)
			p = tip;
		if (tmuxp > p)
			p = tmuxp;
		return p;
	}
	return envp;
}
