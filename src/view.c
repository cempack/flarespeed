/* Reproduces the layout of the original Bubble Tea interface, rendered with
 * charmbracelet/lipgloss and charmbracelet/bubbles (MIT, Charmbracelet, Inc.).
 * See THIRD-PARTY-NOTICES. */
#include "view.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "format.h"
#include "text.h"
#include "widgets.h"

enum {
	MAX_WIDTH = 68,
	/* The narrowest layout whose cards still fit their text; narrower
	 * terminals get the view clipped rather than wrapped. */
	MIN_WIDTH = 56,
	CHART_HEIGHT = 4,
	CARD_HEIGHT = 8,
};

static const char *const spinner_frames[SPINNER_FRAMES] = {"⠋", "⠙", "⠹", "⠸", "⠼",
							   "⠴", "⠦", "⠧", "⠇", "⠏"};

typedef struct {
	rgb16_t from, to;
} accent_t;

typedef struct {
	rgb16_t text, muted, faint, border, good, bad;
	accent_t ping, download, upload;
	rgb16_t help_key, help_desc, help_sep;
} styles_t;

static styles_t styles_for(bool dark)
{
#define LD(light, darkc) (dark ? RGB_HEX(darkc) : RGB_HEX(light))
	return (styles_t){
		.text = LD(0x27272A, 0xE4E4E7),
		.muted = LD(0x71717A, 0xA1A1AA),
		.faint = LD(0xD4D4D8, 0x52525B),
		.border = LD(0xE4E4E7, 0x3F3F46),
		.good = LD(0x16A34A, 0x4ADE80),
		.bad = LD(0xDC2626, 0xF87171),
		.ping = {LD(0xCA8A04, 0xFDE047), LD(0xEA580C, 0xFB923C)},
		.download = {LD(0x0891B2, 0x22D3EE), LD(0x4F46E5, 0x818CF8)},
		.upload = {LD(0xDB2777, 0xF472B6), LD(0x9333EA, 0xC084FC)},
		/* bubbles/help's default styles. */
		.help_key = LD(0x909090, 0x626262),
		.help_desc = LD(0xB2B2B2, 0x4A4A4A),
		.help_sep = LD(0xDADADA, 0x3C3C3C),
	};
#undef LD
}

static accent_t accent_for(const styles_t *s, phase_t p)
{
	switch (p) {
	case PHASE_DOWNLOAD:
		return s->download;
	case PHASE_UPLOAD:
		return s->upload;
	default:
		return s->ping;
	}
}

static int content_width(const model_t *m)
{
	if (m->width == 0)
		return MAX_WIDTH;
	int w = m->width - 2;
	if (w > MAX_WIDTH)
		w = MAX_WIDTH;
	return w < MIN_WIDTH ? MIN_WIDTH : w;
}

/* How many samples a finished transfer chart holds. */
static int chart_slots(const model_t *m)
{
	int64_t slots = m->cfg.duration / SAMPLE_INTERVAL;
	return slots < 1 ? 1 : slots > (1 << 24) ? (1 << 24) : (int)slots;
}

static double peak_bps(const sample_t *samples, size_t n)
{
	double peak = 0;
	for (size_t i = 0; i < n; i++)
		if (samples[i].bps > peak)
			peak = samples[i].bps;
	return peak;
}

static void draw_header(canvas_t *c, const model_t *m, const styles_t *s, int row, int col, int width)
{
	style_t title_pad = {{0, 0, 0}, RGB_HEX(0x7D56F4), STYLE_BG};
	style_t title = {RGB_HEX(0xFFFDF5), RGB_HEX(0x7D56F4), STYLE_FG | STYLE_BG | STYLE_BOLD};
	int x = canvas_text(c, row, col, " ", title_pad);
	x = canvas_text(c, row, x, "SPEEDTEST", title);
	x = canvas_text(c, row, x, " ", title_pad);
	x += 2;

	if (!m->has_meta) {
		canvas_text(c, row, x, "Cloudflare", style_fg(s->muted));
		canvas_text(c, row + 1, col + 1, "locating…", style_fg(s->faint));
		return;
	}
	const meta_t *meta = &m->meta;
	x = canvas_text(c, row, x, "Cloudflare", style_bold_fg(s->text));
	char *where = xasprintf(" · %s (%s)", meta->colo_city, meta->colo_iata);
	canvas_text(c, row, x, where, style_fg(s->muted));
	free(where);

	buf_t parts, line;
	buf_init(&parts);
	buf_init(&line);
	buf_puts(&parts, meta->ip);
	if (*meta->org)
		buf_printf(&parts, " · %s (AS%lld)", meta->org, meta->asn);
	if (*meta->city)
		buf_printf(&parts, " · %s, %s", meta->city, meta->country);
	text_truncate(&line, parts.data, width - 1, "…");
	canvas_text(c, row + 1, col + 1, line.data ? line.data : "", style_fg(s->muted));
	buf_free(&parts);
	buf_free(&line);
}

