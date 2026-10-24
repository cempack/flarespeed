/* Decoding and string escaping follow Go's encoding/json (BSD-3-Clause, The
 * Go Authors). See THIRD-PARTY-NOTICES. */
#include "json.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"

/* encoding/json refuses documents nested deeper than this. */
#define MAX_DEPTH 10000

void meta_free(meta_t *m)
{
	free(m->ip);
	free(m->org);
	free(m->city);
	free(m->country);
	free(m->colo_iata);
	free(m->colo_city);
	memset(m, 0, sizeof(*m));
}

static char *dup_or_empty(const char *s)
{
	return xstrdup(s ? s : "");
}

void meta_copy(meta_t *dst, const meta_t *src)
{
	dst->ip = dup_or_empty(src->ip);
	dst->org = dup_or_empty(src->org);
	dst->city = dup_or_empty(src->city);
	dst->country = dup_or_empty(src->country);
	dst->colo_iata = dup_or_empty(src->colo_iata);
	dst->colo_city = dup_or_empty(src->colo_city);
	dst->asn = src->asn;
}

typedef struct {
	const char *p, *end;
	int depth;
	/* Like encoding/json, a value of the wrong type does not stop decoding
	 * but makes the whole result an error. */
	bool type_error;
} parser_t;

static void skip_ws(parser_t *ps)
{
	while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
		ps->p++;
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Reads \uXXXX at p, or returns -1. */
static long read_u4(const char *p, const char *end)
{
	if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
		return -1;
	long v = 0;
	for (int i = 2; i < 6; i++) {
		int h = hex_value(p[i]);
		if (h < 0)
			return -1;
		v = v * 16 + h;
	}
	return v;
}

static void put_rune(buf_t *out, uint32_t cp)
{
	char enc[4];
	if (cp == 0)
		return; /* C strings cannot hold NUL; drop it */
	buf_append(out, enc, utf8_encode(cp, enc));
}

/* Parses a string literal starting at the opening quote. With out == NULL
 * the string is only validated. */
static bool parse_string(parser_t *ps, buf_t *out)
{
	if (ps->p >= ps->end || *ps->p != '"')
		return false;
	ps->p++;
	while (ps->p < ps->end) {
		unsigned char c = (unsigned char)*ps->p;
		if (c == '"') {
			ps->p++;
			return true;
		}
		if (c < 0x20)
			return false;
		if (c != '\\') {
			uint32_t cp;
			size_t n = utf8_decode(ps->p, (size_t)(ps->end - ps->p), &cp);
			if (out) {
				if (cp == REPLACEMENT_CHAR && n == 1)
					put_rune(out, REPLACEMENT_CHAR);
				else
					buf_append(out, ps->p, n);
			}
			ps->p += n;
			continue;
		}

		if (ps->end - ps->p < 2)
			return false;
		char e = ps->p[1];
		uint32_t cp;
		switch (e) {
		case '"': cp = '"'; break;
		case '\\': cp = '\\'; break;
		case '/': cp = '/'; break;
		case 'b': cp = '\b'; break;
		case 'f': cp = '\f'; break;
		case 'n': cp = '\n'; break;
		case 'r': cp = '\r'; break;
		case 't': cp = '\t'; break;
		case 'u': {
			long r = read_u4(ps->p, ps->end);
			if (r < 0)
				return false;
			ps->p += 6;
			if (r >= 0xD800 && r <= 0xDFFF) {
				long r2 = read_u4(ps->p, ps->end);
				if (r < 0xDC00 && r2 >= 0xDC00 && r2 <= 0xDFFF) {
					ps->p += 6;
					cp = (uint32_t)(0x10000 + ((r - 0xD800) << 10) + (r2 - 0xDC00));
				} else {
					cp = REPLACEMENT_CHAR;
				}
			} else {
				cp = (uint32_t)r;
			}
			if (out)
				put_rune(out, cp);
			continue;
		}
		default:
			return false;
		}
		if (out)
			put_rune(out, cp);
		ps->p += 2;
	}
	return false;
}

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

/* Validates a number literal, reporting whether it is a plain integer. */
static bool parse_number(parser_t *ps, const char **start, size_t *len, bool *integer)
{
	const char *p = ps->p, *end = ps->end;
	*start = p;
	*integer = true;
	if (p < end && *p == '-')
		p++;
	if (p >= end || !is_digit(*p))
		return false;
	if (*p == '0') {
		p++;
	} else {
		while (p < end && is_digit(*p))
			p++;
	}
	if (p < end && *p == '.') {
		*integer = false;
		p++;
		if (p >= end || !is_digit(*p))
			return false;
		while (p < end && is_digit(*p))
			p++;
	}
	if (p < end && (*p == 'e' || *p == 'E')) {
		*integer = false;
		p++;
		if (p < end && (*p == '+' || *p == '-'))
			p++;
		if (p >= end || !is_digit(*p))
			return false;
		while (p < end && is_digit(*p))
			p++;
	}
	*len = (size_t)(p - *start);
	ps->p = p;
	return true;
}

static bool eat_literal(parser_t *ps, const char *word)
{
	size_t n = strlen(word);
	if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0)
		return false;
	ps->p += n;
	return true;
}

