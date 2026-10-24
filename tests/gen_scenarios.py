#!/usr/bin/env python3
"""Generates the view parity scenarios.

The same scenario list is written out twice: as a Go test that renders each
one with the original Go/Bubble Tea implementation (producing the golden
files in tests/golden), and as C data in tests/scenarios.inc that test_view.c
renders with the C port. Run it from the repository root:

    tests/gen_scenarios.py tools/reference

then `GOLDEN_DIR=$PWD/tests/golden go test -run TestGolden` in tools/reference.
"""
import random
import sys

MS = 1_000_000
SEC = 1_000_000_000

META = dict(ip="163.5.3.15", asn=56339,
            org="Ionis Group - Ionis Schools of Technology and Management SAS",
            city="Ivry-sur-Seine", country="FR", iata="CDG", colo_city="Paris")
META_UNI = dict(ip="2001:db8::1", asn=4713, org="NTT Communications 東京 📡",
                city="Île-de-France", country="FR", iata="NRT", colo_city="Tōkyō")
META_EMPTY = dict(ip="10.0.0.1", asn=0, org="", city="", country="", iata="", colo_city="")
META_SHORT = dict(ip="1.1.1.1", asn=13335, org="Cloudflare", city="SF", country="US",
                  iata="SFO", colo_city="San Francisco")

ERR_LONG = ('latency: Get "https://speed.cloudflare.com/__down?bytes=0": proxyconnect tcp: '
            'dial tcp 127.0.0.1:9: connect: connection refused')
ERR_HYPH = ('download: server responded 503 Service-Temporarily-Unavailable-because-of-a-'
            'very-long-hyphenated-reason supercalifragilisticexpialidociousword  end')
ERR_SHORT = 'upload: no data transferred'

PHASES = {"connect": 0, "latency": 1, "download": 2, "upload": 3, "done": 4}


def pings(n, base=22.0, seed=1, spread=4.0):
    r = random.Random(seed)
    return [int((base + r.uniform(-spread / 2, spread)) * MS) + r.randint(0, 999) for _ in range(n)]


def ramp(n, peak, seed=2):
    r = random.Random(seed)
    out = []
    for i in range(n):
        level = peak * min(1.0, (i + 1) / 15.0) * r.uniform(0.9, 1.08)
        out.append(((i + 1) * 100 * MS + r.randint(0, 400_000), level))
    return out


def lat(samples):
    s = sorted(samples)
    n = len(s)
    med = s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) // 2
    jit = sum(abs(a - b) for a, b in zip(samples[1:], samples)) // max(1, n - 1)
    return (med, s[0], s[-1], jit)


def base(name, **kw):
    s = dict(name=name, width=100, dark=True, phase="connect", quitting=False, spinner=0,
             finished=0, duration=10 * SEC, streams=6, pings_cfg=20, meta=None, pings=[],
             latency=None, samples=[[], []], transfers=[None, None], err="")
    s.update(kw)
    return s


P20 = pings(20)
DOWN = ramp(99, 487e6)
UP = ramp(99, 734e6, seed=3)
T_DOWN = (452.3e6, 309947441, 9_998_765_432, 47_250_000)
T_UP = (717.9e6, 541584576, 10_001_234_567, 60_312_345)
DONE = dict(phase="done", meta=META, pings=P20, latency=lat(P20), samples=[DOWN, UP],
            transfers=[T_DOWN, T_UP], finished=23_437_000_000)

