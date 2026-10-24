package main

import (
	"context"
	"fmt"
	"image/color"
	"strings"
	"time"

	"charm.land/bubbles/v2/help"
	"charm.land/bubbles/v2/key"
	"charm.land/bubbles/v2/progress"
	"charm.land/bubbles/v2/spinner"
	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/x/ansi"

	"github.com/elliotmoreau/speedtest/internal/cfspeed"
)

const (
	maxWidth = 68
	// minWidth is the narrowest layout whose cards still fit their text;
	// narrower terminals get the view clipped rather than wrapped.
	minWidth    = 56
	chartHeight = 4
)

type keyMap struct{ rerun, quit key.Binding }

func (k keyMap) ShortHelp() []key.Binding  { return []key.Binding{k.rerun, k.quit} }
func (k keyMap) FullHelp() [][]key.Binding { return [][]key.Binding{k.ShortHelp()} }

var keys = keyMap{
	rerun: key.NewBinding(key.WithKeys("r"), key.WithHelp("r", "run again")),
	quit:  key.NewBinding(key.WithKeys("q", "esc", "ctrl+c"), key.WithHelp("q", "quit")),
}

type accent struct{ from, to color.Color }

type styles struct {
	text, muted, faint, border, good, bad color.Color
	ping, download, upload                accent
}

func newStyles(isDark bool) styles {
	c := lipgloss.LightDark(isDark)
	hex := lipgloss.Color
	return styles{
		text:     c(hex("#27272A"), hex("#E4E4E7")),
		muted:    c(hex("#71717A"), hex("#A1A1AA")),
		faint:    c(hex("#D4D4D8"), hex("#52525B")),
		border:   c(hex("#E4E4E7"), hex("#3F3F46")),
		good:     c(hex("#16A34A"), hex("#4ADE80")),
		bad:      c(hex("#DC2626"), hex("#F87171")),
		ping:     accent{c(hex("#CA8A04"), hex("#FDE047")), c(hex("#EA580C"), hex("#FB923C"))},
		download: accent{c(hex("#0891B2"), hex("#22D3EE")), c(hex("#4F46E5"), hex("#818CF8"))},
		upload:   accent{c(hex("#DB2777"), hex("#F472B6")), c(hex("#9333EA"), hex("#C084FC"))},
	}
}

func (s styles) fg(c color.Color) lipgloss.Style { return lipgloss.NewStyle().Foreground(c) }

func (s styles) accentFor(p phase) accent {
	switch p {
	case phaseDownload:
		return s.download
	case phaseUpload:
		return s.upload
	}
	return s.ping
}

// eventMsg carries an event from a test run, tagged with that run so events
// from a cancelled run are dropped once a new one starts.
type eventMsg struct {
	run int
	msg tea.Msg
}

func listen(run int, events <-chan tea.Msg) tea.Cmd {
	return func() tea.Msg {
		msg, ok := <-events
		if !ok {
			return nil
		}
		return eventMsg{run, msg}
	}
}

type model struct {
	client *cfspeed.Client
	cfg    config

	run      int
	events   chan tea.Msg
	cancel   context.CancelFunc
	started  time.Time
	finished time.Duration

	phase     phase
	meta      *cfspeed.Meta
	pings     []time.Duration
	latency   *cfspeed.Latency
	samples   [2][]cfspeed.Sample // indexed by cfspeed.Direction
	transfers [2]*cfspeed.Throughput
	err       error

	width    int
	quitting bool
	styles   styles
	spinner  spinner.Model
	help     help.Model
}

func newModel(client *cfspeed.Client, cfg config) *model {
	return &model{
		client:  client,
		cfg:     cfg,
		cancel:  func() {},
		styles:  newStyles(true),
		spinner: spinner.New(spinner.WithSpinner(spinner.MiniDot)),
		help:    help.New(),
	}
}

func (m *model) Init() tea.Cmd {
	return tea.Batch(tea.RequestBackgroundColor, m.start())
}

