#include "canvas.h"

#include <stdlib.h>
#include <string.h>

#include "text.h"

style_t style_fg(rgb16_t fg)
{
	return (style_t){fg, {0, 0, 0}, STYLE_FG};
}

style_t style_bold_fg(rgb16_t fg)
{
	return (style_t){fg, {0, 0, 0}, STYLE_FG | STYLE_BOLD};
}

static const cell_t blank_cell = {" ", 1, {{0, 0, 0}, {0, 0, 0}, 0}};

void canvas_init(canvas_t *c)
{
	c->lines = NULL;
	c->n = 0;
	c->cap = 0;
}

void canvas_free(canvas_t *c)
{
	for (size_t i = 0; i < c->cap; i++)
		free(c->lines[i].cells);
	free(c->lines);
	canvas_init(c);
}

void canvas_clear(canvas_t *c)
{
	for (int i = 0; i < c->n; i++)
		c->lines[i].len = 0;
	c->n = 0;
}

void canvas_ensure_rows(canvas_t *c, int rows)
{
	if (rows <= c->n)
		return;
	if ((size_t)rows > c->cap) {
		size_t old = c->cap;
		GROW(c->lines, c->cap, rows);
		for (size_t i = old; i < c->cap; i++)
			c->lines[i] = (line_t){NULL, 0, 0};
	}
	for (int i = c->n; i < rows; i++)
		c->lines[i].len = 0;
	c->n = rows;
}

/* Extends a line with blanks so that column col exists. */
static void line_extend(line_t *l, int col)
{
	if (col < l->len)
		return;
	GROW(l->cells, l->cap, col + 1);
	while (l->len <= col)
		l->cells[l->len++] = blank_cell;
}

/* Clears whatever wide character overlaps column col. */
static void line_break_wide(line_t *l, int col)
{
	if (col >= l->len)
		return;
	if (l->cells[col].width == 0 && col > 0)
		l->cells[col - 1] = blank_cell;
	else if (l->cells[col].width == 2 && col + 1 < l->len)
		l->cells[col + 1] = blank_cell;
	l->cells[col] = blank_cell;
}

static void put_cell(line_t *l, int col, const char *g, size_t len, int width, style_t style)
{
	if (col < 0)
		return;
	line_extend(l, col + (width == 2 ? 1 : 0));
	line_break_wide(l, col);
	if (width == 2)
		line_break_wide(l, col + 1);
	cell_t *cell = &l->cells[col];
	if (len >= CELL_TEXT) {
		/* Keep just the base character of an absurdly long cluster. */
		uint32_t cp;
		len = utf8_decode(g, len, &cp);
	}
	memcpy(cell->text, g, len);
	cell->text[len] = '\0';
	cell->width = (uint8_t)width;
	cell->style = style;
	if (width == 2) {
		cell_t *right = &l->cells[col + 1];
		right->text[0] = '\0';
		right->width = 0;
		right->style = style;
	}
}

int canvas_text(canvas_t *c, int row, int col, const char *s, style_t style)
{
	return canvas_text_n(c, row, col, s, strlen(s), style);
}

int canvas_text_n(canvas_t *c, int row, int col, const char *s, size_t n, style_t style)
{
	canvas_ensure_rows(c, row + 1);
	line_t *l = &c->lines[row];
	for (size_t i = 0; i < n;) {
		int width;
		size_t len = text_cluster(s + i, n - i, &width);
		if (width > 0) {
			put_cell(l, col, s + i, len, width, style);
			col += width;
		}
		i += len;
	}
	return col;
}

int canvas_repeat(canvas_t *c, int row, int col, const char *s, int count, style_t style)
{
	for (int i = 0; i < count; i++)
		col = canvas_text(c, row, col, s, style);
	return col;
}

static bool same_color(rgb16_t a, rgb16_t b)
{
	return a.r == b.r && a.g == b.g && a.b == b.b;
}

static bool same_style(style_t a, style_t b)
{
	if (a.flags != b.flags)
		return false;
	if ((a.flags & STYLE_FG) && !same_color(a.fg, b.fg))
		return false;
	if ((a.flags & STYLE_BG) && !same_color(a.bg, b.bg))
		return false;
	return true;
}

/* A blank looks the same under any foreground or boldness; only its
 * background shows. */
static bool blank_fits(style_t current, style_t blank)
{
	if ((current.flags & STYLE_BG) != (blank.flags & STYLE_BG))
		return false;
	return !(blank.flags & STYLE_BG) || same_color(current.bg, blank.bg);
}

static void put_color(buf_t *out, rgb16_t c, bool bg, profile_t profile)
{
	switch (profile) {
	case PROFILE_TRUECOLOR:
		buf_printf(out, ";%d;2;%u;%u;%u", bg ? 48 : 38, c.r >> 8, c.g >> 8, c.b >> 8);
		break;
	case PROFILE_ANSI256:
		buf_printf(out, ";%d;5;%d", bg ? 48 : 38, color_to_ansi256(c));
		break;
	case PROFILE_ANSI: {
		int idx = color_to_ansi16(c);
		int base = idx < 8 ? (bg ? 40 : 30) : (bg ? 100 : 90);
		buf_printf(out, ";%d", base + idx % 8);
		break;
	}
	default:
		break;
	}
}

static void put_sgr(buf_t *out, style_t s, profile_t profile)
{
	buf_puts(out, "\x1b[0");
	if (s.flags & STYLE_BOLD)
		buf_puts(out, ";1");
	if (profile >= PROFILE_ANSI) {
		if (s.flags & STYLE_FG)
			put_color(out, s.fg, false, profile);
		if (s.flags & STYLE_BG)
			put_color(out, s.bg, true, profile);
	}
	buf_putc(out, 'm');
}

/* The style as the profile can show it: no colors below ANSI, nothing at
 * all without a terminal. */
static style_t visible_style(style_t s, profile_t profile)
{
	if (profile <= PROFILE_NOTTY)
		return STYLE_PLAIN;
	if (profile == PROFILE_ASCII)
		s.flags &= STYLE_BOLD;
	return s;
}

int canvas_render_line(const canvas_t *c, int row, int max_width, profile_t profile, buf_t *out)
{
	if (row < 0 || row >= c->n)
		return 0;
	const line_t *l = &c->lines[row];

	int end = l->len;
	if (max_width > 0 && end > max_width)
		end = max_width;
	/* A wide character cut in half by the edge is dropped entirely. */
	if (end > 0 && end < l->len && l->cells[end].width == 0)
		end--;
	while (end > 0 && l->cells[end - 1].width == 1 && !strcmp(l->cells[end - 1].text, " ") &&
	       !(visible_style(l->cells[end - 1].style, profile).flags & STYLE_BG))
		end--;

	style_t current = STYLE_PLAIN;
	for (int i = 0; i < end; i++) {
		const cell_t *cell = &l->cells[i];
		if (cell->width == 0)
			continue;
		style_t s = visible_style(cell->style, profile);
		bool blank = !strcmp(cell->text, " ");
		if (!(blank ? blank_fits(current, s) : same_style(current, s))) {
			if (s.flags == 0)
				buf_puts(out, "\x1b[m");
			else
				put_sgr(out, s, profile);
			current = s;
		}
		buf_puts(out, cell->text);
	}
	if (current.flags)
		buf_puts(out, "\x1b[m");
	return end;
}
