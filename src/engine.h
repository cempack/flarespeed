/*
 * The speed test itself: metadata, idle latency, then download and upload,
 * run against Cloudflare's speed test service on libcurl's multi interface.
 * Everything happens on the caller's thread: engine_poll waits for network
 * activity (and any extra file descriptors) and advances the test.
 */
#ifndef FLARESPEED_ENGINE_H
#define FLARESPEED_ENGINE_H

#include <curl/curl.h>
#include <stdbool.h>

#include "model.h"

#define DEFAULT_BASE_URL "https://speed.cloudflare.com"

/* Progress callbacks, called from engine_poll and engine_start. */
typedef struct {
	void *ctx;
	void (*phase)(void *ctx, phase_t phase);
	void (*meta)(void *ctx, const meta_t *meta);
	void (*ping)(void *ctx, int64_t ns);
	void (*latency)(void *ctx, latency_t latency);
	void (*sample)(void *ctx, direction_t dir, sample_t sample);
	void (*throughput)(void *ctx, direction_t dir, const throughput_t *result);
	void (*error)(void *ctx, const char *message);
} engine_events_t;

typedef struct engine engine_t;

/* Final results of a completed run. */
typedef struct {
	bool has_meta;
	const meta_t *meta;
	latency_t latency;
	throughput_t download, upload;
} engine_results_t;

engine_t *engine_new(config_t cfg, const char *base_url, const engine_events_t *events);
void engine_free(engine_t *e);

/* Starts a run from the beginning, abandoning any run in progress. */
void engine_start(engine_t *e);
/* Abandons the run in progress, if any. */
void engine_stop(engine_t *e);

/* True while a run is in progress (not finished, failed or stopped). */
bool engine_active(const engine_t *e);
bool engine_failed(const engine_t *e);
const char *engine_error(const engine_t *e);
phase_t engine_phase(const engine_t *e);
void engine_results(const engine_t *e, engine_results_t *out);

/* Waits up to timeout_ms for network activity, the engine's own timers or
 * the extra descriptors, then does all the work that is due. The revents of
 * the extra descriptors are filled in. */
void engine_poll(engine_t *e, struct curl_waitfd *extra, unsigned nextra, int timeout_ms);

#endif
