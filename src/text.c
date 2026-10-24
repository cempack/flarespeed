/* text_wrap and text_truncate port ansi.Wrap and ansi.Truncate from
 * charmbracelet/x/ansi (MIT, Charmbracelet, Inc.). See THIRD-PARTY-NOTICES. */
#include "text.h"

#include <stdbool.h>
#include <string.h>

#include "widths.h"

size_t utf8_decode(const char *str, size_t n, uint32_t *cp)
{
	const unsigned char *s = (const unsigned char *)str;
	unsigned char b = s[0];
	if (b < 0x80) {
		*cp = b;
		return 1;
	}

	size_t len;
	unsigned char lo = 0x80, hi = 0xBF;
	uint32_t c;
	if (b >= 0xC2 && b <= 0xDF) {
		len = 2;
		c = b & 0x1F;
	} else if (b >= 0xE0 && b <= 0xEF) {
		len = 3;
		c = b & 0x0F;
		if (b == 0xE0)
			lo = 0xA0;
		else if (b == 0xED)
			hi = 0x9F; /* no surrogates */
	} else if (b >= 0xF0 && b <= 0xF4) {
		len = 4;
		c = b & 0x07;
		if (b == 0xF0)
			lo = 0x90;
		else if (b == 0xF4)
			hi = 0x8F;
	} else {
		*cp = REPLACEMENT_CHAR;
		return 1;
	}
	if (n < len || s[1] < lo || s[1] > hi) {
		*cp = REPLACEMENT_CHAR;
		return 1;
	}
	c = (c << 6) | (s[1] & 0x3F);
	for (size_t i = 2; i < len; i++) {
		if (s[i] < 0x80 || s[i] > 0xBF) {
			*cp = REPLACEMENT_CHAR;
			return 1;
		}
		c = (c << 6) | (s[i] & 0x3F);
	}
	*cp = c;
	return len;
}

