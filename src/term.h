/* The interactive terminal: raw input, capability queries and an inline
 * renderer that redraws the frame in place below the shell prompt. */
#ifndef FLARESPEED_TERM_H
#define FLARESPEED_TERM_H

#include <stdbool.h>
#include <sys/types.h>
#include <termios.h>

#include "canvas.h"
#include "profile.h"
#include "view.h"

typedef struct {
	int in_fd, out_fd;
	bool close_in;
	bool raw;
	struct termios saved;

	int width, height; /* 0 when unknown */
	profile_t profile;
	bool sync; /* synchronized output (mode 2026) */

	char **lines; /* the frame on screen, as written */
	int *line_widths;
	int nlines, lines_cap;
	bool drawn, redraw_all, broken;
	term_progress_t progress;
	buf_t out;
} term_t;

/* Puts the terminal in raw mode, hides the cursor and sends the capability
 * queries. Returns false (with errno set) if there is no terminal. */
bool term_open(term_t *t, profile_t profile);

/* Rereads the window size; a change makes the next render redraw fully. */
void term_update_size(term_t *t);

/* Draws the canvas in place of the previous frame. */
void term_render(term_t *t, const canvas_t *c, term_progress_t progress);

/* Leaves the cursor on the line after the frame and undoes every mode the
 * program set, then restores the original terminal settings. */
void term_close(term_t *t);

/* Restores the original terminal settings only; safe to call twice. */
void term_restore(term_t *t);

ssize_t term_read(term_t *t, char *buf, size_t n);

#endif
