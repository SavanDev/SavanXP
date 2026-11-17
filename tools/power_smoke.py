#!/usr/bin/env python3
"""The host half of the power-smoke scenario (tools/smoke_catalog.py).

At "NATIVE POWER READY" the guest has written a payload it never flushed and
keeps writing, so the block writeback queue is not empty. This presses the
ACPI power button, waits for the guest to power the machine off (QEMU runs
with -no-shutdown, so it stops instead of exiting), and checks the disk copy
while nothing touches it:

- the kernel took the power::shutdown() path and nothing in it failed;
- both superblocks carry the same sequence and the CLEAN flag;
- the journal header holds no pending transaction.

Then it boots the machine again. The second run of `nativetest power` reads
the payload back and ends the scenario (docs/kernel/POWER.md#the-test).
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qmp_client import QmpClient  # noqa: E402

SECTOR = 512
FLAG_CLEAN = 1
JOURNAL_LBA = 2
JOURNAL_MAGIC = b"SXJNL"


def fail(message: str) -> int:
    print(f"power-smoke: {message}", file=sys.stderr, flush=True)
    return 1


def wait_for_status(qmp: QmpClient, wanted: str, limit: float) -> str:
    deadline = time.monotonic() + limit
    status = ""
    while time.monotonic() < deadline:
        status = qmp.execute("query-status")["return"]["status"]
        if status == wanted:
            break
        time.sleep(0.2)
    return status


def check_volume(image: Path) -> str:
    """What is wrong with the volume as it was left, or "" when it came down clean."""
    with image.open("rb") as disk:
        superblocks = []
        for lba in (0, 1):
            disk.seek(lba * SECTOR)
            _magic, _version, _checksum, sequence, flags = struct.unpack("<8sIIII", disk.read(24))
            superblocks.append((sequence, flags))
        disk.seek(JOURNAL_LBA * SECTOR)
        magic, _checksum, sequence, pending = struct.unpack("<8sIII", disk.read(20))
    print(f"power-smoke: superblocks {superblocks}, journal magic={magic!r} pending={pending}", flush=True)
    if superblocks[0][0] != superblocks[1][0]:
        return f"the two superblocks disagree: {superblocks}"
    if any((flags & FLAG_CLEAN) == 0 for _sequence, flags in superblocks):
        return "the volume was not marked clean"
    if magic.startswith(JOURNAL_MAGIC) and pending != 0:
        return f"the journal still holds pending transaction {sequence}"
    return ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args, _ = parser.parse_known_args()
    serial_log = Path(os.environ["SAVANXP_SMOKE_SERIAL_LOG"])
    image = Path(os.environ["SAVANXP_SMOKE_DISK_IMAGE"])

    with QmpClient(args.socket, timeout=10.0) as qmp:
        qmp.execute("system_powerdown")
        status = wait_for_status(qmp, "shutdown", 60.0)
        if status != "shutdown":
            return fail(f"the guest did not power off (QEMU status {status!r})")
        print("power-smoke: the guest powered off", flush=True)

        serial = serial_log.read_text(encoding="utf-8", errors="replace")
        if "power: shutdown: syncing volumes" not in serial:
            return fail("the power button did not go through power::shutdown()")
        for line in serial.splitlines():
            if "power: a volume did not sync" in line or "power: the writeback queue did not drain" in line:
                return fail(f"the shutdown reported a failure: {line.strip()}")
        problem = check_volume(image)
        if problem:
            return fail(problem)

        # -no-reboot turns a reset into a stop: make it a reset first.
        qmp.execute("set-action", {"reboot": "reset"})
        qmp.execute("system_reset")
        if qmp.execute("query-status")["return"]["status"] != "running":
            qmp.execute("cont")
    print("power-smoke: volume clean after the power button; machine booted again", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
