#!/usr/bin/env python3
"""Linux reference for the same static musl Unix-stream fixture used in QEMU."""
from __future__ import annotations
import argparse
from pathlib import Path
import socket
import subprocess
import tempfile
import threading


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("fixture", type=Path)
    args = parser.parse_args()
    if not args.fixture.exists():
        print("SKIP: pinned musl transport fixture is not installed")
        return 77
    with tempfile.TemporaryDirectory(prefix="ssl-stream-") as temporary:
        path = str(Path(temporary) / "X1")
        errors = []
        with socket.socket(socket.AF_UNIX) as listener:
            listener.bind(path)
            listener.listen(1)
            listener.settimeout(10)

            def serve() -> None:
                try:
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(10)
                        assert connection.recv(1) == b"N"
                        connection.sendall(b"abcdEFGH")
                        assert connection.recv(1) == b""
                        connection.sendall(b"Z")
                except BaseException as error:
                    errors.append(error)

            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            result = subprocess.run([str(args.fixture.resolve()), path], capture_output=True, timeout=15)
            thread.join(timeout=10)
            assert not thread.is_alive(), "server did not end"
            assert not errors, errors
            assert result.returncode == 0 and result.stdout == b"transport ok\n" and not result.stderr, result
            print("LINUX TRANSPORT PASS: nonblocking I/O, vectors, poll, bad pointers, half-close and EOF")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
