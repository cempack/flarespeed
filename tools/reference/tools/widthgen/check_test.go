package main

import (
	"testing"

	"github.com/charmbracelet/x/ansi"
)

func TestClusters(t *testing.T) {
	for _, s := range []string{"é", "👍🏽", "👨‍👩‍👧", "🇫🇷", "☺️", "a‍b", "­", "Île", "東京"} {
		t.Logf("%q width=%d", s, ansi.StringWidth(s))
	}
}
