#!/usr/bin/env python3
"""Minimal deterministic SNTP fixture used by the Linux ntp smoke.

Answers every datagram of >= 48 bytes with a fixed-shape server reply
carrying the host's current time: caller's transmit stamp echoed as origin,
receive and transmit set to now. Version echoes the request's.
"""

from __future__ import annotations

import argparse
import os
import signal
import socket
import struct
import time
from pathlib import Path


NTP_UNIX_OFFSET = 2208988800


def build_reply(request: bytes, now: float) -> bytes | None:
    if len(request) < 48:
        return None
    version = (request[0] >> 3) & 0x07
    ntp = now + NTP_UNIX_OFFSET
    seconds = int(ntp)
    fraction = int((ntp - seconds) * 2**32)
    reply = bytearray(48)
    reply[0] = ((version & 0x07) << 3) | 0x04
    reply[1] = 1
    reply[2] = 6
    reply[3] = 0xEC
    struct.pack_into(">II", reply, 16, seconds, fraction)
    reply[24:32] = request[40:48]
    struct.pack_into(">II", reply, 32, seconds, fraction)
    struct.pack_into(">II", reply, 40, seconds, fraction)
    return bytes(reply)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--port-file", type=Path)
    args = parser.parse_args()

    stop = False

    def request_stop(_signum: int, _frame: object) -> None:
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)

    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind(("127.0.0.1", args.port))
    server.settimeout(1.0)
    port = server.getsockname()[1]
    if args.port_file:
        args.port_file.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.port_file.with_name(args.port_file.name + ".tmp")
        temporary.write_text(str(port) + "\n", encoding="ascii")
        os.replace(temporary, args.port_file)
    print(f"ntp-server ready port={port}", flush=True)

    try:
        while not stop:
            try:
                data, address = server.recvfrom(1024)
            except socket.timeout:
                continue
            reply = build_reply(data, time.time())
            if reply is not None:
                try:
                    server.sendto(reply, address)
                except OSError:
                    pass
    finally:
        server.close()
        if args.port_file:
            args.port_file.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
