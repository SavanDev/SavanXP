#!/usr/bin/env python3
"""Small deterministic TCP echo server used by the tcp and native-network smokes.

It also starts the DNS fixture of tools/dns_fixture.py and the HTTP fixture
of tools/http_fixture.py; the DNS, DNSCOUNT and HTTP commands report their
ports and how often a name was asked."""

from __future__ import annotations

import argparse
import os
import signal
import socket
import struct
import time
import threading
from pathlib import Path

from dns_fixture import DnsFixture
from http_fixture import HttpFixture

# The resolver's fixture (tools/dns_fixture.py), started with the server: the
# guest asks DNS for its port and DNSCOUNT <name> for the queries it received.
DNS: DnsFixture
# The HTTP loader's fixture (tools/http_fixture.py): HTTP gives its port.
HTTP: HttpFixture

SAVED: dict[str, bytes] = {}
SAVED_LOCK = threading.Lock()

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


def handle_client_commands(sock: socket.socket, refused_port: int) -> None:
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
            if command == "RESET" and len(parts) == 1:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                return
            elif command == "HOLD" and len(parts) == 2:
                delay_ms = int(parts[1])
                if not 0 <= delay_ms <= 120000:
                    return
                sock.sendall(b"READY\n")
                time.sleep(delay_ms / 1000.0)
                return
            elif command == "DNS" and len(parts) == 1:
                sock.sendall(f"{DNS.port}\n".encode("ascii"))
            elif command == "HTTP" and len(parts) == 1:
                sock.sendall(f"{HTTP.port}\n".encode("ascii"))
            elif command == "DNSCOUNT" and len(parts) == 2:
                sock.sendall(f"{DNS.zone.count(parts[1])}\n".encode("ascii"))
            elif command == "REFUSE" and len(parts) == 1:
                sock.sendall(f"{refused_port}\n".encode("ascii"))
            elif command == "BULK" and len(parts) == 2:
                count = int(parts[1])
                sent = 0
                while sent < count:
                    chunk = pattern_bytes(sent, min(4096, count - sent))
                    sock.sendall(chunk)
                    sent += len(chunk)
            elif command == "SAVED" and len(parts) == 2:
                with SAVED_LOCK:
                    saved = SAVED.pop(parts[1], b"MISSING")
                sock.sendall(saved)
            elif (command == "ECHO" and len(parts) == 2) or (command == "SAVE" and len(parts) == 3):
                count = int(parts[-1])
                saved_bytes = bytearray()
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
                    if command == "SAVE":
                        if len(saved_bytes) + len(chunk) > 1024:
                            return
                        saved_bytes.extend(chunk)
                    else:
                        sock.sendall(chunk)
                    received += len(chunk)
                if command == "SAVE":
                    with SAVED_LOCK:
                        if len(SAVED) >= 32:
                            return
                        SAVED[parts[1]] = bytes(saved_bytes)
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


def handle_client(sock: socket.socket) -> None:
    # Reserve a TCP port without listening: connects receive a deterministic
    # refusal while this control connection keeps the reservation alive.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as refused:
        refused.bind(("127.0.0.1", 0))
        handle_client_commands(sock, refused.getsockname()[1])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--port-file", type=Path)
    parser.add_argument("--idle-timeout", type=float, default=0.0)
    args = parser.parse_args()

    global DNS, HTTP
    DNS = DnsFixture().start()
    HTTP = HttpFixture().start()

    stop = False

    def request_stop(_signum: int, _frame: object) -> None:
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", args.port))
    server.listen(32)
    slots = threading.BoundedSemaphore(32)
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
            if not slots.acquire(blocking=False):
                client.close()
                continue
            def serve(connection: socket.socket) -> None:
                try:
                    with connection:
                        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                        try:
                            handle_client(connection)
                        except (OSError, ConnectionError):
                            pass
                finally:
                    slots.release()
            threading.Thread(target=serve, args=(client,), daemon=True).start()
            started = time.monotonic()
    finally:
        server.close()
        if args.port_file:
            args.port_file.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
