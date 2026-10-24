# flarespeed

A terminal speed test against Cloudflare's network (speed.cloudflare.com), with a live inline UI. Written in C11, with libcurl as its only dependency.

It measures:

- **Ping**: median idle latency and jitter, from 20 samples on a warm connection.
- **Download / upload**: steady-state throughput over parallel connections, after skipping the TCP ramp-up.
- **Loaded latency**: ping measured while the link is saturated. If it's much higher than idle ping, your connection has bufferbloat.

## Building

You need a C compiler, `make`, `pkg-config` and the libcurl development files (`libcurl4-openssl-dev` on Debian and Ubuntu, `curl` on Arch, `libcurl-devel` on Fedora).

```sh
make
sudo make install        # to /usr/local; PREFIX and DESTDIR work as usual
```

## Usage

```sh
flarespeed
flarespeed -json | jq .download.mbps
```

| Flag | Default | |
|---|---|---|
| `-duration` | `10s` | length of each of the download and upload tests (Go duration syntax: `5s`, `1m30s`, …) |
| `-streams` | `6` | parallel connections per direction |
| `-pings` | `20` | number of idle latency samples |
| `-json` | off | print results as JSON; this is automatic when stdout isn't a terminal |

Keys: `r` runs the test again, `q` (or Esc, or Ctrl+C) quits.

The UI adapts to light and dark terminal backgrounds and to truecolor, 256-color and 16-color terminals, and honours `NO_COLOR`. See `man flarespeed` for the details, the JSON fields and the environment variables.

## Method

Latency is the time to first byte of an empty download, minus the server processing time reported in `Server-Timing`, the same way Cloudflare's own client computes it. Each throughput stream sends 25 MB requests back to back over its own HTTP/1.1 connection. HTTP/2 is turned off on purpose, because it would put every stream on one TCP connection. The reported rate leaves out the first 20% of the test (at most 2 s).

## Tests

```sh
make check               # unit tests, including view parity
make check-integration   # end-to-end runs against a local mock server (needs python3)
make fuzz                # libFuzzer targets in tests/fuzz (needs clang)
```

flarespeed started as a Go program built on Charm's Bubble Tea, and the C version reproduces its output exactly. `tests/golden` holds frames rendered by the original for 43 scenarios, and `make check` compares the C rendering with them cell by cell: characters, colors and attributes.

The vectors in `tests/data` were generated from the same Go libraries. They cover colour blending and downsampling, text width, wrapping and truncation, JSON, durations and number formatting. The Go program lives in `tools/reference`, and `tools/README.md` explains how to regenerate each file.