static bool skip_value(parser_t *ps);

static bool skip_container(parser_t *ps, char close, bool keyed)
{
	if (++ps->depth > MAX_DEPTH)
		return false;
	ps->p++;
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == close) {
		ps->p++;
		ps->depth--;
		return true;
	}
	for (;;) {
		if (keyed) {
			if (!parse_string(ps, NULL))
				return false;
			skip_ws(ps);
			if (ps->p >= ps->end || *ps->p != ':')
				return false;
			ps->p++;
			skip_ws(ps);
		}
		if (!skip_value(ps))
			return false;
		skip_ws(ps);
		if (ps->p >= ps->end)
			return false;
		if (*ps->p == close) {
			ps->p++;
			ps->depth--;
			return true;
		}
		if (*ps->p != ',')
			return false;
		ps->p++;
		skip_ws(ps);
	}
}

static bool skip_value(parser_t *ps)
{
	if (ps->p >= ps->end)
		return false;
	const char *start;
	size_t len;
	bool integer;
	switch (*ps->p) {
	case '{':
		return skip_container(ps, '}', true);
	case '[':
		return skip_container(ps, ']', false);
	case '"':
		return parse_string(ps, NULL);
	case 't':
		return eat_literal(ps, "true");
	case 'f':
		return eat_literal(ps, "false");
	case 'n':
		return eat_literal(ps, "null");
	default:
		return parse_number(ps, &start, &len, &integer);
	}
}

/* Stores a string value into *field; null leaves it alone and any other
 * type is a type error. */
static bool decode_string_field(parser_t *ps, char **field)
{
	if (ps->p < ps->end && *ps->p == '"') {
		buf_t b;
		buf_init(&b);
		bool ok = parse_string(ps, &b);
		if (ok) {
			free(*field);
			*field = buf_take(&b);
		}
		buf_free(&b);
		return ok;
	}
	if (eat_literal(ps, "null"))
		return true;
	ps->type_error = true;
	return skip_value(ps);
}

static bool decode_int_field(parser_t *ps, long long *field)
{
	if (eat_literal(ps, "null"))
		return true;
	if (ps->p < ps->end && (*ps->p == '-' || is_digit(*ps->p))) {
		const char *start;
		size_t len;
		bool integer;
		if (!parse_number(ps, &start, &len, &integer))
			return false;
		char tmp[32];
		if (!integer || len >= sizeof(tmp)) {
			ps->type_error = true;
			return true;
		}
		memcpy(tmp, start, len);
		tmp[len] = '\0';
		errno = 0;
		long long v = strtoll(tmp, NULL, 10);
		if (errno == ERANGE) {
			ps->type_error = true;
			return true;
		}
		*field = v;
		return true;
	}
	ps->type_error = true;
	return skip_value(ps);
}

/* Compares a JSON key with a field name the way encoding/json folds names:
 * ASCII case is ignored, and U+017F and U+212A fold to S and K. */
static bool key_matches(const char *key, const char *field)
{
	const unsigned char *k = (const unsigned char *)key;
	size_t klen = strlen(key);
	for (const char *f = field; *f; f++) {
		char want = *f >= 'a' && *f <= 'z' ? (char)(*f - 32) : *f;
		size_t used;
		char got;
		if (klen >= 2 && k[0] == 0xC5 && k[1] == 0xBF) {
			got = 'S'; /* U+017F long s */
			used = 2;
		} else if (klen >= 3 && k[0] == 0xE2 && k[1] == 0x84 && k[2] == 0xAA) {
			got = 'K'; /* U+212A Kelvin sign */
			used = 3;
		} else if (klen >= 1) {
			got = k[0] >= 'a' && k[0] <= 'z' ? (char)(k[0] - 32) : (char)k[0];
			used = 1;
		} else {
			return false;
		}
		if (got != want)
			return false;
		k += used;
		klen -= used;
	}
	return klen == 0;
}

typedef struct {
	const char *name;
	char **string;
	long long *integer;
	bool colo;
} field_t;

static bool decode_object(parser_t *ps, const field_t *fields, size_t nfields, meta_t *m);

static bool decode_field_value(parser_t *ps, const field_t *f, meta_t *m)
{
	if (f->string)
		return decode_string_field(ps, f->string);
	if (f->integer)
		return decode_int_field(ps, f->integer);
	/* colo: a nested object, or null. */
	if (eat_literal(ps, "null"))
		return true;
	if (ps->p < ps->end && *ps->p == '{') {
		const field_t colo_fields[] = {
			{"iata", &m->colo_iata, NULL, false},
			{"city", &m->colo_city, NULL, false},
		};
		return decode_object(ps, colo_fields, ARRAY_LEN(colo_fields), m);
	}
	ps->type_error = true;
	return skip_value(ps);
}

