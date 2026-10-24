/*
 * Measurement follows the original implementation:
 *
 *  - Latency is time to first byte of an empty download on a warm
 *    connection, minus the processing time Cloudflare reports in
 *    Server-Timing. The first request only warms the connection up.
 *  - Throughput runs several parallel HTTP/1.1 connections (HTTP/2 would
 *    multiplex them onto one TCP connection) issuing 25 MB requests back to
 *    back for a fixed time. The reported rate leaves out the first 20% of
 *    the test (at most 2 s), while connections ramp up.
 *  - While the link is saturated, latency keeps being sampled every 250 ms;
 *    the gap between that and idle latency reveals bufferbloat.
 */
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "util.h"

#define CHUNK_SIZE 25000000
#define META_LIMIT (1 << 20)
#define HEADER_LIMIT (64 << 10)
#define NOISE_SIZE (1 << 20)
#define RATE_WINDOW NS_PER_SEC
#define LOADED_PING_INTERVAL (250 * NS_PER_MS)
#define RETRY_DELAY (500 * NS_PER_MS)
#define CONNECT_TIMEOUT_MS 10000L
/* The Go version could wait forever on a stalled probe; give up instead. */
#define PROBE_TIMEOUT_MS 15000L

typedef enum {
	XFER_META,
	XFER_PING,
	XFER_DOWNLOAD,
	XFER_UPLOAD,
} xfer_kind_t;

typedef struct {
	engine_t *e;
	CURL *h;
	xfer_kind_t kind;
	bool active;
	/* When a failed stream may try again; 0 when not waiting. */
	int64_t retry_at;
	int64_t started;
	int64_t up_remaining;
	size_t up_off;
	/* The last error a stream saw; it matters only if nothing got
	 * through at all. */
	char *last_error;
	char errbuf[CURL_ERROR_SIZE];
	char status[128];
	int code; /* of the response being received */
	buf_t server_timing;
	buf_t body;
} xfer_t;

struct engine {
	config_t cfg;
	engine_events_t ev;
	char *base, *referer, *url_meta, *url_ping, *url_down, *url_up;
	CURLM *multi;
	struct curl_slist *upload_headers;
	unsigned char *noise;

	xfer_t probe;
	xfer_t *streams;

	bool active, failed;
	char *error;
	phase_t phase;

	bool has_meta;
	meta_t meta;
	latency_t latency;
	throughput_t results[2];

	/* Idle latency. */
	bool primed;
	int64_t *pings;
	size_t npings, pings_cap;

	/* The transfer test in progress. */
	direction_t dir;
	int64_t start, deadline, warmup, next_tick;
	int64_t total;
	/* Upload bytes count as libcurl sends them, so if the server refused
	 * uploads and never accepted any, the test measured nothing even though
	 * bytes went out before the refusals came back. A request counts as
	 * accepted once it completes, or once it has run longer than a refusal
	 * takes to arrive (accept_age). */
	bool accepted;
	int64_t accept_age;
	char *refusal;
	point_t *history;
	size_t nhistory, history_cap;
	int64_t *loaded;
	size_t nloaded, loaded_cap;
	int64_t ping_base, next_ping; /* next_ping is 0 when none is due */
};

static size_t write_cb(char *data, size_t size, size_t n, void *ud)
{
	xfer_t *x = ud;
	size_t len = size * n;
	switch (x->kind) {
	case XFER_DOWNLOAD:
		/* Only real payload counts, not the body of an error reply. */
		if (x->code == 200)
			x->e->total += (int64_t)len;
		break;
	case XFER_META:
		if (x->body.len + len > META_LIMIT)
			return 0; /* abort: no metadata response is this large */
		buf_append(&x->body, data, len);
		break;
	default:
		break;
	}
	return len;
}

/* Feeds an upload with incompressible filler, counting each byte as libcurl
 * takes it. */
static size_t read_cb(char *out, size_t size, size_t n, void *ud)
{
	xfer_t *x = ud;
	size_t want = size * n;
	if (x->up_remaining <= 0)
		return 0;
	if ((int64_t)want > x->up_remaining)
		want = (size_t)x->up_remaining;
	for (size_t done = 0; done < want;) {
		size_t take = want - done;
		if (take > NOISE_SIZE - x->up_off)
			take = NOISE_SIZE - x->up_off;
		memcpy(out + done, x->e->noise + x->up_off, take);
		x->up_off = (x->up_off + take) % NOISE_SIZE;
		done += take;
	}
	x->up_remaining -= (int64_t)want;
	x->e->total += (int64_t)want;
	return want;
}

