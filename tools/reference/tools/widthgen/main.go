// widthgen prints the code point ranges whose display width is not 1,
// according to the same library lipgloss uses.
package main

import (
	"fmt"

	"github.com/charmbracelet/x/ansi"
)

func main() {
	start, cur := -1, -1
	flush := func(end int) {
		if start >= 0 {
			fmt.Printf("\t{0x%04X, 0x%04X, %d},\n", start, end, cur)
		}
	}
	for r := 0x20; r <= 0x10FFFF; r++ {
		w := 1
		if r >= 0xD800 && r <= 0xDFFF {
			w = 1 // surrogates never reach the table (decoded as U+FFFD)
		} else if r == 0x7F || (r >= 0x80 && r < 0xA0) {
			w = 0
		} else {
			w = ansi.StringWidth(string(rune(r)))
		}
		if w == 1 {
			flush(r - 1)
			start, cur = -1, -1
			continue
		}
		if start >= 0 && w == cur {
			continue
		}
		flush(r - 1)
		start, cur = r, w
	}
	flush(0x10FFFF)
}
