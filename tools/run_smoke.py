#!/usr/bin/env python3
"""Run a supported i386 smoke against a GRUB ISO and a disk snapshot."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

import legacy_boot_test

SCENARIOS = ("smoke", "boot", "processes", "desktop")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=SCENARIOS, default="smoke")
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--disk-image", required=True, type=Path)
    parser.add_argument("--desktop-elf", type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    args = parser.parse_args()
    command = ["--iso", str(args.iso), "--disk-image", str(args.disk_image),
               "--expect-user", "--accel", args.accel, "--timeout", str(args.timeout),
               "--artifacts", str(args.artifacts)]
    if args.scenario in ("smoke", "processes"):
        command += ["--expect-processes"]
    if args.scenario in ("smoke", "desktop"):
        if args.desktop_elf is None:
            parser.error("--desktop-elf is required for desktop checks")
        command += ["--expect-key", "a", "--expect-key", "b", "--expect-desktop", str(args.desktop_elf)]
    sys.argv = [sys.argv[0], *command]
    return legacy_boot_test.main()


if __name__ == "__main__":
    raise SystemExit(main())