static void trim(const char **s, size_t *len)
{
	while (*len && (**s == ' ' || **s == '\t')) {
		(*s)++;
		(*len)--;
	}
	while (*len && ((*s)[*len - 1] == ' ' || (*s)[*len - 1] == '\t' || (*s)[*len - 1] == '\r' ||
			(*s)[*len - 1] == '\n'))
		(*len)--;
}

static size_t header_cb(char *data, size_t size, size_t n, void *ud)
{
	xfer_t *x = ud;
	size_t len = size * n;
	if (len >= 5 && memcmp(data, "HTTP/", 5) == 0) {
		/* A new response (after a redirect, say) replaces the last. */
		buf_clear(&x->server_timing);
		const char *sp = memchr(data, ' ', len);
		const char *status = sp ? sp + 1 : data + len;
		size_t slen = (size_t)(data + len - status);
		trim(&status, &slen);
		x->code = 0;
		for (size_t i = 0; i < slen && i < 3 && status[i] >= '0' && status[i] <= '9'; i++)
			x->code = x->code * 10 + (status[i] - '0');
		if (slen >= sizeof(x->status))
			slen = sizeof(x->status) - 1;
		memcpy(x->status, status, slen);
		x->status[slen] = '\0';
	} else if (len > 14 && strncasecmp(data, "Server-Timing:", 14) == 0) {
		const char *value = data + 14;
		size_t vlen = len - 14;
		trim(&value, &vlen);
		if (x->server_timing.len + vlen < HEADER_LIMIT) {
			if (x->server_timing.len)
				buf_puts(&x->server_timing, ", ");
			buf_append(&x->server_timing, value, vlen);
		}
	}
	return len;
}

static void xfer_init(engine_t *e, xfer_t *x)
{
	memset(x, 0, sizeof(*x));
	x->e = e;
	buf_init(&x->server_timing);
	buf_init(&x->body);
	x->h = curl_easy_init();
	if (!x->h)
		die("could not initialize libcurl");

	CURL *h = x->h;
	curl_easy_setopt(h, CURLOPT_PRIVATE, x);
	curl_easy_setopt(h, CURLOPT_ERRORBUFFER, x->errbuf);
	curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(h, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
	curl_easy_setopt(h, CURLOPT_REFERER, e->referer);
	curl_easy_setopt(h, CURLOPT_USERAGENT, "flarespeed/" FLARESPEED_VERSION);
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, CONNECT_TIMEOUT_MS);
	curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(h, CURLOPT_TCP_KEEPIDLE, 30L);
	curl_easy_setopt(h, CURLOPT_TCP_KEEPINTVL, 30L);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
	curl_easy_setopt(h, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
	curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 256L * 1024);
#if LIBCURL_VERSION_NUM >= 0x075700
	/* Abandoning a request must not wait for a slow DNS lookup. */
	curl_easy_setopt(h, CURLOPT_QUICK_EXIT, 1L);
#endif
	curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(h, CURLOPT_HEADERDATA, x);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, x);
	curl_easy_setopt(h, CURLOPT_READFUNCTION, read_cb);
	curl_easy_setopt(h, CURLOPT_READDATA, x);
}

static void xfer_free(xfer_t *x)
{
	curl_easy_cleanup(x->h);
	buf_free(&x->server_timing);
	buf_free(&x->body);
	free(x->last_error);
}

static void xfer_remove(xfer_t *x)
{
	if (x->active) {
		curl_multi_remove_handle(x->e->multi, x->h);
		x->active = false;
	}
}

