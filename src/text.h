/* UTF-8 text measurement and shaping for terminal display. */
#ifndef FLARESPEED_TEXT_H
#define FLARESPEED_TEXT_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define REPLACEMENT_CHAR 0xFFFDu

/* Decodes one code point from s (n > 0 bytes available) and returns how many
 * bytes it used. Malformed input decodes as U+FFFD, consuming one byte. */
size_t utf8_decode(const char *s, size_t n, uint32_t *cp);
/* Encodes cp into out (at least 4 bytes) and returns its length. */
size_t utf8_encode(uint32_t cp, char *out);

/* Display width of a single code point: 0, 1 or 2 columns. */
int cp_width(uint32_t cp);

/* Returns the byte length of the grapheme cluster at the start of s and its
 * display width. */
size_t text_cluster(const char *s, size_t n, int *width);

/* Display width of a UTF-8 string. */
int text_width(const char *s);

/* Appends s to out, cut to at most limit columns with tail marking the cut,
 * like ansi.Truncate. */
void text_truncate(buf_t *out, const char *s, int limit, const char *tail);

/* Returns a copy of s that is safe to show on a terminal: control and bidi
 * override characters are removed and malformed UTF-8 is replaced. */
char *text_sanitize(const char *s);

/* Appends s word-wrapped to limit columns, matching ansi.Wrap on plain
 * text. */
void text_wrap(buf_t *out, const char *s, int limit);

#endif
