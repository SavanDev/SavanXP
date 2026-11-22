#!/usr/bin/env python3
"""A deterministic HTTP server for the guest's loader tests (sxn/sxnhttp.h).

Raw sockets rather than http.server: half of what it serves is wrong on
purpose, and every response goes out in small pieces with pauses between
them, splitting the status line, the headers, chunk-size lines and CRLFs, so
the client's parser meets real TCP fragmentation. One path per case:

  /length            Content-Length body "length body"
  /chunked           chunked "chunked body", with an extension and a trailer
  /close             HTTP/1.0, no length: the body ends at the close
  /echo              the request head the client sent, as the body
  /type              Content-Type text/plain; charset=utf-8
  /interim           100 Continue before the real response
  /notfound          404 with a body
  /big               40000 bytes with a Content-Length
  /both              Content-Length and chunked together
  /twolengths        two Content-Lengths that disagree
  /short             Content-Length 100, ten bytes, then the close
  /chunk-short       a chunk cut by the close
  /reset             Content-Length 100, ten bytes, then a reset
  /badchunk          a chunk size that is not hex
  /bigheader         17 KiB of headers
  /gzip              Content-Encoding gzip
  /redirect-relative 302 to "length?from=redirect"
  /redirect-chain/N  302 to /redirect-chain/N-1; /redirect-chain/0 is a page
  /redirect-file     302 to file:///system/samples/welcome.html
  /redirect-https    302 to https://test.savanxp/
  /redirect-none     302 without a Location
  /trickle           headers, then a byte every half second for a minute
  /silent            reads the request and answers nothing for a minute
  /tls-root.pem      the TLS fixture's test root, when one is running

Only the standard library.
"""

from __future__ import annotations

import socket
import struct
import threading
import time

PAUSE = 0.03


def pieces(data: bytes, cuts: list[int]) -> list[bytes]:
    """`data` cut at the given offsets."""
    out = []
    start = 0
    for cut in cuts:
        if start < cut < len(data):
            out.append(data[start:cut])
            start = cut
    out.append(data[start:])
    return out


def head(status: str, headers: list[str], version: str = "HTTP/1.1") -> bytes:
    return (version + " " + status + "\r\n" + "".join(h + "\r\n" for h in headers) + "\r\n").encode("ascii")


