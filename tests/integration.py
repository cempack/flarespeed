#!/usr/bin/env python3
"""End-to-end tests of the flarespeed binary against tests/mock_server.py.

    tests/integration.py ./flarespeed [wrapper...]

A wrapper (for example "valgrind --error-exitcode=99 --leak-check=full")
runs every invocation under it.
"""
import json
import os
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.abspath(sys.argv[1])
WRAP = sys.argv[2:]
failures = 0


def server(*args):
    p = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "mock_server.py"), *args],
                         stdout=subprocess.PIPE, text=True)
    port = int(p.stdout.readline())
    return p, f"http://127.0.0.1:{port}"


def run(base, *args, timeout=120, interrupt_after=None):
    env = dict(os.environ, FLARESPEED_BASE_URL=base)
    env.pop("HTTPS_PROXY", None)
    env.pop("https_proxy", None)
    env.pop("http_proxy", None)
    p = subprocess.Popen(WRAP + [BIN, "-json", *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, env=env)
    if interrupt_after is not None:
        time.sleep(interrupt_after)
        p.send_signal(signal.SIGINT)
    out, err = p.communicate(timeout=timeout)
    return p.returncode, out, err


def check(name, cond, detail=""):
    global failures
    if cond:
        print(f"ok   {name}")
    else:
        failures += 1
        print(f"FAIL {name}: {detail}")


def case(name, server_args, flags, fn, **kw):
    srv, base = server(*server_args)
    try:
        code, out, err = run(base, *flags, **kw)
        fn(name, code, out, err, base)
    finally:
        srv.kill()
        srv.wait()


def normal(name, code, out, err, base):
    check(name + " exit", code == 0, f"code {code}, stderr {err!r}")
    try:
        r = json.loads(out)
    except ValueError:
        return check(name + " json", False, out)
    check(name + " meta", r.get("server") == {"colo": "CDG", "city": "Paris"} and
          r["client"]["org"] == "Example <Net> & Co" and r["client"]["asn"] == 64500, r)
    check(name + " html escaping", "\\u003cNet\\u003e \\u0026 Co" in out, out)
    lat = r["latency"]
    # The mock server takes 20 ms and says so in Server-Timing; on loopback
    # only the mock's own overhead should be left once that is subtracted.
    check(name + " server time subtracted", 0 <= lat["median_ms"] < 15, lat)
    check(name + " latency ordered", lat["min_ms"] <= lat["median_ms"] <= lat["max_ms"], lat)
    for d in ("download", "upload"):
        check(f"{name} {d} rate", r[d]["mbps"] > 1 and r[d]["bytes"] > 0, r[d])
        check(f"{name} {d} loaded latency", r[d]["loaded_latency_ms"] >= 0, r[d])


def no_meta(name, code, out, err, base):
    check(name + " exit", code == 0, err)
    r = json.loads(out) if code == 0 else {}
    check(name + " no meta fields", "server" not in r and "client" not in r, out)


def fails_with(expected):
    def fn(name, code, out, err, base):
        msg = err.strip().replace(base, "BASE")
        check(name, code == 1 and msg.startswith("flarespeed: " + expected) and out == "", f"{code} {msg!r}")
    return fn


case("normal run", [], ["-duration", "2s", "-pings", "5"], normal)
case("meta 500", ["--meta", "500"], ["-duration", "2s", "-pings", "3"], no_meta)
case("meta invalid", ["--meta", "bad"], ["-duration", "2s", "-pings", "3"], no_meta)
case("ping 503", ["--ping", "503"], [], fails_with("latency: ping: server responded 503 Service Unavailable"))
case("download 429", ["--down", "429"], ["-duration", "2s", "-pings", "2"],
     fails_with("download: server responded 429 Too Many Requests"))
case("upload 403", ["--up", "403"], ["-duration", "2s", "-pings", "2"],
     fails_with("upload: server responded 403 Forbidden"))
case("dropped connections", ["--down", "drop"], ["-duration", "2s", "-pings", "2"], normal)
case("slow link", ["--rate", "20000"], ["-duration", "3s", "-pings", "2", "-streams", "2"],
     lambda name, code, out, err, base: check(
         name, code == 0 and 0.2 < json.loads(out)["download"]["mbps"] < 0.5, out + err))
case("interrupted while measuring latency", ["--ping-delay", "400"], ["-pings", "50"],
     fails_with('latency: Get "BASE/__down?bytes=0": context canceled'), interrupt_after=1.0)
case("interrupted while downloading", [], ["-duration", "20s", "-pings", "2"],
     fails_with("download: context canceled"), interrupt_after=1.5)

srv, base = server()
srv.kill()
srv.wait()
code, out, err = run(base)
check("connection refused", code == 1 and err.startswith(f'flarespeed: latency: Get "{base}/__down?bytes=0": '),
      err)

print(f"{failures} failures")
sys.exit(1 if failures else 0)
