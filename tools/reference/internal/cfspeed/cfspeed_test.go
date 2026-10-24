package cfspeed

import (
	"net/http"
	"sync/atomic"
	"testing"
	"time"
)

func TestServerTime(t *testing.T) {
	tests := []struct {
		name   string
		header []string
		want   time.Duration
	}{
		{"none", nil, 0},
		{
			"sums cfSpeed entries across header lines",
			[]string{"cfSpeedEdge;dur=4, cfSpeedWorker;dur=23", `cfL4;desc="?proto=TCP&rtt=17465"`},
			27 * time.Millisecond,
		},
		{
			"prefers cfRequestDuration",
			[]string{"cfRequestDuration;dur=12.5, cfSpeedEdge;dur=4"},
			12500 * time.Microsecond,
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			h := http.Header{"Server-Timing": tt.header}
			if got := serverTime(h); got != tt.want {
				t.Errorf("serverTime = %v, want %v", got, tt.want)
			}
		})
	}
}

func TestSummarize(t *testing.T) {
	ms := func(v ...int) []time.Duration {
		out := make([]time.Duration, len(v))
		for i, n := range v {
			out[i] = time.Duration(n) * time.Millisecond
		}
		return out
	}
	l := summarize(ms(20, 24, 22, 30))
	if l.Median != 23*time.Millisecond {
		t.Errorf("median = %v, want 23ms", l.Median)
	}
	if l.Min != 20*time.Millisecond || l.Max != 30*time.Millisecond {
		t.Errorf("min/max = %v/%v, want 20ms/30ms", l.Min, l.Max)
	}
	// |24-20| + |22-24| + |30-22| = 14, over 3 intervals.
	if want := 14 * time.Millisecond / 3; l.Jitter != want {
		t.Errorf("jitter = %v, want %v", l.Jitter, want)
	}
	if empty := summarize(nil); empty.Median != 0 || empty.Samples != nil {
		t.Errorf("summarize(nil) = %+v, want zero", empty)
	}
}

func TestPayload(t *testing.T) {
	var sent atomic.Int64
	p := &payload{remaining: 3<<20 + 7, sent: &sent}
	buf := make([]byte, 64<<10)
	var total int64
	for {
		n, err := p.Read(buf)
		total += int64(n)
		if err != nil {
			break
		}
	}
	if total != 3<<20+7 || sent.Load() != total {
		t.Errorf("read %d bytes, counted %d, want %d", total, sent.Load(), 3<<20+7)
	}
}
