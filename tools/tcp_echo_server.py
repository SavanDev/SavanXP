#!/usr/bin/env python3
"""Small deterministic TCP echo server used by the Linux tcp smoke."""

from __future__ import annotations

import argparse
import os
import signal
import socket
import time
from pathlib import Path


PATTERN_BASE = 7
PATTERN_STEP = 31


def pattern_bytes(start: int, count: int) -> bytes:
    return bytes(((index * PATTERN_STEP + PATTERN_BASE) & 0xFF) for index in range(start, start + count))

def read_line(sock: socket.socket, buffer: bytearray) -> str | None:
    while True:
        marker = buffer.find(b"\n")
        if marker >= 0:
            line = bytes(buffer[:marker])
            del buffer[: marker + 1]
            return line.rstrip(b"\r").decode("ascii", errors="replace")
        chunk = sock.recv(4096)
        if not chunk:
            return None
        buffer.extend(chunk)


def handle_client(sock: socket.socket) -> None:
    sock.settimeout(60.0)
    buffer = bytearray()
    while True:
        line = read_line(sock, buffer)
        if line is None:
            return
        if line == "QUIT":
            return
        parts = line.split()
        if not parts:
            continue
        command = parts[0].upper()
        try:
            if command == "BULK" and len(parts) == 2:
                count = int(parts[1])
                sent = 0
                while sent < count:
                    chunk = pattern_bytes(sent, min(4096, count - sent))
                    sock.sendall(chunk)
                    sent += len(chunk)
            elif command == "ECHO" and len(parts) == 2:
                count = int(parts[1])
                received = 0
                while received < count:
                    take = min(4096, count - received)
                    if buffer:
                        chunk = bytes(buffer[:take])
                        del buffer[:take]
                    else:
                        chunk = sock.recv(take)
                    if not chunk:
                        return
                    sock.sendall(chunk)
                    received += len(chunk)
            elif command == "DELAY" and len(parts) == 3:
                delay_ms = int(parts[1])
                count = int(parts[2])
                time.sleep(max(0, delay_ms) / 1000.0)
                sent = 0
                while sent < count:
                    chunk = pattern_bytes(sent, min(4096, count - sent))
                    sock.sendall(chunk)
                    sent += len(chunk)
            else:
                sock.sendall(b"ERR\n")
        except (ValueError, OSError, ConnectionError):
            return


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--port-file", type=Path)
    parser.add_argument("--idle-timeout", type=float, default=0.0)
    args = parser.parse_args()

    stop = False

    def request_stop(_signum: int, _frame: object) -> None:
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", args.port))
    server.listen(1)
    server.settimeout(1.0)
    port = server.getsockname()[1]
    if args.port_file:
        args.port_file.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.port_file.with_name(args.port_file.name + ".tmp")
        temporary.write_text(str(port) + "\n", encoding="ascii")
        os.replace(temporary, args.port_file)
    print(f"tcp-echo-server ready port={port}", flush=True)

    started = time.monotonic()
    try:
        while not stop:
            if args.idle_timeout > 0 and time.monotonic() - started > args.idle_timeout:
                break
            try:
                client, _address = server.accept()
            except socket.timeout:
                continue
            with client:
                client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                try:
                    handle_client(client)
                except (OSError, ConnectionError):
                    pass
            started = time.monotonic()
    finally:
        server.close()
        if args.port_file:
            args.port_file.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
