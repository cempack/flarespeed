#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "text.h"

void test_utf8_roundtrip(void)
{
	for (uint32_t cp = 0; cp <= 0x10FFFF; cp += (cp < 0x3000 ? 1 : 97)) {
		if (cp >= 0xD800 && cp <= 0xDFFF)
			continue;
		char enc[4];
		size_t n = utf8_encode(cp, enc);
		uint32_t back;
		CHECK_INT(utf8_decode(enc, n, &back), n);
		CHECK_INT(back, cp);
	}
	uint32_t cp;
	/* Overlong, surrogate, truncated and out-of-range sequences. */
	static const char *bad[] = {"\xC0\xAF", "\xED\xA0\x80", "\xE2\x82", "\xF4\x90\x80\x80", "\xFF", "\x80"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK_INT(utf8_decode(bad[i], strlen(bad[i]), &cp), 1);
		CHECK_INT(cp, REPLACEMENT_CHAR);
	}
}

/* Runs fn over every vector line, handing it the decoded fields. */
static void each_vector(void (*fn)(int limit, const char *in, const char *wrapped,
				   const char *truncated, int width))
{
	FILE *f = fopen("data/text.txt", "r");
	CHECK(f != NULL);
	if (!f)
		return;
	static char line[1 << 16];
	char a[1 << 14], b[1 << 14], c[1 << 14];
	int limit, width;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%d %16383s %16383s %16383s %d", &limit, a, b, c, &width) != 5)
			continue;
		char *in = unhex(a, NULL), *wrapped = unhex(b, NULL), *truncated = unhex(c, NULL);
		fn(limit, in, wrapped, truncated, width);
		free(in);
		free(wrapped);
		free(truncated);
	}
	fclose(f);
}

static void check_width(int limit, const char *in, const char *w, const char *t, int width)
{
	(void)limit, (void)w, (void)t;
	if (text_width(in) != width)
		check_fail(__FILE__, __LINE__, "text_width(\"%s\") = %d, want %d", in, text_width(in), width);
}

static void check_truncate(int limit, const char *in, const char *w, const char *want, int width)
{
	(void)w, (void)width;
	buf_t out;
	buf_init(&out);
	text_truncate(&out, in, limit, "…");
	if (strcmp(out.data ? out.data : "", want) != 0)
		check_fail(__FILE__, __LINE__, "truncate(\"%s\", %d)\n   got  \"%s\"\n   want \"%s\"", in,
			   limit, out.data ? out.data : "", want);
	buf_free(&out);
}

static void check_wrap(int limit, const char *in, const char *want, const char *t, int width)
{
	(void)t, (void)width;
	buf_t out;
	buf_init(&out);
	text_wrap(&out, in, limit);
	if (strcmp(out.data ? out.data : "", want) != 0)
		check_fail(__FILE__, __LINE__, "wrap(\"%s\", %d)\n   got  \"%s\"\n   want \"%s\"", in, limit,
			   out.data ? out.data : "", want);
	buf_free(&out);
}

void test_text_width(void)
{
	each_vector(check_width);
	CHECK_INT(text_width("↓ DOWNLOAD ⠋ ━─╭│"), 17);
	CHECK_INT(text_width(""), 0);
}

void test_text_truncate(void)
{
	each_vector(check_truncate);
}

void test_text_wrap(void)
{
	each_vector(check_wrap);
}

void test_text_sanitize(void)
{
	char *s = text_sanitize("Evil\x1b[31mOrg\x07 \xC2\x9B" "2J\xE2\x80\xAEtxt \xFF ok");
	CHECK_STR(s, "Evil[31mOrg 2Jtxt \xEF\xBF\xBD ok");
	free(s);
	s = text_sanitize("Île-de-France 東京 👍🏽");
	CHECK_STR(s, "Île-de-France 東京 👍🏽");
	free(s);
}
