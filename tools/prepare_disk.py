#!/usr/bin/env python3
"""Assemble a fresh GPT/UEFI SavanXP installation on Linux."""

import argparse
import os
import subprocess
import tempfile
from pathlib import Path

from block_device import claim, geometry, require_whole_disk, rescan, write_at

SECTOR = 512
MIB = 1024 * 1024
ESP_MIB = 96
ESP_START = MIB
ROOT_START = ESP_START + ESP_MIB * MIB
GPT_PRIMARY = 34 * SECTOR
GPT_BACKUP = 33 * SECTOR


def prepare(fd: int, capacity: int, work: Path, args: argparse.Namespace) -> str:
    root_size = args.root_sectors * SECTOR
    if not 0 < root_size <= 1024 * MIB:
        raise ValueError("the SxFS root must be at most 1 GiB")
    if capacity < ROOT_START + root_size + GPT_BACKUP:
        raise ValueError(f"target needs at least {(ROOT_START + root_size + GPT_BACKUP + MIB - 1) // MIB} MiB")
    table = work / "table.img"
    root_guid = subprocess.check_output(
        [args.tools / "gpt-image", table, str(capacity // SECTOR),
         str(ESP_MIB * MIB // SECTOR), str(args.root_sectors)], text=True).strip().lower()
    root = work / "root.img"
    subprocess.run([args.python, args.sync, "--image", root, "--source", args.diskfs,
                    "--cli", args.tools / "sxfs-cli", "--sectors", str(args.root_sectors)], check=True)

    esp = work / "esp.img"
    fat = args.tools / "fat-image"
    config = work / "limine.conf"
    config.write_text(args.config.read_text().replace("@ROOT_GUID@", root_guid), encoding="utf-8")
    subprocess.run([fat, "create-fat32", esp, str(ESP_MIB)], check=True)
    for source, destination in ((config, "/limine.conf"),
                                (args.boot / "EFI/BOOT/BOOTX64.EFI", "/EFI/BOOT/BOOTX64.EFI"),
                                (args.boot / "boot/kernel.elf", "/boot/kernel.elf"),
                                (args.boot / "boot/initramfs.cpio", "/boot/initramfs.cpio")):
        subprocess.run([fat, "put", esp, source, destination], check=True)

    for image, offset in ((esp, ESP_START), (root, ROOT_START)):
        with image.open("rb") as payload:
            while data := payload.read(MIB):
                if args.device or data != bytes(len(data)):
                    write_at(fd, data, offset)
                offset += len(data)
    # Flush the filesystems before publishing the table that points to them.
    os.fsync(fd)
    with table.open("rb") as layout:
        layout.seek(capacity - GPT_BACKUP)
        write_at(fd, layout.read(GPT_BACKUP), capacity - GPT_BACKUP)
        layout.seek(0)
        write_at(fd, layout.read(GPT_PRIMARY), 0)
    os.fsync(fd)
    return root_guid


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--output", type=Path, help="create a new raw GPT disk image")
    target.add_argument("--device", type=Path, help="prepare a whole Linux block device")
    parser.add_argument("--erase", action="store_true", help="authorize replacing the physical disk layout")
    for name in ("boot", "diskfs", "tools", "config", "sync", "python"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--root-sectors", type=int, default=2097152)
    args = parser.parse_args()
    if bool(args.device) != args.erase:
        parser.error("--device and --erase must be used together")

    try:
        if args.device:
            device = args.device.resolve(strict=True)
            with tempfile.TemporaryDirectory(prefix="savanxp-prepare-") as scratch:
                with claim(device, writable=True) as fd:
                    require_whole_disk(fd)
                    capacity, sector_size = geometry(fd)
                    if sector_size != SECTOR:
                        raise ValueError("the destination must use 512-byte logical sectors")
                    print(f"Preparing {device} ({capacity // MIB} MiB), replacing its partition table", flush=True)
                    guid = prepare(fd, capacity, Path(scratch), args)
                    try:
                        rescan(fd)
                    except OSError as error:
                        print(f"Reconnect the disk to publish its new partitions ({error}).")
            print(f"SxFS root: /dev/disk/by-partuuid/{guid}")
        else:
            output = args.output.absolute()
            if output.exists() or output.is_symlink():
                raise ValueError(f"output already exists: {output}")
            capacity = ROOT_START + args.root_sectors * SECTOR + MIB
            with tempfile.TemporaryDirectory(prefix=".savanxp-prepare-", dir=output.parent) as scratch:
                work = Path(scratch)
                staged = work / "disk.img"
                with staged.open("xb") as disk:
                    disk.truncate(capacity)
                    guid = prepare(disk.fileno(), capacity, work, args)
                os.link(staged, output)
            print(f"Prepared {output} (root PARTUUID {guid})")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"prepare-disk: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