/* One metric: pending, live while its phase runs, then final. */
static void draw_card(canvas_t *c, const model_t *m, const styles_t *s, int row, int col, int width, phase_t p)
{
	accent_t acc = accent_for(s, p);
	int inner = width - 4; /* border and padding */
	const char *label, *unit = "";
	char value[FMT_LEN] = "", detail[FMT_LEN + 16] = "", tmp[FMT_LEN];
	bool done = false;

	if (p == PHASE_LATENCY) {
		label = "◉ PING";
		unit = "ms";
		if (m->has_latency) {
			done = true;
			fmt_sig3(value, ns_to_ms(m->latency.median));
			fmt_ms(tmp, m->latency.jitter);
			snprintf(detail, sizeof(detail), "jitter %s", tmp);
		} else if (m->npings > 0) {
			fmt_sig3(value, ns_to_ms(m->pings[m->npings - 1]));
			snprintf(detail, sizeof(detail), "sample %zu/%d", m->npings, m->cfg.pings);
		}
	} else {
		direction_t dir = p == PHASE_UPLOAD ? DIR_UPLOAD : DIR_DOWNLOAD;
		label = dir == DIR_UPLOAD ? "↑ UPLOAD" : "↓ DOWNLOAD";
		size_t n = m->nsamples[dir];
		if (m->has_transfer[dir]) {
			const throughput_t *res = &m->transfer[dir];
			done = true;
			fmt_rate(value, &unit, res->bps);
			if (res->loaded.median > 0) {
				fmt_ms(tmp, res->loaded.median);
				snprintf(detail, sizeof(detail), "loaded %s", tmp);
			} else {
				snprintf(detail, sizeof(detail), "loaded —");
			}
		} else if (n > 0) {
			fmt_rate(value, &unit, m->samples[dir][n - 1].bps);
			fmt_rate_text(tmp, peak_bps(m->samples[dir], n));
			snprintf(detail, sizeof(detail), "peak %s", tmp);
		}
	}

	bool active = m->phase == p && model_running(m);
	seg_t label_seg = {label, style_bold_fg(s->muted)};
	seg_t status = {"", STYLE_PLAIN};
	if (active) {
		label_seg.style = style_bold_fg(acc.from);
		status = (seg_t){spinner_frames[m->spinner_frame % SPINNER_FRAMES], style_fg(acc.from)};
	} else if (done) {
		label_seg.style = style_bold_fg(acc.from);
		status = (seg_t){"✓", style_fg(s->good)};
	}

	draw_box(c, row, col, width, CARD_HEIGHT, active ? acc.from : s->border);
	int x = col + 2;
	draw_spread(c, row + 1, x, &label_seg, 1, &status, 1, inner);
	if (!value[0]) {
		draw_bigtext(c, row + 2, x, "--", s->faint, s->faint);
		unit = "";
	} else if (bigtext_width(value) <= inner) {
		draw_bigtext(c, row + 2, x, value, acc.from, acc.to);
	} else {
		canvas_text(c, row + 3, x, value, style_bold_fg(acc.from)); /* too narrow for the big font */
	}
	canvas_text(c, row + 5, x, unit, style_fg(s->muted));
	buf_t cut;
	buf_init(&cut);
	text_truncate(&cut, detail, inner, "…");
	canvas_text(c, row + 6, x, cut.data ? cut.data : "", style_fg(s->muted));
	buf_free(&cut);
}

