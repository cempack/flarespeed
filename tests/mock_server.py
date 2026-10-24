#!/usr/bin/env python3
"""A stand-in for speed.cloudflare.com, for integration tests.

Serves /meta, /__down?bytes=N and /__up over plain HTTP/1.1 on 127.0.0.1,
with switchable misbehaviour. Prints the port it listens on, then serves
until killed.
"""
import argparse
import http.server
import json
import socketserver
import sys
import time

META = {
    "hostname": "speed.cloudflare.com", "clientIp": "192.0.2.7", "asn": 64500,
    "asOrganization": "Example <Net> & Co", "country": "FR", "city": "Île-de-France",
    "colo": {"iata": "CDG", "city": "Paris"},
}
CHUNK = b"\0" * (256 * 1024)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, code, body=b"", headers=()):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        opts = self.server.opts
        path, _, query = self.path.partition("?")
        if path == "/meta":
            if opts.meta == "500":
                return self.reply(500, b"oops")
            body = b"{not json" if opts.meta == "bad" else json.dumps(META).encode()
            return self.reply(200, body)
        if path != "/__down":
            return self.reply(404)
        size = int(query.partition("=")[2] or 0)
        if size == 0:
            if opts.ping == "503":
                return self.reply(503)
            if opts.ping == "hang":
                time.sleep(3600)
            time.sleep(opts.ping_delay / 1000)
            return self.reply(200, headers=[("Server-Timing",
                                             f"cfSpeedEdge;dur={opts.ping_delay / 2}, "
                                             f"cfSpeedWorker;dur={opts.ping_delay / 2}")])
        if opts.down == "429":
            return self.reply(429)
        self.send_response(200)
        self.send_header("Content-Length", str(size))
        self.end_headers()
        sent, start = 0, time.monotonic()
        piece = len(CHUNK) if not opts.rate else max(256, int(opts.rate / 50))
        while sent < size:
            n = min(piece, size - sent)
            if opts.down == "drop" and sent > size // 2:
                self.close_connection = True
                return  # cut the response short
            self.wfile.write(CHUNK[:n])
            sent += n
            if opts.rate:
                ahead = sent / opts.rate - (time.monotonic() - start)
                if ahead > 0:
                    time.sleep(ahead)

    def do_POST(self):
        opts = self.server.opts
        length = int(self.headers.get("Content-Length", 0))
        if opts.up == "403":
            self.close_connection = True
            return self.reply(403)
        left = length
        while left > 0:
            data = self.rfile.read(min(left, 1 << 20))
            if not data:
                return
            left -= len(data)
        self.reply(200, b"ok")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass  # clients abort transfers at the end of every test phase


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--meta", default="ok", choices=["ok", "500", "bad"])
    p.add_argument("--ping", default="ok", choices=["ok", "503", "hang"])
    p.add_argument("--down", default="ok", choices=["ok", "429", "drop"])
    p.add_argument("--up", default="ok", choices=["ok", "403"])
    p.add_argument("--rate", type=float, default=0, help="download bytes per second per stream")
    p.add_argument("--ping-delay", type=float, default=20, help="server processing time in ms")
    opts = p.parse_args()
    srv = Server(("127.0.0.1", 0), Handler)
    srv.opts = opts
    print(srv.server_address[1], flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
