#!/usr/bin/env python3
"""Run a supported i386 smoke against a GRUB ISO and a disk snapshot."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

import legacy_boot_test
import i386_graphics_test
import i386_services_test

SERVICE_SCENARIOS = ("services", "device-smoke", "audio-smoke", "net-smoke", "tcp-smoke", "media-smoke", "power-smoke", "native-smoke", "storage-smoke", "partition-smoke")
SCENARIOS = SERVICE_SCENARIOS + ("smoke", "boot", "processes", "memory", "desktop", "posix", "shared", "shared-memory", "sxgui-smoke", "taskbar", "calc-smoke", "windowd-smoke", "doom", "ccleste", "live-desktop")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=SCENARIOS, default="smoke")
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--disk-image", required=True, type=Path)
    parser.add_argument("--desktop-elf", type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=360)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    args = parser.parse_args()
    if args.scenario in SERVICE_SCENARIOS:
        iso = args.iso.with_name("SavanXP-live.iso") if args.scenario == "storage-smoke" else args.iso
        return i386_services_test.run(iso, args.disk_image, args.artifacts, args.scenario, args.timeout, args.accel)
    if args.scenario in ("desktop", "sxgui-smoke", "taskbar", "calc-smoke", "doom", "ccleste", "live-desktop"):
        iso = args.iso.with_name("SavanXP-live.iso") if args.scenario == "live-desktop" else args.iso
        return i386_graphics_test.run(iso, args.disk_image, args.artifacts,
            "calc" if args.scenario == "calc-smoke" else args.scenario, args.accel, args.timeout)
    command = ["--iso", str(args.iso), "--disk-image", str(args.disk_image),
               "--expect-user", "--accel", args.accel, "--timeout", str(args.timeout),
               "--artifacts", str(args.artifacts)]
    if args.scenario == "windowd-smoke":
        command += ["--expect-windowd"]
    if args.scenario in ("smoke", "shared", "shared-memory"):
        command += ["--expect-shared"]
    if args.scenario in ("smoke", "posix"):
        command += ["--expect-posix"]
    if args.scenario in ("smoke", "processes", "memory"):
        command += ["--expect-processes"]
    if args.scenario in ("memory", "shared-memory"):
        command += ["--memory-mib", "32"]
    sys.argv = [sys.argv[0], *command]
    result = legacy_boot_test.main()
    if result == 0 and args.scenario == "smoke":
        return i386_graphics_test.run(args.iso, args.disk_image, args.artifacts / "sxgui",
                                      "sxgui-smoke", args.accel, args.timeout)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
