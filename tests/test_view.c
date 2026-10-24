/*
 * View parity: every scenario in scenarios.inc was rendered by the original
 * Go/Bubble Tea implementation into tests/golden. Both renderings are parsed
 * into grids of cells and compared cell by cell: the character, and for
 * visible characters the foreground color and boldness, and for every cell
 * the background color.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "model.h"
#include "text.h"
#include "view.h"

typedef struct {
	const char *ip, *org, *city, *country, *colo_iata, *colo_city;
	long long asn;
} scen_meta_t;

typedef struct {
	const char *name;
	int width, dark, phase, quitting, spinner;
	int64_t finished;
	config_t cfg;
	int has_meta;
	scen_meta_t meta;
	const int64_t *pings;
	size_t npings;
	int has_latency;
	latency_t latency;
	const sample_t *samples[2];
	size_t nsamples[2];
	int has_transfer[2];
	throughput_t transfer[2];
	const char *err;
} scenario_t;

#include "scenarios.inc"

typedef struct {
	char text[CELL_TEXT];
	int fg, bg; /* 0xRRGGBB, or -1 for the default */
	int bold;
} gcell_t;

typedef struct {
	gcell_t *cells;
	int len;
} grow_t;

typedef struct {
	grow_t rows[128];
	int nrows;
} grid_t;

static void apply_sgr(const char *params, int *fg, int *bg, int *bold)
{
	int v[32], n = 0;
	const char *p = params;
	if (!*p)
		v[n++] = 0;
	while (*p && n < 32) {
		char *end;
		v[n++] = (int)strtol(p, &end, 10);
		p = end;
		if (*p == ';' || *p == ':')
			p++;
		else if (*p)
			break;
	}
	for (int i = 0; i < n; i++) {
		switch (v[i]) {
		case 0: *fg = *bg = -1; *bold = 0; break;
		case 1: *bold = 1; break;
		case 22: *bold = 0; break;
		case 39: *fg = -1; break;
		case 49: *bg = -1; break;
		case 38:
		case 48:
			if (i + 4 < n && v[i + 1] == 2) {
				int c = (v[i + 2] << 16) | (v[i + 3] << 8) | v[i + 4];
				*(v[i] == 38 ? fg : bg) = c;
				i += 4;
			}
			break;
		}
	}
}

static void parse(const char *s, grid_t *g)
{
	memset(g, 0, sizeof(*g));
	g->nrows = 1;
	int fg = -1, bg = -1, bold = 0, col = 0;
	size_t n = strlen(s);
	for (size_t i = 0; i < n;) {
		if (s[i] == '\x1b' && s[i + 1] == '[') {
			size_t j = i + 2;
			while (j < n && !(s[j] >= 0x40 && s[j] <= 0x7E))
				j++;
			if (j < n && s[j] == 'm') {
				char params[128];
				size_t len = j - (i + 2) < sizeof(params) - 1 ? j - (i + 2) : sizeof(params) - 1;
				memcpy(params, s + i + 2, len);
				params[len] = '\0';
				apply_sgr(params, &fg, &bg, &bold);
			}
			i = j + 1;
			continue;
		}
		if (s[i] == '\n') {
			if (g->nrows < 128)
				g->nrows++;
			col = 0;
			i++;
			continue;
		}
		int w;
		size_t len = text_cluster(s + i, n - i, &w);
		grow_t *r = &g->rows[g->nrows - 1];
		if (w > 0) {
			if (col + w > r->len) {
				r->cells = realloc(r->cells, (size_t)(col + w) * sizeof(gcell_t));
				for (int k = r->len; k < col + w; k++)
					r->cells[k] = (gcell_t){" ", -1, -1, 0};
				r->len = col + w;
			}
			gcell_t *cell = &r->cells[col];
			size_t cl = len < CELL_TEXT ? len : CELL_TEXT - 1;
			memcpy(cell->text, s + i, cl);
			cell->text[cl] = '\0';
			cell->fg = fg;
			cell->bg = bg;
			cell->bold = bold;
			col += w;
		}
		i += len;
	}
}

static void free_grid(grid_t *g)
{
	for (int i = 0; i < g->nrows; i++)
		free(g->rows[i].cells);
}

static gcell_t cell_at(const grid_t *g, int row, int col)
{
	if (row < g->nrows && col < g->rows[row].len)
		return g->rows[row].cells[col];
	return (gcell_t){" ", -1, -1, 0};
}