/* Issues a request on x, which must not be in flight. */
static void xfer_begin(xfer_t *x, xfer_kind_t kind)
{
	engine_t *e = x->e;
	CURL *h = x->h;
	x->kind = kind;
	x->errbuf[0] = '\0';
	x->status[0] = '\0';
	x->code = 0;
	buf_clear(&x->server_timing);
	buf_clear(&x->body);
	x->started = mono_ns();

	switch (kind) {
	case XFER_META:
	case XFER_PING:
		curl_easy_setopt(h, CURLOPT_URL, kind == XFER_META ? e->url_meta : e->url_ping);
		curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
		curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, PROBE_TIMEOUT_MS);
		break;
	case XFER_DOWNLOAD:
		curl_easy_setopt(h, CURLOPT_URL, e->url_down);
		curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
		curl_easy_setopt(h, CURLOPT_HTTPHEADER, NULL);
		break;
	case XFER_UPLOAD:
		x->up_remaining = CHUNK_SIZE;
		x->up_off = 0;
		curl_easy_setopt(h, CURLOPT_URL, e->url_up);
		curl_easy_setopt(h, CURLOPT_POST, 1L);
		curl_easy_setopt(h, CURLOPT_POSTFIELDS, NULL);
		curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)CHUNK_SIZE);
		curl_easy_setopt(h, CURLOPT_HTTPHEADER, e->upload_headers);
		break;
	}
	if (curl_multi_add_handle(e->multi, h) == CURLM_OK)
		x->active = true;
}

/* Describes a failed request the way Go's net/http words it. */
static char *request_error(const xfer_t *x, CURLcode res, long code)
{
	if (res != CURLE_OK) {
		const char *method = x->kind == XFER_UPLOAD ? "Post" : "Get";
		const char *url = x->kind == XFER_META	   ? x->e->url_meta
				  : x->kind == XFER_PING   ? x->e->url_ping
				  : x->kind == XFER_UPLOAD ? x->e->url_up
							   : x->e->url_down;
		return xasprintf("%s \"%s\": %s", method, url, x->errbuf[0] ? x->errbuf : curl_easy_strerror(res));
	}
	if (code != 200) {
		if (x->status[0])
			return xasprintf("server responded %s", x->status);
		return xasprintf("server responded %ld", code);
	}
	return NULL;
}

static void stop_transfers(engine_t *e)
{
	xfer_remove(&e->probe);
	for (int i = 0; i < e->cfg.streams; i++) {
		xfer_remove(&e->streams[i]);
		e->streams[i].retry_at = 0;
	}
	e->next_ping = 0;
}

static void set_phase(engine_t *e, phase_t p)
{
	e->phase = p;
	if (e->ev.phase)
		e->ev.phase(e->ev.ctx, p);
}

static void fail(engine_t *e, const char *fmt, ...) PRINTF_LIKE(2, 3);

static void fail(engine_t *e, const char *fmt, ...)
{
	buf_t b;
	va_list ap;
	buf_init(&b);
	va_start(ap, fmt);
	buf_vprintf(&b, fmt, ap);
	va_end(ap);
	stop_transfers(e);
	free(e->error);
	e->error = buf_take(&b);
	e->failed = true;
	e->active = false;
	if (e->ev.error)
		e->ev.error(e->ev.ctx, e->error);
}

static bool in_transfer_phase(const engine_t *e)
{
	return e->active && (e->phase == PHASE_DOWNLOAD || e->phase == PHASE_UPLOAD);
}

static void start_transfer_test(engine_t *e, direction_t dir)
{
	e->dir = dir;
	set_phase(e, dir == DIR_UPLOAD ? PHASE_UPLOAD : PHASE_DOWNLOAD);
	e->start = mono_ns();
	/* Saturate rather than overflow for absurdly long tests. */
	e->deadline = e->cfg.duration > INT64_MAX - e->start ? INT64_MAX : e->start + e->cfg.duration;
	/* Connection setup and TCP slow start make the first moments
	 * unrepresentative; they count toward neither the rate nor loaded
	 * latency. */
	e->warmup = e->cfg.duration / 5 < 2 * NS_PER_SEC ? e->cfg.duration / 5 : 2 * NS_PER_SEC;
	e->next_tick = e->start + SAMPLE_INTERVAL;
	e->total = 0;
	e->accepted = false;
	e->accept_age = NS_PER_SEC + 5 * (e->latency.median < NS_PER_SEC ? e->latency.median : NS_PER_SEC);
	free(e->refusal);
	e->refusal = NULL;
	e->nhistory = 0;
	GROW(e->history, e->history_cap, 1);
	e->history[e->nhistory++] = (point_t){0, 0};
	e->nloaded = 0;
	e->ping_base = e->start + e->warmup;
	e->next_ping = e->ping_base;
	for (int i = 0; i < e->cfg.streams; i++) {
		xfer_t *x = &e->streams[i];
		free(x->last_error);
		x->last_error = NULL;
		x->retry_at = 0;
		xfer_begin(x, dir == DIR_UPLOAD ? XFER_UPLOAD : XFER_DOWNLOAD);
	}
}

