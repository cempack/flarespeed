/* The building blocks of the view, each drawn straight into a canvas. */
#ifndef FLARESPEED_WIDGETS_H
#define FLARESPEED_WIDGETS_H

#include <stddef.h>

#include "canvas.h"

/* A run of text in one style. */
typedef struct {
	const char *text;
	style_t style;
} seg_t;

int segs_width(const seg_t *segs, int n);

/* Draws segments in order and returns the column after them. */
int draw_segs(canvas_t *c, int row, int col, const seg_t *segs, int n);

/* Places left and right at opposite ends of a line width columns wide,
 * cutting left short with "…" when both do not fit. */
void draw_spread(canvas_t *c, int row, int col, const seg_t *left, int nleft, const seg_t *right, int nright,
		 int width);

/* Width of s in the big three-row font. */
int bigtext_width(const char *s);

/* Draws s in the big font across three rows, colored with a left-to-right
 * gradient. Characters missing from the font are skipped. */
void draw_bigtext(canvas_t *c, int row, int col, const char *s, rgb16_t from, rgb16_t to);

/* Draws values as a bar chart width columns wide and height rows tall.
 * slots is how many values the finished chart will hold, so a partly filled
 * chart grows from the left and the rest is a faint baseline. When there is
 * room each value gets its own bar with a gap after it; when there are more
 * slots than columns each column shows the average of its slots. */
void draw_chart(canvas_t *c, int row, int col, const double *values, size_t n, int slots, int width, int height,
		rgb16_t from, rgb16_t to, rgb16_t faint);

/* Draws a thin progress bar filled to pct with a gradient. */
void draw_progress(canvas_t *c, int row, int col, int width, double pct, rgb16_t from, rgb16_t to,
		   rgb16_t empty);

/* Draws a rounded border around a width × height block. */
void draw_box(canvas_t *c, int row, int col, int width, int height, rgb16_t border);

#endif
