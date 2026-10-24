/*
 * flarespeed measures an internet connection against Cloudflare's speed
 * test network, with a live terminal UI.
 *
 * The command line follows Go's flag package; parse_go_uint, parse_go_int,
 * underscore_ok and go_quote port strconv from the Go standard library
 * (BSD-3-Clause, The Go Authors). See THIRD-PARTY-NOTICES.
 */
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "duration.h"
#include "engine.h"
#include "format.h"
#include "json.h"
#include "profile.h"
#include "text.h"
#include "util.h"

#define MAX_STREAMS 1000
#define MAX_PINGS 1000000

static int signal_pipe[2] = {-1, -1};

static void on_signal(int sig)
{
	int saved = errno;
	unsigned char b = (unsigned char)sig;
	if (write(signal_pipe[1], &b, 1) < 0) {
		/* Nothing to do: a full pipe already holds a pending signal. */
	}
	errno = saved;
}

static void setup_signals(void)
{
	if (pipe(signal_pipe) != 0)
		die("pipe: %s", strerror(errno));
	for (int i = 0; i < 2; i++) {
		fcntl(signal_pipe[i], F_SETFL, fcntl(signal_pipe[i], F_GETFL) | O_NONBLOCK);
		fcntl(signal_pipe[i], F_SETFD, FD_CLOEXEC);
	}
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	int sigs[] = {SIGINT, SIGTERM, SIGHUP, SIGWINCH};
	for (size_t i = 0; i < ARRAY_LEN(sigs); i++) {
		/* Under nohup, SIGHUP stays ignored, as it does for the Go
		 * runtime when nothing asks for it. */
		struct sigaction old;
		if (sigs[i] == SIGHUP && sigaction(SIGHUP, NULL, &old) == 0 && old.sa_handler == SIG_IGN)
			continue;
		sigaction(sigs[i], &sa, NULL);
	}
	/* Writes to a closed socket must fail with EPIPE, not kill us. */
	signal(SIGPIPE, SIG_IGN);
}

static const char *env_lookup(const char *name)
{
	return getenv(name);
}

/* The command line, parsed with the rules of Go's flag package. */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage of %s:\n"
		"  -duration duration\n"
		"    \thow long to run each of the download and upload tests (default 10s)\n"
		"  -json\n"
		"    \tprint results as JSON instead of showing the UI (default when stdout isn't a terminal)\n"
		"  -pings int\n"
		"    \tnumber of latency samples (default 20)\n"
		"  -streams int\n"
		"    \tparallel connections per direction (default 6)\n",
		argv0);
}

NORETURN static void flag_fail(const char *argv0, const char *fmt, ...) PRINTF_LIKE(2, 3);

static void flag_fail(const char *argv0, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	usage(argv0);
	exit(2);
}

static char lower(char c)
{
	return (char)(c | ('x' - 'X'));
}

/* strconv's underscoreOK: underscores may only separate digits. */
static bool underscore_ok(const char *s)
{
	char saw = '^';
	if (*s == '-' || *s == '+')
		s++;
	bool hex = false;
	size_t i = 0, n = strlen(s);
	if (n >= 2 && s[0] == '0' && (lower(s[1]) == 'b' || lower(s[1]) == 'o' || lower(s[1]) == 'x')) {
		i = 2;
		saw = '0';
		hex = lower(s[1]) == 'x';
	}
	for (; i < n; i++) {
		if ((s[i] >= '0' && s[i] <= '9') || (hex && lower(s[i]) >= 'a' && lower(s[i]) <= 'f')) {
			saw = '0';
			continue;
		}
		if (s[i] == '_') {
			if (saw != '0')
				return false;
			saw = '_';
			continue;
		}
		if (saw == '_')
			return false;
		saw = '!';
	}
	return saw != '_';
}

enum { PARSE_OK, PARSE_SYNTAX, PARSE_RANGE };