static void finish_transfer_test(engine_t *e, int64_t now)
{
	point_t end = {now - e->start, e->total};
	stop_transfers(e);
	if (end.bytes == 0) {
		const char *why = "no data transferred";
		for (int i = 0; i < e->cfg.streams; i++) {
			if (e->streams[i].last_error) {
				why = e->streams[i].last_error;
				break;
			}
		}
		fail(e, "%s: %s", direction_name(e->dir), why);
		return;
	}
	if (e->dir == DIR_UPLOAD && e->refusal && !e->accepted) {
		fail(e, "%s: %s", direction_name(e->dir), e->refusal);
		return;
	}
	point_t from = point_before(e->history, e->nhistory, e->warmup);
	throughput_t r = {point_rate(from, end), end.bytes, end.at, latency_summarize(e->loaded, e->nloaded)};
	e->results[e->dir] = r;
	if (e->ev.throughput)
		e->ev.throughput(e->ev.ctx, e->dir, &r);
	if (e->dir == DIR_DOWNLOAD) {
		start_transfer_test(e, DIR_UPLOAD);
	} else {
		e->active = false;
		set_phase(e, PHASE_DONE);
	}
}

/* Time to first byte minus the server's own processing time. */
static int64_t ping_duration(const xfer_t *x)
{
	curl_off_t pre = 0, first = 0;
	curl_easy_getinfo(x->h, CURLINFO_PRETRANSFER_TIME_T, &pre);
	curl_easy_getinfo(x->h, CURLINFO_STARTTRANSFER_TIME_T, &first);
	int64_t d = first > pre ? (int64_t)(first - pre) * 1000 : 0;
	int64_t st = server_timing_ns(x->server_timing.data ? x->server_timing.data : "");
	if (st > 0 && st < d)
		d -= st;
	return d;
}

static void meta_done(engine_t *e, xfer_t *x, CURLcode res, long code)
{
	/* Location info is cosmetic, so failing to fetch it is not fatal. */
	meta_t m;
	if (res == CURLE_OK && code == 200 && meta_parse(x->body.data ? x->body.data : "", x->body.len, &m)) {
		if (e->has_meta)
			meta_free(&e->meta);
		e->meta = m;
		e->has_meta = true;
		if (e->ev.meta)
			e->ev.meta(e->ev.ctx, &e->meta);
	}
	set_phase(e, PHASE_LATENCY);
	e->primed = false;
	e->npings = 0;
	xfer_begin(&e->probe, XFER_PING);
}

static void ping_done(engine_t *e, xfer_t *x, CURLcode res, long code, int64_t now)
{
	char *err = request_error(x, res, code);
	if (e->phase == PHASE_LATENCY) {
		if (err) {
			fail(e, "latency: %s%s", res == CURLE_OK ? "ping: " : "", err);
			free(err);
			return;
		}
		if (!e->primed) {
			/* The first request paid for TCP and TLS setup. */
			e->primed = true;
			xfer_begin(x, XFER_PING);
			return;
		}
		int64_t d = ping_duration(x);
		GROW(e->pings, e->pings_cap, e->npings + 1);
		e->pings[e->npings++] = d;
		if (e->ev.ping)
			e->ev.ping(e->ev.ctx, d);
		if ((int)e->npings < e->cfg.pings) {
			xfer_begin(x, XFER_PING);
			return;
		}
		e->latency = latency_summarize(e->pings, e->npings);
		if (e->ev.latency)
			e->ev.latency(e->ev.ctx, e->latency);
		start_transfer_test(e, DIR_DOWNLOAD);
		return;
	}

	if (in_transfer_phase(e)) {
		if (!err) {
			GROW(e->loaded, e->loaded_cap, e->nloaded + 1);
			e->loaded[e->nloaded++] = ping_duration(x);
		}
		/* Like a Go ticker: the next ping goes on the next 250 ms tick
		 * after this one started, or straight away if that has passed. */
		int64_t ticks = (x->started - e->ping_base) / LOADED_PING_INTERVAL + 1;
		int64_t next = e->ping_base + ticks * LOADED_PING_INTERVAL;
		e->next_ping = next > now ? next : now;
	}
	free(err);
}

