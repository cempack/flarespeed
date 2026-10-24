// vecgen prints test vectors for the C port. Strings are hex-encoded with
// a "-" prefix so no field is ever empty.
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"math"
	"math/rand/v2"
	"net/http"
	"os"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"time"
)

func hx(s string) string { return fmt.Sprintf("%x", "-"+s) }

// Copies of the Go version's logic (package main is not importable).
type Meta struct {
	IP      string `json:"clientIp"`
	ASN     int    `json:"asn"`
	Org     string `json:"asOrganization"`
	City    string `json:"city"`
	Country string `json:"country"`
	Colo    struct {
		IATA string `json:"iata"`
		City string `json:"city"`
	} `json:"colo"`
}

func sig3(v float64) string {
	for _, decimals := range []int{2, 1} {
		if s := strconv.FormatFloat(v, 'f', decimals, 64); len(s) <= 4 {
			return s
		}
	}
	return strconv.FormatFloat(v, 'f', 0, 64)
}
func formatRate(bps float64) (string, string) {
	switch {
	case bps >= 1e9:
		return sig3(bps / 1e9), "Gbps"
	case bps >= 1e6:
		return sig3(bps / 1e6), "Mbps"
	}
	return sig3(bps / 1e3), "Kbps"
}
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

var (
	requestDurationRe = regexp.MustCompile(`(?i)(?:^|,\s*)cfReq(?:uest)?Dur(?:ation)?;\s*dur=([0-9.]+)`)
	speedDurationRe   = regexp.MustCompile(`(?i)(?:^|,\s*)cfSpeed[a-z]*;\s*dur=([0-9.]+)`)
)

func serverTime(h http.Header) time.Duration {
	header := strings.Join(h.Values("Server-Timing"), ", ")
	toDuration := func(ms string) time.Duration {
		v, _ := strconv.ParseFloat(ms, 64)
		return time.Duration(v * float64(time.Millisecond))
	}
	if m := requestDurationRe.FindStringSubmatch(header); m != nil {
		return toDuration(m[1])
	}
	var sum time.Duration
	for _, m := range speedDurationRe.FindAllStringSubmatch(header, -1) {
		sum += toDuration(m[1])
	}
	return sum
}

func summarize(samples []time.Duration) (med, mn, mx, jit time.Duration) {
	if len(samples) == 0 {
		return
	}
	var diffs time.Duration
	for i := 1; i < len(samples); i++ {
		diffs += (samples[i] - samples[i-1]).Abs()
	}
	sorted := slices.Sorted(slices.Values(samples))
	med, mn, mx = sorted[len(sorted)/2], sorted[0], sorted[len(sorted)-1]
	if n := len(sorted); n%2 == 0 {
		med = (sorted[n/2-1] + sorted[n/2]) / 2
	}
	if len(samples) > 1 {
		jit = diffs / time.Duration(len(samples)-1)
	}
	return
}

func pick[T any](r *rand.Rand, xs []T) T { return xs[r.IntN(len(xs))] }