/* strconv.ParseUint(s, 0, 64). */
static int parse_go_uint(const char *s0, unsigned long long *out)
{
	const char *s = s0;
	if (!*s)
		return PARSE_SYNTAX;
	unsigned base = 10;
	size_t n = strlen(s);
	if (s[0] == '0') {
		if (n >= 3 && lower(s[1]) == 'b')
			base = 2, s += 2;
		else if (n >= 3 && lower(s[1]) == 'o')
			base = 8, s += 2;
		else if (n >= 3 && lower(s[1]) == 'x')
			base = 16, s += 2;
		else
			base = 8, s += 1;
	}
	bool underscores = false;
	unsigned long long v = 0;
	for (; *s; s++) {
		unsigned d;
		if (*s == '_') {
			underscores = true;
			continue;
		} else if (*s >= '0' && *s <= '9') {
			d = (unsigned)(*s - '0');
		} else if (lower(*s) >= 'a' && lower(*s) <= 'z') {
			d = (unsigned)(lower(*s) - 'a' + 10);
		} else {
			return PARSE_SYNTAX;
		}
		if (d >= base)
			return PARSE_SYNTAX;
		if (v > (~0ULL - d) / base) {
			*out = ~0ULL;
			return PARSE_RANGE;
		}
		v = v * base + d;
	}
	if (underscores && !underscore_ok(s0))
		return PARSE_SYNTAX;
	*out = v;
	return PARSE_OK;
}

/* strconv.ParseInt(s, 0, 64). */
static int parse_go_int(const char *s, long long *out)
{
	if (!*s)
		return PARSE_SYNTAX;
	bool neg = false;
	if (*s == '+' || *s == '-')
		neg = *s++ == '-';
	unsigned long long un;
	int rc = parse_go_uint(s, &un);
	if (rc == PARSE_SYNTAX)
		return rc;
	const unsigned long long cutoff = 1ULL << 63;
	if ((!neg && un >= cutoff) || (neg && un > cutoff))
		return PARSE_RANGE;
	*out = neg ? (long long)(0 - un) : (long long)un;
	return PARSE_OK;
}

/* strconv.Quote, for echoing values back in error messages. */
static void go_quote(buf_t *b, const char *s)
{
	size_t n = strlen(s);
	buf_putc(b, '"');
	for (size_t i = 0; i < n;) {
		uint32_t cp;
		size_t len = utf8_decode(s + i, n - i, &cp);
		if (cp == REPLACEMENT_CHAR && len == 1) {
			buf_printf(b, "\\x%02x", (unsigned char)s[i]);
		} else if (cp == '"' || cp == '\\') {
			buf_putc(b, '\\');
			buf_putc(b, (char)cp);
		} else if (cp < 0x20 || cp == 0x7F) {
			switch (cp) {
			case '\a': buf_puts(b, "\\a"); break;
			case '\b': buf_puts(b, "\\b"); break;
			case '\f': buf_puts(b, "\\f"); break;
			case '\n': buf_puts(b, "\\n"); break;
			case '\r': buf_puts(b, "\\r"); break;
			case '\t': buf_puts(b, "\\t"); break;
			case '\v': buf_puts(b, "\\v"); break;
			default: buf_printf(b, "\\x%02x", (unsigned)cp); break;
			}
		} else if ((cp >= 0x80 && cp <= 0x9F) || cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) ||
			   (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) || cp == 0xFEFF) {
			buf_printf(b, "\\u%04x", (unsigned)cp);
		} else {
			buf_append(b, s + i, len);
		}
		i += len;
	}
	buf_putc(b, '"');
}

/* strconv.ParseBool. */
static bool parse_go_bool(const char *s, bool *out)
{
	static const char *const yes[] = {"1", "t", "T", "TRUE", "true", "True"};
	static const char *const no[] = {"0", "f", "F", "FALSE", "false", "False"};
	for (size_t i = 0; i < ARRAY_LEN(yes); i++) {
		if (strcmp(s, yes[i]) == 0) {
			*out = true;
			return true;
		}
		if (strcmp(s, no[i]) == 0) {
			*out = false;
			return true;
		}
	}
	return false;
}

typedef struct {
	config_t cfg;
	long long streams, pings;
	bool json;
} options_t;

