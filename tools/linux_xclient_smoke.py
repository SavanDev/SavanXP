#!/usr/bin/env python3
"""L5: exact Linux pixels, root/local pointer coordinates, resize and caption close."""
from __future__ import annotations
import argparse
import math
import os
import time
from pathlib import Path
from PIL import Image
from qmp_client import QmpClient
from win32_input import Screen, wait_for_run

ROOT = Path(__file__).resolve().parent.parent


def reference() -> Image.Image:
    return Image.open(ROOT / "subsystems/linux/profile/expected/screens/xclient-0-75x50.png").convert("RGB").crop((0, 0, 200, 150))


def resized_reference() -> Image.Image:
    # The same Linux rasterized shapes at the fixture's new coordinates.
    source = reference()
    result = Image.new("RGB", (260, 180), "white")
    result.paste(source.crop((10, 10, 70, 50)), (10, 10))
    result.paste(source.crop((120, 70, 180, 130)), (180, 100))
    return result


def wait_serial(log: Path, token: str, timeout: float = 10, start: int = 0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if token in log.read_bytes()[start:].decode(errors="replace"):
            return
        time.sleep(.05)
    raise AssertionError(f"guest did not report {token!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args, _ = parser.parse_known_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    log = Path(os.environ["SAVANXP_SMOKE_SERIAL_LOG"])
    with QmpClient(args.socket, timeout=10) as qmp:
        screen = Screen(qmp)
        x, y, sw, sh = wait_for_run(screen, 20, (255, 255, 255), 200)
        print(f"linux-xclient: content at {x},{y}, screen {sw}x{sh}", flush=True)

        def move(px: int, py: int) -> None:
            qmp.send_input([
                {"type": "abs", "data": {"axis": "x", "value": math.ceil(px * 32767 / (sw - 1))}},
                {"type": "abs", "data": {"axis": "y", "value": math.ceil(py * 32767 / (sh - 1))}},
            ])

        def capture(name: str, size: tuple[int, int], expected: Image.Image) -> None:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                width, height, rgb = screen.grab()
                image = Image.frombytes("RGB", (width, height), rgb)
                crop = image.crop((x, y, x + size[0], y + size[1]))
                if crop.tobytes() == expected.tobytes():
                    image.save(args.out_dir / f"{name}.png")
                    crop.save(args.out_dir / f"{name}-content.png")
                    print(f"linux-xclient: {name} matches Linux pixel for pixel", flush=True)
                    return
                image.save(args.out_dir / f"{name}-failed.png")
                time.sleep(.1)
            raise AssertionError(f"{name}: content differs from Linux")

        # Keep the desktop cursor outside the pixels being compared.
        move(10, 460)
        capture("initial", (200, 150), reference())
        for rx, ry in [(75, 50), (0, 0), (149, 99), (100, 20), (400, 300), (620, 10), (10, 460)]:
            start = log.stat().st_size
            move(rx, ry)
            wait_serial(log, f"linux-xclient: pointer {rx} {ry} window {rx-x} {ry-y}", start=start)
        # Drag the bottom-right frame edge; the client area grows by 60x30.
        move(x + 201, y + 151)
        qmp.send_input([{"type": "btn", "data": {"button": "left", "down": True}}])
        time.sleep(.1)
        move(x + 261, y + 181)
        time.sleep(.1)
        qmp.send_input([{"type": "btn", "data": {"button": "left", "down": False}}])
        wait_serial(log, "linux-xclient: resize 260x180")
        wait_serial(log, "linux-xclient: expose 260x180")
        move(10, 460)
        capture("resized", (260, 180), resized_reference())
        move(x + 260 - 8, y - 12)
        qmp.send_input([{"type": "btn", "data": {"button": "left", "down": True}}])
        qmp.send_input([{"type": "btn", "data": {"button": "left", "down": False}}])
        wait_serial(log, "linux-xclient: delete requested")
        wait_serial(log, "linux-xclient: closed")
    print("linux-xclient: recorded pointer path, redraw and caption close PASS", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
