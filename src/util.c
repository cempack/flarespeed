#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *progname = "flarespeed";

void die(const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "%s: ", progname);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void *check(void *p)
{
	if (!p)
		die("out of memory");
	return p;
}

void *xmalloc(size_t n)
{
	return check(malloc(n ? n : 1));
}

void *xcalloc(size_t count, size_t size)
{
	return check(calloc(count ? count : 1, size ? size : 1));
}

void *xrealloc(void *p, size_t n)
{
	return check(realloc(p, n ? n : 1));
}

void *xreallocarray(void *p, size_t count, size_t size)
{
	if (size && count > SIZE_MAX / size)
		die("out of memory");
	return xrealloc(p, count * size);
}

char *xstrdup(const char *s)
{
	return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
	char *d = xmalloc(n + 1);
	memcpy(d, s, n);
	d[n] = '\0';
	return d;
}

char *xasprintf(const char *fmt, ...)
{
	buf_t b;
	va_list ap;
	buf_init(&b);
	va_start(ap, fmt);
	buf_vprintf(&b, fmt, ap);
	va_end(ap);
	return buf_take(&b);
}

void buf_init(buf_t *b)
{
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

void buf_free(buf_t *b)
{
	free(b->data);
	buf_init(b);
}

void buf_clear(buf_t *b)
{
	b->len = 0;
	if (b->data)
		b->data[0] = '\0';
}

void buf_reserve(buf_t *b, size_t extra)
{
	if (extra > SIZE_MAX - b->len - 1)
		die("out of memory");
	size_t need = b->len + extra + 1;
	if (need <= b->cap)
		return;
	size_t cap = b->cap ? b->cap : 64;
	while (cap < need)
		cap = cap > SIZE_MAX / 2 ? need : cap * 2;
	b->data = xrealloc(b->data, cap);
	b->cap = cap;
}

void buf_append(buf_t *b, const void *p, size_t n)
{
	buf_reserve(b, n);
	if (n)
		memcpy(b->data + b->len, p, n);
	b->len += n;
	b->data[b->len] = '\0';
}

void buf_puts(buf_t *b, const char *s)
{
	buf_append(b, s, strlen(s));
}

void buf_putc(buf_t *b, char c)
{
	buf_append(b, &c, 1);
}

void buf_vprintf(buf_t *b, const char *fmt, va_list ap)
{
	va_list copy;
	va_copy(copy, ap);
	int n = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if (n < 0)
		return;
	buf_reserve(b, (size_t)n);
	vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
	b->len += (size_t)n;
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	buf_vprintf(b, fmt, ap);
	va_end(ap);
}

char *buf_take(buf_t *b)
{
	char *s = b->data ? b->data : xstrdup("");
	buf_init(b);
	return s;
}

int64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

void fp_init(void)
{
#if defined(__i386__) && !defined(__SSE2_MATH__) && defined(__GNUC__)
	/* The x87 FPU rounds to a 64-bit mantissa by default, and rounding that
	 * again to a double can land one ulp away from the correctly rounded
	 * result Go computes with SSE2. Set its precision control to 53 bits. */
	unsigned short cw;
	__asm__ volatile("fnstcw %0" : "=m"(cw));
	cw = (unsigned short)((cw & ~0x300u) | 0x200u);
	__asm__ volatile("fldcw %0" : : "m"(cw));
#endif
}
