// Command speedtest measures your internet connection against Cloudflare's
// speed test network, with a live terminal UI.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"math"
	"os"
	"os/signal"
	"time"

	tea "charm.land/bubbletea/v2"
	"github.com/charmbracelet/x/term"

	"github.com/elliotmoreau/speedtest/internal/cfspeed"
)

func main() {
	var (
		cfg    config
		asJSON bool
	)
	flag.DurationVar(&cfg.duration, "duration", 10*time.Second, "how long to run each of the download and upload tests")
	flag.IntVar(&cfg.streams, "streams", 6, "parallel connections per direction")
	flag.IntVar(&cfg.pings, "pings", 20, "number of latency samples")
	flag.BoolVar(&asJSON, "json", false, "print results as JSON instead of showing the UI (default when stdout isn't a terminal)")
	flag.Parse()

	switch {
	case cfg.duration < 2*time.Second:
		fatalf("-duration must be at least 2s")
	case cfg.streams < 1:
		fatalf("-streams must be at least 1")
	case cfg.pings < 2:
		fatalf("-pings must be at least 2")
	}

	client := cfspeed.New()
	if asJSON || !term.IsTerminal(os.Stdout.Fd()) {
		if err := runJSON(client, cfg); err != nil {
			fatalf("%v", err)
		}
		return
	}

	m := newModel(client, cfg)
	if _, err := tea.NewProgram(m).Run(); err != nil {
		fatalf("%v", err)
	}
	if m.err != nil {
		os.Exit(1)
	}
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "speedtest: "+format+"\n", args...)
	os.Exit(1)
}

func runJSON(c *cfspeed.Client, cfg config) error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()

	r, err := runTest(ctx, c, cfg, func(tea.Msg) {})
	if err != nil {
		return err
	}
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	return enc.Encode(newReport(r))
}

type report struct {
	Server   *serverReport  `json:"server,omitempty"`
	Client   *clientReport  `json:"client,omitempty"`
	Latency  latencyReport  `json:"latency"`
	Download transferReport `json:"download"`
	Upload   transferReport `json:"upload"`
}

type serverReport struct {
	Colo string `json:"colo"`
	City string `json:"city"`
}

type clientReport struct {
	IP      string `json:"ip"`
	ASN     int    `json:"asn"`
	Org     string `json:"org"`
	City    string `json:"city"`
	Country string `json:"country"`
}

type latencyReport struct {
	MedianMs float64 `json:"median_ms"`
	JitterMs float64 `json:"jitter_ms"`
	MinMs    float64 `json:"min_ms"`
	MaxMs    float64 `json:"max_ms"`
}

type transferReport struct {
	Mbps            float64 `json:"mbps"`
	Bytes           int64   `json:"bytes"`
	LoadedLatencyMs float64 `json:"loaded_latency_ms"`
}

func newReport(r results) report {
	rep := report{
		Latency: latencyReport{
			MedianMs: round2(ms(r.latency.Median)),
			JitterMs: round2(ms(r.latency.Jitter)),
			MinMs:    round2(ms(r.latency.Min)),
			MaxMs:    round2(ms(r.latency.Max)),
		},
		Download: newTransferReport(r.download),
		Upload:   newTransferReport(r.upload),
	}
	if m := r.meta; m != nil {
		rep.Server = &serverReport{Colo: m.Colo.IATA, City: m.Colo.City}
		rep.Client = &clientReport{IP: m.IP, ASN: m.ASN, Org: m.Org, City: m.City, Country: m.Country}
	}
	return rep
}

func newTransferReport(t cfspeed.Throughput) transferReport {
	return transferReport{
		Mbps:            round2(t.Bps / 1e6),
		Bytes:           t.Bytes,
		LoadedLatencyMs: round2(ms(t.Loaded.Median)),
	}
}

func round2(v float64) float64 { return math.Round(v*100) / 100 }
