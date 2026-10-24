/* Memory, buffer and clock helpers shared by every module. */
#ifndef FLARESPEED_UTIL_H
#define FLARESPEED_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS_PER_MS INT64_C(1000000)
#define NS_PER_SEC INT64_C(1000000000)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#if defined(__GNUC__)
#define PRINTF_LIKE(fmt, args) __attribute__((format(printf, fmt, args)))
#define NORETURN __attribute__((noreturn))
#else
#define PRINTF_LIKE(fmt, args)
#define NORETURN
#endif

/* Allocation never fails: running out of memory ends the program. */
void *xmalloc(size_t n);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t n);
void *xreallocarray(void *p, size_t count, size_t size);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) PRINTF_LIKE(1, 2);

/* Grows an array so it can hold at least need elements. */
#define GROW(ptr, cap, need)                                                   \
	do {                                                                   \
		if ((size_t)(need) > (size_t)(cap)) {                          \
			size_t grow_cap_ = (size_t)(cap) ? (size_t)(cap) : 8;  \
			while (grow_cap_ < (size_t)(need))                     \
				grow_cap_ *= 2;                                \
			(ptr) = xreallocarray((ptr), grow_cap_, sizeof(*(ptr))); \
			(cap) = grow_cap_;                                     \
		}                                                              \
	} while (0)

/* buf is a growable byte string; data is always NUL-terminated. */
typedef struct {
	char *data;
	size_t len;
	size_t cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
void buf_clear(buf_t *b);
void buf_reserve(buf_t *b, size_t extra);
void buf_append(buf_t *b, const void *p, size_t n);
void buf_puts(buf_t *b, const char *s);
void buf_putc(buf_t *b, char c);
void buf_printf(buf_t *b, const char *fmt, ...) PRINTF_LIKE(2, 3);
void buf_vprintf(buf_t *b, const char *fmt, va_list ap) PRINTF_LIKE(2, 0);
/* Takes ownership of the contents, leaving b empty. */
char *buf_take(buf_t *b);

/* Monotonic clock in nanoseconds. */
int64_t mono_ns(void);

/* Makes floating-point arithmetic round to double precision on the x87 FPU,
 * as it does everywhere else, so results match Go's bit for bit. Call it
 * before any floating-point work. */
void fp_init(void);

/* The program name used in diagnostics. */
extern const char *progname;

NORETURN void die(const char *fmt, ...) PRINTF_LIKE(1, 2);

#endif
