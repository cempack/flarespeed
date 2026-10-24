#include "model.h"

#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "util.h"

const char *direction_name(direction_t dir)
{
	return dir == DIR_UPLOAD ? "upload" : "download";
}

void model_init(model_t *m, config_t cfg)
{
	memset(m, 0, sizeof(*m));
	m->cfg = cfg;
	m->dark = true;
}

static void forget_run(model_t *m)
{
	if (m->has_meta)
		meta_free(&m->meta);
	free(m->pings);
	free(m->samples[0]);
	free(m->samples[1]);
	free(m->err);
}

void model_free(model_t *m)
{
	forget_run(m);
	memset(m, 0, sizeof(*m));
}

void model_restart(model_t *m, int64_t now)
{
	model_t keep = *m;
	forget_run(m);
	model_init(m, keep.cfg);
	m->started = now;
	m->width = keep.width;
	m->dark = keep.dark;
	m->spinner_frame = keep.spinner_frame;
}

void model_set_phase(model_t *m, phase_t phase, int64_t now)
{
	m->phase = phase;
	if (phase == PHASE_DONE)
		m->finished = now - m->started;
}

/* Replaces *s with a copy that is safe to put on a terminal. */
static void sanitize(char **s)
{
	char *clean = text_sanitize(*s);
	free(*s);
	*s = clean;
}

void model_set_meta(model_t *m, const meta_t *meta)
{
	if (m->has_meta)
		meta_free(&m->meta);
	meta_copy(&m->meta, meta);
	char **fields[] = {&m->meta.ip, &m->meta.org, &m->meta.city, &m->meta.country, &m->meta.colo_iata,
			   &m->meta.colo_city};
	for (size_t i = 0; i < ARRAY_LEN(fields); i++)
		sanitize(fields[i]);
	m->has_meta = true;
}

void model_add_ping(model_t *m, int64_t ns)
{
	GROW(m->pings, m->pings_cap, m->npings + 1);
	m->pings[m->npings++] = ns;
}

void model_set_latency(model_t *m, latency_t latency)
{
	m->latency = latency;
	m->has_latency = true;
}

void model_add_sample(model_t *m, direction_t dir, sample_t sample)
{
	GROW(m->samples[dir], m->samples_cap[dir], m->nsamples[dir] + 1);
	m->samples[dir][m->nsamples[dir]++] = sample;
}

void model_set_transfer(model_t *m, direction_t dir, throughput_t result)
{
	m->transfer[dir] = result;
	m->has_transfer[dir] = true;
}

void model_set_error(model_t *m, const char *msg)
{
	free(m->err);
	m->err = text_sanitize(msg);
}

bool model_running(const model_t *m)
{
	return !m->err && m->phase != PHASE_DONE;
}

double model_transfer_progress(const model_t *m, direction_t dir)
{
	size_t n = m->nsamples[dir];
	if (n == 0)
		return 0;
	double p = (double)m->samples[dir][n - 1].elapsed / (double)m->cfg.duration;
	return p < 1 ? (p > 0 ? p : 0) : 1;
}

double model_overall_progress(const model_t *m)
{
	switch (m->phase) {
	case PHASE_LATENCY:
		return 0.1 * (double)m->npings / (double)m->cfg.pings;
	case PHASE_DOWNLOAD:
		return 0.1 + 0.45 * model_transfer_progress(m, DIR_DOWNLOAD);
	case PHASE_UPLOAD:
		return 0.55 + 0.45 * model_transfer_progress(m, DIR_UPLOAD);
	default:
		return 0;
	}
}