func main() {
	r := rand.New(rand.NewPCG(11, 13))
	switch os.Args[1] {
	case "meta":
		fixed := []string{
			`{"hostname":"speed.cloudflare.com","clientIp":"163.5.3.15","httpProtocol":"HTTP/1.1","asn":56339,"asOrganization":"Ionis Group - Ionis Schools of Technology and Management SAS","country":"FR","city":"Ivry-sur-Seine","region":"Île-de-France","postalCode":"94200","latitude":"48.81568","longitude":"2.38487","colo":{"iata":"CDG","lat":49.012798,"lon":2.55,"cca2":"FR","region":"Europe","city":"Paris"}}`,
			`{}`, `null`, `[]`, `"x"`, `1`, `true`, ``, `   `, `{"asn":"1"}`, `{"asn":1.5}`, `{"asn":1e3}`, `{"asn":-7}`,
			`{"asn":99999999999999999999}`, `{"asn":null,"city":null}`, `{"colo":null}`, `{"colo":[]}`, `{"colo":"x"}`,
			`{"CLIENTIP":"1.2.3.4","ASOrganization":"x"}`, `{"city":"a","city":"b"}`, `{"aſn":5}`, `{"K":1}`,
			`{"city":"😀 \ud800 \udc00x é A"}`, `{"city":"bad\xff\xfeutf"}`, `{"city":"tab	x"}`,
			`{"city":"a\qb"}`, `{"city":"x"} trailing garbage`, `{"city":"x",}`, `{"city" "x"}`, `{"colo":{"IATA":"ams","City":"Amsterdam","x":[1,{"y":null}]}}`,
			`{"asn":01}`, `{"asn":-}`, `{"asn":1.}`, `{"asn":.5}`, `{"asn":1e}`, `{"x":tru}`, `{"x":[1,2,]}`, `{"x":{"a":1,}}`,
			`{"city":"<b>&amp;</b>"}`, `{"city":"  "}`, `{"city":"\u0000nul"}`, strings.Repeat("[", 10001) + strings.Repeat("]", 10001),
			`{"x":` + strings.Repeat("[", 9998) + strings.Repeat("]", 9998) + `,"city":"deep"}`,
			`{"x":` + strings.Repeat("[", 10000) + strings.Repeat("]", 10000) + `}`,
			`{"clientIp":"::1","asn":0,"asOrganization":"","country":"","city":"","colo":{"iata":"","city":""}}`,
			"\t\n {\"city\" :\r\"spaced\" } ",
		}
		tokens := []string{`{`, `}`, `[`, `]`, `,`, `:`, `"clientIp"`, `"asn"`, `"city"`, `"colo"`, `"iata"`, `"x"`, `1`, `-2`, `3.5`, `null`, `true`, `"s"`, `"é"`, ` `}
		inputs := fixed
		for i := 0; i < 2000; i++ {
			var b strings.Builder
			b.WriteString("{")
			for j := r.IntN(8); j > 0; j-- {
				b.WriteString(pick(r, tokens))
			}
			if r.IntN(3) > 0 {
				b.WriteString("}")
			}
			inputs = append(inputs, b.String())
		}
		for _, in := range inputs {
			var m Meta
			err := json.NewDecoder(strings.NewReader(in)).Decode(&m)
			ok := 1
			if err != nil {
				ok, m = 0, Meta{}
			}
			fmt.Printf("%s %d %s %d %s %s %s %s %s\n", hx(in), ok, hx(m.IP), m.ASN, hx(m.Org), hx(m.City), hx(m.Country), hx(m.Colo.IATA), hx(m.Colo.City))
		}
	case "jsonstr":
		parts := []string{"a", " ", "\"", "\\", "/", "\b", "\f", "\n", "\r", "\t", "\x01", "\x1f", "\x7f", "<", ">", "&", "é", "東", "👍", " ", " ", "\xff", "\xc3", "\xed\xa0\x80"}
		for i := 0; i < 3000; i++ {
			var b strings.Builder
			for j := r.IntN(10); j > 0; j-- {
				b.WriteString(pick(r, parts))
			}
			out, _ := json.Marshal(b.String())
			fmt.Printf("%s %s\n", hx(b.String()), hx(string(out)))
		}
	case "jsonnum":
		round2 := func(v float64) float64 { return math.Round(v*100) / 100 }
		for i := 0; i < 5000; i++ {
			var v float64
			switch i % 4 {
			case 0:
				v = round2(r.Float64() * 2000)
			case 1:
				v = round2(r.Float64() * 1e7)
			case 2:
				v = round2(r.Float64())
			default:
				v = round2(float64(r.IntN(100000)) / 7)
			}
			out, _ := json.Marshal(v)
			fmt.Printf("%s %s\n", strconv.FormatFloat(v, 'g', -1, 64), out)
		}
	case "duration":
		fixed := []string{"10s", "2s", "1m30s", "1.5h", ".5s", "5.s", "1h2m3s4ms5us6ns", "1µs", "1μs", "0", "-0", "+0", "", "s", "1", "1x", "-1s", "+3s",
			"1.0000000000000000000001s", "9223372036854775807ns", "9223372036854775808ns", "-9223372036854775808ns", "2562047h47m16.854775807s",
			"2562048h", "100000000000000000000s", "0.000000001s", "0.0000000001s", "1s1", "1..5s", ".s", "1.2.3s", "12345678901234567890ns", "3000ms"}
		chars := []string{"0", "1", "5", "9", ".", "s", "m", "h", "ms", "us", "ns", "µs", "-", "+", "x"}
		inputs := fixed
		for i := 0; i < 3000; i++ {
			var b strings.Builder
			for j := 1 + r.IntN(8); j > 0; j-- {
				b.WriteString(pick(r, chars))
			}
			inputs = append(inputs, b.String())
		}
		for _, in := range inputs {
			d, err := time.ParseDuration(in)
			ok := 1
			if err != nil {
				ok, d = 0, 0
			}
			fmt.Printf("%s %d %d\n", hx(in), ok, int64(d))
		}
	case "format":
		for i := 0; i < 5000; i++ {
			var bps float64
			switch i % 5 {
			case 0:
				bps = r.Float64() * 2e9
			case 1:
				bps = r.Float64() * 2e7
			case 2:
				bps = r.Float64() * 2e4
			case 3:
				bps = math.Pow(10, r.Float64()*12)
			default:
				bps = float64(r.IntN(1000000)) * 1000
			}
			v, u := formatRate(bps)
			ns := r.Int64N(int64(200 * time.Second))
			if i%3 == 0 {
				ns = r.Int64N(int64(2 * time.Second))
			}
			n := r.Int64N(1 << 40)
			if i%4 == 0 {
				n = r.Int64N(5000)
			}
			fmt.Printf("%s %s %s %d %s %s %s %d %s\n",
				strconv.FormatFloat(bps, 'g', -1, 64), v, u, ns,
				sig3(float64(ns)/float64(time.Millisecond)),
				strconv.FormatFloat(time.Duration(ns).Seconds(), 'f', 1, 64)+"s",
				strconv.FormatFloat(float64(ns)/1e6, 'g', -1, 64),
				n, hx(formatBytes(n)))
		}
	case "summarize":
		for i := 0; i < 2000; i++ {
			n := r.IntN(25)
			s := make([]time.Duration, n)
			parts := []string{}
			for j := range s {
				s[j] = time.Duration(r.Int64N(int64(500 * time.Millisecond)))
				parts = append(parts, strconv.FormatInt(int64(s[j]), 10))
			}
			med, mn, mx, jit := summarize(s)
			fmt.Printf("%d %d %d %d %d %s\n", med, mn, mx, jit, n, strings.Join(parts, " "))
		}
	case "servertiming":
		fixed := [][]string{
			{"cfSpeedEdge;dur=4, cfSpeedWorker;dur=23", `cfL4;desc="?proto=TCP&rtt=17465"`},
			{"cfRequestDuration;dur=12.5, cfSpeedEdge;dur=4"},
			{"cfReqDur;dur=0, cfSpeedEdge;dur=4"},
			{"CFSPEEDEDGE;DUR=1.5"}, {"x, cfSpeed;dur=1"}, {"cfSpeedX ;dur=1"}, {"cfSpeedX;  dur=7"},
			{"cfSpeedX;dur=1.2.3, cfSpeedY;dur=2"}, {"cfSpeedX;dur=., cfSpeedY;dur=.5"}, {"cfSpeedX;dur=999999999999999999999"},
			{"cfSpeedA;dur=4611686018427", "cfSpeedB;dur=4611686018427"}, {",cfSpeedA;dur=1"}, {",,  cfSpeedA;dur=1,cfSpeedB;dur=2"},
			{"cfRequestDurationX;dur=1"}, {"cfRequestDur;dur=3", "cfReqDuration;dur=4"}, {"xcfSpeedA;dur=1"}, {"cfSpeed1;dur=1"},
			{"cfSpeedA;dur=1,cfSpeedB;dur=2,"}, {}, {""}, {"cfspeedaaa;dur=0.001"},
		}
		names := []string{"cfSpeedEdge", "cfSpeedWorker", "cfspeed", "CFSPEEDX", "cfRequestDuration", "cfReqDur", "cfReqDuration", "cfRequestDur", "cfL4", "other", "cfSpeed9"}
		seps := []string{", ", ",", ",  ", " , ", ";", ",\t"}
		vals := []string{"1", "2.5", ".5", "1.", "0", "12.345", "1.2.3", ".", "100"}
		var inputs [][]string = fixed
		for i := 0; i < 2000; i++ {
			var lines []string
			for l := 1 + r.IntN(2); l > 0; l-- {
				var b strings.Builder
				for j := 1 + r.IntN(4); j > 0; j-- {
					if b.Len() > 0 {
						b.WriteString(pick(r, seps))
					}
					b.WriteString(pick(r, names) + pick(r, []string{";dur=", "; dur=", ";DUR=", ";desc=x;dur=", ";dur= "}) + pick(r, vals))
				}
				lines = append(lines, b.String())
			}
			inputs = append(inputs, lines)
		}
		for _, lines := range inputs {
			h := http.Header{}
			for _, l := range lines {
				h.Add("Server-Timing", l)
			}
			fmt.Printf("%s %d\n", hx(strings.Join(h.Values("Server-Timing"), ", ")), int64(serverTime(h)))
		}
	}
	_ = bytes.MinRead
}