static void stream_done(engine_t *e, xfer_t *x, CURLcode res, long code, int64_t now)
{
	if (!in_transfer_phase(e))
		return;
	char *err = request_error(x, res, code);
	if (!err) {
		e->accepted = true;
		xfer_begin(x, x->kind);
		return;
	}
	if (res == CURLE_OK) {
		free(e->refusal);
		e->refusal = xstrdup(err);
	}
	free(x->last_error);
	x->last_error = err;
	/* Back off briefly rather than hammering a server that refuses us. */
	x->retry_at = now + RETRY_DELAY;
}

static void process_messages(engine_t *e)
{
	CURLMsg *msg;
	int left;
	while ((msg = curl_multi_info_read(e->multi, &left))) {
		if (msg->msg != CURLMSG_DONE)
			continue;
		CURL *h = msg->easy_handle;
		CURLcode res = msg->data.result;
		char *priv = NULL;
		long code = 0;
		curl_easy_getinfo(h, CURLINFO_PRIVATE, &priv);
		curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
		xfer_t *x = (xfer_t *)(void *)priv;
		if (!x)
			continue;
		xfer_remove(x);
		int64_t now = mono_ns();
		switch (x->kind) {
		case XFER_META:
			meta_done(e, x, res, code);
			break;
		case XFER_PING:
			ping_done(e, x, res, code, now);
			break;
		case XFER_DOWNLOAD:
		case XFER_UPLOAD:
			stream_done(e, x, res, code, now);
			break;
		}
	}
}

static void run_timers(engine_t *e, int64_t now)
{
	if (!in_transfer_phase(e))
		return;
	if (now >= e->deadline) {
		finish_transfer_test(e, now);
		return;
	}
	if (now >= e->next_tick) {
		point_t p = {now - e->start, e->total};
		GROW(e->history, e->history_cap, e->nhistory + 1);
		e->history[e->nhistory++] = p;
		sample_t s = {p.at, point_rate(point_before(e->history, e->nhistory, p.at - RATE_WINDOW), p)};
		/* A late wakeup skips the ticks it missed, like a Go ticker. */
		e->next_tick = e->start + ((now - e->start) / SAMPLE_INTERVAL + 1) * SAMPLE_INTERVAL;
		if (e->ev.sample)
			e->ev.sample(e->ev.ctx, e->dir, s);
	}
	for (int i = 0; i < e->cfg.streams; i++) {
		xfer_t *x = &e->streams[i];
		if (x->active && now - x->started >= e->accept_age)
			e->accepted = true;
		if (!x->active && x->retry_at && now >= x->retry_at) {
			x->retry_at = 0;
			xfer_begin(x, x->kind);
		}
	}
	if (e->next_ping && !e->probe.active && now >= e->next_ping) {
		e->next_ping = 0;
		xfer_begin(&e->probe, XFER_PING);
	}
}

/* Milliseconds until the engine's next timer, rounded up. */
static int64_t timer_wait_ms(const engine_t *e, int64_t now)
{
	if (!in_transfer_phase(e))
		return INT64_MAX;
	int64_t next = e->deadline;
	if (e->next_tick < next)
		next = e->next_tick;
	for (int i = 0; i < e->cfg.streams; i++)
		if (e->streams[i].retry_at && e->streams[i].retry_at < next)
			next = e->streams[i].retry_at;
	if (e->next_ping && !e->probe.active && e->next_ping < next)
		next = e->next_ping;
	return next <= now ? 0 : (next - now + NS_PER_MS - 1) / NS_PER_MS;
}

