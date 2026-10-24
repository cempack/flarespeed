// colorgen prints C tables and test vectors derived from the Go libraries.
package main

import (
	"fmt"
	"image/color"
	"math/rand/v2"
	"os"

	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/colorprofile"
	"github.com/charmbracelet/x/ansi"
	"github.com/lucasb-eyer/go-colorful"
)

func rgb16(c color.Color) (uint32, uint32, uint32) {
	r, g, b, _ := c.RGBA()
	return r, g, b
}

func main() {
	switch os.Args[1] {
	case "table16":
		for i := 0; i < 256; i++ {
			fmt.Printf("%d,", ansi.Convert16(ansi.IndexedColor(i)))
			if i%16 == 15 {
				fmt.Println()
			}
		}
	case "vectors":
		// random 16-bit colors and their 256/16 conversions
		r := rand.New(rand.NewPCG(1, 2))
		for i := 0; i < 20000; i++ {
			c := colorful.Color{R: float64(r.IntN(65536)) / 65535, G: float64(r.IntN(65536)) / 65535, B: float64(r.IntN(65536)) / 65535}
			if i < 4096 { // also cover exact 8-bit colors
				c = colorful.Color{R: float64(r.IntN(256)) / 255, G: float64(r.IntN(256)) / 255, B: float64(r.IntN(256)) / 255}
			}
			R, G, B := rgb16(c)
			fmt.Printf("%d %d %d %d %d\n", R, G, B,
				colorprofile.ANSI256.Convert(c).(ansi.IndexedColor),
				colorprofile.ANSI.Convert(c).(ansi.BasicColor))
		}
	case "blend":
		// Blend1D outputs for the palette, at several step counts
		stops := [][2]string{{"#22D3EE", "#818CF8"}, {"#818CF8", "#22D3EE"}, {"#F472B6", "#C084FC"}, {"#FDE047", "#FB923C"},
			{"#0891B2", "#4F46E5"}, {"#DB2777", "#9333EA"}, {"#CA8A04", "#EA580C"}, {"#52525B", "#52525B"}, {"#000000", "#FFFFFF"}}
		for _, s := range stops {
			for _, n := range []int{1, 2, 3, 4, 7, 11, 13, 17, 26, 52, 64} {
				out := lipgloss.Blend1D(n, lipgloss.Color(s[0]), lipgloss.Color(s[1]))
				fmt.Printf("%s %s %d", s[0], s[1], n)
				for _, c := range out {
					R, G, B := rgb16(c)
					fmt.Printf(" %d,%d,%d", R, G, B)
				}
				fmt.Println()
			}
		}
	}
}
