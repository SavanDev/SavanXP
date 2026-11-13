#!/usr/bin/env python3
"""The host half of the win32-window smoke scenario (tools/smoke_catalog.py).

At "WIN32 WINDOW READY" the guest runs window.exe under windowd. This finds
its client area on the screen, plays the fixture's input script into it over
QMP (tools/win32_input.py), and compares the client area the script's `dump`
captured with the golden image, the harness's own, which equals Windows XP's
outside the text box. The guest compares the messages.
"""

from __future__ import annotations

import argparse
import gzip
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qmp_client import QmpClient  # noqa: E402
import win32_input  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "subsystems/win32/fixtures"
CLIENT = (320, 240)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    with QmpClient(args.socket, timeout=10.0) as qmp:
        screen = win32_input.Screen(qmp)
        x, y, width, height = win32_input.wait_for_marker(screen, 30.0)
        print(f"win32-window: client area at {x},{y} on a {width}x{height} screen", flush=True)
        player = win32_input.Player(qmp, screen, (x, y), (width, height), CLIENT, args.out_dir)
        player.play(FIXTURE / "window/input.txt")
    dump = args.out_dir / "window.ppm"
    got = win32_input.read_ppm(dump)
    want = win32_input.read_ppm_bytes(gzip.decompress((FIXTURE / "expected/window.ppm.gz").read_bytes()))
    if got != want:
        differ = sum(1 for i in range(0, len(want[2]), 3) if got[2][i:i + 3] != want[2][i:i + 3])
        print(f"win32-window: {differ} pixels of {dump} differ from the golden image", flush=True)
        return 1
    print("win32-window: the client area matches the golden image", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
