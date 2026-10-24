/* libFuzzer target for text shaping and the canvas: arbitrary bytes are
 * measured, cut, wrapped, sanitized, drawn and rendered in every profile. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "canvas.h"
#include "text.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 2)
		return 0;
	int limit = data[0] % 80;
	char *s = malloc(size);
	memcpy(s, data + 1, size - 1);
	s[size - 1] = '\0';

	buf_t b;
	buf_init(&b);
	(void)text_width(s);
	text_truncate(&b, s, limit, "…");
	buf_clear(&b);
	text_wrap(&b, s, limit);
	char *clean = text_sanitize(s);

	canvas_t c;
	canvas_init(&c);
	style_t st = {{1, 2, 3}, {4, 5, 6}, (uint8_t)(data[0] & 7)};
	canvas_text(&c, data[0] % 3, limit / 2, clean, st);
	canvas_text(&c, 0, limit / 3, s, st); /* overlapping, unsanitized */
	for (int p = PROFILE_NOTTY; p <= PROFILE_TRUECOLOR; p++)
		for (int r = 0; r < c.n; r++) {
			buf_clear(&b);
			canvas_render_line(&c, r, limit, (profile_t)p, &b);
		}
	canvas_free(&c);
	free(clean);
	buf_free(&b);
	free(s);
	return 0;
}
