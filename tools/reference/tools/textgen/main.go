// textgen prints wrap/truncate/width vectors: limit, then hex-encoded
// input, wrapped output and truncated output, then the width.
package main

import (
	"fmt"
	"math/rand/v2"
	"strings"

	"github.com/charmbracelet/x/ansi"
)

func main() {
	fixed := []string{
		`latency: Get "https://speed.cloudflare.com/__down?bytes=0": proxyconnect tcp: dial tcp 127.0.0.1:9: connect: connection refused`,
		"download: server responded 429 Too Many Requests",
		"a-very-long-hyphenated-word-that-goes-on-and-on",
		"supercalifragilisticexpialidocious",
		"  leading and trailing  ",
		"multiple   spaces   between",
		"line one\nline two\n\nline four",
		"Île-de-France · Ivry-sur-Seine",
		"東京都 渋谷区 東京都 渋谷区 東京都",
		"emoji 👍🏽 and 👨‍👩‍👧 family 🇫🇷 flag",
		"combining ééé marks",
		"nbsp joined words here",
		"ideographic　space",
		"",
		"-",
		"--",
		"a - b - c",
		"x",
		strings.Repeat("y", 40),
	}
	parts := []string{" ", "  ", "-", "a", "bb", "ccc", "word", "é", "東", "👍🏽", "é", "\n", " ", "　", "xxxxxxxxxxxxxxxx", "a-b", "🇫🇷"}
	r := rand.New(rand.NewPCG(7, 9))
	var inputs []string
	inputs = append(inputs, fixed...)
	for i := 0; i < 3000; i++ {
		var b strings.Builder
		n := r.IntN(12)
		for j := 0; j < n; j++ {
			b.WriteString(parts[r.IntN(len(parts))])
		}
		inputs = append(inputs, b.String())
	}
	for i, s := range inputs {
		limits := []int{1 + r.IntN(30)}
		if i < len(fixed) {
			limits = []int{1, 2, 3, 5, 8, 13, 20, 44, 52, 64}
		}
		for _, l := range limits {
			fmt.Printf("%d %x %x %x %d\n", l, "-"+s, "-"+ansi.Wrap(s, l, ""), "-"+ansi.Truncate(s, l, "…"), ansi.StringWidth(s))
		}
	}
}
