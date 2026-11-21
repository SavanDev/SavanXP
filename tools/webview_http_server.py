#!/usr/bin/env python3
"""Tiny HTTP fixture for the Web Viewer visual scenario.

Serves a directory of static pages plus one redirect (/old.html -> 302 to
/index.html) and /stall, a download that never finishes: headers, then one
byte every two seconds for a minute, so the viewer's idle budget never runs
out. It is the page Stop and background loading are tried on. Three more
answer the N4 steps: /chunked.html, a page sent chunked in small pieces with
pauses; /contradictory, a Content-Length and chunking together; and
/truncated, a Content-Length the body never reaches before the close.
Each request has its own thread, so a stalled one does not hold the others. Binds an ephemeral port on loopback and prints READY <port>: the
guest reaches the host at 10.0.2.2 through QEMU slirp, so the scenario types
http://10.0.2.2:<port>/... into the address bar.

Only the standard library, quiet by default: request logging would interleave
with the READY line the parent parses.
"""

import argparse
import os
import sys
import time
from functools import partial
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    server_version = "WebViewFixture/0.3"

    def log_message(self, *args):
        pass

    def _send_page(self, name):
        path = os.path.join(self.directory, name)
        try:
            with open(path, "rb") as stream:
                body = stream.read()
        except OSError:
            self.send_response(404)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        # The viewer under test asks HTTP/1.0; answer 1.0 with close so no
        # chunked body is ever needed.
        target = self.path.split("?", 1)[0]
        if target == "/chunked.html":
            # Status line, headers, chunk sizes and CRLFs all split across
            # writes, so the page only renders if the client reassembles.
            page = b"<html><head><title>Chunked</title></head><body>" \
                   b"<h1>Chunked page</h1><p>Sent in pieces.</p></body></html>"
            data = b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n" \
                   b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
            for start in range(0, len(page), 17):
                piece = page[start:start + 17]
                data += b"%x\r\n" % len(piece) + piece + b"\r\n"
            data += b"0\r\n\r\n"
            try:
                for start in range(0, len(data), 11):
                    self.wfile.write(data[start:start + 11])
                    self.wfile.flush()
                    time.sleep(0.01)
            except OSError:
                pass
            return
        if target == "/contradictory":
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                             b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                             b"5\r\nhello\r\n0\r\n\r\n")
            return
        if target == "/truncated":
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 500\r\n"
                             b"Connection: close\r\n\r\n<h1>Half a page</h1>")
            return
        if target == "/stall":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                for _ in range(30):
                    self.wfile.write(b" ")
                    self.wfile.flush()
                    time.sleep(2)
            except OSError:
                pass
            return
        if target == "/old.html":
            self.send_response(302)
            self.send_header("Location", "/index.html")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        if target == "/" or target == "":
            target = "/index.html"
        self._send_page(target.lstrip("/"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", required=True)
    parser.add_argument("--bind", default="127.0.0.1")
    args = parser.parse_args()
    handler = partial(Handler)
    Handler.directory = args.dir
    server = ThreadingHTTPServer((args.bind, 0), handler)
    server.daemon_threads = True
    port = server.server_address[1]
    print("READY %d" % port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
