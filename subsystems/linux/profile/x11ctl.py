#!/usr/bin/env python3
"""Drive an X server for the L0 reference runs, without Xlib.

    x11ctl.py SOCKET warp X Y       move the pointer to root coordinates
    x11ctl.py SOCKET pointer        print the pointer's root coordinates
    x11ctl.py SOCKET delete WID     send WM_DELETE_WINDOW to a top-level window

It speaks the core protocol directly on the server's Unix socket, so the
connection it makes never appears in the client's trace.
"""

import socket
import struct
import sys


class X:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.s.sendall(struct.pack("<BxHHHHxx", ord("l"), 11, 0, 0, 0))
        head = self.recv(8)
        if head[0] != 1:
            raise SystemExit("x11ctl: connection refused")
        body = self.recv(struct.unpack_from("<H", head, 6)[0] * 4)
        vlen = struct.unpack_from("<H", body, 16)[0]
        nfmt = body[21]
        screen = 32 + vlen + (-vlen % 4) + 8 * nfmt
        self.root = struct.unpack_from("<I", body, screen)[0]

    def recv(self, n):
        data = b""
        while len(data) < n:
            chunk = self.s.recv(n - len(data))
            if not chunk:
                raise SystemExit("x11ctl: connection closed")
            data += chunk
        return data

    def reply(self):
        while True:
            head = self.recv(32)
            if head[0] == 1:
                return head + self.recv(struct.unpack_from("<I", head, 4)[0] * 4)
            if head[0] == 0:
                raise SystemExit(f"x11ctl: X error {head[1]} for request {head[10]}")

    def sync(self):
        self.s.sendall(struct.pack("<BxH", 43, 1))  # GetInputFocus
        self.reply()

    def atom(self, name):
        n = name.encode()
        self.s.sendall(struct.pack("<BBHHxx", 16, 0, 2 + (len(n) + 3) // 4, len(n)) + n + b"\0" * (-len(n) % 4))
        return struct.unpack_from("<I", self.reply(), 8)[0]


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    x = X(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "warp":
        px, py = int(sys.argv[3]), int(sys.argv[4])
        x.s.sendall(struct.pack("<BxHIIhhHHhh", 41, 6, 0, x.root, 0, 0, 0, 0, px, py))
        x.sync()
    elif cmd == "pointer":
        x.s.sendall(struct.pack("<BxHI", 38, 2, x.root))
        r = x.reply()
        print(*struct.unpack_from("<hh", r, 16))
    elif cmd == "delete":
        wid = int(sys.argv[3], 0)
        protocols = x.atom("WM_PROTOCOLS")
        delete = x.atom("WM_DELETE_WINDOW")
        event = struct.pack("<BBHII", 33, 32, 0, wid, protocols) + struct.pack("<5I", delete, 0, 0, 0, 0)
        x.s.sendall(struct.pack("<BBHII", 25, 0, 11, wid, 0) + event)  # SendEvent
        x.sync()
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
