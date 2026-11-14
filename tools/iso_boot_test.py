#!/usr/bin/env python3
"""Check the BIOS El Torito catalog and boot this branch's i386 kernel."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys

import legacy_boot_test


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--disk-image", type=Path)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    parser.add_argument("--layout-only", action="store_true")
    args = parser.parse_args()
    xorriso = shutil.which("xorriso")
    if xorriso is None:
        parser.error("xorriso was not found in PATH")
    report = subprocess.run([xorriso, "-indev", str(args.iso), "-report_el_torito", "plain"],
                            text=True, capture_output=True)
    if report.returncode != 0 or "El Torito boot img" not in report.stdout or "BIOS" not in report.stdout:
        parser.error("ISO has no inspectable BIOS El Torito boot entry")
    print("ISO BIOS El Torito catalog: PASS", flush=True)
    if args.layout_only:
        return 0
    command = ["--iso", str(args.iso), "--timeout", str(args.timeout), "--accel", args.accel]
    if args.disk_image is not None:
        command += ["--disk-image", str(args.disk_image), "--expect-user"]
    sys.argv = [sys.argv[0], *command]
    return legacy_boot_test.main()


if __name__ == "__main__":
    raise SystemExit(main())
