/* libFuzzer target for everything that parses outside input: the /meta
 * JSON, Server-Timing, durations, terminfo files and color replies. The
 * first byte picks the parser. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "duration.h"
#include "input.h"
#include "json.h"
#include "profile.h"
#include "stats.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 1)
		return 0;
	char *s = malloc(size);
	memcpy(s, data + 1, size - 1);
	s[size - 1] = '\0';
	switch (data[0] % 5) {
	case 0: {
		meta_t m;
		if (meta_parse((const char *)data + 1, size - 1, &m)) {
			buf_t b;
			buf_init(&b);
			json_put_string(&b, m.org);
			json_put_string(&b, m.city);
			buf_free(&b);
			meta_free(&m);
		}
		break;
	}
	case 1:
		(void)server_timing_ns(s);
		break;
	case 2: {
		int64_t d;
		(void)duration_parse(s, &d);
		break;
	}
	case 3:
		(void)terminfo_declares_truecolor(data + 1, size - 1);
		break;
	case 4:
		(void)input_color_is_dark(s);
		break;
	}
	free(s);
	return 0;
}
