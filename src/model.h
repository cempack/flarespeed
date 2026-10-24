/* Everything the UI shows about a test run, updated by engine events. */
#ifndef FLARESPEED_MODEL_H
#define FLARESPEED_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "json.h"
#include "stats.h"

typedef enum {
	PHASE_CONNECT,
	PHASE_LATENCY,
	PHASE_DOWNLOAD,
	PHASE_UPLOAD,
	PHASE_DONE,
} phase_t;

typedef enum {
	DIR_DOWNLOAD,
	DIR_UPLOAD,
} direction_t;

const char *direction_name(direction_t dir);

/* How often throughput is sampled while a transfer test runs. */
#define SAMPLE_INTERVAL (100 * NS_PER_MS)

typedef struct {
	int64_t duration; /* of each transfer test, in ns */
	int streams;
	int pings;
} config_t;

/* A live throughput reading. */
typedef struct {
	int64_t elapsed;
	/* Rate over the trailing second, in bits per second. */
	double bps;
} sample_t;

/* The outcome of a download or upload test. */
typedef struct {
	/* Steady-state rate after warm-up, in bits per second. */
	double bps;
	int64_t bytes;
	int64_t duration;
	/* Latency measured while the link was saturated. */
	latency_t loaded;
} throughput_t;

typedef struct {
	config_t cfg;

	phase_t phase;
	bool has_meta;
	meta_t meta;
	int64_t *pings;
	size_t npings, pings_cap;
	bool has_latency;
	latency_t latency;
	sample_t *samples[2];
	size_t nsamples[2], samples_cap[2];
	bool has_transfer[2];
	throughput_t transfer[2];
	char *err;

	int64_t started, finished;

	/* Presentation state that survives restarts. */
	int width;
	bool quitting;
	bool dark;
	int spinner_frame;
} model_t;

void model_init(model_t *m, config_t cfg);
void model_free(model_t *m);
/* Forgets the previous run, keeping the configuration and presentation
 * state, and marks now as the start of a new one. */
void model_restart(model_t *m, int64_t now);

void model_set_phase(model_t *m, phase_t phase, int64_t now);
void model_set_meta(model_t *m, const meta_t *meta);
void model_add_ping(model_t *m, int64_t ns);
void model_set_latency(model_t *m, latency_t latency);
void model_add_sample(model_t *m, direction_t dir, sample_t sample);
void model_set_transfer(model_t *m, direction_t dir, throughput_t result);
void model_set_error(model_t *m, const char *msg);

bool model_running(const model_t *m);
/* Fraction of the transfer test in one direction that has elapsed. */
double model_transfer_progress(const model_t *m, direction_t dir);
/* Fraction of the whole run, weighting phases roughly by duration. */
double model_overall_progress(const model_t *m);

#endif
