#include "app.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "input.h"
#include "term.h"
#include "view.h"

/* Rendering is capped at 60 frames a second, like Bubble Tea. */
#define FRAME_INTERVAL (NS_PER_SEC / 60)
/* How long the first frame may wait for the terminal's replies, so it is
 * drawn in the right colors from the start. */
#define QUERY_WAIT (300 * NS_PER_MS)

typedef struct {
	model_t m;
	engine_t *engine;
	term_t term;
	input_t input;
	canvas_t canvas;
	int signal_fd;

	bool dirty, quit, interrupted, input_open, replies_done;
	int64_t opened, spinner_next, last_render;
} app_t;

static void on_phase(void *ctx, phase_t p)
{
	app_t *a = ctx;
	model_set_phase(&a->m, p, mono_ns());
	a->dirty = true;
}

static void on_meta(void *ctx, const meta_t *meta)
{
	app_t *a = ctx;
	model_set_meta(&a->m, meta);
	a->dirty = true;
}

static void on_ping(void *ctx, int64_t ns)
{
	app_t *a = ctx;
	model_add_ping(&a->m, ns);
	a->dirty = true;
}

static void on_latency(void *ctx, latency_t l)
{
	app_t *a = ctx;
	model_set_latency(&a->m, l);
	a->dirty = true;
}

static void on_sample(void *ctx, direction_t dir, sample_t s)
{
	app_t *a = ctx;
	model_add_sample(&a->m, dir, s);
	a->dirty = true;
}

static void on_throughput(void *ctx, direction_t dir, const throughput_t *r)
{
	app_t *a = ctx;
	model_set_transfer(&a->m, dir, *r);
	a->dirty = true;
}

static void on_error(void *ctx, const char *msg)
{
	app_t *a = ctx;
	model_set_error(&a->m, msg);
	a->dirty = true;
}

/* Starting (or restarting) a run nudges the spinner at once, as Bubble
 * Tea's spinner.Tick does, then every twelfth of a second. */
static void start_run(app_t *a)
{
	int64_t now = mono_ns();
	model_restart(&a->m, now);
	engine_start(a->engine);
	a->m.spinner_frame = (a->m.spinner_frame + 1) % SPINNER_FRAMES;
	a->spinner_next = now + SPINNER_INTERVAL;
	a->dirty = true;
}

static void on_input(void *ctx, input_event_t ev)
{
	app_t *a = ctx;
	switch (ev.kind) {
	case INPUT_QUIT:
		a->m.quitting = true;
		a->quit = true;
		break;
	case INPUT_RERUN:
		if (!a->quit)
			start_run(a);
		break;
	case INPUT_BACKGROUND:
		a->m.dark = ev.dark;
		a->dirty = true;
		break;
	case INPUT_SYNC_SUPPORTED:
		a->term.sync = true;
		break;
	case INPUT_DA1:
		a->replies_done = true;
		a->dirty = true;
		break;
	}
}

static void read_input(app_t *a);

/* Before giving the terminal back, wait (briefly) for the replies to the
 * startup queries, so they are not echoed into the shell. */
static void drain_replies(app_t *a)
{
	while (!a->replies_done && a->input_open) {
		int64_t left = a->opened + QUERY_WAIT - mono_ns();
		if (left <= 0)
			break;
		struct pollfd pfd = {a->term.in_fd, POLLIN, 0};
		if (poll(&pfd, 1, (int)((left + NS_PER_MS - 1) / NS_PER_MS)) <= 0)
			break;
		bool quitting = a->m.quitting;
		read_input(a);
		a->m.quitting = quitting;
	}
}

static void read_input(app_t *a)
{
	char buf[512];
	ssize_t n = term_read(&a->term, buf, sizeof(buf));
	if (n <= 0) {
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
			a->input_open = false; /* the terminal stopped sending */
		return;
	}
	input_feed(&a->input, buf, (size_t)n, mono_ns(), on_input, a);
}

