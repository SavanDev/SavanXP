#!/usr/bin/env python3
"""The host half of the win32-window smoke scenario (tools/smoke_catalog.py).

At "WIN32 WINDOW READY" the guest runs window.exe under windowd. This finds
its client area on the screen, plays the fixture's input script into it over
QMP (tools/win32_input.py), and compares the client area the script's `dump`
captured with the golden image, the harness's own, which equals Windows XP's
outside the text box. The guest compares the messages.

play_fixture() is shared with win32-dialog (tools/win32_dialog_smoke.py).
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


def play_fixture(scenario: str, script: str, client: tuple[int, int], dumps: list[str]) -> int:
    """Plays fixtures/SCRIPT into the window found by its marker, then
    compares each dump with expected/NAME.ppm.gz. 0 when all match."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args, _ = parser.parse_known_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    with QmpClient(args.socket, timeout=10.0) as qmp:
        screen = win32_input.Screen(qmp)
        x, y, width, height = win32_input.wait_for_marker(screen, 30.0)
        print(f"{scenario}: client area at {x},{y} on a {width}x{height} screen", flush=True)
        player = win32_input.Player(qmp, screen, (x, y), (width, height), client, args.out_dir)
        player.play(FIXTURE / script)
    failures = 0
    for name in dumps:
        dump = args.out_dir / f"{name}.ppm"
        got = win32_input.read_ppm(dump)
        want = win32_input.read_ppm_bytes(gzip.decompress((FIXTURE / f"expected/{name}.ppm.gz").read_bytes()))
        if got != want:
            differ = sum(1 for i in range(0, len(want[2]), 3) if got[2][i:i + 3] != want[2][i:i + 3])
            print(f"{scenario}: {differ} pixels of {dump} differ from the golden image", flush=True)
            failures += 1
        else:
            print(f"{scenario}: {name} matches the golden image", flush=True)
    return 1 if failures else 0


def main() -> int:
    return play_fixture("win32-window", "window/input.txt", (320, 240), ["window"])


if __name__ == "__main__":
    sys.exit(main())
