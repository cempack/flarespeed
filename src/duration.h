/* Go-style duration strings such as "10s", "1m30s" or "1.5h". */
#ifndef FLARESPEED_DURATION_H
#define FLARESPEED_DURATION_H

#include <stdbool.h>
#include <stdint.h>

/* Parses s like time.ParseDuration, storing nanoseconds in *out. */
bool duration_parse(const char *s, int64_t *out);

#endif
