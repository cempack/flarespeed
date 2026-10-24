/* A port of time.ParseDuration from the Go standard library (BSD-3-Clause,
 * The Go Authors). See THIRD-PARTY-NOTICES. */
#include "duration.h"

#include <stddef.h>
#include <string.h>

#define LIMIT (UINT64_C(1) << 63)

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

/* Consumes leading digits; fails if the value exceeds 1<<63. */
static bool leading_int(const char **s, uint64_t *out)
{
	uint64_t x = 0;
	for (; is_digit(**s); (*s)++) {
		if (x > LIMIT / 10)
			return false;
		x = x * 10 + (uint64_t)(**s - '0');
		if (x > LIMIT)
			return false;
	}
	*out = x;
	return true;
}

/* Consumes leading digits as a fraction, ignoring digits past what fits. */
static void leading_fraction(const char **s, uint64_t *x, double *scale)
{
	bool overflow = false;
	*x = 0;
	*scale = 1;
	for (; is_digit(**s); (*s)++) {
		if (overflow)
			continue;
		if (*x > (LIMIT - 1) / 10) {
			overflow = true;
			continue;
		}
		uint64_t y = *x * 10 + (uint64_t)(**s - '0');
		if (y > LIMIT) {
			overflow = true;
			continue;
		}
		*x = y;
		*scale *= 10;
	}
}

static bool unit_of(const char *u, size_t n, uint64_t *unit)
{
	static const struct {
		const char *name;
		uint64_t ns;
	} units[] = {
		{"ns", 1},
		{"us", 1000},
		{"\xC2\xB5s", 1000}, /* U+00B5 micro sign */
		{"\xCE\xBCs", 1000}, /* U+03BC Greek mu */
		{"ms", 1000000},
		{"s", UINT64_C(1000000000)},
		{"m", UINT64_C(60000000000)},
		{"h", UINT64_C(3600000000000)},
	};
	for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
		if (strlen(units[i].name) == n && memcmp(units[i].name, u, n) == 0) {
			*unit = units[i].ns;
			return true;
		}
	}
	return false;
}

bool duration_parse(const char *s, int64_t *out)
{
	uint64_t d = 0;
	bool neg = false;

	if (*s == '-' || *s == '+') {
		neg = *s == '-';
		s++;
	}
	if (strcmp(s, "0") == 0) {
		*out = 0;
		return true;
	}
	if (*s == '\0')
		return false;

	while (*s) {
		uint64_t v, f = 0, unit;
		double scale = 1;

		if (!(*s == '.' || is_digit(*s)))
			return false;
		const char *before = s;
		if (!leading_int(&s, &v))
			return false;
		bool pre = s != before;

		bool post = false;
		if (*s == '.') {
			s++;
			const char *frac = s;
			leading_fraction(&s, &f, &scale);
			post = s != frac;
		}
		if (!pre && !post)
			return false;

		size_t i = 0;
		while (s[i] && s[i] != '.' && !is_digit(s[i]))
			i++;
		if (i == 0 || !unit_of(s, i, &unit))
			return false;
		s += i;

		if (v > LIMIT / unit)
			return false;
		v *= unit;
		if (f > 0) {
			v += (uint64_t)((double)f * ((double)unit / scale));
			if (v > LIMIT)
				return false;
		}
		d += v;
		if (d > LIMIT)
			return false;
	}

	if (neg) {
		*out = d == LIMIT ? INT64_MIN : -(int64_t)d;
		return true;
	}
	if (d > LIMIT - 1)
		return false;
	*out = (int64_t)d;
	return true;
}