/* Returns the number of differing cells, reporting the first few. */
static int compare(const char *name, const grid_t *want, const grid_t *got)
{
	int diffs = 0;
	int rows = want->nrows > got->nrows ? want->nrows : got->nrows;
	for (int r = 0; r < rows; r++) {
		int wl = r < want->nrows ? want->rows[r].len : 0, gl = r < got->nrows ? got->rows[r].len : 0;
		int cols = wl > gl ? wl : gl;
		for (int c = 0; c < cols; c++) {
			gcell_t a = cell_at(want, r, c), b = cell_at(got, r, c);
			bool blank = !strcmp(a.text, " ");
			bool same = !strcmp(a.text, b.text) && a.bg == b.bg &&
				    (blank || (a.fg == b.fg && a.bold == b.bold));
			if (!same && diffs++ < 6)
				check_fail(__FILE__, __LINE__,
					   "%s row %d col %d: want \"%s\" fg=%06x bg=%06x bold=%d, got \"%s\" fg=%06x "
					   "bg=%06x bold=%d",
					   name, r, c, a.text, a.fg, a.bg, a.bold, b.text, b.fg, b.bg, b.bold);
		}
	}
	if (want->nrows != got->nrows)
		check_fail(__FILE__, __LINE__, "%s: %d rows, want %d", name, got->nrows, want->nrows);
	return diffs;
}

static char *read_golden(const char *name, int *state, int *value)
{
	char path[256];
	snprintf(path, sizeof(path), "golden/%s.ans", name);
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	char first[64];
	if (!fgets(first, sizeof(first), f) || sscanf(first, "progress %d %d", state, value) != 2) {
		fclose(f);
		return NULL;
	}
	buf_t b;
	buf_init(&b);
	char chunk[4096];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
		buf_append(&b, chunk, n);
	fclose(f);
	return buf_take(&b);
}

static void build_model(const scenario_t *s, model_t *m)
{
	model_init(m, s->cfg);
	m->width = s->width;
	m->dark = s->dark;
	m->spinner_frame = s->spinner % SPINNER_FRAMES;
	m->phase = (phase_t)s->phase;
	if (s->has_meta) {
		meta_t meta = {xstrdup(s->meta.ip),	   xstrdup(s->meta.org),       xstrdup(s->meta.city),
			       xstrdup(s->meta.country), xstrdup(s->meta.colo_iata), xstrdup(s->meta.colo_city),
			       s->meta.asn};
		model_set_meta(m, &meta);
		meta_free(&meta);
	}
	for (size_t i = 0; i < s->npings; i++)
		model_add_ping(m, s->pings[i]);
	if (s->has_latency)
		model_set_latency(m, s->latency);
	for (int d = 0; d < 2; d++) {
		for (size_t i = 0; i < s->nsamples[d]; i++)
			model_add_sample(m, (direction_t)d, s->samples[d][i]);
		if (s->has_transfer[d])
			model_set_transfer(m, (direction_t)d, s->transfer[d]);
	}
	if (s->err)
		model_set_error(m, s->err);
	m->quitting = s->quitting;
	m->finished = s->finished;
}

void test_view_parity(void)
{
	canvas_t c;
	canvas_init(&c);
	for (size_t i = 0; i < ARRAY_LEN(scenarios); i++) {
		const scenario_t *s = &scenarios[i];
		int want_state, want_value;
		char *golden = read_golden(s->name, &want_state, &want_value);
		if (!golden) {
			check_fail(__FILE__, __LINE__, "missing golden file for %s", s->name);
			continue;
		}

		model_t m;
		build_model(s, &m);
		term_progress_t progress;
		view_render(&m, &c, &progress);
		buf_t out;
		buf_init(&out);
		for (int r = 0; r < c.n; r++) {
			if (r)
				buf_putc(&out, '\n');
			canvas_render_line(&c, r, s->width, PROFILE_TRUECOLOR, &out);
		}

		grid_t want, got;
		parse(golden, &want);
		parse(out.data ? out.data : "", &got);
		compare(s->name, &want, &got);
		if ((int)progress.state != want_state || progress.value != want_value)
			check_fail(__FILE__, __LINE__, "%s: terminal progress %d/%d, want %d/%d", s->name,
				   progress.state, progress.value, want_state, want_value);
		free_grid(&want);
		free_grid(&got);
		buf_free(&out);
		free(golden);
		model_free(&m);
	}
	canvas_free(&c);
}