size_t utf8_encode(uint32_t cp, char *out)
{
	unsigned char *o = (unsigned char *)out;
	if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		cp = REPLACEMENT_CHAR;
	if (cp < 0x80) {
		o[0] = (unsigned char)cp;
		return 1;
	}
	if (cp < 0x800) {
		o[0] = (unsigned char)(0xC0 | (cp >> 6));
		o[1] = (unsigned char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		o[0] = (unsigned char)(0xE0 | (cp >> 12));
		o[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		o[2] = (unsigned char)(0x80 | (cp & 0x3F));
		return 3;
	}
	o[0] = (unsigned char)(0xF0 | (cp >> 18));
	o[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
	o[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
	o[3] = (unsigned char)(0x80 | (cp & 0x3F));
	return 4;
}

int cp_width(uint32_t cp)
{
	if (cp < 0x20)
		return 0;
	if (cp < 0x7F)
		return 1;
	size_t lo = 0, hi = ARRAY_LEN(width_ranges);
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (cp < width_ranges[mid].lo)
			hi = mid;
		else if (cp > width_ranges[mid].hi)
			lo = mid + 1;
		else
			return width_ranges[mid].width;
	}
	return 1;
}

static bool is_regional_indicator(uint32_t cp)
{
	return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

static bool is_emoji_modifier(uint32_t cp)
{
	return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

static bool is_control(uint32_t cp)
{
	return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
}

size_t text_cluster(const char *s, size_t n, int *width)
{
	uint32_t base, cp;
	size_t i = utf8_decode(s, n, &base);
	int w = cp_width(base);
	if (is_control(base)) {
		*width = w;
		return i;
	}
	if (is_regional_indicator(base) && i < n) {
		size_t len = utf8_decode(s + i, n - i, &cp);
		if (is_regional_indicator(cp)) {
			*width = 2;
			return i + len;
		}
	}

	bool after_zwj = false;
	while (i < n) {
		size_t len = utf8_decode(s + i, n - i, &cp);
		int cw = cp_width(cp);
		if (cw == 0 && !is_control(cp)) {
			/* Combining marks, joiners and variation selectors extend
			 * the cluster; VS16 asks for emoji presentation. */
			if (cp == 0xFE0F && w == 1 && (base >= 0x2000 || base == 0xA9 || base == 0xAE))
				w = 2;
			after_zwj = cp == 0x200D;
			i += len;
			continue;
		}
		if (w == 2 && cw == 2 && (after_zwj || is_emoji_modifier(cp))) {
			after_zwj = false;
			i += len;
			continue;
		}
		break;
	}
	*width = w;
	return i;
}

int text_width(const char *s)
{
	size_t n = strlen(s);
	int total = 0;
	for (size_t i = 0; i < n;) {
		unsigned char c = (unsigned char)s[i];
		if (c >= 0x20 && c < 0x7F && (i + 1 == n || (unsigned char)s[i + 1] < 0x80)) {
			total++;
			i++;
			continue;
		}
		int w;
		i += text_cluster(s + i, n - i, &w);
		total += w;
	}
	return total;
}

void text_truncate(buf_t *out, const char *s, int limit, const char *tail)
{
	if (text_width(s) <= limit) {
		buf_puts(out, s);
		return;
	}
	limit -= text_width(tail);
	if (limit < 0)
		return;

	size_t n = strlen(s);
	int cur = 0;
	for (size_t i = 0; i < n;) {
		int w;
		size_t len = text_cluster(s + i, n - i, &w);
		if (cur + w > limit) {
			buf_puts(out, tail);
			return;
		}
		cur += w;
		buf_append(out, s + i, len);
		i += len;
	}
}

static bool unsafe_on_terminal(uint32_t cp)
{
	return is_control(cp) || cp == 0x061C || cp == 0x200E || cp == 0x200F ||
	       (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
}

char *text_sanitize(const char *s)
{
	buf_t b;
	buf_init(&b);
	size_t n = strlen(s);
	for (size_t i = 0; i < n;) {
		uint32_t cp;
		size_t len = utf8_decode(s + i, n - i, &cp);
		if (!unsafe_on_terminal(cp)) {
			char enc[4];
			buf_append(&b, enc, utf8_encode(cp, enc));
		}
		i += len;
	}
	return buf_take(&b);
}

/* unicode.IsSpace, as used by ansi.Wrap. */
static bool is_space(uint32_t cp)
{
	switch (cp) {
	case '\t': case '\n': case '\v': case '\f': case '\r': case ' ':
	case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029:
	case 0x202F: case 0x205F: case 0x3000:
		return true;
	}
	return cp >= 0x2000 && cp <= 0x200A;
}

/* State for the ansi.Wrap port; the field names follow the original. */
typedef struct {
	buf_t *out;
	buf_t word, space;
	int space_width, cur_width, word_len;
} wrapper_t;

static void wrap_add_space(wrapper_t *w)
{
	if (w->space_width == 0 && w->space.len == 0)
		return;
	w->cur_width += w->space_width;
	buf_append(w->out, w->space.data, w->space.len);
	buf_clear(&w->space);
	w->space_width = 0;
}

static void wrap_add_word(wrapper_t *w)
{
	if (w->word.len == 0)
		return;
	wrap_add_space(w);
	w->cur_width += w->word_len;
	buf_append(w->out, w->word.data, w->word.len);
	buf_clear(&w->word);
	w->word_len = 0;
}

static void wrap_add_newline(wrapper_t *w)
{
	buf_putc(w->out, '\n');
	w->cur_width = 0;
	buf_clear(&w->space);
	w->space_width = 0;
}

/* Settles pending whitespace before a line break or the end of input. */
static void wrap_flush_space(wrapper_t *w, int limit)
{
	if (w->word_len != 0)
		return;
	if (w->cur_width + w->space_width > limit)
		w->cur_width = 0;
	else
		buf_append(w->out, w->space.data, w->space.len);
	buf_clear(&w->space);
	w->space_width = 0;
}

void text_wrap(buf_t *out, const char *s, int limit)
{
	if (limit < 1) {
		buf_puts(out, s);
		return;
	}

	wrapper_t w = {.out = out};
	buf_init(&w.word);
	buf_init(&w.space);
	size_t n = strlen(s);

	for (size_t i = 0; i < n;) {
		unsigned char c = (unsigned char)s[i];
		if (c >= 0x80) {
			int width;
			size_t len = text_cluster(s + i, n - i, &width);
			uint32_t r;
			size_t rlen = utf8_decode(s + i, n - i, &r);
			if (r != REPLACEMENT_CHAR && is_space(r) && r != 0xA0) {
				wrap_add_word(&w);
				buf_append(&w.space, s + i, rlen);
				w.space_width += width;
			} else {
				if (w.word_len + width > limit)
					wrap_add_word(&w);
				buf_append(&w.word, s + i, len);
				w.word_len += width;
				if (w.cur_width + w.word_len + w.space_width > limit)
					wrap_add_newline(&w);
				if (w.word_len == limit)
					wrap_add_word(&w);
			}
			i += len;
			continue;
		}

		if (c == '\n') {
			wrap_flush_space(&w, limit);
			wrap_add_word(&w);
			wrap_add_newline(&w);
		} else if (is_space(c)) {
			wrap_add_word(&w);
			buf_putc(&w.space, (char)c);
			w.space_width++;
		} else if (c == '-') {
			wrap_add_space(&w);
			if (w.cur_width + w.word_len >= limit) {
				buf_putc(&w.word, (char)c);
				w.word_len++;
			} else {
				wrap_add_word(&w);
				buf_putc(out, (char)c);
				w.cur_width++;
			}
		} else if (c != 0x7F) {
			if (w.cur_width == limit)
				wrap_add_newline(&w);
			buf_putc(&w.word, (char)c);
			w.word_len++;
			if (w.word_len == limit)
				wrap_add_word(&w);
			if (w.cur_width + w.word_len + w.space_width > limit)
				wrap_add_newline(&w);
		} else {
			/* DEL rides along without taking space. */
			buf_putc(&w.word, (char)c);
		}
		i++;
	}

	wrap_flush_space(&w, limit);
	wrap_add_word(&w);
	buf_free(&w.word);
	buf_free(&w.space);
}
