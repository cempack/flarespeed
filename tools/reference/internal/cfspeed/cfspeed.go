// Package cfspeed measures latency and throughput against Cloudflare's speed
// test service (speed.cloudflare.com).
//
// Latency follows the official client's method: time to first byte of an
// empty download, minus the processing time the server reports in its
// Server-Timing header. Throughput runs several parallel streams for a fixed
// duration and reports the steady-state rate once TCP has ramped up.
package cfspeed

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptrace"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// DefaultBaseURL is Cloudflare's public speed test endpoint.
const DefaultBaseURL = "https://speed.cloudflare.com"

// SampleInterval is how often MeasureThroughput reports progress.
const SampleInterval = 100 * time.Millisecond

const (
	// Each stream issues requests of this size back to back until the test
	// duration elapses.
	chunkSize = 25_000_000

	// rateWindow is the trailing window behind the live throughput readout.
	rateWindow = time.Second

	loadedPingInterval = 250 * time.Millisecond
)

// Client runs speed tests against a Cloudflare speed test endpoint.
type Client struct {
	BaseURL string

	// probe carries metadata and latency requests on a single warm
	// connection, separate from the throughput streams.
	probe *http.Client
}

// New returns a Client for Cloudflare's public endpoint.
func New() *Client {
	return &Client{
		BaseURL: DefaultBaseURL,
		probe:   &http.Client{Transport: newTransport(1)},
	}
}

// Meta describes the client and the Cloudflare data center serving the test.
type Meta struct {
	IP      string `json:"clientIp"`
	ASN     int    `json:"asn"`
	Org     string `json:"asOrganization"`
	City    string `json:"city"`
	Country string `json:"country"`
	Colo    struct {
		IATA string `json:"iata"`
		City string `json:"city"`
	} `json:"colo"`
}

// Meta fetches information about the client's connection and test server.
func (c *Client) Meta(ctx context.Context) (Meta, error) {
	req, err := c.newRequest(ctx, http.MethodGet, "/meta", nil)
	if err != nil {
		return Meta{}, err
	}
	resp, err := c.probe.Do(req)
	if err != nil {
		return Meta{}, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return Meta{}, fmt.Errorf("meta: server responded %s", resp.Status)
	}
	var m Meta
	return m, json.NewDecoder(resp.Body).Decode(&m)
}

// Latency summarizes a series of round-trip measurements.
type Latency struct {
	Samples []time.Duration
	Median  time.Duration
	Min     time.Duration
	Max     time.Duration
	// Jitter is the mean difference between consecutive samples.
	Jitter time.Duration
}

// MeasureLatency takes n latency samples, calling onSample after each one.
func (c *Client) MeasureLatency(ctx context.Context, n int, onSample func(time.Duration)) (Latency, error) {
	// The first request pays for TCP and TLS setup; make it up front so every
	// measured sample reuses a warm connection.
	if _, err := c.ping(ctx); err != nil {
		return Latency{}, err
	}
	samples := make([]time.Duration, 0, n)
	for range n {
		d, err := c.ping(ctx)
		if err != nil {
			return Latency{}, err
		}
		samples = append(samples, d)
		if onSample != nil {
			onSample(d)
		}
	}
	return summarize(samples), nil
}

func summarize(samples []time.Duration) Latency {
	if len(samples) == 0 {
		return Latency{}
	}
	var diffs time.Duration
	for i := 1; i < len(samples); i++ {
		diffs += (samples[i] - samples[i-1]).Abs()
	}
	sorted := slices.Sorted(slices.Values(samples))
	l := Latency{
		Samples: samples,
		Median:  sorted[len(sorted)/2],
		Min:     sorted[0],
		Max:     sorted[len(sorted)-1],
	}
	if n := len(sorted); n%2 == 0 {
		l.Median = (sorted[n/2-1] + sorted[n/2]) / 2
	}
	if len(samples) > 1 {
		l.Jitter = diffs / time.Duration(len(samples)-1)
	}
	return l
}

// ping measures one round trip on the probe connection.
func (c *Client) ping(ctx context.Context) (time.Duration, error) {
	var (
		mu           sync.Mutex
		wrote, first time.Time
	)
	trace := &httptrace.ClientTrace{
		WroteRequest: func(httptrace.WroteRequestInfo) {
			mu.Lock()
			wrote = time.Now()
			mu.Unlock()
		},
		GotFirstResponseByte: func() {
			mu.Lock()
			first = time.Now()
			mu.Unlock()
		},
	}
	req, err := c.newRequest(httptrace.WithClientTrace(ctx, trace), http.MethodGet, "/__down?bytes=0", nil)
	if err != nil {
		return 0, err
	}
	resp, err := c.probe.Do(req)
	if err != nil {
		return 0, err
	}
	_, err = io.Copy(io.Discard, resp.Body)
	resp.Body.Close()
	if err != nil {
		return 0, err
	}
	if resp.StatusCode != http.StatusOK {
		return 0, fmt.Errorf("ping: server responded %s", resp.Status)
	}

	mu.Lock()
	d := first.Sub(wrote)
	mu.Unlock()
	if st := serverTime(resp.Header); st > 0 && st < d {
		d -= st
	}
	return d, nil
}

