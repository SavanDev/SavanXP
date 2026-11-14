#!/usr/bin/env python3
"""Launch this branch's GRUB/BIOS i386 system, retaining the persistent disk."""
from __future__ import annotations

import argparse
import fcntl
import os
import shutil
import subprocess
from pathlib import Path


def find_qemu(binary: str = "qemu-system-i386") -> str | None:
    return shutil.which(os.environ.get("SAVANXP_QEMU", binary))


def build_qemu_args(
    *, iso: Path, disk_image: Path, debug_log: Path, accel: str, smp: int = 1,
    headless: bool = False, debug: bool = False,
    monitor_path: Path | None = None, serial_path: Path | None = None,
    qmp_path: Path | None = None,
) -> list[str]:
    if smp != 1:
        raise ValueError("The i386 kernel currently supports one CPU only")
    command = [
        "qemu-system-i386", "-machine", "pc", "-cpu", "qemu32", "-accel", accel,
        "-m", "1024M", "-smp", "1", "-boot", "d",
        "-drive", f"file={iso},media=cdrom,format=raw,readonly=on,if=ide,index=2",
        "-drive", f"file={disk_image},media=disk,format=raw,if=ide,index=0",
        "-vga", "std", "-display", "none" if headless else "gtk",
        "-nic", "none", "-serial", f"file:{serial_path}" if serial_path else "stdio",
        "-debugcon", f"file:{debug_log}", "-global", "isa-debugcon.iobase=0xe9",
        "-no-reboot", "-monitor", f"unix:{monitor_path},server=on,wait=off" if monitor_path else "none",
    ]
    if qmp_path is not None:
        command += ["-qmp", f"unix:{qmp_path},server=on,wait=off"]
    if debug:
        command += ["-s", "-S"]
    return command


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--disk-image", required=True, type=Path)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    parser.add_argument("--smp", type=int, choices=(1,), default=1)
    parser.add_argument("--legacy", action="store_true", help="compatibility alias; i386 is already selected")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--qmp-path", type=Path)
    parser.add_argument("--monitor-path", type=Path)
    parser.add_argument("--serial-log", type=Path)
    args = parser.parse_args()
    iso = args.iso.resolve()
    disk = args.disk_image.resolve()
    for path in (iso, disk):
        if not path.is_file():
            parser.error(f"missing {path}; run ./build.sh iso first")
    qemu = find_qemu()
    if not qemu:
        parser.error("qemu-system-i386 was not found in PATH or SAVANXP_QEMU")
    command = build_qemu_args(
        iso=iso, disk_image=disk, debug_log=iso.parent / "debugcon.log",
        accel=args.accel, smp=args.smp, headless=args.headless, debug=args.debug,
        qmp_path=args.qmp_path, monitor_path=args.monitor_path, serial_path=args.serial_log,
    )
    command[0] = qemu
    print("qemu:", " ".join(command), flush=True)
    with disk.with_name(disk.name + ".lock").open("a+") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