static void read_signals(app_t *a)
{
	unsigned char sigs[64];
	ssize_t n = read(a->signal_fd, sigs, sizeof(sigs));
	for (ssize_t i = 0; i < n; i++) {
		switch (sigs[i]) {
		case SIGWINCH:
			term_update_size(&a->term);
			a->m.width = a->term.width;
			a->dirty = true;
			break;
		case SIGINT:
			a->interrupted = true;
			a->quit = true;
			break;
		case SIGTERM:
			a->quit = true;
			break;
		case SIGHUP:
			/* The terminal is gone: nothing left to draw on. */
			term_restore(&a->term);
			signal(SIGHUP, SIG_DFL);
			raise(SIGHUP);
			break;
		}
	}
}

static void render(app_t *a)
{
	term_progress_t progress;
	view_render(&a->m, &a->canvas, &progress);
	term_render(&a->term, &a->canvas, progress);
	a->last_render = mono_ns();
	a->dirty = false;
}

/* When the next frame may be drawn, or INT64_MAX if none is wanted. */
static int64_t render_due(const app_t *a)
{
	if (!a->dirty)
		return INT64_MAX;
	int64_t due = a->last_render + FRAME_INTERVAL;
	if (!a->replies_done && a->opened + QUERY_WAIT > due)
		due = a->opened + QUERY_WAIT;
	return due;
}

/* Static so that the engine, which is deliberately not freed on exit, stays
 * reachable: libcurl's cleanup waits for DNS lookups still in progress, which
 * would keep a quitting program alive for as long as a stalled resolver
 * takes. */
static app_t app;

int app_run(config_t cfg, const char *base_url, profile_t profile, int signal_fd)
{
	memset(&app, 0, sizeof(app));
	app.signal_fd = signal_fd;
	if (!term_open(&app.term, profile))
		die("could not open TTY: %s", strerror(errno));
	app.opened = mono_ns();
	app.input_open = true;
	input_init(&app.input);
	canvas_init(&app.canvas);
	model_init(&app.m, cfg);
	app.m.width = app.term.width;

	engine_events_t events = {&app, on_phase, on_meta, on_ping, on_latency, on_sample, on_throughput, on_error};
	app.engine = engine_new(cfg, base_url, &events);
	start_run(&app);

	while (!app.quit) {
		int64_t now = mono_ns();
		int64_t next = now + NS_PER_SEC;
		if (app.spinner_next && app.spinner_next < next)
			next = app.spinner_next;
		if (render_due(&app) < next)
			next = render_due(&app);
		if (input_deadline(&app.input) < next)
			next = input_deadline(&app.input);
		int64_t wait = next > now ? (next - now + NS_PER_MS - 1) / NS_PER_MS : 0;

		struct curl_waitfd fds[2] = {{signal_fd, CURL_WAIT_POLLIN, 0}, {app.term.in_fd, CURL_WAIT_POLLIN, 0}};
		engine_poll(app.engine, fds, app.input_open ? 2 : 1, (int)wait);

		/* libcurl reports only POLLIN; ask again to see hangups too. */
		struct pollfd pfds[2] = {{signal_fd, POLLIN, 0}, {app.term.in_fd, POLLIN, 0}};
		if (poll(pfds, app.input_open ? 2 : 1, 0) > 0) {
			if (pfds[0].revents & POLLIN)
				read_signals(&app);
			if (app.input_open && pfds[1].revents & (POLLIN | POLLHUP | POLLERR))
				read_input(&app);
		}
		now = mono_ns();
		input_tick(&app.input, now, on_input, &app);

		if (!model_running(&app.m)) {
			app.spinner_next = 0;
		} else if (app.spinner_next && now >= app.spinner_next) {
			app.m.spinner_frame = (app.m.spinner_frame + 1) % SPINNER_FRAMES;
			app.spinner_next += SPINNER_INTERVAL;
			if (app.spinner_next <= now)
				app.spinner_next = now + SPINNER_INTERVAL;
			app.dirty = true;
		}

		if (app.term.broken)
			break;
		if (!app.quit && now >= render_due(&app))
			render(&app);
	}

	/* Quitting by key drops the help line, so the frame left behind is a
	 * clean summary; a signal leaves the last frame as it was. */
	if (app.m.quitting || app.dirty)
		render(&app);
	drain_replies(&app);
	term_close(&app.term);

	int status = app.m.err ? 1 : 0;
	if (app.interrupted) {
		fprintf(stderr, "%s: program was interrupted\n", progname);
		status = 1;
	}
	canvas_free(&app.canvas);
	model_free(&app.m);
	return status;
}
