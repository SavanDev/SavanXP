#!/usr/bin/env python3
"""Run the pinned, unchanged musl/Xlib client over real TCP into the engine."""
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
    assert hashlib.sha256(args.fixture.read_bytes()).hexdigest() == hashes["xclient"]
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
                        print(f"xclient host: {name} matches Linux pixel for pixel", flush=True)
                        return
                    pump(.1)
                raise AssertionError(f"{name}: content differs from Linux")

            wait("client", "pointer 75 50 window 75 50\n")
            reference = Image.open(REPO / "subsystems/linux/profile/expected/screens/xclient-0-75x50.png").convert("RGB").crop((0, 0, 200, 150))
            compare("initial", reference)
            for x, y in [(0, 0), (149, 99), (100, 20), (400, 300), (620, 10), (10, 460)]:
                command(f"pointer {x} {y}")
                wait("client", f"pointer {x} {y} window {x} {y}\n")
            command("resize 260 180")
            wait("client", "resize 260x180\n")
            wait("client", "expose 260x180\n")
            resized = Image.new("RGB", (260, 180), "white")
            resized.paste(reference.crop((10, 10, 70, 50)), (10, 10))
            resized.paste(reference.crop((120, 70, 180, 130)), (180, 100))
            compare("resized", resized)
            command("close")
            wait("client", "closed\n")
            assert client.wait(timeout=5) == 0, logs
            assert server.wait(timeout=5) == 0, logs
            logs["server"] += server.stdout.read()
            logs["client"] += client.stdout.read()
            assert not client.stderr.read(), "Xlib reported an error"
            assert logs["server"].count(b"MAP ") == 1 and logs["server"].count(b"DESTROY ") == 1, logs
            assert b"X11 HOST PASS" in logs["server"], logs
            print("X11 XLIB PASS: pinned hash, Linux pixels, pointer path, resize, clean close")
            return 0
        finally:
            selector.close()
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
