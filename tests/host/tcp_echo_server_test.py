#!/usr/bin/env python3
"""Protocol test for the Linux TCP smoke helper."""

from __future__ import annotations

import argparse
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def pattern(count: int) -> bytes:
    return bytes((index * 31 + 7) & 0xFF for index in range(count))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="savanxp-tcp-") as directory:
        root = Path(directory)
        port_file = root / "port"
        log_file = root / "server.log"
        with log_file.open("wb") as log:
            process = subprocess.Popen(
                [str(args.server), "--port", "0", "--port-file", str(port_file)],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                deadline = time.monotonic() + 5.0
                while time.monotonic() < deadline and not port_file.is_file():
                    if process.poll() is not None:
                        raise RuntimeError("TCP echo server exited before publishing a port")
                    time.sleep(0.02)
                if not port_file.is_file():
                    raise RuntimeError("TCP echo server did not publish a port")
                port = int(port_file.read_text(encoding="ascii").strip())
                with socket.create_connection(("127.0.0.1", port), timeout=5.0) as client:
                    client.settimeout(5.0)
                    client.sendall(b"BULK 5000\n")
                    received = b""
                    while len(received) < 5000:
                        received += client.recv(5000 - len(received))
                    if received != pattern(5000):
                        raise AssertionError("BULK pattern mismatch")

                    payload = pattern(9000)
                    client.sendall(b"ECHO 9000\n" + payload)
                    echoed = b""
                    while len(echoed) < 9000:
                        echoed += client.recv(9000 - len(echoed))
                    if echoed != payload:
                        raise AssertionError("ECHO payload mismatch")

                    client.sendall(b"DELAY 10 300\n")
                    delayed = b""
                    while len(delayed) < 300:
                        delayed += client.recv(300 - len(delayed))
                    if delayed != pattern(300):
                        raise AssertionError("DELAY pattern mismatch")
            finally:
                process.terminate()
                process.wait(timeout=5.0)
    print("TCP echo server: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
