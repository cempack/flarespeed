#include "harness.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

static int failures;
static const char *current;

void check_fail(const char *file, int line, const char *fmt, ...)
{
	va_list ap;
	failures++;
	fprintf(stderr, "FAIL %s (%s:%d): ", current, file, line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

char *unhex(const char *hex, size_t *len)
{
	size_t n = strlen(hex) / 2;
	char *out = malloc(n + 1);
	if (!out)
		abort();
	for (size_t i = 0; i < n; i++) {
		unsigned v;
		sscanf(hex + 2 * i, "%2x", &v);
		out[i] = (char)v;
	}
	out[n] = '\0';
	if (n > 0) {
		memmove(out, out + 1, n); /* drop the "-" prefix */
		n--;
	}
	if (len)
		*len = n;
	return out;
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} tests[] = {
#define ENTRY(name) {#name, test_##name},
		TESTS(ENTRY)
#undef ENTRY
	};

	int ran = 0;
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (argc > 1 && !strstr(tests[i].name, argv[1]))
			continue;
		current = tests[i].name;
		int before = failures;
		tests[i].fn();
		printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
		ran++;
	}
	printf("%d tests, %d failures\n", ran, failures);
	return failures ? 1 : 0;
}