void engine_poll(engine_t *e, struct curl_waitfd *extra, unsigned nextra, int timeout_ms)
{
	int64_t wait = timer_wait_ms(e, mono_ns());
	if (wait < timeout_ms)
		timeout_ms = (int)wait;
	if (timeout_ms < 0)
		timeout_ms = 0;
	curl_multi_poll(e->multi, extra, nextra, timeout_ms, NULL);
	int running;
	curl_multi_perform(e->multi, &running);
	process_messages(e);
	run_timers(e, mono_ns());
}

engine_t *engine_new(config_t cfg, const char *base_url, const engine_events_t *events)
{
	engine_t *e = xcalloc(1, sizeof(*e));
	e->cfg = cfg;
	e->ev = *events;
	e->base = xstrdup(base_url);
	e->referer = xasprintf("%s/", base_url);
	e->url_meta = xasprintf("%s/meta", base_url);
	e->url_ping = xasprintf("%s/__down?bytes=0", base_url);
	e->url_down = xasprintf("%s/__down?bytes=%d", base_url, CHUNK_SIZE);
	e->url_up = xasprintf("%s/__up", base_url);
	e->upload_headers = curl_slist_append(NULL, "Content-Type: application/octet-stream");
	/* Waiting for "100 Continue" would stall every upload request. */
	e->upload_headers = curl_slist_append(e->upload_headers, "Expect:");
	if (!e->upload_headers)
		die("out of memory");

	e->multi = curl_multi_init();
	if (!e->multi)
		die("could not initialize libcurl");
	curl_multi_setopt(e->multi, CURLMOPT_PIPELINING, (long)CURLPIPE_NOTHING);
	curl_multi_setopt(e->multi, CURLMOPT_MAXCONNECTS, (long)cfg.streams + 2);

	/* Incompressible filler for uploads: splitmix64 output. */
	e->noise = xmalloc(NOISE_SIZE);
	uint64_t state = (uint64_t)mono_ns() ^ ((uint64_t)getpid() << 32);
	for (size_t i = 0; i < NOISE_SIZE; i += 8) {
		uint64_t z = (state += UINT64_C(0x9E3779B97F4A7C15));
		z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
		z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
		z ^= z >> 31;
		memcpy(e->noise + i, &z, 8);
	}

	xfer_init(e, &e->probe);
	e->streams = xcalloc((size_t)cfg.streams, sizeof(*e->streams));
	for (int i = 0; i < cfg.streams; i++)
		xfer_init(e, &e->streams[i]);
	return e;
}

void engine_free(engine_t *e)
{
	if (!e)
		return;
	stop_transfers(e);
	xfer_free(&e->probe);
	for (int i = 0; i < e->cfg.streams; i++)
		xfer_free(&e->streams[i]);
	free(e->streams);
	curl_multi_cleanup(e->multi);
	curl_slist_free_all(e->upload_headers);
	if (e->has_meta)
		meta_free(&e->meta);
	free(e->noise);
	free(e->pings);
	free(e->history);
	free(e->loaded);
	free(e->error);
	free(e->refusal);
	free(e->base);
	free(e->referer);
	free(e->url_meta);
	free(e->url_ping);
	free(e->url_down);
	free(e->url_up);
	free(e);
}

void engine_start(engine_t *e)
{
	stop_transfers(e);
	if (e->has_meta)
		meta_free(&e->meta);
	e->has_meta = false;
	free(e->error);
	e->error = NULL;
	e->failed = false;
	e->active = true;
	memset(&e->latency, 0, sizeof(e->latency));
	memset(e->results, 0, sizeof(e->results));
	set_phase(e, PHASE_CONNECT);
	xfer_begin(&e->probe, XFER_META);
}

void engine_stop(engine_t *e)
{
	stop_transfers(e);
	e->active = false;
}

bool engine_active(const engine_t *e)
{
	return e->active;
}

bool engine_failed(const engine_t *e)
{
	return e->failed;
}

const char *engine_error(const engine_t *e)
{
	return e->error;
}

phase_t engine_phase(const engine_t *e)
{
	return e->phase;
}

void engine_results(const engine_t *e, engine_results_t *out)
{
	out->has_meta = e->has_meta;
	out->meta = e->has_meta ? &e->meta : NULL;
	out->latency = e->latency;
	out->download = e->results[DIR_DOWNLOAD];
	out->upload = e->results[DIR_UPLOAD];
}
