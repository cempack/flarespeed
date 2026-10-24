/* Measurement arithmetic: latency summaries, throughput rates and the
 * server's self-reported processing time. */
#ifndef FLARESPEED_STATS_H
#define FLARESPEED_STATS_H

#include <stddef.h>
#include <stdint.h>

/* Summary of a series of round trips, in nanoseconds. */
typedef struct {
	int64_t median, min, max;
	/* Mean difference between consecutive samples. */
	int64_t jitter;
} latency_t;

latency_t latency_summarize(const int64_t *samples, size_t n);

/* Bytes transferred by a point in time since the start of a test. */
typedef struct {
	int64_t at;
	int64_t bytes;
} point_t;

/* Rate between two points in bits per second; 0 if no time passed. */
double point_rate(point_t from, point_t to);

/* The latest point recorded at or before t, or the first point. */
point_t point_before(const point_t *history, size_t n, int64_t t);

/* Server processing time from a Server-Timing header, the way Cloudflare's
 * own client reads it: a cfRequestDuration entry if present, otherwise the
 * sum of every cfSpeed* entry. */
int64_t server_timing_ns(const char *header);

#endif
