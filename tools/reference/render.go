package main

import (
	"fmt"
	"image/color"
	"math"
	"strconv"
	"strings"
	"time"

	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/x/ansi"
)

// glyphs is a 3×5 pixel font for the big readouts. Each glyph is drawn into
// three terminal rows by pairing pixel rows into half blocks.
var glyphs = map[rune][5]string{
	'0': {"###", "#.#", "#.#", "#.#", "###"},
	'1': {".#.", "##.", ".#.", ".#.", "###"},
	'2': {"###", "..#", "###", "#..", "###"},
	'3': {"###", "..#", "###", "..#", "###"},
	'4': {"#.#", "#.#", "###", "..#", "..#"},
	'5': {"###", "#..", "###", "..#", "###"},
	'6': {"###", "#..", "###", "#.#", "###"},
	'7': {"###", "..#", "..#", "..#", "..#"},
	'8': {"###", "#.#", "###", "#.#", "###"},
	'9': {"###", "#.#", "###", "..#", "###"},
	'.': {".", ".", ".", ".", "#"},
	'-': {"...", "...", "###", "...", "..."},
}

// bigText renders s in the glyph font, three rows tall, coloured with a
// left-to-right gradient. Characters missing from the font are skipped.
func bigText(s string, from, to color.Color) string {
	var rows [3][]rune
	for i, r := range s {
		g, ok := glyphs[r]
		if !ok {
			continue
		}
		if i > 0 {
			for row := range rows {
				rows[row] = append(rows[row], ' ')
			}
		}
		for row := range rows {
			top := g[row*2]
			bottom := strings.Repeat(".", len(top))
			if row*2+1 < len(g) {
				bottom = g[row*2+1]
			}
			for x := range len(top) {
				rows[row] = append(rows[row], halfBlock(top[x] == '#', bottom[x] == '#'))
			}
		}
	}

	width := len(rows[0])
	if width == 0 {
		return ""
	}
	ramp := lipgloss.Blend1D(width, from, to)
	lines := make([]string, len(rows))
	for row, cells := range rows {
		var b strings.Builder
		for x, c := range cells {
			b.WriteString(lipgloss.NewStyle().Foreground(ramp[x]).Render(string(c)))
		}
		lines[row] = b.String()
	}
	return strings.Join(lines, "\n")
}

func halfBlock(top, bottom bool) rune {
	switch {
	case top && bottom:
		return '█'
	case top:
		return '▀'
	case bottom:
		return '▄'
	}
	return ' '
}

var bars = []rune(" ▁▂▃▄▅▆▇█")

// chart draws values as a bar chart width columns wide and height rows tall.
// slots is how many values the finished chart will hold, so a partly filled
// chart grows from the left and the rest is drawn as a faint baseline. When
// there's room, each value gets its own bar with a gap after it; when there
// are more slots than columns, each column shows the average of its slots.
func chart(values []float64, slots, width, height int, from, to, faint color.Color) string {
	const (
		future  = -1 // slot not yet measured
		gap     = -2 // space between bars
		padding = -3 // leftover columns after the last slot
	)
	peak := 0.0
	for _, v := range values {
		peak = max(peak, v)
	}
	// Bars of equal width need a whole number of columns per slot.
	perSlot := width / slots
	spaced := perSlot >= 3
	levels := make([]float64, width)
	for i := range levels {
		lo, hi := i*slots/width, (i+1)*slots/width
		if spaced {
			lo, hi = i/perSlot, i/perSlot+1
		}
		switch {
		case spaced && lo >= slots:
			levels[i] = padding
		case lo >= len(values):
			levels[i] = future
		case spaced && i%perSlot == perSlot-1:
			levels[i] = gap
		default:
			hi = min(max(hi, lo+1), len(values))
			sum := 0.0
			for _, v := range values[lo:hi] {
				sum += v
			}
			if peak > 0 {
				levels[i] = sum / float64(hi-lo) / peak
			}
		}
	}

	ramp := lipgloss.Blend1D(height, to, from)
	faintStyle := lipgloss.NewStyle().Foreground(faint)
	lines := make([]string, height)
	for row := range height {
		below := float64((height - 1 - row) * 8)
		var data, rest strings.Builder
		for _, level := range levels {
			switch level {
			case padding:
				rest.WriteRune(' ')
			case future:
				if row == height-1 {
					rest.WriteRune('·')
				} else {
					rest.WriteRune(' ')
				}
			case gap:
				data.WriteRune(' ')
			default:
				eighths := int(math.Round(level*float64(height*8) - below))
				if row == height-1 && level > 0 {
					eighths = max(eighths, 1)
				}
				data.WriteRune(bars[min(max(eighths, 0), 8)])
			}
		}
		lines[row] = lipgloss.NewStyle().Foreground(ramp[row]).Render(data.String()) +
			faintStyle.Render(rest.String())
	}
	return strings.Join(lines, "\n")
}

// spread places left and right at opposite ends of a line width columns
// wide, truncating left when both don't fit.
func spread(left, right string, width int) string {
	left = ansi.Truncate(left, width-lipgloss.Width(right)-1, "…")
	return left + strings.Repeat(" ", max(1, width-lipgloss.Width(left)-lipgloss.Width(right))) + right
}

// sig3 formats v to about three significant digits: 4.87, 48.7, 487.
func sig3(v float64) string {
	for _, decimals := range []int{2, 1} {
		if s := strconv.FormatFloat(v, 'f', decimals, 64); len(s) <= 4 {
			return s
		}
	}
	return strconv.FormatFloat(v, 'f', 0, 64)
}

// formatRate splits a bit rate into a display number and unit.
func formatRate(bps float64) (string, string) {
	switch {
	case bps >= 1e9:
		return sig3(bps / 1e9), "Gbps"
	case bps >= 1e6:
		return sig3(bps / 1e6), "Mbps"
	}
	return sig3(bps / 1e3), "Kbps"
}

func formatRateText(bps float64) string {
	value, unit := formatRate(bps)
	return value + " " + unit
}

func ms(d time.Duration) float64 { return float64(d) / float64(time.Millisecond) }

func formatMs(d time.Duration) string { return sig3(ms(d)) + " ms" }

func formatBytes(n int64) string {
	const unit = 1000
	if n < unit {
		return fmt.Sprintf("%d B", n)
	}
	div, exp := int64(unit), 0
	for v := n / unit; v >= unit; v /= unit {
		div *= unit
		exp++
	}
	return sig3(float64(n)/float64(div)) + " " + string("kMGTPE"[exp]) + "B"
}

func formatSeconds(d time.Duration) string {
	return strconv.FormatFloat(d.Seconds(), 'f', 1, 64) + "s"
}