SCENARIOS = [
    base("connect"),
    base("connect_light", dark=False),
    base("connect_meta", meta=META, spinner=3),
    base("latency_start", phase="latency", meta=META),
    base("latency_7", phase="latency", meta=META, pings=P20[:7], spinner=7),
    base("latency_20", phase="latency", meta=META, pings=P20, spinner=13),
    base("latency_light", phase="latency", meta=META, pings=P20[:12], dark=False),
    base("download_start", phase="download", meta=META, pings=P20, latency=lat(P20)),
    base("download_1", phase="download", meta=META, pings=P20, latency=lat(P20),
         samples=[DOWN[:1], []]),
    base("download_37", phase="download", meta=META, pings=P20, latency=lat(P20),
         samples=[DOWN[:37], []], spinner=5),
    base("download_99", phase="download", meta=META, pings=P20, latency=lat(P20),
         samples=[DOWN, []], spinner=9),
    base("upload_50", phase="upload", meta=META, pings=P20, latency=lat(P20),
         samples=[DOWN, UP[:50]], transfers=[T_DOWN, None], spinner=2),
    base("upload_light", phase="upload", meta=META, pings=P20, latency=lat(P20),
         samples=[DOWN, UP[:80]], transfers=[T_DOWN, None], dark=False),
    base("done", **DONE),
    base("done_light", **dict(DONE, dark=False)),
    base("done_w56", **dict(DONE, width=56)),
    base("done_w57", **dict(DONE, width=57)),
    base("done_w59", **dict(DONE, width=59)),
    base("done_w0", **dict(DONE, width=0)),
    base("done_w40", **dict(DONE, width=40)),
    base("done_w200", **dict(DONE, width=200)),
    base("done_quitting", **dict(DONE, quitting=True)),
    base("done_gbps", **dict(DONE, transfers=[(1.2345e9, 12_000_000_000, 10 * SEC, 3 * MS),
                                              (9.87e9, 98_000_000_000, 10 * SEC, 0)],
                             samples=[ramp(99, 1.3e9, 5), ramp(99, 9.9e9, 6)])),
    base("done_kbps", **dict(DONE, transfers=[(512e3, 640_000, 10 * SEC, 1_234 * MS),
                                              (48.7e3, 60_000, 10 * SEC, 2_500 * MS)],
                             samples=[ramp(99, 600e3, 7), ramp(99, 50e3, 8)])),
    base("done_fallback_w56", **dict(DONE, width=56,
                                     transfers=[(999.96e6, 1_000_000_000, 10 * SEC, 1_500 * MS),
                                                (1000e3 - 1, 1_000_000, 10 * SEC, 999 * MS)],
                                     latency=(1_234_567_890, 1_000_000_000, 2_000_000_000,
                                              123_456_789))),
    base("done_fallback_w100", **dict(DONE,
                                      transfers=[(999.96e6, 1_000_000_000, 10 * SEC, 1_500 * MS),
                                                 (1000e3 - 1, 1_000_000, 10 * SEC, 999 * MS)],
                                      latency=(1_234_567_890, 1, 2, 3))),
    base("done_zero_peak", **dict(DONE, samples=[[(i * 100 * MS, 0.0) for i in range(1, 50)], []],
                                  transfers=[(0.0, 0, 10 * SEC, 0), (0.0, 0, 10 * SEC, 0)])),
    base("meta_unicode", phase="latency", meta=META_UNI, pings=P20[:3], width=70),
    base("meta_unicode_narrow", phase="latency", meta=META_UNI, pings=P20[:3], width=58),
    base("meta_empty", phase="download", meta=META_EMPTY, pings=P20, latency=lat(P20),
         samples=[DOWN[:20], []]),
    base("meta_short", phase="upload", meta=META_SHORT, pings=P20, latency=lat(P20),
         samples=[DOWN, UP[:3]], transfers=[T_DOWN, None]),
    base("short_test", phase="download", meta=META, duration=3 * SEC, streams=1, pings_cfg=5,
         pings=pings(5, seed=9), latency=lat(pings(5, seed=9)), samples=[ramp(17, 80e6, 10), []]),
    base("short_latency", phase="latency", meta=META, duration=2 * SEC, pings_cfg=5,
         pings=pings(3, seed=11)),
    base("two_pings", phase="latency", meta=META, pings_cfg=2, pings=pings(1, seed=12)),
    base("many_pings", phase="latency", meta=META, pings_cfg=100, pings=pings(64, seed=13),
         spinner=4),
    base("ping_extremes", phase="latency", meta=META, pings=[300_000, 2_500 * MS, 22 * MS, 0, 87 * MS]),
    base("error_latency", meta=META, phase="latency", pings=P20[:4], err=ERR_LONG, width=80),
    base("error_download", meta=META, phase="download", pings=P20, latency=lat(P20),
         samples=[DOWN[:30], []], err=ERR_HYPH, width=70),
    base("error_w56", phase="latency", err=ERR_HYPH, width=56),
    base("error_short_light", phase="upload", meta=META, pings=P20, latency=lat(P20),
         samples=[DOWN, UP[:10]], transfers=[T_DOWN, None], err=ERR_SHORT, dark=False),
    base("error_quitting", phase="connect", err=ERR_SHORT, quitting=True),
    base("loaded_dash", **dict(DONE, transfers=[(T_DOWN[0], T_DOWN[1], T_DOWN[2], 0),
                                                (T_UP[0], T_UP[1], T_UP[2], 0)])),
    base("big_streams", phase="upload", meta=META, streams=1234567, pings=P20, latency=lat(P20),
         samples=[DOWN, UP[:5]], transfers=[T_DOWN, None], width=56),
]


