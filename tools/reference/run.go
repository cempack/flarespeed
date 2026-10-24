package main

import (
	"context"
	"fmt"
	"time"

	tea "charm.land/bubbletea/v2"

	"github.com/elliotmoreau/speedtest/internal/cfspeed"
)

type config struct {
	duration time.Duration
	streams  int
	pings    int
}

type phase int

const (
	phaseConnect phase = iota
	phaseLatency
	phaseDownload
	phaseUpload
	phaseDone
)

func phaseFor(dir cfspeed.Direction) phase {
	if dir == cfspeed.Upload {
		return phaseUpload
	}
	return phaseDownload
}

// Events reported by runTest as the test progresses.
type (
	phaseMsg   phase
	metaMsg    cfspeed.Meta
	pingMsg    time.Duration
	latencyMsg cfspeed.Latency
	sampleMsg  struct {
		dir    cfspeed.Direction
		sample cfspeed.Sample
	}
	throughputMsg struct {
		dir    cfspeed.Direction
		result cfspeed.Throughput
	}
	errMsg struct{ err error }
)

type results struct {
	meta     *cfspeed.Meta
	latency  cfspeed.Latency
	download cfspeed.Throughput
	upload   cfspeed.Throughput
}

// runTest runs every phase in order, reporting progress through emit.
func runTest(ctx context.Context, c *cfspeed.Client, cfg config, emit func(tea.Msg)) (results, error) {
	var r results

	emit(phaseMsg(phaseConnect))
	// Location info is cosmetic, so failing to fetch it isn't fatal.
	if meta, err := c.Meta(ctx); err == nil {
		r.meta = &meta
		emit(metaMsg(meta))
	}

	emit(phaseMsg(phaseLatency))
	lat, err := c.MeasureLatency(ctx, cfg.pings, func(d time.Duration) { emit(pingMsg(d)) })
	if err != nil {
		return r, fmt.Errorf("latency: %w", err)
	}
	r.latency = lat
	emit(latencyMsg(lat))

	for _, dir := range []cfspeed.Direction{cfspeed.Download, cfspeed.Upload} {
		emit(phaseMsg(phaseFor(dir)))
		res, err := c.MeasureThroughput(ctx, dir, cfspeed.ThroughputOptions{
			Duration: cfg.duration,
			Streams:  cfg.streams,
			OnSample: func(s cfspeed.Sample) { emit(sampleMsg{dir, s}) },
		})
		if err != nil {
			return r, fmt.Errorf("%s: %w", dir, err)
		}
		if dir == cfspeed.Download {
			r.download = res
		} else {
			r.upload = res
		}
		emit(throughputMsg{dir, res})
	}

	emit(phaseMsg(phaseDone))
	return r, nil
}
