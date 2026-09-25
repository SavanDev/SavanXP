#!/usr/bin/env python3
"""Port-owned QMP display assertion for the Media Player smoke.

The guest's ``--gpu-hold`` mode emits ``MEDIAPLAYER DISPLAY READY`` only after
presenting a decoded frame. This callback captures that held frame and rejects
blank/flat output through the native Unix QMP socket.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))

from qmp_client import QmpClient  # noqa: E402

MINIMUM_COLORS = 16
EXPECTED_SIZE = (1280, 800)


def wait_for_screenshot(path: Path, timeout: float = 8.0):
    from PIL import Image

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and path.stat().st_size > 0:
            try:
                with Image.open(path) as image:
                    return image.convert("RGB").copy()
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError(f"screendump did not produce {path}")


def check_image(image, label: str) -> int:
    if image.size != EXPECTED_SIZE:
        print(f"  FALLA: {label} screenshot is {image.size}, expected {EXPECTED_SIZE}")
        return 1
    colors = image.getcolors(maxcolors=1 << 20)
    distinct = len(colors) if colors is not None else 1 << 20
    print(f"  colores distintos en pantalla: {distinct}")
    if distinct < MINIMUM_COLORS:
        print(f"  FALLA: la pantalla tiene {distinct} color(es), esperaba >= {MINIMUM_COLORS}")
        return 1
    print(f"  {label}: PASS")
    return 0


def capture_socket(socket_path: Path, out_dir: Path, name: str) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    ppm = out_dir / f"{name}.ppm"
    png = out_dir / f"{name}.png"
    ppm.unlink(missing_ok=True)
    png.unlink(missing_ok=True)
    with QmpClient(socket_path, timeout=5.0) as client:
        client.screenshot(ppm)
    image = wait_for_screenshot(ppm)
    image.save(png)
    ppm.unlink(missing_ok=True)
    return check_image(image, "display frame")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, required=True, help="QMP Unix socket")
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--name", default="avsync")
    args = parser.parse_args()

    return capture_socket(args.socket, args.out_dir, args.name)


if __name__ == "__main__":
    raise SystemExit(main())
