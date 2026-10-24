#include "format.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

/* strconv.FormatFloat(v, 'f', decimals, 64). */
static void format_fixed(char out[FMT_LEN], double v, int decimals)
{
	if (isnan(v))
		snprintf(out, FMT_LEN, "NaN");
	else if (isinf(v))
		snprintf(out, FMT_LEN, "%s", v > 0 ? "+Inf" : "-Inf");
	else
		snprintf(out, FMT_LEN, "%.*f", decimals, v);
}

void fmt_sig3(char out[FMT_LEN], double v)
{
	for (int decimals = 2; decimals >= 1; decimals--) {
		format_fixed(out, v, decimals);
		if (strlen(out) <= 4)
			return;
	}
	format_fixed(out, v, 0);
}

void fmt_rate(char value[FMT_LEN], const char **unit, double bps)
{
	if (bps >= 1e9) {
		fmt_sig3(value, bps / 1e9);
		*unit = "Gbps";
	} else if (bps >= 1e6) {
		fmt_sig3(value, bps / 1e6);
		*unit = "Mbps";
	} else {
		fmt_sig3(value, bps / 1e3);
		*unit = "Kbps";
	}
}

void fmt_rate_text(char out[FMT_LEN], double bps)
{
	char value[FMT_LEN];
	const char *unit;
	fmt_rate(value, &unit, bps);
	snprintf(out, FMT_LEN, "%.340s %.4s", value, unit);
}

double ns_to_ms(int64_t ns)
{
	return (double)ns / (double)NS_PER_MS;
}

double ns_to_seconds(int64_t ns)
{
	int64_t sec = ns / NS_PER_SEC, nsec = ns % NS_PER_SEC;
	return (double)sec + (double)nsec / 1e9;
}

void fmt_ms(char out[FMT_LEN], int64_t ns)
{
	char v[FMT_LEN];
	fmt_sig3(v, ns_to_ms(ns));
	snprintf(out, FMT_LEN, "%.340s ms", v);
}

void fmt_bytes(char out[FMT_LEN], int64_t n)
{
	const int64_t unit = 1000;
	if (n < unit) {
		snprintf(out, FMT_LEN, "%lld B", (long long)n);
		return;
	}
	int64_t div = unit;
	int exp = 0;
	for (int64_t v = n / unit; v >= unit; v /= unit) {
		div *= unit;
		exp++;
	}
	char v[FMT_LEN];
	fmt_sig3(v, (double)n / (double)div);
	snprintf(out, FMT_LEN, "%.340s %cB", v, "kMGTPE"[exp]);
}

void fmt_seconds(char out[FMT_LEN], int64_t ns)
{
	format_fixed(out, ns_to_seconds(ns), 1);
	size_t len = strlen(out);
	if (len + 1 < FMT_LEN) {
		out[len] = 's';
		out[len + 1] = '\0';
	}
}
