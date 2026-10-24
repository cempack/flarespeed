#include "input.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

enum {
	S_GROUND,
	S_ESC,
	S_CSI,
	S_OSC,
	S_OSC_ESC,
	S_STRING,     /* DCS, SOS, PM, APC: skipped up to ST */
	S_STRING_ESC,
	S_SS3,
	S_PASTE,
};

static const char paste_end[] = "\x1b[201~";

void input_init(input_t *in)
{
	memset(in, 0, sizeof(*in));
}

static void emit(input_handler_fn fn, void *ctx, input_event_kind_t kind, bool dark)
{
	input_event_t ev = {kind, dark};
	fn(ctx, ev);
}

static void collect(input_t *in, char c)
{
	if (in->len + 1 < sizeof(in->seq))
		in->seq[in->len++] = c;
	else
		in->overflow = true;
	in->seq[in->len] = '\0';
}

static void reset_seq(input_t *in)
{
	in->len = 0;
	in->overflow = false;
	in->seq[0] = '\0';
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* strconv.ParseUint(s, 16, 32), failing as 0. */
static unsigned long parse_hex(const char *s, size_t n)
{
	if (n == 0)
		return 0;
	unsigned long v = 0;
	for (size_t i = 0; i < n; i++) {
		int d = hex_digit(s[i]);
		if (d < 0 || v > 0xFFFFFFFUL)
			return 0;
		v = v * 16 + (unsigned long)d;
	}
	return v;
}

bool input_color_is_dark(const char *spec)
{
	unsigned long rgb[3];
	if (spec[0] == '#') {
		/* colorful.Hex: #rgb or #rrggbb. */
		size_t n = strlen(spec + 1);
		if (n != 3 && n != 6)
			return true;
		for (size_t i = 0; i < n; i++)
			if (hex_digit(spec[1 + i]) < 0)
				return true;
		for (int i = 0; i < 3; i++)
			rgb[i] = n == 6 ? parse_hex(spec + 1 + 2 * i, 2) : parse_hex(spec + 1 + i, 1) * 17;
	} else {
		size_t parts;
		if (strncmp(spec, "rgb:", 4) == 0) {
			spec += 4;
			parts = 3;
		} else if (strncmp(spec, "rgba:", 5) == 0) {
			spec += 5;
			parts = 4;
		} else {
			return true; /* unparsable: assumed dark */
		}
		const char *p = spec;
		for (size_t i = 0; i < parts; i++) {
			const char *slash = strchr(p, '/');
			if ((i + 1 < parts) != (slash != NULL))
				return true;
			size_t len = slash ? (size_t)(slash - p) : strlen(p);
			if (i < 3) {
				unsigned long v = parse_hex(p, len);
				rgb[i] = v > 0xFF ? (v >> 8) & 0xFF : v; /* ansi's shift */
			}
			p = slash ? slash + 1 : p + len;
		}
	}
	unsigned long max = rgb[0], min = rgb[0];
	for (int i = 1; i < 3; i++) {
		if (rgb[i] > max)
			max = rgb[i];
		if (rgb[i] < min)
			min = rgb[i];
	}
	double l = ((double)max / 255 + (double)min / 255) / 2;
	return l < 0.5;
}

static void dispatch_csi(input_t *in, char final, input_handler_fn fn, void *ctx)
{
	if (in->overflow)
		return;
	if (final == '~' && strcmp(in->seq, "200") == 0) {
		in->state = S_PASTE;
		in->paste_match = 0;
		return;
	}
	if (final == 'c' && in->seq[0] == '?') {
		emit(fn, ctx, INPUT_DA1, false);
		return;
	}
	/* DECRPM for mode 2026: "?2026;2$y" means supported but off. */
	if (final == 'y' && strcmp(in->seq, "?2026;2$") == 0)
		emit(fn, ctx, INPUT_SYNC_SUPPORTED, false);
}

static void dispatch_osc(input_t *in, input_handler_fn fn, void *ctx)
{
	if (!in->overflow && strncmp(in->seq, "11;", 3) == 0)
		emit(fn, ctx, INPUT_BACKGROUND, input_color_is_dark(in->seq + 3));
}

static void ground(input_t *in, unsigned char c, int64_t now, input_handler_fn fn, void *ctx)
{
	switch (c) {
	case 0x1b:
		in->state = S_ESC;
		in->esc_at = now;
		break;
	case 0x03:
	case 'q':
		emit(fn, ctx, INPUT_QUIT, false);
		break;
	case 'r':
		emit(fn, ctx, INPUT_RERUN, false);
		break;
	default:
		break;
	}
}

void input_feed(input_t *in, const char *data, size_t n, int64_t now, input_handler_fn fn, void *ctx)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)data[i];
	again:
		switch (in->state) {
		case S_GROUND:
			ground(in, c, now, fn, ctx);
			break;
		case S_ESC:
			reset_seq(in);
			switch (c) {
			case '[':
				in->state = S_CSI;
				break;
			case ']':
				in->state = S_OSC;
				break;
			case 'P': case 'X': case '^': case '_':
				in->state = S_STRING;
				break;
			case 'O':
				in->state = S_SS3;
				break;
			default:
				/* Alt+key (including Alt+Esc): nothing we bind. */
				in->state = S_GROUND;
				break;
			}
			break;
		case S_CSI:
			if (c >= 0x40 && c <= 0x7E) {
				in->state = S_GROUND;
				dispatch_csi(in, (char)c, fn, ctx);
			} else if (c >= 0x20 && c <= 0x3F) {
				collect(in, (char)c);
			} else if (c == 0x1b) {
				in->state = S_ESC;
				in->esc_at = now;
			} else {
				in->state = S_GROUND; /* malformed; drop it */
			}
			break;
		case S_OSC:
			if (c == 0x07) {
				in->state = S_GROUND;
				dispatch_osc(in, fn, ctx);
			} else if (c == 0x1b) {
				in->state = S_OSC_ESC;
			} else {
				collect(in, (char)c);
			}
			break;
		case S_OSC_ESC:
			/* ESC \ is the string terminator; any other byte aborts the
			 * string and starts a new escape sequence. */
			if (c == '\\') {
				in->state = S_GROUND;
				dispatch_osc(in, fn, ctx);
				break;
			}
			in->state = S_ESC;
			in->esc_at = now;
			goto again;
		case S_STRING:
			if (c == 0x1b)
				in->state = S_STRING_ESC;
			break;
		case S_STRING_ESC:
			in->state = c == '\\' ? S_GROUND : S_STRING;
			break;
		case S_SS3:
			in->state = S_GROUND;
			break;
		case S_PASTE:
			/* Pasted text is not typed keys; skip to the paste end. */
			if (c == (unsigned char)paste_end[in->paste_match]) {
				if (++in->paste_match == sizeof(paste_end) - 1)
					in->state = S_GROUND;
			} else {
				in->paste_match = c == 0x1b ? 1 : 0;
			}
			break;
		}
	}
}

void input_tick(input_t *in, int64_t now, input_handler_fn fn, void *ctx)
{
	if (in->state == S_ESC && now - in->esc_at >= ESC_TIMEOUT) {
		in->state = S_GROUND;
		emit(fn, ctx, INPUT_QUIT, false);
	}
}

int64_t input_deadline(const input_t *in)
{
	return in->state == S_ESC ? in->esc_at + ESC_TIMEOUT : INT64_MAX;
}
