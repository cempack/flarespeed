/* server_timing_ns reads Server-Timing the way Cloudflare's speed test
 * client does (github.com/cloudflare/speedtest). */
#include "stats.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "format.h"
#include "util.h"

static int compare_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return (x > y) - (x < y);
}

latency_t latency_summarize(const int64_t *samples, size_t n)
{
	latency_t l = {0, 0, 0, 0};
	if (n == 0)
		return l;

	int64_t diffs = 0;
	for (size_t i = 1; i < n; i++) {
		int64_t d = samples[i] - samples[i - 1];
		diffs += d < 0 ? -d : d;
	}
	int64_t *sorted = xreallocarray(NULL, n, sizeof(*sorted));
	memcpy(sorted, samples, n * sizeof(*sorted));
	qsort(sorted, n, sizeof(*sorted), compare_i64);

	l.min = sorted[0];
	l.max = sorted[n - 1];
	l.median = n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
	if (n > 1)
		l.jitter = diffs / (int64_t)(n - 1);
	free(sorted);
	return l;
}

double point_rate(point_t from, point_t to)
{
	if (to.at <= from.at)
		return 0;
	return (double)(to.bytes - from.bytes) * 8 / ns_to_seconds(to.at - from.at);
}

point_t point_before(const point_t *history, size_t n, int64_t t)
{
	for (size_t i = n - 1; i > 0; i--)
		if (history[i].at <= t)
			return history[i];
	return history[0];
}

/* \s in Go's regexp syntax. */
static bool is_re_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static bool eat(const char **p, const char *word)
{
	size_t n = strlen(word);
	if (strncasecmp(*p, word, n) != 0)
		return false;
	*p += n;
	return true;
}

static bool is_letter(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Matches ";\s*dur=([0-9.]+)" at p, storing the value span. */
static bool match_value(const char *p, const char **value, size_t *len)
{
	if (*p != ';')
		return false;
	p++;
	while (is_re_space(*p))
		p++;
	if (!eat(&p, "dur="))
		return false;
	size_t n = strspn(p, "0123456789.");
	if (n == 0)
		return false;
	*value = p;
	*len = n;
	return true;
}

/* cfReq(?:uest)?Dur(?:ation)? */
static bool match_request_duration(const char *p, const char **value, size_t *len)
{
	if (!eat(&p, "cfReq"))
		return false;
	eat(&p, "uest");
	if (!eat(&p, "Dur"))
		return false;
	eat(&p, "ation");
	return match_value(p, value, len);
}

/* cfSpeed[a-z]* */
static bool match_speed_duration(const char *p, const char **value, size_t *len)
{
	if (!eat(&p, "cfSpeed"))
		return false;
	while (is_letter(*p))
		p++;
	return match_value(p, value, len);
}

/* time.Duration(strconv.ParseFloat(value) * float64(time.Millisecond)),
 * including how Go on amd64 turns out-of-range floats into MinInt64. */
static int64_t ms_to_duration(const char *value, size_t len)
{
	char tmp[64];
	size_t digits = 0, dots = 0;
	for (size_t i = 0; i < len; i++)
		value[i] == '.' ? dots++ : digits++;
	if (dots > 1 || digits == 0)
		return 0; /* ParseFloat syntax error yields 0 */
	if (len >= sizeof(tmp))
		return INT64_MIN; /* far beyond the int64 range */
	memcpy(tmp, value, len);
	tmp[len] = '\0';
	double ns = strtod(tmp, NULL) * 1e6;
	if (!(ns < 9223372036854775808.0))
		return INT64_MIN;
	return (int64_t)ns;
}

/* Entries can start where Go's (?:^|,\s*) matches: at the beginning or after
 * a comma and any whitespace. Scanning those positions left to right finds
 * the same leftmost matches as the regular expressions in the Go version. */
int64_t server_timing_ns(const char *header)
{
	const char *value;
	size_t len;

	for (const char *p = header;;) {
		if (match_request_duration(p, &value, &len))
			return ms_to_duration(value, len);
		p = strchr(p, ',');
		if (!p)
			break;
		p++;
		while (is_re_space(*p))
			p++;
	}

	/* Durations add up with Go's wrapping int64 arithmetic. */
	uint64_t sum = 0;
	for (const char *p = header;;) {
		if (match_speed_duration(p, &value, &len)) {
			sum += (uint64_t)ms_to_duration(value, len);
			p = value + len;
		}
		p = strchr(p, ',');
		if (!p)
			break;
		p++;
		while (is_re_space(*p))
			p++;
	}
	return (int64_t)sum;
}
