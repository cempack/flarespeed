/* A grid of styled cells that the view draws into and the renderer turns
 * into terminal output. */
#ifndef FLARESPEED_CANVAS_H
#define FLARESPEED_CANVAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "color.h"
#include "profile.h"
#include "util.h"

enum {
	STYLE_FG = 1 << 0,
	STYLE_BG = 1 << 1,
	STYLE_BOLD = 1 << 2,
};

typedef struct {
	rgb16_t fg, bg;
	uint8_t flags;
} style_t;

#define STYLE_PLAIN ((style_t){{0, 0, 0}, {0, 0, 0}, 0})

style_t style_fg(rgb16_t fg);
style_t style_bold_fg(rgb16_t fg);

#define CELL_TEXT 32

typedef struct {
	/* One grapheme cluster as UTF-8; empty for the right half of a wide
	 * character. */
	char text[CELL_TEXT];
	uint8_t width;
	style_t style;
} cell_t;

typedef struct {
	cell_t *cells;
	int len;
	size_t cap;
} line_t;

typedef struct {
	line_t *lines;
	int n;
	size_t cap;
} canvas_t;

void canvas_init(canvas_t *c);
void canvas_free(canvas_t *c);
/* Empties the canvas, keeping its memory for the next frame. */
void canvas_clear(canvas_t *c);
/* Makes sure the canvas has at least rows lines. */
void canvas_ensure_rows(canvas_t *c, int rows);

/* Draws UTF-8 text at row, col and returns the column after it. */
int canvas_text(canvas_t *c, int row, int col, const char *s, style_t style);
/* The same for the first n bytes of s. */
int canvas_text_n(canvas_t *c, int row, int col, const char *s, size_t n, style_t style);

/* Draws the same text count times. */
int canvas_repeat(canvas_t *c, int row, int col, const char *s, int count, style_t style);

/* Appends one line of the canvas as terminal output for the given color
 * profile, cut to max_width columns when max_width > 0. Trailing blank cells
 * are left out. Returns the width of what was written. */
int canvas_render_line(const canvas_t *c, int row, int max_width, profile_t profile, buf_t *out);

#endif