static bool decode_object(parser_t *ps, const field_t *fields, size_t nfields, meta_t *m)
{
	if (++ps->depth > MAX_DEPTH)
		return false;
	ps->p++; /* '{' */
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == '}') {
		ps->p++;
		ps->depth--;
		return true;
	}
	buf_t key;
	buf_init(&key);
	bool ok = false;
	for (;;) {
		buf_clear(&key);
		if (!parse_string(ps, &key))
			break;
		skip_ws(ps);
		if (ps->p >= ps->end || *ps->p != ':')
			break;
		ps->p++;
		skip_ws(ps);

		const field_t *match = NULL;
		for (size_t i = 0; i < nfields && !match; i++)
			if (key_matches(key.data ? key.data : "", fields[i].name))
				match = &fields[i];
		if (!(match ? decode_field_value(ps, match, m) : skip_value(ps)))
			break;

		skip_ws(ps);
		if (ps->p >= ps->end)
			break;
		if (*ps->p == '}') {
			ps->p++;
			ps->depth--;
			ok = true;
			break;
		}
		if (*ps->p != ',')
			break;
		ps->p++;
		skip_ws(ps);
	}
	buf_free(&key);
	return ok;
}

bool meta_parse(const char *data, size_t len, meta_t *out)
{
	memset(out, 0, sizeof(*out));
	parser_t ps = {data, data + len, 0, false};
	skip_ws(&ps);
	if (ps.p >= ps.end)
		return false;

	bool ok;
	if (*ps.p == '{') {
		const field_t fields[] = {
			{"clientIp", &out->ip, NULL, false},
			{"asn", NULL, &out->asn, false},
			{"asOrganization", &out->org, NULL, false},
			{"city", &out->city, NULL, false},
			{"country", &out->country, NULL, false},
			{"colo", NULL, NULL, true},
		};
		ok = decode_object(&ps, fields, ARRAY_LEN(fields), out);
	} else if (eat_literal(&ps, "null")) {
		ok = true;
	} else {
		ok = skip_value(&ps) && false; /* any other type is a type error */
	}

	if (!ok || ps.type_error) {
		meta_free(out);
		return false;
	}
	/* Fill absent fields so callers never see NULL. */
	char **all[] = {&out->ip, &out->org, &out->city, &out->country, &out->colo_iata, &out->colo_city};
	for (size_t i = 0; i < ARRAY_LEN(all); i++)
		if (!*all[i])
			*all[i] = xstrdup("");
	return true;
}

void json_put_string(buf_t *b, const char *s)
{
	static const char hex[] = "0123456789abcdef";
	size_t n = strlen(s);
	buf_putc(b, '"');
	for (size_t i = 0; i < n;) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x80) {
			switch (c) {
			case '"': buf_puts(b, "\\\""); break;
			case '\\': buf_puts(b, "\\\\"); break;
			case '\b': buf_puts(b, "\\b"); break;
			case '\f': buf_puts(b, "\\f"); break;
			case '\n': buf_puts(b, "\\n"); break;
			case '\r': buf_puts(b, "\\r"); break;
			case '\t': buf_puts(b, "\\t"); break;
			case '<': case '>': case '&':
				buf_puts(b, "\\u00");
				buf_putc(b, hex[c >> 4]);
				buf_putc(b, hex[c & 0xF]);
				break;
			default:
				if (c < 0x20) {
					buf_puts(b, "\\u00");
					buf_putc(b, hex[c >> 4]);
					buf_putc(b, hex[c & 0xF]);
				} else {
					buf_putc(b, (char)c);
				}
			}
			i++;
			continue;
		}
		uint32_t cp;
		size_t len = utf8_decode(s + i, n - i, &cp);
		if (cp == REPLACEMENT_CHAR && len == 1)
			buf_puts(b, "\xEF\xBF\xBD"); /* U+FFFD, written raw like Go */
		else if (cp == 0x2028 || cp == 0x2029)
			buf_printf(b, "\\u%04x", (unsigned)cp);
		else
			buf_append(b, s + i, len);
		i += len;
	}
	buf_putc(b, '"');
}

void json_put_number(buf_t *b, double v)
{
	char tmp[400];
	if (v == 0) {
		buf_puts(b, signbit(v) ? "-0" : "0");
		return;
	}
	/* The fewest decimals that read back as v is what strconv's shortest
	 * 'f' formatting produces. */
	for (int decimals = 0; decimals <= 340; decimals++) {
		snprintf(tmp, sizeof(tmp), "%.*f", decimals, v);
		if (strtod(tmp, NULL) == v)
			break;
	}
	buf_puts(b, tmp);
}