// start cancels any test in flight and begins a fresh one.
func (m *model) start() tea.Cmd {
	m.cancel()
	ctx, cancel := context.WithCancel(context.Background())
	events := make(chan tea.Msg, 64)
	*m = model{
		client:  m.client,
		cfg:     m.cfg,
		run:     m.run + 1,
		events:  events,
		cancel:  cancel,
		started: time.Now(),
		width:   m.width,
		styles:  m.styles,
		spinner: m.spinner,
		help:    m.help,
	}

	client, cfg := m.client, m.cfg
	go func() {
		defer close(events)
		emit := func(msg tea.Msg) {
			select {
			case events <- msg:
			case <-ctx.Done():
			}
		}
		if _, err := runTest(ctx, client, cfg, emit); err != nil && ctx.Err() == nil {
			emit(errMsg{err})
		}
	}()
	return tea.Batch(listen(m.run, events), m.spinner.Tick)
}

func (m *model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.BackgroundColorMsg:
		isDark := msg.IsDark()
		m.styles = newStyles(isDark)
		m.help.Styles = help.DefaultStyles(isDark)

	case tea.WindowSizeMsg:
		m.width = msg.Width

	case tea.KeyPressMsg:
		switch {
		case key.Matches(msg, keys.quit):
			m.quitting = true
			m.cancel()
			return m, tea.Quit
		case key.Matches(msg, keys.rerun):
			return m, m.start()
		}

	case spinner.TickMsg:
		if !m.running() {
			return m, nil
		}
		var cmd tea.Cmd
		m.spinner, cmd = m.spinner.Update(msg)
		return m, cmd

	case eventMsg:
		if msg.run != m.run {
			return m, nil
		}
		m.handleEvent(msg.msg)
		return m, listen(m.run, m.events)
	}
	return m, nil
}

func (m *model) handleEvent(msg tea.Msg) {
	switch msg := msg.(type) {
	case phaseMsg:
		m.phase = phase(msg)
		if m.phase == phaseDone {
			m.finished = time.Since(m.started)
		}
	case metaMsg:
		meta := cfspeed.Meta(msg)
		m.meta = &meta
	case pingMsg:
		m.pings = append(m.pings, time.Duration(msg))
	case latencyMsg:
		lat := cfspeed.Latency(msg)
		m.latency = &lat
	case sampleMsg:
		m.samples[msg.dir] = append(m.samples[msg.dir], msg.sample)
	case throughputMsg:
		m.transfers[msg.dir] = &msg.result
	case errMsg:
		m.err = msg.err
	}
}

func (m *model) running() bool { return m.err == nil && m.phase != phaseDone }

func (m *model) View() tea.View {
	w := m.contentWidth()
	var b strings.Builder
	b.WriteString(m.headerView(w))
	b.WriteString("\n\n")
	b.WriteString(m.cardsView(w))
	b.WriteString("\n")
	b.WriteString(m.panelView(w))
	if !m.quitting {
		b.WriteString("\n\n ")
		b.WriteString(m.help.View(keys))
	}

	frame := lipgloss.NewStyle().Margin(1, 0, 0, 1)
	if m.width > 0 {
		frame = frame.MaxWidth(m.width)
	}
	v := tea.NewView(frame.Render(b.String()))
	switch {
	case m.err != nil:
		v.ProgressBar = tea.NewProgressBar(tea.ProgressBarError, 100)
	case m.running():
		v.ProgressBar = tea.NewProgressBar(tea.ProgressBarDefault, int(m.overallProgress()*100))
	}
	return v
}

func (m *model) contentWidth() int {
	if m.width == 0 {
		return maxWidth
	}
	return max(minWidth, min(maxWidth, m.width-2))
}

// overallProgress weights the phases roughly by how long they take.
func (m *model) overallProgress() float64 {
	switch m.phase {
	case phaseLatency:
		return 0.1 * float64(len(m.pings)) / float64(m.cfg.pings)
	case phaseDownload:
		return 0.1 + 0.45*m.transferProgress(cfspeed.Download)
	case phaseUpload:
		return 0.55 + 0.45*m.transferProgress(cfspeed.Upload)
	}
	return 0
}

func (m *model) transferProgress(dir cfspeed.Direction) float64 {
	samples := m.samples[dir]
	if len(samples) == 0 {
		return 0
	}
	return min(1, float64(samples[len(samples)-1].Elapsed)/float64(m.cfg.duration))
}