def go_str(s):
    out = []
    for b in s.encode():
        if b == 0x22 or b == 0x5C or b < 0x20 or b >= 0x7F:
            out.append("\\x%02x" % b)
        else:
            out.append(chr(b))
    return '"' + "".join(out) + '"'


def c_str(s):
    out = []
    for b in s.encode():
        if b == 0x22 or b == 0x5C or b < 0x20 or b >= 0x7F or b == 0x3F:
            out.append("\\%03o" % b)
        else:
            out.append(chr(b))
    return '"' + "".join(out) + '"'


def write_go(path):
    lines = ["// Code generated by tests/gen_scenarios.py. DO NOT EDIT.", "",
             "package main", "", "import (", '\t"errors"', '\t"fmt"', '\t"os"',
             '\t"path/filepath"', '\t"testing"', '\t"time"', "",
             '\t"charm.land/bubbles/v2/help"', '\t"charm.land/bubbles/v2/spinner"', "",
             '\t"github.com/elliotmoreau/speedtest/internal/cfspeed"', ")", "",
             "func TestGolden(t *testing.T) {",
             '\tdir := os.Getenv("GOLDEN_DIR")',
             '\tif dir == "" {', '\t\tt.Skip("GOLDEN_DIR not set")', "\t}"]
    for s in SCENARIOS:
        lines.append("\t{")
        lines.append("\t\tm := newModel(nil, config{duration: %d, streams: %d, pings: %d})"
                     % (s["duration"], s["streams"], s["pings_cfg"]))
        lines.append("\t\tm.width = %d" % s["width"])
        dark = "true" if s["dark"] else "false"
        lines.append("\t\tm.styles = newStyles(%s)" % dark)
        lines.append("\t\tm.help.Styles = help.DefaultStyles(%s)" % dark)
        lines.append("\t\tfor i := 0; i < %d; i++ {" % s["spinner"])
        lines.append("\t\t\tm.spinner, _ = m.spinner.Update(spinner.TickMsg{ID: m.spinner.ID()})")
        lines.append("\t\t}")
        lines.append("\t\tm.phase = phase(%d)" % PHASES[s["phase"]])
        if s["meta"]:
            mt = s["meta"]
            lines.append("\t\tmeta := cfspeed.Meta{IP: %s, ASN: %d, Org: %s, City: %s, Country: %s}"
                         % (go_str(mt["ip"]), mt["asn"], go_str(mt["org"]), go_str(mt["city"]),
                            go_str(mt["country"])))
            lines.append("\t\tmeta.Colo.IATA = %s" % go_str(mt["iata"]))
            lines.append("\t\tmeta.Colo.City = %s" % go_str(mt["colo_city"]))
            lines.append("\t\tm.meta = &meta")
        for p in s["pings"]:
            lines.append("\t\tm.pings = append(m.pings, time.Duration(%d))" % p)
        if s["latency"]:
            lines.append("\t\tm.latency = &cfspeed.Latency{Median: %d, Min: %d, Max: %d, Jitter: %d}"
                         % s["latency"])
        for d in range(2):
            for el, bps in s["samples"][d]:
                lines.append("\t\tm.samples[%d] = append(m.samples[%d], cfspeed.Sample{Elapsed: %d, Bps: %r})"
                             % (d, d, el, float(bps)))
            t = s["transfers"][d]
            if t:
                lines.append("\t\tm.transfers[%d] = &cfspeed.Throughput{Bps: %r, Bytes: %d, Duration: %d, "
                             "Loaded: cfspeed.Latency{Median: %d}}" % (d, float(t[0]), t[1], t[2], t[3]))
        if s["err"]:
            lines.append("\t\tm.err = errors.New(%s)" % go_str(s["err"]))
        lines.append("\t\tm.quitting = %s" % ("true" if s["quitting"] else "false"))
        lines.append("\t\tm.finished = time.Duration(%d)" % s["finished"])
        lines.append("\t\tv := m.View()")
        lines.append("\t\tstate, value := 0, 0")
        lines.append("\t\tif v.ProgressBar != nil {")
        lines.append("\t\t\tstate, value = int(v.ProgressBar.State), v.ProgressBar.Value")
        lines.append("\t\t}")
        lines.append('\t\tout := fmt.Sprintf("progress %d %d\\n%s", state, value, v.Content)')
        lines.append('\t\tif err := os.WriteFile(filepath.Join(dir, %s), []byte(out), 0o644); err != nil {'
                     % go_str(s["name"] + ".ans"))
        lines.append("\t\t\tt.Fatal(err)")
        lines.append("\t\t}")
        lines.append("\t}")
    lines.append("}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def write_c(path):
    out = ["/* Generated by tests/gen_scenarios.py. Do not edit. */", ""]
    for i, s in enumerate(SCENARIOS):
        if s["pings"]:
            out.append("static const int64_t pings_%d[] = {%s};" % (i, ", ".join(map(str, s["pings"]))))
        for d in range(2):
            if s["samples"][d]:
                out.append("static const sample_t samples_%d_%d[] = {%s};" % (
                    i, d, ", ".join("{%d, %r}" % (el, float(b)) for el, b in s["samples"][d])))
    out.append("")
    out.append("static const scenario_t scenarios[] = {")
    for i, s in enumerate(SCENARIOS):
        mt = s["meta"] or {}
        t = [x or (0.0, 0, 0, 0) for x in s["transfers"]]
        lat_ = s["latency"] or (0, 0, 0, 0)
        out.append("\t{")
        out.append("\t\t.name = %s, .width = %d, .dark = %d, .phase = %d, .quitting = %d, .spinner = %d,"
                   % (c_str(s["name"]), s["width"], s["dark"], PHASES[s["phase"]], s["quitting"],
                      s["spinner"]))
        out.append("\t\t.finished = %d, .cfg = {%d, %d, %d}," % (s["finished"], s["duration"], s["streams"],
                                                              s["pings_cfg"]))
        if s["meta"]:
            out.append("\t\t.has_meta = 1, .meta = {.ip = %s, .org = %s, .city = %s, .country = %s,"
                       % tuple(c_str(mt[k]) for k in ("ip", "org", "city", "country")))
            out.append("\t\t\t.colo_iata = %s, .colo_city = %s, .asn = %d},"
                       % (c_str(mt["iata"]), c_str(mt["colo_city"]), mt["asn"]))
        if s["pings"]:
            out.append("\t\t.pings = pings_%d, .npings = %d," % (i, len(s["pings"])))
        if s["latency"]:
            out.append("\t\t.has_latency = 1, .latency = {%d, %d, %d, %d}," % lat_)
        for d in range(2):
            if s["samples"][d]:
                out.append("\t\t.samples[%d] = samples_%d_%d, .nsamples[%d] = %d," % (d, i, d, d,
                                                                                    len(s["samples"][d])))
            if s["transfers"][d]:
                out.append("\t\t.has_transfer[%d] = 1, .transfer[%d] = {%r, %d, %d, {%d, 0, 0, 0}},"
                           % (d, d, float(t[d][0]), t[d][1], t[d][2], t[d][3]))
        if s["err"]:
            out.append("\t\t.err = %s," % c_str(s["err"]))
        out.append("\t},")
    out.append("};")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    write_go(sys.argv[1] + "/golden_test.go")
    write_c("tests/scenarios.inc")
