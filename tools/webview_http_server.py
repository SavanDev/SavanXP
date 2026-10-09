#!/usr/bin/env python3
"""Tiny HTTP fixture for the Web Viewer visual scenario.

Serves a directory of static pages plus one redirect (/old.html -> 302 to
/index.html). Binds an ephemeral port on loopback and prints READY <port>: the
guest reaches the host at 10.0.2.2 through QEMU slirp, so the scenario types
http://10.0.2.2:<port>/... into the address bar.

Only the standard library, quiet by default: request logging would interleave
with the READY line the parent parses.
"""

import argparse
import os
import sys
from functools import partial
from http.server import BaseHTTPRequestHandler, HTTPServer


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
    server = HTTPServer((args.bind, 0), handler)
    port = server.server_address[1]
    print("READY %d" % port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