static void draw_cards(canvas_t *c, const model_t *m, const styles_t *s, int row, int col, int width)
{
	int card_width = (width - 2) / 3;
	static const phase_t order[] = {PHASE_LATENCY, PHASE_DOWNLOAD, PHASE_UPLOAD};
	for (int i = 0; i < 3; i++)
		draw_card(c, m, s, row, col + i * (card_width + 1), card_width, order[i]);
}

/* Charts the phase in progress. Returns the panel height. */
static int draw_live_panel(canvas_t *c, const model_t *m, const styles_t *s, int row, int col, int width)
{
	int inner = width - 4;
	accent_t acc = accent_for(s, m->phase);
	const char *title;
	char title_buf[64], progress[2 * FMT_LEN + 4] = "";
	double *values = NULL;
	size_t n = 0;
	int slots = 1;
	double pct = 0;

	switch (m->phase) {
	case PHASE_CONNECT:
		title = "Finding the nearest Cloudflare edge";
		break;
	case PHASE_LATENCY:
		title = "Measuring latency";
		snprintf(progress, sizeof(progress), "%zu/%d", m->npings, m->cfg.pings);
		n = m->npings;
		values = xreallocarray(NULL, n ? n : 1, sizeof(*values));
		for (size_t i = 0; i < n; i++)
			values[i] = ns_to_ms(m->pings[i]);
		slots = m->cfg.pings;
		pct = (double)m->npings / (double)m->cfg.pings;
		break;
	default: {
		direction_t dir = m->phase == PHASE_UPLOAD ? DIR_UPLOAD : DIR_DOWNLOAD;
		snprintf(title_buf, sizeof(title_buf), "%s · %d streams",
			 dir == DIR_UPLOAD ? "Uploading" : "Downloading", m->cfg.streams);
		title = title_buf;
		n = m->nsamples[dir];
		values = xreallocarray(NULL, n ? n : 1, sizeof(*values));
		for (size_t i = 0; i < n; i++)
			values[i] = m->samples[dir][i].bps;
		slots = chart_slots(m);
		pct = model_transfer_progress(m, dir);
		char a[FMT_LEN], b[FMT_LEN];
		fmt_seconds(a, (int64_t)(pct * (double)m->cfg.duration)); /* pct is within [0, 1] */
		fmt_seconds(b, m->cfg.duration);
		snprintf(progress, sizeof(progress), "%s / %s", a, b);
		break;
	}
	}

	draw_box(c, row, col, width, 8, s->border);
	int x = col + 2;
	seg_t left[] = {
		{spinner_frames[m->spinner_frame % SPINNER_FRAMES], style_fg(acc.from)},
		{" ", STYLE_PLAIN},
		{title, style_bold_fg(s->text)},
	};
	seg_t right = {progress, style_fg(s->muted)};
	draw_spread(c, row + 1, x, left, 3, &right, 1, inner);
	draw_chart(c, row + 2, x, values, n, slots, inner, CHART_HEIGHT, acc.from, acc.to, s->faint);
	draw_progress(c, row + 2 + CHART_HEIGHT, x, inner, pct, acc.from, acc.to, s->border);
	free(values);
	return 8;
}

