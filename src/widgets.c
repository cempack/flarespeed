/* The progress bar and spinner follow charmbracelet/bubbles, and text layout
 * charmbracelet/lipgloss (MIT, Charmbracelet, Inc.). See THIRD-PARTY-NOTICES. */
#include "widgets.h"

#include <math.h>
#include <string.h>

#include "text.h"

int segs_width(const seg_t *segs, int n)
{
	int w = 0;
	for (int i = 0; i < n; i++)
		w += text_width(segs[i].text);
	return w;
}

int draw_segs(canvas_t *c, int row, int col, const seg_t *segs, int n)
{
	for (int i = 0; i < n; i++)
		col = canvas_text(c, row, col, segs[i].text, segs[i].style);
	return col;
}

/* Draws segments cut to limit columns, ansi.Truncate style: the tail takes
 * the style of the segment where the cut falls. Returns the column after. */
static int draw_segs_truncated(canvas_t *c, int row, int col, const seg_t *segs, int n, int limit,
			       const char *tail)
{
	if (segs_width(segs, n) <= limit)
		return draw_segs(c, row, col, segs, n);
	limit -= text_width(tail);
	if (limit < 0)
		return col;

	int used = 0;
	for (int i = 0; i < n; i++) {
		const char *s = segs[i].text;
		size_t len = strlen(s);
		for (size_t j = 0; j < len;) {
			int w;
			size_t cl = text_cluster(s + j, len - j, &w);
			if (used + w > limit)
				return canvas_text(c, row, col, tail, segs[i].style);
			col = canvas_text_n(c, row, col, s + j, cl, segs[i].style);
			used += w;
			j += cl;
		}
	}
	return col;
}

void draw_spread(canvas_t *c, int row, int col, const seg_t *left, int nleft, const seg_t *right, int nright,
		 int width)
{
	int wr = segs_width(right, nright);
	int start = col;
	col = draw_segs_truncated(c, row, col, left, nleft, width - wr - 1, "…");
	int pad = width - (col - start) - wr;
	col += pad > 1 ? pad : 1;
	draw_segs(c, row, col, right, nright);
}

/* A 3×5 pixel font. Each glyph is drawn into three terminal rows by pairing
 * pixel rows into half blocks. */
static const char *glyph_rows(char ch)
{
	switch (ch) {
	case '0': return "###" "#.#" "#.#" "#.#" "###";
	case '1': return ".#." "##." ".#." ".#." "###";
	case '2': return "###" "..#" "###" "#.." "###";
	case '3': return "###" "..#" "###" "..#" "###";
	case '4': return "#.#" "#.#" "###" "..#" "..#";
	case '5': return "###" "#.." "###" "..#" "###";
	case '6': return "###" "#.." "###" "#.#" "###";
	case '7': return "###" "..#" "..#" "..#" "..#";
	case '8': return "###" "#.#" "###" "#.#" "###";
	case '9': return "###" "#.#" "###" "..#" "###";
	case '.': return "....#";
	case '-': return "..." "..." "###" "..." "...";
	default: return NULL;
	}
}

static int glyph_width(const char *rows)
{
	return (int)strlen(rows) / 5;
}

int bigtext_width(const char *s)
{
	int w = 0;
	for (size_t i = 0; s[i]; i++) {
		const char *g = glyph_rows(s[i]);
		if (!g)
			continue;
		if (i > 0)
			w++; /* gap, counted by byte offset as in the Go version */
		w += glyph_width(g);
	}
	return w;
}

static const char *half_block(bool top, bool bottom)
{
	if (top && bottom)
		return "█";
	if (top)
		return "▀";
	if (bottom)
		return "▄";
	return " ";
}

void draw_bigtext(canvas_t *c, int row, int col, const char *s, rgb16_t from, rgb16_t to)
{
	int width = bigtext_width(s);
	if (width == 0)
		return;
	rgb16_t ramp[256];
	if (width > (int)ARRAY_LEN(ramp))
		width = (int)ARRAY_LEN(ramp);
	color_blend(ramp, width, from, to);

	int x = 0;
	for (size_t i = 0; s[i] && x < width; i++) {
		const char *g = glyph_rows(s[i]);
		if (!g)
			continue;
		if (i > 0) {
			for (int r = 0; r < 3; r++)
				canvas_text(c, row + r, col + x, " ", style_fg(ramp[x]));
			x++;
		}
		int gw = glyph_width(g);
		for (int gx = 0; gx < gw && x < width; gx++, x++) {
			for (int r = 0; r < 3; r++) {
				bool top = g[(2 * r) * gw + gx] == '#';
				bool bottom = 2 * r + 1 < 5 && g[(2 * r + 1) * gw + gx] == '#';
				canvas_text(c, row + r, col + x, half_block(top, bottom), style_fg(ramp[x]));
			}
		}
	}
}

