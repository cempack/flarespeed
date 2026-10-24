/* Colors with 16 bits per channel, blended and downsampled exactly as
 * lipgloss and colorprofile do it. */
#ifndef FLARESPEED_COLOR_H
#define FLARESPEED_COLOR_H

#include <stdint.h>

typedef struct {
	uint16_t r, g, b;
} rgb16_t;

/* A color given as 8-bit channels, like lipgloss.Color("#RRGGBB"). */
#define RGB_HEX(hex)                                                           \
	((rgb16_t){(uint16_t)((((hex) >> 16) & 0xFF) * 257),                   \
		   (uint16_t)((((hex) >> 8) & 0xFF) * 257),                    \
		   (uint16_t)(((hex) & 0xFF) * 257)})

/* Fills out[0..steps) with a CIELAB gradient from one color to another,
 * like lipgloss.Blend1D(steps, from, to). */
void color_blend(rgb16_t *out, int steps, rgb16_t from, rgb16_t to);

/* Nearest xterm 256-color palette index, like ansi.Convert256. */
int color_to_ansi256(rgb16_t c);

/* Nearest of the 16 basic ANSI colors, like ansi.Convert16. */
int color_to_ansi16(rgb16_t c);

#endif
