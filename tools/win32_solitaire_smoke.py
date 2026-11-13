#!/usr/bin/env python3
"""The host half of the win32-solitaire smoke scenario (tools/smoke_catalog.py).

At "WIN32 SOLITAIRE READY" the guest runs the user's own sol.exe under
windowd with its clock fixed, so the first deal is the harness's. Solitaire
draws no marker, so its window is found by its green client area (the first
row of 585 pixels of 0x008000; the menu bar is the 20 rows above it). This
plays fixtures/solitaire/input-1.txt into it, then input-2.txt into the
second run's window once the guest logs "WIN32 SOLITAIRE RUN 2" (read from
the serial log the runner names), and compares the SHA-256 of every image the
scripts dumped with expected/solitaire.sha256, the harness's own
(docs/userland/WIN32.md, "Solitaire"). The guest checks the exit statuses and the registry.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qmp_client import QmpClient  # noqa: E402
import win32_input  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "subsystems/win32/fixtures"
BOARD = "0,128,0:585:-20"


SECOND = b"WIN32 SOLITAIRE RUN 2"


def wait_second_run(timeout: float) -> None:
    """Until the guest logs that the first run has exited and the second
    starts, then a moment for the first window to leave the screen."""
    log = Path(os.environ["SAVANXP_SMOKE_SERIAL_LOG"])
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log.exists() and SECOND in log.read_bytes():
            time.sleep(2.0)
            return
        time.sleep(0.5)
    raise TimeoutError("the second run never started")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args, _ = parser.parse_known_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    with QmpClient(args.socket, timeout=10.0) as qmp:
        screen = win32_input.Screen(qmp)
        for run in (1, 2):
            if run == 2:
                wait_second_run(60.0)
            x, y, width, height = win32_input.wait_for_anchor(screen, 120.0, BOARD)
            print(f"win32-solitaire: run {run}: board at {x},{y} on a {width}x{height} screen", flush=True)
            player = win32_input.Player(qmp, screen, (x, y), (width, height), (585, 404), args.out_dir)
            player.play(FIXTURE / f"solitaire/input-{run}.txt")
    failures = 0
    for line in (FIXTURE / "expected/solitaire.sha256").read_text().splitlines():
        digest, name = line.split()
        dump = args.out_dir / name
        if not dump.exists():
            print(f"win32-solitaire: {name} was not dumped", flush=True)
            failures += 1
        elif hashlib.sha256(dump.read_bytes()).hexdigest() != digest:
            print(f"win32-solitaire: {dump} differs from the harness's", flush=True)
            failures += 1
        else:
            print(f"win32-solitaire: {name} matches", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
