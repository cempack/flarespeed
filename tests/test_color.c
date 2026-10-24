#include <stdio.h>
#include <stdlib.h>

#include "color.h"
#include "harness.h"

static rgb16_t parse_hex(const char *s)
{
	unsigned long v = strtoul(s + 1, NULL, 16);
	return RGB_HEX(v);
}

void test_color_blend(void)
{
	FILE *f = fopen("data/blend.txt", "r");
	CHECK(f != NULL);
	if (!f)
		return;
	char line[8192];
	int lines = 0;
	while (fgets(line, sizeof(line), f)) {
		char from[16], to[16];
		int steps, off;
		if (sscanf(line, "%15s %15s %d%n", from, to, &steps, &off) != 3)
			continue;
		rgb16_t out[128];
		color_blend(out, steps, parse_hex(from), parse_hex(to));
		const char *p = line + off;
		for (int i = 0; i < steps; i++) {
			unsigned r, g, b;
			int used;
			if (sscanf(p, " %u,%u,%u%n", &r, &g, &b, &used) != 3) {
				check_fail(__FILE__, __LINE__, "bad vector line");
				break;
			}
			p += used;
			if (out[i].r != r || out[i].g != g || out[i].b != b)
				check_fail(__FILE__, __LINE__, "blend %s→%s/%d[%d] = %u,%u,%u, want %u,%u,%u", from,
					   to, steps, i, out[i].r, out[i].g, out[i].b, r, g, b);
		}
		lines++;
	}
	fclose(f);
	CHECK(lines > 90);
}

void test_color_convert(void)
{
	FILE *f = fopen("data/convert256.txt", "r");
	CHECK(f != NULL);
	if (!f)
		return;
	unsigned r, g, b;
	int want256, want16, n = 0, bad = 0;
	while (fscanf(f, "%u %u %u %d %d", &r, &g, &b, &want256, &want16) == 5) {
		rgb16_t c = {(uint16_t)r, (uint16_t)g, (uint16_t)b};
		int got256 = color_to_ansi256(c), got16 = color_to_ansi16(c);
		if ((got256 != want256 || got16 != want16) && bad++ < 10)
			check_fail(__FILE__, __LINE__, "convert(%u,%u,%u) = %d/%d, want %d/%d", r, g, b, got256,
				   got16, want256, want16);
		n++;
	}
	fclose(f);
	CHECK_INT(n, 20000);
	CHECK_INT(bad, 0);
}
