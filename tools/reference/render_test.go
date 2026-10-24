package main

import (
	"strings"
	"testing"

	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/x/ansi"
)

func TestSig3(t *testing.T) {
	for v, want := range map[float64]string{
		0.5:    "0.50",
		4.873:  "4.87",
		9.996:  "10.0",
		48.73:  "48.7",
		99.96:  "100",
		487.3:  "487",
		1234.6: "1235",
	} {
		if got := sig3(v); got != want {
			t.Errorf("sig3(%v) = %q, want %q", v, got, want)
		}
	}
}

func TestFormatRate(t *testing.T) {
	for bps, want := range map[float64]string{
		512e3:  "512 Kbps",
		48.7e6: "48.7 Mbps",
		1.25e9: "1.25 Gbps",
	} {
		if got := formatRateText(bps); got != want {
			t.Errorf("formatRateText(%v) = %q, want %q", bps, got, want)
		}
	}
}

func TestFormatBytes(t *testing.T) {
	for n, want := range map[int64]string{
		999:           "999 B",
		5_000:         "5.00 kB",
		812_000_000:   "812 MB",
		1_210_000_000: "1.21 GB",
	} {
		if got := formatBytes(n); got != want {
			t.Errorf("formatBytes(%d) = %q, want %q", n, got, want)
		}
	}
}

func chartRows(values []float64, slots, width, height int) []string {
	c := lipgloss.Color("#ffffff")
	return strings.Split(ansi.Strip(chart(values, slots, width, height, c, c, c)), "\n")
}

func TestChartSpacedBars(t *testing.T) {
	// 20 slots across 62 columns: 3 columns per slot, so bars two wide with
	// a gap, then the unfilled remainder as a dotted baseline.
	rows := chartRows([]float64{1, 1, 1, 1, 1}, 20, 62, 2)
	want := "██ ██ ██ ██ ██ " + strings.Repeat("·", 45) + "  "
	if rows[1] != want {
		t.Errorf("bottom row\n got %q\nwant %q", rows[1], want)
	}
	for _, row := range rows {
		if w := lipgloss.Width(row); w != 62 {
			t.Errorf("row width %d, want 62: %q", w, row)
		}
	}
}

func TestChartAveragesDenseSlots(t *testing.T) {
	// 4 slots in 2 columns: each column averages two values.
	rows := chartRows([]float64{1, 1, 0, 0.5}, 4, 2, 1)
	if want := "█▂"; rows[0] != want {
		t.Errorf("got %q, want %q", rows[0], want)
	}
}

func TestChartEmpty(t *testing.T) {
	rows := chartRows(nil, 100, 10, 2)
	if rows[0] != strings.Repeat(" ", 10) || rows[1] != strings.Repeat("·", 10) {
		t.Errorf("empty chart rendered %q", rows)
	}
}

func TestBigTextWidth(t *testing.T) {
	// Glyphs are three columns (the dot one) with a one-column gap.
	c := lipgloss.Color("#ffffff")
	for s, want := range map[string]int{"487": 11, "48.7": 13, "--": 7} {
		out := bigText(s, c, c)
		if got := lipgloss.Width(out); got != want {
			t.Errorf("bigText(%q) width = %d, want %d", s, got, want)
		}
		if got := lipgloss.Height(out); got != 3 {
			t.Errorf("bigText(%q) height = %d, want 3", s, got)
		}
	}
}