/* Both throughput graphs side by side once the test ends. */
static int draw_summary_panel(canvas_t *c, const model_t *m, const styles_t *s, int row, int col, int width)
{
	int inner = width - 4;
	int half = (inner - 3) / 2;
	int slots = chart_slots(m);
	int x = col + 2;

	draw_box(c, row, col, width, 8, s->border);
	for (int d = 0; d < 2; d++) {
		direction_t dir = (direction_t)d;
		accent_t acc = dir == DIR_UPLOAD ? s->upload : s->download;
		int gx = x + d * (half + 3);
		size_t n = m->nsamples[dir];
		double *values = xreallocarray(NULL, n ? n : 1, sizeof(*values));
		for (size_t i = 0; i < n; i++)
			values[i] = m->samples[dir][i].bps;

		char peak[FMT_LEN], peak_text[FMT_LEN + 8];
		fmt_rate_text(peak, peak_bps(m->samples[dir], n));
		snprintf(peak_text, sizeof(peak_text), "peak %s", peak);
		seg_t label = {dir == DIR_UPLOAD ? "↑ upload" : "↓ download", style_bold_fg(acc.from)};
		seg_t right = {peak_text, style_fg(s->muted)};
		draw_spread(c, row + 1, gx, &label, 1, &right, 1, half);
		draw_chart(c, row + 2, gx, values, n, slots, half, CHART_HEIGHT - 1, acc.from, acc.to, s->faint);
		free(values);
	}

	int64_t transferred = 0;
	for (int d = 0; d < 2; d++)
		if (m->has_transfer[d])
			transferred += m->transfer[d].bytes;
	char took[FMT_LEN], bytes[FMT_LEN], done_text[FMT_LEN + 16], rest[FMT_LEN + 24];
	fmt_seconds(took, m->finished);
	fmt_bytes(bytes, transferred);
	snprintf(done_text, sizeof(done_text), "Done in %s", took);
	snprintf(rest, sizeof(rest), " · %s transferred", bytes);
	int fx = canvas_text(c, row + 6, x, "✓ ", style_fg(s->good));
	fx = canvas_text(c, row + 6, fx, done_text, style_fg(s->text));
	canvas_text(c, row + 6, fx, rest, style_fg(s->muted));
	return 8;
}

static int draw_error_panel(canvas_t *c, const model_t *m, const styles_t *s, int row, int col, int width)
{
	int inner = width - 4;
	buf_t wrapped;
	buf_init(&wrapped);
	text_wrap(&wrapped, m->err, inner);
	int nlines = 1;
	for (size_t i = 0; i < wrapped.len; i++)
		nlines += wrapped.data[i] == '\n';
	int height = 2 + 1 + nlines + 2;

	draw_box(c, row, col, width, height, s->bad);
	int x = col + 2;
	canvas_text(c, row + 1, x, "✗ Test failed", style_bold_fg(s->bad));
	char *text = wrapped.data ? wrapped.data : (char *)"";
	for (int i = 0; i < nlines; i++) {
		char *nl = strchr(text, '\n');
		size_t len = nl ? (size_t)(nl - text) : strlen(text);
		canvas_text_n(c, row + 2 + i, x, text, len, style_fg(s->text));
		text = nl ? nl + 1 : text + len;
	}
	canvas_text(c, row + 3 + nlines, x, "Check your connection, then press r to try again.", style_fg(s->muted));
	buf_free(&wrapped);
	return height;
}

static void draw_help(canvas_t *c, const styles_t *s, int row, int col)
{
	seg_t segs[] = {
		{"r", style_fg(s->help_key)},   {" ", STYLE_PLAIN}, {"run again", style_fg(s->help_desc)},
		{" • ", style_fg(s->help_sep)}, {"q", style_fg(s->help_key)}, {" ", STYLE_PLAIN},
		{"quit", style_fg(s->help_desc)},
	};
	draw_segs(c, row, col, segs, (int)ARRAY_LEN(segs));
}

void view_render(const model_t *m, canvas_t *c, term_progress_t *progress)
{
	styles_t s = styles_for(m->dark);
	int w = content_width(m);
	canvas_clear(c);

	/* One blank line of top margin, then a column of left margin. */
	int row = 1;
	draw_header(c, m, &s, row, 1, w);
	row += 3;
	draw_cards(c, m, &s, row, 1, w);
	row += CARD_HEIGHT;
	if (m->err)
		row += draw_error_panel(c, m, &s, row, 1, w);
	else if (m->phase == PHASE_DONE)
		row += draw_summary_panel(c, m, &s, row, 1, w);
	else
		row += draw_live_panel(c, m, &s, row, 1, w);
	if (!m->quitting) {
		draw_help(c, &s, row + 1, 2);
		row += 2;
	}
	canvas_ensure_rows(c, row);

	if (progress) {
		if (m->err)
			*progress = (term_progress_t){TERM_PROGRESS_ERROR, 100};
		else if (model_running(m))
			*progress = (term_progress_t){TERM_PROGRESS_NORMAL,
						      (int)fmax(0.0, fmin(model_overall_progress(m) * 100, 100.0))};
		else
			*progress = (term_progress_t){TERM_PROGRESS_NONE, 0};
	}
}