var (
	requestDurationRe = regexp.MustCompile(`(?i)(?:^|,\s*)cfReq(?:uest)?Dur(?:ation)?;\s*dur=([0-9.]+)`)
	speedDurationRe   = regexp.MustCompile(`(?i)(?:^|,\s*)cfSpeed[a-z]*;\s*dur=([0-9.]+)`)
)

// serverTime extracts the server's own processing time from Server-Timing,
// the same way Cloudflare's speed test client does: a cfRequestDuration
// entry if present, otherwise the sum of every cfSpeed* entry.
func serverTime(h http.Header) time.Duration {
	header := strings.Join(h.Values("Server-Timing"), ", ")
	toDuration := func(ms string) time.Duration {
		v, _ := strconv.ParseFloat(ms, 64)
		return time.Duration(v * float64(time.Millisecond))
	}
	if m := requestDurationRe.FindStringSubmatch(header); m != nil {
		return toDuration(m[1])
	}
	var sum time.Duration
	for _, m := range speedDurationRe.FindAllStringSubmatch(header, -1) {
		sum += toDuration(m[1])
	}
	return sum
}

// Direction selects which way data flows in a throughput test.
type Direction int

const (
	Download Direction = iota
	Upload
)

func (d Direction) String() string {
	if d == Upload {
		return "upload"
	}
	return "download"
}

// Sample is a live throughput reading.
type Sample struct {
	Elapsed time.Duration
	// Bps is the rate over the trailing second, in bits per second.
	Bps float64
}

// Throughput is the outcome of a download or upload test.
type Throughput struct {
	// Bps is the steady-state rate after warm-up, in bits per second.
	Bps      float64
	Bytes    int64
	Duration time.Duration
	// Loaded is latency measured while the link was saturated; the gap
	// between this and idle latency reveals bufferbloat.
	Loaded Latency
}

// ThroughputOptions configures MeasureThroughput.
type ThroughputOptions struct {
	Duration time.Duration
	Streams  int
	// OnSample, if set, is called every SampleInterval.
	OnSample func(Sample)
}

type point struct {
	at    time.Duration
	bytes int64
}

func rate(from, to point) float64 {
	if to.at <= from.at {
		return 0
	}
	return float64(to.bytes-from.bytes) * 8 / (to.at - from.at).Seconds()
}

// MeasureThroughput saturates the link in one direction for opts.Duration.
func (c *Client) MeasureThroughput(ctx context.Context, dir Direction, opts ThroughputOptions) (Throughput, error) {
	testCtx, cancel := context.WithTimeout(ctx, opts.Duration)
	defer cancel()

	transport := newTransport(opts.Streams)
	defer transport.CloseIdleConnections()
	hc := &http.Client{Transport: transport}

	// Connection setup and TCP slow start make the first moments
	// unrepresentative, so they count toward neither the final rate nor
	// loaded latency.
	warmup := min(opts.Duration/5, 2*time.Second)

	var (
		wg     sync.WaitGroup
		total  atomic.Int64
		errs   = make([]error, opts.Streams)
		loaded []time.Duration
	)
	for i := range opts.Streams {
		wg.Go(func() { errs[i] = c.stream(testCtx, hc, dir, &total) })
	}
	wg.Go(func() { loaded = c.pingLoop(testCtx, warmup) })

	start := time.Now()
	history := []point{{}}
	ticker := time.NewTicker(SampleInterval)
	defer ticker.Stop()
	for running := true; running; {
		select {
		case <-testCtx.Done():
			running = false
		case now := <-ticker.C:
			p := point{now.Sub(start), total.Load()}
			history = append(history, p)
			if opts.OnSample != nil {
				opts.OnSample(Sample{Elapsed: p.at, Bps: rate(pointBefore(history, p.at-rateWindow), p)})
			}
		}
	}
	end := point{time.Since(start), total.Load()}
	wg.Wait()

	if err := ctx.Err(); err != nil {
		return Throughput{}, err
	}
	if end.bytes == 0 {
		for _, err := range errs {
			if err != nil {
				return Throughput{}, err
			}
		}
		return Throughput{}, errors.New("no data transferred")
	}
	from := pointBefore(history, warmup)
	return Throughput{
		Bps:      rate(from, end),
		Bytes:    end.bytes,
		Duration: end.at,
		Loaded:   summarize(loaded),
	}, nil
}