static void set_flag(const char *argv0, options_t *o, const char *name, const char *value)
{
	int rc = PARSE_OK;
	if (!strcmp(name, "duration"))
		rc = duration_parse(value, &o->cfg.duration) ? PARSE_OK : PARSE_SYNTAX;
	else
		rc = parse_go_int(value, !strcmp(name, "streams") ? &o->streams : &o->pings);
	if (rc != PARSE_OK) {
		buf_t q;
		buf_init(&q);
		go_quote(&q, value);
		flag_fail(argv0, "invalid value %s for flag -%s: %s", q.data, name,
			  rc == PARSE_RANGE ? "value out of range" : "parse error");
	}
}

static void parse_args(int argc, char **argv, options_t *o)
{
	const char *argv0 = argv[0] ? argv[0] : "flarespeed";
	for (int i = 1; i < argc; i++) {
		const char *s = argv[i];
		if (strlen(s) < 2 || s[0] != '-')
			return; /* the first non-flag argument ends the flags */
		int minuses = 1;
		if (s[1] == '-') {
			minuses = 2;
			if (s[2] == '\0')
				return; /* "--" ends the flags */
		}
		const char *name_start = s + minuses;
		if (!*name_start || *name_start == '-' || *name_start == '=')
			flag_fail(argv0, "bad flag syntax: %s", s);
		const char *eq = strchr(name_start + 1, '=');
		char name[64];
		size_t nlen = eq ? (size_t)(eq - name_start) : strlen(name_start);
		if (nlen >= sizeof(name))
			flag_fail(argv0, "flag provided but not defined: -%.*s", (int)nlen, name_start);
		memcpy(name, name_start, nlen);
		name[nlen] = '\0';
		const char *value = eq ? eq + 1 : NULL;

		if (strcmp(name, "json") == 0) {
			if (!value) {
				o->json = true;
			} else if (!parse_go_bool(value, &o->json)) {
				buf_t q;
				buf_init(&q);
				go_quote(&q, value);
				flag_fail(argv0, "invalid boolean value %s for -%s: parse error", q.data, name);
			}
			continue;
		}
		if (strcmp(name, "duration") && strcmp(name, "streams") && strcmp(name, "pings")) {
			if (!strcmp(name, "help") || !strcmp(name, "h")) {
				usage(argv0);
				exit(0);
			}
			flag_fail(argv0, "flag provided but not defined: -%s", name);
		}
		if (!value) {
			if (i + 1 >= argc)
				flag_fail(argv0, "flag needs an argument: -%s", name);
			value = argv[++i];
		}
		set_flag(argv0, o, name, value);
	}
}

/* The JSON report, in the Go version's field order and formatting. */

static double round2(double v)
{
	return round(v * 100) / 100;
}

static void put_field_number(buf_t *b, const char *indent, const char *key, double v, bool last)
{
	buf_printf(b, "%s\"%s\": ", indent, key);
	json_put_number(b, v);
	buf_puts(b, last ? "\n" : ",\n");
}

static void put_transfer(buf_t *b, const char *key, const throughput_t *t, bool last)
{
	buf_printf(b, "  \"%s\": {\n", key);
	put_field_number(b, "    ", "mbps", round2(t->bps / 1e6), false);
	buf_printf(b, "    \"bytes\": %lld,\n", (long long)t->bytes);
	put_field_number(b, "    ", "loaded_latency_ms", round2(ns_to_ms(t->loaded.median)), true);
	buf_puts(b, last ? "  }\n" : "  },\n");
}

