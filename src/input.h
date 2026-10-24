/* Decodes terminal input into the few events the program cares about. */
#ifndef FLARESPEED_INPUT_H
#define FLARESPEED_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

/* A lone ESC becomes the Esc key if nothing follows within this time. */
#define ESC_TIMEOUT (50 * NS_PER_MS)

typedef enum {
	INPUT_QUIT,	      /* q, Esc or Ctrl+C */
	INPUT_RERUN,	      /* r */
	INPUT_BACKGROUND,     /* reply to the background color query */
	INPUT_SYNC_SUPPORTED, /* synchronized output can be enabled */
	INPUT_DA1,	      /* reply to the device attributes query */
} input_event_kind_t;

typedef struct {
	input_event_kind_t kind;
	bool dark; /* for INPUT_BACKGROUND */
} input_event_t;

typedef void (*input_handler_fn)(void *ctx, input_event_t ev);

typedef struct {
	int state;
	char seq[256];
	size_t len;
	bool overflow;
	int64_t esc_at;
	size_t paste_match;
} input_t;

void input_init(input_t *in);

/* Feeds bytes read from the terminal at time now. */
void input_feed(input_t *in, const char *data, size_t n, int64_t now, input_handler_fn fn, void *ctx);

/* Settles a pending lone ESC once ESC_TIMEOUT has passed. */
void input_tick(input_t *in, int64_t now, input_handler_fn fn, void *ctx);

/* When input_tick next needs calling, or INT64_MAX. */
int64_t input_deadline(const input_t *in);

/* Whether an OSC 11 color reply ("rgb:1e1e/1e1e/2e2e", "#rrggbb", ...)
 * describes a dark background, following uv.BackgroundColorEvent.IsDark. */
bool input_color_is_dark(const char *spec);

#endif
