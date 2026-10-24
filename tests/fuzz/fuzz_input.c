/* libFuzzer target for the terminal input decoder, fed in random slices
 * with random gaps in time. */
#include <stdint.h>
#include <stddef.h>

#include "input.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void sink(void *ctx, input_event_t ev)
{
	(*(int *)ctx) += (int)ev.kind + ev.dark;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	input_t in;
	input_init(&in);
	int count = 0;
	int64_t now = 0;
	for (size_t i = 0; i < size;) {
		size_t n = 1 + data[i] % 16;
		now += (int64_t)(data[i] >> 4) * 10 * NS_PER_MS;
		i++;
		if (n > size - i)
			n = size - i;
		input_feed(&in, (const char *)data + i, n, now, sink, &count);
		input_tick(&in, now, sink, &count);
		i += n;
	}
	input_tick(&in, now + ESC_TIMEOUT, sink, &count);
	return 0;
}
