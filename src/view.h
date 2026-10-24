/* Draws the whole interface for a model. */
#ifndef FLARESPEED_VIEW_H
#define FLARESPEED_VIEW_H

#include "canvas.h"
#include "model.h"

/* What the terminal's own progress indicator (OSC 9;4) should show. */
typedef enum {
	TERM_PROGRESS_NONE,
	TERM_PROGRESS_NORMAL,
	TERM_PROGRESS_ERROR,
} term_progress_state_t;

typedef struct {
	term_progress_state_t state;
	int value; /* 0-100 */
} term_progress_t;

/* The spinner animation, advanced twelve times a second. */
#define SPINNER_FRAMES 10
#define SPINNER_INTERVAL (NS_PER_SEC / 12)

/* Redraws c from scratch for the model's current state. */
void view_render(const model_t *m, canvas_t *c, term_progress_t *progress);

#endif
