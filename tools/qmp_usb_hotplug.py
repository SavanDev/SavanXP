#!/usr/bin/env python3
"""Host side of usb-hotplug-smoke (docs/roadmaps/open/USB_ROADMAP.md).

Runs when kbdtest prints KBD SMOKE READY, on a machine with no 8042 and one
USB keyboard. It pulls the keyboard out and plugs a new one in, plugs a USB
stick in and pulls it out, waiting each time for the guest to log what it
saw; then it types kbd-smoke's keys. They can only reach kbdtest through the
keyboard plugged in after boot, so the guest's own PASS proves the HID half,
and the log lines checked here prove the storage half.

The serial log comes from SAVANXP_SMOKE_SERIAL_LOG, which run_smoke.py sets
for its callbacks.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from qmp_client import QmpClient, send_kbd_smoke_actions  # noqa: E402

STICK_MIB = 8


class Failure(Exception):
    pass


def wait_for(log: Path, text: str, count: int, timeout: float = 15.0) -> None:
    """Wait until `text` appears `count` times in the guest's serial log."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            seen = log.read_text(encoding="utf-8", errors="replace").count(text)
        except OSError:
            seen = 0
        if seen >= count:
            return
        time.sleep(0.2)
    raise Failure(f"the guest never logged {text!r} ({count} times)")


def wait_for_pattern(log: Path, pattern: str, timeout: float = 15.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if re.search(pattern, log.read_text(encoding="utf-8", errors="replace")):
                return
        except OSError:
            pass
        time.sleep(0.2)
    raise Failure(f"the guest never logged a line matching {pattern!r}")


def count(log: Path, text: str) -> int:
    return log.read_text(encoding="utf-8", errors="replace").count(text)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, required=True, help="QMP Unix socket")
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    log = Path(os.environ["SAVANXP_SMOKE_SERIAL_LOG"])
    args.out_dir.mkdir(parents=True, exist_ok=True)
    stick = args.out_dir / "hotstick.img"
    with stick.open("wb") as handle:
        handle.truncate(STICK_MIB * 1024 * 1024)

    try:
        with QmpClient(args.socket, timeout=10.0) as client:
            # The keyboard: out, then a new one in.
            keyboards = count(log, "usb-hid: keyboard on")
            client.execute("device_del", {"id": "usbkbd"})
            wait_for(log, "627:1 removed", 1)
            client.execute("device_add", {"driver": "usb-kbd", "bus": "xhci.0", "id": "usbkbd2"})
            wait_for(log, "usb-hid: keyboard on", keyboards + 1)
            print("qmp_usb_hotplug: keyboard pulled out and plugged in again", flush=True)

            # A stick: in, registered with its size; then out, and released.
            client.execute(
                "blockdev-add",
                {
                    "driver": "raw",
                    "node-name": "hotstick",
                    "file": {"driver": "file", "filename": str(stick)},
                },
            )
            client.execute(
                "device_add",
                {"driver": "usb-storage", "bus": "xhci.0", "drive": "hotstick", "id": "hotstick",
                 "serial": "SXHOT0"},
            )
            wait_for(log, f"serial \"SXHOT0\", {STICK_MIB * 2048} sectors", 1)
            client.execute("device_del", {"id": "hotstick"})
            wait_for_pattern(log, r"usb-storage: usb\d+ removed")
            print("qmp_usb_hotplug: stick plugged in, registered, pulled out and released", flush=True)

            time.sleep(0.5)
            send_kbd_smoke_actions(client)
    except Failure as error:
        print(f"qmp_usb_hotplug: {error}", file=sys.stderr, flush=True)
        return 1
    finally:
        stick.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