class HttpFixture:
    def __init__(self, bind: str = "127.0.0.1") -> None:
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind((bind, 0))
        self.server.listen(32)
        self.port = self.server.getsockname()[1]
        # tools/tls_fixture.py's root, set by the server that starts both.
        self.tls_root: bytes = b""

    def start(self) -> "HttpFixture":
        threading.Thread(target=self._serve, daemon=True).start()
        return self

    def _serve(self) -> None:
        while True:
            try:
                connection, _ = self.server.accept()
            except OSError:
                return
            threading.Thread(target=self._handle, args=(connection,), daemon=True).start()

    def _handle(self, connection: socket.socket) -> None:
        with connection:
            connection.settimeout(70.0)
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            try:
                request = b""
                while b"\r\n\r\n" not in request and len(request) < 8192:
                    chunk = connection.recv(1024)
                    if not chunk:
                        return
                    request += chunk
                line = request.split(b"\r\n", 1)[0].decode("ascii", "replace")
                parts = line.split()
                if len(parts) != 3 or parts[0] != "GET":
                    return
                path = parts[1].split("?", 1)[0]
                self._respond(connection, path, request.split(b"\r\n\r\n", 1)[0] + b"\r\n\r\n")
            except (OSError, ValueError):
                return

    def _send(self, connection: socket.socket, parts: list[bytes], pause: float = PAUSE) -> None:
        for part in parts:
            connection.sendall(part)
            time.sleep(pause)

    def _respond(self, connection: socket.socket, path: str, request: bytes) -> None:
        send = lambda data, cuts=(1, 7, 13, 20, 33, 47, 61): self._send(connection, pieces(data, list(cuts)))
        if path == "/tls-root.pem" and self.tls_root:
            self._send(connection, [head("200 OK", ["Content-Type: application/x-pem-file",
                                                    "Content-Length: %d" % len(self.tls_root)]) + self.tls_root])
        elif path == "/length":
            send(head("200 OK", ["Content-Type: text/html", "Content-Length: 11"]) + b"length body")
        elif path == "/chunked":
            body = (b"8;ext=1\r\nchunked \r\n4\r\nbody\r\n0\r\nX-Trailer: done\r\n\r\n")
            data = head("200 OK", ["Content-Type: text/html", "Transfer-Encoding: chunked"]) + body
            split = len(data) - len(body)
            send(data, (2, 9, 30, split + 1, split + 4, split + 10, split + 12, split + 20, split + 26))
        elif path == "/close":
            send(head("200 OK", ["Content-Type: text/html"], "HTTP/1.0") + b"close-delimited body")
        elif path == "/echo":
            send(head("200 OK", ["Content-Length: %d" % len(request)]) + request)
        elif path == "/type":
            send(head("200 OK", ["Content-Type: text/plain; charset=utf-8", "Content-Length: 5"]) + b"plain")
        elif path == "/interim":
            send(head("100 Continue", []) + head("200 OK", ["Content-Length: 7"]) + b"interim")
        elif path == "/notfound":
            send(head("404 Not Found", ["Content-Length: 7"]) + b"missing")
        elif path == "/big":
            body = bytes((index * 31 + 7) & 0xFF for index in range(40000))
            connection.sendall(head("200 OK", ["Content-Length: %d" % len(body)]) + body)
        elif path == "/both":
            send(head("200 OK", ["Content-Length: 5", "Transfer-Encoding: chunked"]) + b"5\r\nhello\r\n0\r\n\r\n")
        elif path == "/twolengths":
            send(head("200 OK", ["Content-Length: 5", "Content-Length: 6"]) + b"hello!")
        elif path == "/short":
            send(head("200 OK", ["Content-Length: 100"]) + b"0123456789")
        elif path == "/chunk-short":
            send(head("200 OK", ["Transfer-Encoding: chunked"]) + b"5\r\nabc")
        elif path == "/reset":
            send(head("200 OK", ["Content-Length: 100"]) + b"0123456789")
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        elif path == "/badchunk":
            send(head("200 OK", ["Transfer-Encoding: chunked"]) + b"zz\r\nhello\r\n0\r\n\r\n")
        elif path == "/bigheader":
            pad = ["X-Pad-%02d: %s" % (index, "p" * 1000) for index in range(17)]
            connection.sendall(head("200 OK", pad + ["Content-Length: 2"]) + b"ok")
        elif path == "/gzip":
            send(head("200 OK", ["Content-Encoding: gzip", "Content-Length: 4"]) + b"\x1f\x8b\x08\x00")
        elif path == "/redirect-relative":
            send(head("302 Found", ["Location: length?from=redirect", "Content-Length: 0"]))
        elif path.startswith("/redirect-chain/"):
            try:
                count = int(path.rsplit("/", 1)[1])
            except ValueError:
                return
            if count <= 0:
                send(head("200 OK", ["Content-Length: 9"]) + b"chain end")
            else:
                send(head("302 Found", ["Location: /redirect-chain/%d" % (count - 1), "Content-Length: 0"]))
        elif path == "/redirect-file":
            send(head("302 Found", ["Location: file:///system/samples/welcome.html", "Content-Length: 0"]))
        elif path == "/redirect-https":
            send(head("301 Moved Permanently", ["Location: https://test.savanxp/", "Content-Length: 0"]))
        elif path == "/redirect-none":
            send(head("302 Found", ["Content-Length: 0"]))
        elif path == "/trickle":
            connection.sendall(head("200 OK", ["Content-Length: 1000"]))
            for _ in range(120):
                connection.sendall(b".")
                time.sleep(0.5)
        elif path == "/silent":
            time.sleep(60)
        else:
            send(head("404 Not Found", ["Content-Length: 0"]))