func (m *model) headerView(width int) string {
	s := m.styles
	title := lipgloss.NewStyle().
		Bold(true).
		Foreground(lipgloss.Color("#FFFDF5")).
		Background(lipgloss.Color("#7D56F4")).
		Padding(0, 1).
		Render("SPEEDTEST")

	server := s.fg(s.muted).Render("Cloudflare")
	client := s.fg(s.faint).Render("locating…")
	if m.meta != nil {
		server = s.fg(s.text).Bold(true).Render("Cloudflare") +
			s.fg(s.muted).Render(fmt.Sprintf(" · %s (%s)", m.meta.Colo.City, m.meta.Colo.IATA))
		parts := []string{m.meta.IP}
		if m.meta.Org != "" {
			parts = append(parts, fmt.Sprintf("%s (AS%d)", m.meta.Org, m.meta.ASN))
		}
		if m.meta.City != "" {
			parts = append(parts, m.meta.City+", "+m.meta.Country)
		}
		client = s.fg(s.muted).Render(ansi.Truncate(strings.Join(parts, " · "), width-1, "…"))
	}
	return title + "  " + server + "\n " + client
}

func (m *model) cardsView(width int) string {
	cardWidth := (width - 2) / 3
	return lipgloss.JoinHorizontal(lipgloss.Top,
		m.card(cardWidth, phaseLatency), " ",
		m.card(cardWidth, phaseDownload), " ",
		m.card(cardWidth, phaseUpload),
	)
}

// card shows one metric: pending, live while its phase runs, then final.
func (m *model) card(width int, p phase) string {
	s := m.styles
	acc := s.accentFor(p)
	inner := width - 4 // border and padding

	var (
		label, value, unit, detail string
		done                       bool
	)
	switch p {
	case phaseLatency:
		label, unit = "◉ PING", "ms"
		switch {
		case m.latency != nil:
			done = true
			value = sig3(ms(m.latency.Median))
			detail = "jitter " + formatMs(m.latency.Jitter)
		case len(m.pings) > 0:
			value = sig3(ms(m.pings[len(m.pings)-1]))
			detail = fmt.Sprintf("sample %d/%d", len(m.pings), m.cfg.pings)
		}
	case phaseDownload, phaseUpload:
		dir := cfspeed.Download
		label = "↓ DOWNLOAD"
		if p == phaseUpload {
			dir, label = cfspeed.Upload, "↑ UPLOAD"
		}
		if res := m.transfers[dir]; res != nil {
			done = true
			value, unit = formatRate(res.Bps)
			detail = "loaded —"
			if res.Loaded.Median > 0 {
				detail = "loaded " + formatMs(res.Loaded.Median)
			}
		} else if samples := m.samples[dir]; len(samples) > 0 {
			value, unit = formatRate(samples[len(samples)-1].Bps)
			detail = "peak " + formatRateText(peakBps(samples))
		}
	}

	active := m.phase == p && m.running()
	labelStyle := s.fg(s.muted).Bold(true)
	status := ""
	switch {
	case active:
		labelStyle = s.fg(acc.from).Bold(true)
		status = s.fg(acc.from).Render(m.spinner.View())
	case done:
		labelStyle = s.fg(acc.from).Bold(true)
		status = s.fg(s.good).Render("✓")
	}
	header := spread(labelStyle.Render(label), status, inner)

	var number string
	switch {
	case value == "":
		number = bigText("--", s.faint, s.faint)
		unit = ""
	case lipgloss.Width(bigText(value, acc.from, acc.to)) <= inner:
		number = bigText(value, acc.from, acc.to)
	default: // too narrow for the big font
		number = "\n" + s.fg(acc.from).Bold(true).Render(value) + "\n"
	}

	border := s.border
	if active {
		border = acc.from
	}
	return lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(border).
		Padding(0, 1).
		Width(width).
		Render(strings.Join([]string{
			header,
			number,
			s.fg(s.muted).Render(unit),
			s.fg(s.muted).Render(ansi.Truncate(detail, inner, "…")),
		}, "\n"))
}

