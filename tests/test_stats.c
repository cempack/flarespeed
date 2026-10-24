#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "duration.h"
#include "format.h"
#include "harness.h"
#include "stats.h"

static void each_line(const char *path, void (*fn)(char *line))
{
	FILE *f = fopen(path, "r");
	CHECK(f != NULL);
	if (!f)
		return;
	char *line = NULL;
	size_t cap = 0;
	while (getline(&line, &cap, f) > 0)
		fn(line);
	free(line);
	fclose(f);
}

static void check_duration(char *line)
{
	char hex[256];
	int ok;
	long long want;
	if (sscanf(line, "%255s %d %lld", hex, &ok, &want) != 3)
		return;
	char *in = unhex(hex, NULL);
	int64_t got = 0;
	bool got_ok = duration_parse(in, &got);
	if (got_ok != ok || (ok && got != want))
		check_fail(__FILE__, __LINE__, "duration_parse(\"%s\") = %d/%lld, want %d/%lld", in, got_ok,
			   (long long)got, ok, want);
	free(in);
}

void test_duration_parse(void)
{
	each_line("data/duration.txt", check_duration);
}

static void check_format(char *line)
{
	char bps_s[64], value[32], unit[16], sig_ms[32], secs[32], ms_g[64], bytes_hex[64];
	long long ns, n;
	if (sscanf(line, "%63s %31s %15s %lld %31s %31s %63s %lld %63s", bps_s, value, unit, &ns, sig_ms, secs,
		   ms_g, &n, bytes_hex) != 9)
		return;
	char got[FMT_LEN];
	const char *got_unit;
	fmt_rate(got, &got_unit, strtod(bps_s, NULL));
	if (strcmp(got, value) != 0 || strcmp(got_unit, unit) != 0)
		check_fail(__FILE__, __LINE__, "fmt_rate(%s) = %s %s, want %s %s", bps_s, got, got_unit, value, unit);

	fmt_sig3(got, ns_to_ms(ns));
	if (strcmp(got, sig_ms) != 0)
		check_fail(__FILE__, __LINE__, "sig3(ms(%lld)) = %s, want %s", ns, got, sig_ms);
	if (ns_to_ms(ns) != strtod(ms_g, NULL))
		check_fail(__FILE__, __LINE__, "ns_to_ms(%lld) differs from Go", ns);

	fmt_seconds(got, ns);
	if (strcmp(got, secs) != 0)
		check_fail(__FILE__, __LINE__, "fmt_seconds(%lld) = %s, want %s", ns, got, secs);

	char *want_bytes = unhex(bytes_hex, NULL);
	fmt_bytes(got, n);
	if (strcmp(got, want_bytes) != 0)
		check_fail(__FILE__, __LINE__, "fmt_bytes(%lld) = %s, want %s", n, got, want_bytes);
	free(want_bytes);
}

void test_format(void)
{
	each_line("data/format.txt", check_format);

	char out[FMT_LEN];
	fmt_sig3(out, 9.996);
	CHECK_STR(out, "10.0");
	fmt_sig3(out, 1234.5);
	CHECK_STR(out, "1234");
	fmt_ms(out, 21800000);
	CHECK_STR(out, "21.8 ms");
	fmt_rate_text(out, 48.7e6);
	CHECK_STR(out, "48.7 Mbps");
}

static void check_summarize(char *line)
{
	long long med, mn, mx, jit;
	int n, off;
	if (sscanf(line, "%lld %lld %lld %lld %d%n", &med, &mn, &mx, &jit, &n, &off) != 5)
		return;
	int64_t samples[64];
	char *p = line + off;
	for (int i = 0; i < n; i++)
		samples[i] = strtoll(p, &p, 10);
	latency_t l = latency_summarize(samples, (size_t)n);
	if (l.median != med || l.min != mn || l.max != mx || l.jitter != jit)
		check_fail(__FILE__, __LINE__, "summarize(n=%d) = %lld/%lld/%lld/%lld, want %lld/%lld/%lld/%lld", n,
			   (long long)l.median, (long long)l.min, (long long)l.max, (long long)l.jitter, med, mn, mx,
			   jit);
}

void test_summarize(void)
{
	each_line("data/summarize.txt", check_summarize);

	point_t history[] = {{0, 0}, {100, 10}, {200, 20}, {300, 30}};
	CHECK_INT(point_before(history, 4, 250).at, 200);
	CHECK_INT(point_before(history, 4, -5).at, 0);
	CHECK_INT(point_before(history, 4, 1000).at, 300);
	CHECK(point_rate((point_t){0, 0}, (point_t){1000000000, 125}) == 1000.0);
	CHECK(point_rate((point_t){5, 0}, (point_t){5, 100}) == 0.0);
}

static void check_server_timing(char *line)
{
	char *hex = strtok(line, " \n"), *want_s = strtok(NULL, " \n");
	if (!hex || !want_s)
		return;
	char *in = unhex(hex, NULL);
	long long want = strtoll(want_s, NULL, 10);
	int64_t got = server_timing_ns(in);
	if (got != want)
		check_fail(__FILE__, __LINE__, "server_timing_ns(\"%s\") = %lld, want %lld", in, (long long)got, want);
	free(in);
}

void test_server_timing(void)
{
	each_line("data/servertiming.txt", check_server_timing);
}
