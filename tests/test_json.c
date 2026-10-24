#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "json.h"

/* Calls fn with the whitespace-separated fields of every line in a vector
 * file. */
static void each_line(const char *path, void (*fn)(char **fields, int n))
{
	FILE *f = fopen(path, "r");
	CHECK(f != NULL);
	if (!f)
		return;
	char *line = NULL;
	size_t cap = 0;
	while (getline(&line, &cap, f) > 0) {
		char *fields[16];
		int n = 0;
		for (char *tok = strtok(line, " \n"); tok && n < 16; tok = strtok(NULL, " \n"))
			fields[n++] = tok;
		fn(fields, n);
	}
	free(line);
	fclose(f);
}

static void check_meta(char **f, int n)
{
	if (n != 9)
		return;
	size_t len;
	char *in = unhex(f[0], &len);
	int want_ok = atoi(f[1]);
	meta_t m;
	bool ok = meta_parse(in, len, &m);
	if (ok != want_ok) {
		check_fail(__FILE__, __LINE__, "meta_parse(%.120s) ok=%d, want %d", in, ok, want_ok);
	} else if (ok) {
		const char *got[] = {m.ip, m.org, m.city, m.country, m.colo_iata, m.colo_city};
		int idx[] = {2, 4, 5, 6, 7, 8};
		for (int i = 0; i < 6; i++) {
			/* C strings cannot hold the NUL that "\u0000" decodes to;
			 * the parser drops it, so compare without NULs. */
			size_t want_len, j = 0;
			char *want = unhex(f[idx[i]], &want_len);
			for (size_t k = 0; k < want_len; k++)
				if (want[k])
					want[j++] = want[k];
			want[j] = '\0';
			if (strcmp(got[i], want) != 0)
				check_fail(__FILE__, __LINE__, "meta_parse(%.120s) field %d = \"%s\", want \"%s\"", in,
					   idx[i], got[i], want);
			free(want);
		}
		if (m.asn != atoll(f[3]))
			check_fail(__FILE__, __LINE__, "meta_parse(%.120s) asn = %lld, want %s", in, m.asn, f[3]);
		meta_free(&m);
	}
	free(in);
}

void test_meta_parse(void)
{
	each_line("data/meta.txt", check_meta);
}

static void check_json_string(char **f, int n)
{
	if (n != 2)
		return;
	char *in = unhex(f[0], NULL), *want = unhex(f[1], NULL);
	buf_t b;
	buf_init(&b);
	json_put_string(&b, in);
	if (strcmp(b.data, want) != 0)
		check_fail(__FILE__, __LINE__, "json_put_string = %s, want %s", b.data, want);
	buf_free(&b);
	free(in);
	free(want);
}

void test_json_string(void)
{
	each_line("data/jsonstr.txt", check_json_string);
}

static void check_json_number(char **f, int n)
{
	if (n != 2)
		return;
	buf_t b;
	buf_init(&b);
	json_put_number(&b, strtod(f[0], NULL));
	if (strcmp(b.data, f[1]) != 0)
		check_fail(__FILE__, __LINE__, "json_put_number(%s) = %s, want %s", f[0], b.data, f[1]);
	buf_free(&b);
}

void test_json_number(void)
{
	each_line("data/jsonnum.txt", check_json_number);
}