// pointBefore returns the latest recorded point at or before t.
func pointBefore(history []point, t time.Duration) point {
	for i := len(history) - 1; i > 0; i-- {
		if history[i].at <= t {
			return history[i]
		}
	}
	return history[0]
}

// stream issues back-to-back transfers until ctx is done. It returns the last
// error it saw, which matters only if nothing got through at all.
func (c *Client) stream(ctx context.Context, hc *http.Client, dir Direction, total *atomic.Int64) error {
	var lastErr error
	for ctx.Err() == nil {
		err := c.transfer(ctx, hc, dir, total)
		if err == nil || ctx.Err() != nil {
			continue
		}
		lastErr = err
		// Back off briefly rather than hammering a server that's refusing us.
		select {
		case <-ctx.Done():
		case <-time.After(500 * time.Millisecond):
		}
	}
	return lastErr
}

func (c *Client) transfer(ctx context.Context, hc *http.Client, dir Direction, total *atomic.Int64) error {
	var (
		req *http.Request
		err error
	)
	if dir == Download {
		req, err = c.newRequest(ctx, http.MethodGet, "/__down?bytes="+strconv.Itoa(chunkSize), nil)
	} else {
		req, err = c.newRequest(ctx, http.MethodPost, "/__up", &payload{remaining: chunkSize, sent: total})
	}
	if err != nil {
		return err
	}
	if dir == Upload {
		req.ContentLength = chunkSize
		req.Header.Set("Content-Type", "application/octet-stream")
	}

	resp, err := hc.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("%s: server responded %s", dir, resp.Status)
	}
	var sink io.Writer = io.Discard
	if dir == Download {
		sink = counter{total}
	}
	_, err = io.Copy(sink, resp.Body)
	return err
}

// pingLoop samples latency until ctx is done, starting after warmup so the
// link is actually under load.
func (c *Client) pingLoop(ctx context.Context, warmup time.Duration) []time.Duration {
	select {
	case <-ctx.Done():
		return nil
	case <-time.After(warmup):
	}
	ticker := time.NewTicker(loadedPingInterval)
	defer ticker.Stop()
	var samples []time.Duration
	for {
		if d, err := c.ping(ctx); err == nil {
			samples = append(samples, d)
		}
		select {
		case <-ctx.Done():
			return samples
		case <-ticker.C:
		}
	}
}

func (c *Client) newRequest(ctx context.Context, method, path string, body io.Reader) (*http.Request, error) {
	req, err := http.NewRequestWithContext(ctx, method, c.BaseURL+path, body)
	if err != nil {
		return nil, err
	}
	// The meta endpoint refuses requests that don't come from the speed
	// test page.
	req.Header.Set("Referer", c.BaseURL+"/")
	return req, nil
}

// newTransport returns an HTTP/1.1-only transport. A speed test wants one TCP
// connection per stream; HTTP/2 would multiplex every stream onto a single
// connection and defeat the point of running them in parallel.
func newTransport(conns int) *http.Transport {
	t := &http.Transport{
		Proxy:               http.ProxyFromEnvironment,
		DialContext:         (&net.Dialer{Timeout: 10 * time.Second, KeepAlive: 30 * time.Second}).DialContext,
		TLSHandshakeTimeout: 10 * time.Second,
		MaxIdleConnsPerHost: conns,
		DisableCompression:  true,
		ReadBufferSize:      64 << 10,
		WriteBufferSize:     64 << 10,
	}
	t.Protocols = new(http.Protocols)
	t.Protocols.SetHTTP1(true)
	return t
}

// counter is a sink that tallies the bytes written to it.
type counter struct{ n *atomic.Int64 }

func (c counter) Write(p []byte) (int, error) {
	c.n.Add(int64(len(p)))
	return len(p), nil
}

// noise is incompressible upload filler shared by every upload stream.
var noise = func() []byte {
	b := make([]byte, 1<<20)
	rand.Read(b)
	return b
}()

// payload is an upload body that yields remaining bytes of noise, tallying
// each byte as the transport reads it.
type payload struct {
	remaining int64
	off       int
	sent      *atomic.Int64
}

func (p *payload) Read(b []byte) (int, error) {
	if p.remaining <= 0 {
		return 0, io.EOF
	}
	if int64(len(b)) > p.remaining {
		b = b[:p.remaining]
	}
	n := copy(b, noise[p.off:])
	p.off = (p.off + n) % len(noise)
	p.remaining -= int64(n)
	p.sent.Add(int64(n))
	return n, nil
}