func (m *model) panelView(width int) string {
	switch {
	case m.err != nil:
		return m.errorPanel(width)
	case m.phase == phaseDone:
		return m.summaryPanel(width)
	}
	return m.livePanel(width)
}

func (m *model) panel(width int, border color.Color, content string) string {
	return lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(border).
		Padding(0, 1).
		Width(width).
		Render(content)
}

// livePanel charts the phase in progress.
func (m *model) livePanel(width int) string {
	s := m.styles
	inner := width - 4
	acc := s.accentFor(m.phase)
	spin := s.fg(acc.from).Render(m.spinner.View())

	var (
		title, progressText string
		values              []float64
		slots               = 1
		pct                 float64
	)
	switch m.phase {
	case phaseConnect:
		title = "Finding the nearest Cloudflare edge"
	case phaseLatency:
		title = "Measuring latency"
		progressText = fmt.Sprintf("%d/%d", len(m.pings), m.cfg.pings)
		for _, d := range m.pings {
			values = append(values, ms(d))
		}
		slots = m.cfg.pings
		pct = float64(len(m.pings)) / float64(m.cfg.pings)
	case phaseDownload, phaseUpload:
		dir := cfspeed.Download
		title = "Downloading"
		if m.phase == phaseUpload {
			dir, title = cfspeed.Upload, "Uploading"
		}
		title += fmt.Sprintf(" · %d streams", m.cfg.streams)
		for _, sample := range m.samples[dir] {
			values = append(values, sample.Bps)
		}
		slots = int(m.cfg.duration / cfspeed.SampleInterval)
		pct = m.transferProgress(dir)
		progressText = formatSeconds(time.Duration(pct*float64(m.cfg.duration))) +
			" / " + formatSeconds(m.cfg.duration)
	}

	header := spread(spin+" "+s.fg(s.text).Bold(true).Render(title), s.fg(s.muted).Render(progressText), inner)

	bar := progress.New(
		progress.WithColors(acc.from, acc.to),
		progress.WithoutPercentage(),
		progress.WithWidth(inner),
		progress.WithFillCharacters('━', '─'),
	)
	bar.EmptyColor = s.border

	return m.panel(width, s.border, strings.Join([]string{
		header,
		chart(values, slots, inner, chartHeight, acc.from, acc.to, s.faint),
		bar.ViewAs(pct),
	}, "\n"))
}

// summaryPanel shows both throughput graphs side by side once the test ends.
func (m *model) summaryPanel(width int) string {
	s := m.styles
	inner := width - 4
	half := (inner - 3) / 2
	slots := int(m.cfg.duration / cfspeed.SampleInterval)

	graph := func(dir cfspeed.Direction, label string, acc accent) string {
		var values []float64
		for _, sample := range m.samples[dir] {
			values = append(values, sample.Bps)
		}
		head := spread(
			s.fg(acc.from).Bold(true).Render(label),
			s.fg(s.muted).Render("peak "+formatRateText(peakBps(m.samples[dir]))),
			half,
		)
		return head + "\n" + chart(values, slots, half, chartHeight-1, acc.from, acc.to, s.faint)
	}

	var transferred int64
	for _, res := range m.transfers {
		if res != nil {
			transferred += res.Bytes
		}
	}
	footer := s.fg(s.good).Render("✓ ") +
		s.fg(s.text).Render("Done in "+formatSeconds(m.finished)) +
		s.fg(s.muted).Render(" · "+formatBytes(transferred)+" transferred")

	return m.panel(width, s.border, lipgloss.JoinHorizontal(lipgloss.Top,
		graph(cfspeed.Download, "↓ download", s.download),
		"   ",
		graph(cfspeed.Upload, "↑ upload", s.upload),
	)+"\n\n"+footer)
}

func (m *model) errorPanel(width int) string {
	s := m.styles
	return m.panel(width, s.bad,
		s.fg(s.bad).Bold(true).Render("✗ Test failed")+"\n"+
			s.fg(s.text).Width(width-4).Render(m.err.Error())+"\n\n"+
			s.fg(s.muted).Render("Check your connection, then press r to try again."))
}

func peakBps(samples []cfspeed.Sample) float64 {
	peak := 0.0
	for _, sample := range samples {
		peak = max(peak, sample.Bps)
	}
	return peak
}
