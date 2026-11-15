#!/usr/bin/env python3
"""Run the pinned, unchanged musl Xeyes over real TCP into the engine."""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import time
from PIL import Image

REPO = Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()
    interpreter = args.root / "lib/ld-musl-x86_64.so.1"
    if not interpreter.exists() or not args.fixture.exists():
        print("SKIP: pinned Alpine musl/Xlib fixture is not installed")
        return 77
    hashes = dict(line.split()[::-1] for line in (REPO / "subsystems/linux/profile/expected/fixtures.sha256").read_text().splitlines() if line.strip())
    assert hashlib.sha256(args.fixture.read_bytes()).hexdigest() == "c2c0ba389108f921f760e9ff668e0374eff247c31aef8207ce411acc66d5830a"
    selector = selectors.DefaultSelector()
    processes = []
    logs = {"server": b"", "client": b""}
    with tempfile.TemporaryDirectory(prefix="ssl-x11-") as temporary:
        try:
            server = subprocess.Popen([str(args.server)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
            processes.append(server)
            port = int(server.stdout.readline().split()[1])
            env = {**os.environ, "DISPLAY": f"127.0.0.1:{port-6000}", "XAUTHORITY": f"{temporary}/no-auth"}
            client = subprocess.Popen([str(interpreter), "--library-path", f"{args.root}/usr/lib:{args.root}/lib", str(args.fixture)],
                                      env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
            processes.append(client)
            selector.register(server.stdout, selectors.EVENT_READ, "server")
            selector.register(client.stdout, selectors.EVENT_READ, "client")
            selector.register(client.stderr, selectors.EVENT_READ, "client")
            selector.register(server.stderr, selectors.EVENT_READ, "server")

            def pump(seconds: float = .05) -> None:
                for key, _ in selector.select(seconds):
                    data = os.read(key.fd, 65536)
                    if data:
                        logs[key.data] += data
                    else:
                        selector.unregister(key.fileobj)

            def wait(name: str, token: str) -> None:
                deadline = time.monotonic() + 10
                while token.encode() not in logs[name]:
                    assert time.monotonic() < deadline, (name, token, logs)
                    pump()

            def command(text: str) -> None:
                server.stdin.write((text + "\n").encode())
                server.stdin.flush()

            def compare(name: str, expected: Image.Image) -> None:
                path = Path(temporary) / f"{name}.ppm"
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    previous = logs["server"].count(b"SHOT\n")
                    command(f"shot {path}")
                    while logs["server"].count(b"SHOT\n") == previous:
                        assert time.monotonic() < deadline, logs
                        pump()
                    actual = Image.open(path).convert("RGB")
                    if actual.size == expected.size and actual.tobytes() == expected.tobytes():
                        print(f"xeyes host: {name} matches Linux pixel for pixel", flush=True)
                        return
                    pump(.1)
                actual.save(REPO / "build/xeyes-host-failed.png")
                raise AssertionError(f"{name}: content differs from Linux; {logs}")

            wait("server", "MAP ")
            for i, (x, y) in enumerate([(75, 50), (0, 0), (149, 99), (100, 20), (400, 300), (620, 10), (10, 460)]):
                command(f"pointer {x} {y}")
                reference = Image.open(REPO / f"subsystems/linux/profile/expected/screens/xeyes-{i}-{x}x{y}.png").convert("RGB").crop((1, 1, 151, 101))
                compare(f"pointer-{i}", reference)
            command("resize 210 130")
            command("pointer 400 300")
            resized = Image.open(REPO / "subsystems/linux/profile/expected/screens/xeyes-resized-400x300.png").convert("RGB")
            compare("resized", resized)
            command("close")
            assert client.wait(timeout=5) == 0, logs
            assert server.wait(timeout=5) == 0, logs
            logs["server"] += server.stdout.read()
            logs["client"] += client.stdout.read()
            assert logs["client"] == b'Xlib:  extension "XInputExtension" missing on display "' + env["DISPLAY"].encode() + b'".\n', logs
            assert logs["server"].count(b"MAP ") == 1 and logs["server"].count(b"DESTROY ") == 1, logs
            assert b"X11 HOST PASS" in logs["server"], logs
            print("X11 XEYES PASS: pinned hash, Linux pixels, pointer path, resize, clean close")
            return 0
        except Exception:
            pump(.1)
            print(logs)
            raise
        finally:
            selector.close()
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
