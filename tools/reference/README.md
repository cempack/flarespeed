# speedtest

A terminal speed test against Cloudflare's network (speed.cloudflare.com), with a live UI built on [Charm](https://charm.land)'s Bubble Tea, Lip Gloss and Bubbles.

It measures:

- **Ping**: median idle latency and jitter, from 20 samples on a warm connection.
- **Download / upload**: steady-state throughput over parallel connections, after skipping the TCP ramp-up.
- **Loaded latency**: ping measured while the link is saturated. If it's much higher than idle ping, your connection has bufferbloat.

## Usage

```sh
go build -o speedtest .
./speedtest
```

| Flag | Default | |
|---|---|---|
| `-duration` | `10s` | length of each of the download and upload tests |
| `-streams` | `6` | parallel connections per direction |
| `-pings` | `20` | number of idle latency samples |
| `-json` | off | print results as JSON; this is automatic when stdout isn't a terminal |

Keys: `r` runs the test again, `q` quits.

```sh
./speedtest -json | jq .download.mbps
```

## Method

Latency is time-to-first-byte of an empty download minus the server processing time reported in `Server-Timing`, the same way Cloudflare's own client computes it. Each throughput stream issues 25 MB requests back to back over its own HTTP/1.1 connection. HTTP/2 is turned off on purpose, because it would put every stream on one TCP connection. The reported rate excludes the first 20% of the test (at most 2 s).
