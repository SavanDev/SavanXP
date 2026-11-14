#!/usr/bin/env python3
"""The host half of the linux-restart smoke scenario (tools/smoke_catalog.py).

At "LINUX RESTART READY" the guest has run SSL's `files write` fixture and
synced the volume. This restarts the machine: QEMU runs with -no-reboot,
which turns a reset into a stop, so the reboot action is set to reset first.
The second boot runs linuxtest --restart again, which finds the file and
reads it back (docs/userland/LINUX.md).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qmp_client import QmpClient  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args, _ = parser.parse_known_args()
    with QmpClient(args.socket, timeout=10.0) as qmp:
        qmp.execute("set-action", {"reboot": "reset"})
        qmp.execute("system_reset")
    print("linux-restart: machine reset", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
