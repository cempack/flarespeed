/* A tiny test harness: every test_*.c file defines the functions listed in
 * TESTS below, and run_tests calls them in order. */
#ifndef FLARESPEED_HARNESS_H
#define FLARESPEED_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TESTS(X)                                                               \
	X(utf8_roundtrip)                                                      \
	X(text_width)                                                          \
	X(text_truncate)                                                       \
	X(text_sanitize)                                                       \
	X(text_wrap)                                                           \
	X(color_blend)                                                         \
	X(color_convert)                                                       \
	X(meta_parse)                                                          \
	X(json_string)                                                         \
	X(json_number)                                                         \
	X(duration_parse)                                                      \
	X(format)                                                              \
	X(summarize)                                                           \
	X(server_timing)                                                       \
	X(view_parity)

#define DECLARE_TEST(name) void test_##name(void);
TESTS(DECLARE_TEST)
#undef DECLARE_TEST

/* Decodes a hex field written by the Go vector generators, which prefix
 * every string with "-" so that no field is empty. Returns a malloc'd,
 * NUL-terminated string and stores its length in *len if len is not NULL. */
char *unhex(const char *hex, size_t *len);

void check_fail(const char *file, int line, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond))                                                   \
			check_fail(__FILE__, __LINE__, "%s", #cond);           \
	} while (0)

#define CHECK_INT(got, want)                                                   \
	do {                                                                   \
		long long got_ = (long long)(got), want_ = (long long)(want);  \
		if (got_ != want_)                                             \
			check_fail(__FILE__, __LINE__, "%s = %lld, want %lld", \
				   #got, got_, want_);                         \
	} while (0)

#define CHECK_STR(got, want)                                                   \
	do {                                                                   \
		const char *got_ = (got), *want_ = (want);                     \
		if (strcmp(got_, want_) != 0)                                  \
			check_fail(__FILE__, __LINE__, "%s\n   got  \"%s\"\n   want \"%s\"", \
				   #got, got_, want_);                         \
	} while (0)

#endif
