#!/usr/bin/env python3
"""The host half of the power-button-smoke scenario (tools/smoke_catalog.py).

At "NATIVE BUTTON READY" the real session runs with a window that turns down
the first close it is asked for and takes the second. This presses the ACPI
power button twice:

1. The first press has to reach init (the kernel tells it instead of powering
   off), end in "an application cancelled the shutdown", and leave the machine
   running.
2. The second has to end the session (the window closes first) and power the
   machine off through power::shutdown(), with the volume left clean.

Then it boots the machine again, where `nativetest power-button` checks the
mark the window wrote as it closed (docs/kernel/POWER.md#the-power-button).
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from power_smoke import check_volume, wait_for_status  # noqa: E402
from qmp_client import QmpClient  # noqa: E402

PRESSED = "power: power button pressed; telling pid"
REQUESTED = "init: shutdown requested by the power button; ending the session"
DECLINED = "nativetest: session-end: the client turns the close down"
CANCELLED = "init: an application cancelled the shutdown"
CLOSED = "nativetest: session-end: the client closes"
ENDED = "init: session ended and services stopped; powering off"
SYNCING = "power: shutdown: syncing volumes"


def fail(message: str) -> int:
    print(f"power-button-smoke: {message}", file=sys.stderr, flush=True)
    return 1


def wait_for_serial(serial_log: Path, text: str, limit: float) -> str:
    deadline = time.monotonic() + limit
    serial = ""
    while time.monotonic() < deadline:
        serial = serial_log.read_text(encoding="utf-8", errors="replace")
        if text in serial:
            break
        time.sleep(0.2)
    return serial


def in_order(serial: str, lines: list[str], start: int = 0) -> str:
    """The first of `lines` missing or out of order after `start`, or ""."""
    position = start
    for line in lines:
        found = serial.find(line, position)
        if found < 0:
            return line
        position = found + len(line)
    return ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args, _ = parser.parse_known_args()
    serial_log = Path(os.environ["SAVANXP_SMOKE_SERIAL_LOG"])
    image = Path(os.environ["SAVANXP_SMOKE_DISK_IMAGE"])

    with QmpClient(args.socket, timeout=10.0) as qmp:
        # First press: the window turns its close down, the session goes on.
        qmp.execute("system_powerdown")
        serial = wait_for_serial(serial_log, CANCELLED, 60.0)
        missing = in_order(serial, [PRESSED, REQUESTED, DECLINED, CANCELLED])
        if missing:
            return fail(f"first press: missing or out of order: {missing!r}")
        if SYNCING in serial:
            return fail("first press: the kernel powered off instead of telling init")
        status = qmp.execute("query-status")["return"]["status"]
        if status != "running":
            return fail(f"first press: the machine did not stay up (QEMU status {status!r})")
        print("power-button-smoke: the first press was cancelled by the window", flush=True)
        after_first = serial.find(CANCELLED) + len(CANCELLED)

        # Second press: the session ends, then the power goes.
        qmp.execute("system_powerdown")
        status = wait_for_status(qmp, "shutdown", 90.0)
        if status != "shutdown":
            return fail(f"second press: the guest did not power off (QEMU status {status!r})")
        serial = serial_log.read_text(encoding="utf-8", errors="replace")
        missing = in_order(serial, [PRESSED, REQUESTED, CLOSED, ENDED, SYNCING], after_first)
        if missing:
            return fail(f"second press: missing or out of order: {missing!r}")
        for line in serial.splitlines():
            if "power: a volume did not sync" in line or "power: the writeback queue did not drain" in line:
                return fail(f"the shutdown reported a failure: {line.strip()}")
        print("power-button-smoke: the second press ended the session and powered off", flush=True)
        problem = check_volume(image)
        if problem:
            return fail(problem)

        # -no-reboot turns a reset into a stop: make it a reset first.
        qmp.execute("set-action", {"reboot": "reset"})
        qmp.execute("system_reset")
        if qmp.execute("query-status")["return"]["status"] != "running":
            qmp.execute("cont")
    print("power-button-smoke: machine booted again", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
