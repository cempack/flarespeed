/* libFuzzer target for the layout: random model states, including values
 * the engine never produces, must render without trouble. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "view.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

typedef struct {
	const uint8_t *p;
	size_t n;
} reader_t;

static uint64_t take(reader_t *r, int bytes)
{
	uint64_t v = 0;
	for (int i = 0; i < bytes; i++) {
		v = v << 8;
		if (r->n) {
			v |= *r->p++;
			r->n--;
		}
	}
	return v;
}

static double take_double(reader_t *r)
{
	uint64_t bits = take(r, 8);
	double d;
	memcpy(&d, &bits, sizeof(d));
	return d;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	reader_t r = {data, size};
	config_t cfg = {(int64_t)(2 + take(&r, 2)) * NS_PER_SEC, 1 + (int)take(&r, 1), 2 + (int)take(&r, 1)};
	model_t m;
	model_init(&m, cfg);
	m.width = (int)take(&r, 1) * 2;
	m.dark = take(&r, 1) & 1;
	m.spinner_frame = (int)take(&r, 1) % SPINNER_FRAMES;
	m.quitting = take(&r, 1) & 1;
	m.phase = (phase_t)(take(&r, 1) % 5);
	if (take(&r, 1) & 1) {
		meta_t meta = {strdup("1.2.3.4"), strdup("Org é 東京"), strdup("City"), strdup("FR"),
			       strdup("CDG"), strdup("Paris"), (long long)take(&r, 4)};
		model_set_meta(&m, &meta);
		meta_free(&meta);
	}
	int npings = (int)take(&r, 1);
	for (int i = 0; i < npings; i++)
		model_add_ping(&m, (int64_t)take(&r, 4) * 1000);
	if (take(&r, 1) & 1)
		model_set_latency(&m, (latency_t){(int64_t)take(&r, 4), 0, 0, (int64_t)take(&r, 4)});
	for (int d = 0; d < 2; d++) {
		int n = (int)take(&r, 1);
		for (int i = 0; i < n; i++) {
			double v = (take(&r, 1) & 7) == 0 ? take_double(&r) : (double)take(&r, 4) * 1000;
			model_add_sample(&m, (direction_t)d, (sample_t){(int64_t)take(&r, 4) * 1000, v});
		}
		if (take(&r, 1) & 1)
			model_set_transfer(&m, (direction_t)d,
					   (throughput_t){take_double(&r), (int64_t)take(&r, 5), 0,
							  {(int64_t)take(&r, 4), 0, 0, 0}});
	}
	if (take(&r, 1) == 7)
		model_set_error(&m, "download: server responded 503 Service-Unavailable-with-a-long-hyphenated-reason");
	m.finished = (int64_t)take(&r, 5);

	canvas_t c;
	canvas_init(&c);
	term_progress_t tp;
	view_render(&m, &c, &tp);
	buf_t b;
	buf_init(&b);
	for (int p = PROFILE_NOTTY; p <= PROFILE_TRUECOLOR; p++)
		for (int row = 0; row < c.n; row++) {
			buf_clear(&b);
			canvas_render_line(&c, row, m.width, (profile_t)p, &b);
		}
	buf_free(&b);
	canvas_free(&c);
	model_free(&m);
	return 0;
}
