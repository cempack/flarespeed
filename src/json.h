/* The little JSON the program speaks: decoding Cloudflare's /meta response
 * and encoding the results report, both with encoding/json semantics. */
#ifndef FLARESPEED_JSON_H
#define FLARESPEED_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

/* Who the client is and which Cloudflare data center serves the test. Every
 * string is non-NULL after a successful parse. */
typedef struct {
	char *ip, *org, *city, *country;
	char *colo_iata, *colo_city;
	long long asn;
} meta_t;

void meta_free(meta_t *m);
void meta_copy(meta_t *dst, const meta_t *src);

/* Decodes a /meta body. On failure *out is left zeroed. */
bool meta_parse(const char *data, size_t len, meta_t *out);

/* Appends s as a JSON string literal, escaped like encoding/json. */
void json_put_string(buf_t *b, const char *s);

/* Appends v in encoding/json's shortest round-trip form. */
void json_put_number(buf_t *b, double v);

#endif