static void print_report(const engine_results_t *r)
{
	buf_t b;
	buf_init(&b);
	buf_puts(&b, "{\n");
	if (r->has_meta) {
		const meta_t *m = r->meta;
		buf_puts(&b, "  \"server\": {\n    \"colo\": ");
		json_put_string(&b, m->colo_iata);
		buf_puts(&b, ",\n    \"city\": ");
		json_put_string(&b, m->colo_city);
		buf_puts(&b, "\n  },\n  \"client\": {\n    \"ip\": ");
		json_put_string(&b, m->ip);
		buf_printf(&b, ",\n    \"asn\": %lld,\n    \"org\": ", m->asn);
		json_put_string(&b, m->org);
		buf_puts(&b, ",\n    \"city\": ");
		json_put_string(&b, m->city);
		buf_puts(&b, ",\n    \"country\": ");
		json_put_string(&b, m->country);
		buf_puts(&b, "\n  },\n");
	}
	buf_puts(&b, "  \"latency\": {\n");
	put_field_number(&b, "    ", "median_ms", round2(ns_to_ms(r->latency.median)), false);
	put_field_number(&b, "    ", "jitter_ms", round2(ns_to_ms(r->latency.jitter)), false);
	put_field_number(&b, "    ", "min_ms", round2(ns_to_ms(r->latency.min)), false);
	put_field_number(&b, "    ", "max_ms", round2(ns_to_ms(r->latency.max)), true);
	buf_puts(&b, "  },\n");
	put_transfer(&b, "download", &r->download, false);
	put_transfer(&b, "upload", &r->upload, true);
	buf_puts(&b, "}\n");
	fwrite(b.data, 1, b.len, stdout);
	buf_free(&b);
	if (fflush(stdout) != 0)
		die("write error: %s", strerror(errno));
}

static void die_sanitized(const char *msg)
{
	char *clean = text_sanitize(msg);
	die("%s", clean);
}

static int run_json(config_t cfg, const char *base_url)
{
	static const engine_events_t no_events = {0};
	engine_t *e = engine_new(cfg, base_url, &no_events);
	engine_start(e);
	while (engine_active(e)) {
		struct curl_waitfd fd = {signal_pipe[0], CURL_WAIT_POLLIN, 0};
		engine_poll(e, &fd, 1, 1000);
		unsigned char sigs[64];
		ssize_t n = read(signal_pipe[0], sigs, sizeof(sigs));
		for (ssize_t i = 0; i < n; i++) {
			if (sigs[i] == SIGINT) {
				/* As in the Go version, cancellation surfaces as the
				 * error of whatever step was running. */
				phase_t p = engine_phase(e);
				char *msg = p == PHASE_DOWNLOAD || p == PHASE_UPLOAD
						    ? xasprintf("%s: context canceled",
								p == PHASE_UPLOAD ? "upload" : "download")
						    : xasprintf("latency: Get \"%s/__down?bytes=0\": context canceled",
								base_url);
				/* No cleanup: libcurl would wait out any DNS lookup
				 * still in progress. Exiting frees everything. */
				die("%s", msg);
			}
			if (sigs[i] == SIGTERM || sigs[i] == SIGHUP) {
				signal(sigs[i], SIG_DFL);
				raise(sigs[i]);
			}
		}
	}
	if (engine_failed(e))
		die_sanitized(engine_error(e));
	engine_results_t r;
	engine_results(e, &r);
	print_report(&r);
	engine_free(e);
	return 0;
}

int main(int argc, char **argv)
{
	fp_init();
	options_t o = {{10 * NS_PER_SEC, 6, 20}, 6, 20, false};
	parse_args(argc, argv, &o);

	if (o.cfg.duration < 2 * NS_PER_SEC)
		die("-duration must be at least 2s");
	if (o.streams < 1)
		die("-streams must be at least 1");
	if (o.streams > MAX_STREAMS)
		die("-streams must be at most %d", MAX_STREAMS);
	if (o.pings < 2)
		die("-pings must be at least 2");
	if (o.pings > MAX_PINGS)
		die("-pings must be at most %d", MAX_PINGS);
	o.cfg.streams = (int)o.streams;
	o.cfg.pings = (int)o.pings;

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
		die("could not initialize libcurl");
	setup_signals();

	const char *base = getenv("FLARESPEED_BASE_URL");
	if (!base || !*base)
		base = DEFAULT_BASE_URL;

	bool tty = isatty(STDOUT_FILENO);
	int status;
	if (o.json || !tty)
		status = run_json(o.cfg, base);
	else
		status = app_run(o.cfg, base, profile_detect(tty, env_lookup), signal_pipe[0]);
	/* No curl_global_cleanup: an abandoned DNS lookup may still be running
	 * in a resolver thread, and exiting reclaims everything anyway. */
	return status;
}