void draw_chart(canvas_t *c, int row, int col, const double *values, size_t n, int slots, int width, int height,
		rgb16_t from, rgb16_t to, rgb16_t faint)
{
	static const char *const bars[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
	/* Markers stored in place of a column's level. */
	static const double FUTURE = -1, GAP = -2, PADDING = -3;

	if (width <= 0 || height <= 0 || slots <= 0)
		return;
	double peak = 0;
	for (size_t i = 0; i < n; i++)
		if (values[i] > peak && isfinite(values[i]))
			peak = values[i];

	/* Bars of equal width need a whole number of columns per slot. */
	int per_slot = width / slots;
	bool spaced = per_slot >= 3;
	double levels[1024];
	if (width > (int)ARRAY_LEN(levels))
		width = (int)ARRAY_LEN(levels);
	for (int i = 0; i < width; i++) {
		long lo = (long)i * slots / width, hi = (long)(i + 1) * slots / width;
		if (spaced) {
			lo = i / per_slot;
			hi = lo + 1;
		}
		if (spaced && lo >= slots) {
			levels[i] = PADDING;
		} else if ((size_t)lo >= n) {
			levels[i] = FUTURE;
		} else if (spaced && i % per_slot == per_slot - 1) {
			levels[i] = GAP;
		} else {
			if (hi < lo + 1)
				hi = lo + 1;
			if ((size_t)hi > n)
				hi = (long)n;
			double sum = 0;
			for (long j = lo; j < hi; j++)
				sum += values[j];
			levels[i] = peak > 0 ? sum / (double)(hi - lo) / peak : 0;
		}
	}

	rgb16_t ramp[64];
	if (height > (int)ARRAY_LEN(ramp))
		height = (int)ARRAY_LEN(ramp);
	color_blend(ramp, height, to, from);
	for (int r = 0; r < height; r++) {
		double below = (double)((height - 1 - r) * 8);
		style_t data = style_fg(ramp[r]), rest = style_fg(faint);
		for (int i = 0; i < width; i++) {
			double level = levels[i];
			const char *ch;
			style_t st = data;
			if (level == PADDING) {
				ch = " ";
				st = rest;
			} else if (level == FUTURE) {
				ch = r == height - 1 ? "·" : " ";
				st = rest;
			} else if (level == GAP) {
				ch = " ";
			} else {
				double e = round(level * (double)(height * 8) - below);
				/* Out-of-range input must not reach an undefined
				 * float-to-int conversion. */
				int eighths = isfinite(e) ? (int)fmax(-1.0, fmin(e, 9.0)) : 0;
				if (r == height - 1 && level > 0 && eighths < 1)
					eighths = 1;
				ch = bars[eighths < 0 ? 0 : eighths > 8 ? 8 : eighths];
			}
			canvas_text(c, row + r, col + i, ch, st);
		}
	}
}

void draw_progress(canvas_t *c, int row, int col, int width, double pct, rgb16_t from, rgb16_t to,
		   rgb16_t empty)
{
	if (width <= 0)
		return;
	double f = round((double)width * pct);
	int filled = isfinite(f) ? (int)fmax(0.0, fmin(f, (double)width)) : 0;
	rgb16_t blend[1024];
	if (width > (int)ARRAY_LEN(blend))
		width = (int)ARRAY_LEN(blend);
	if (filled > width)
		filled = width;
	color_blend(blend, width, from, to);
	for (int i = 0; i < filled; i++)
		canvas_text(c, row, col + i, "━", style_fg(blend[i]));
	canvas_repeat(c, row, col + filled, "─", width - filled, style_fg(empty));
}

void draw_box(canvas_t *c, int row, int col, int width, int height, rgb16_t border)
{
	style_t st = style_fg(border);
	canvas_text(c, row, col, "╭", st);
	canvas_repeat(c, row, col + 1, "─", width - 2, st);
	canvas_text(c, row, col + width - 1, "╮", st);
	for (int r = 1; r < height - 1; r++) {
		canvas_text(c, row + r, col, "│", st);
		canvas_text(c, row + r, col + width - 1, "│", st);
	}
	canvas_text(c, row + height - 1, col, "╰", st);
	canvas_repeat(c, row + height - 1, col + 1, "─", width - 2, st);
	canvas_text(c, row + height - 1, col + width - 1, "╯", st);
}
