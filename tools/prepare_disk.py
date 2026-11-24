#!/usr/bin/env python3
"""Assemble a fresh GPT/UEFI SavanXP installation on Linux."""

import argparse
import os
import stat
import subprocess
import sys
import tempfile
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path

from block_device import claim, device_geometry, device_node, disk_sequence, rescan, write_at

SECTOR = 512
MIB = 1024 * 1024
ESP_MIB = 96
ESP_START = MIB
ROOT_START = ESP_START + ESP_MIB * MIB
GPT_PRIMARY = 34 * SECTOR
GPT_BACKUP = 33 * SECTOR


@dataclass(frozen=True)
class DiskTarget:
    number: int
    capacity: int
    sequence: int


def inspect_disk(status: os.stat_result) -> DiskTarget:
    if not stat.S_ISBLK(status.st_mode):
        raise ValueError("the destination is not a block device")
    node = device_node(status.st_rdev)
    if (node / "partition").exists():
        raise ValueError("select the whole disk to prepare it")
    capacity, sector_size = device_geometry(node)
    if sector_size != SECTOR:
        raise ValueError("the destination must use 512-byte logical sectors")
    return DiskTarget(status.st_rdev, capacity, disk_sequence(node))


def stage(capacity: int, work: Path, args: argparse.Namespace) -> str:
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
    return root_guid


def install(fd: int, capacity: int, work: Path, *, physical: bool) -> None:
    with ((work / "esp.img").open("rb") as esp, (work / "root.img").open("rb") as root,
          (work / "table.img").open("rb") as table):
        esp_size = os.fstat(esp.fileno()).st_size
        root_size = os.fstat(root.fileno()).st_size
        if (esp_size != ESP_MIB * MIB or not 0 < root_size <= 1024 * MIB or root_size % SECTOR or
                ROOT_START + root_size + GPT_BACKUP > capacity or os.fstat(table.fileno()).st_size != capacity):
            raise ValueError("the staged files do not fit the destination layout")
        for payload, offset in ((esp, ESP_START), (root, ROOT_START)):
            while data := payload.read(MIB):
                if physical or data != bytes(len(data)):
                    write_at(fd, data, offset)
                offset += len(data)
        # Flush the filesystems before publishing the table that points to them.
        os.fsync(fd)
        table.seek(capacity - GPT_BACKUP)
        write_at(fd, table.read(GPT_BACKUP), capacity - GPT_BACKUP)
        table.seek(0)
        write_at(fd, table.read(GPT_PRIMARY), 0)
        os.fsync(fd)


def install_device(device: Path, target: DiskTarget, work: Path, sudo: str = "") -> None:
    with ExitStack() as resources:
        try:
            fd = resources.enter_context(claim(device, writable=True))
        except PermissionError as error:
            if not sudo:
                raise PermissionError(f"write access to {device} requires sudo or device permissions") from error
            print(f"Requesting sudo to write {device}", flush=True)
            subprocess.run([sudo, "--", sys.executable, "-E", "-S", "-B", str(Path(__file__).resolve()),
                            "--write-staged", str(device), str(target.number), str(target.capacity),
                            str(target.sequence), str(work)], check=True)
            return

        if inspect_disk(os.fstat(fd)) != target:
            raise ValueError(f"{device} changed since staging began")
        install(fd, target.capacity, work, physical=True)
        try:
            rescan(fd)
        except OSError as error:
            print(f"Reconnect the disk to publish its new partitions ({error}).")


def write_staged(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Write a staged SavanXP installation to its selected disk.")
    parser.add_argument("device", type=Path)
    parser.add_argument("number", type=int)
    parser.add_argument("capacity", type=int)
    parser.add_argument("sequence", type=int)
    parser.add_argument("work", type=Path)
    args = parser.parse_args(argv)
    try:
        install_device(args.device, DiskTarget(args.number, args.capacity, args.sequence), args.work)
    except (OSError, ValueError) as error:
        parser.exit(1, f"prepare-disk: {error}\n")
    return 0


def main() -> int:
    if sys.argv[1:2] == ["--write-staged"]:
        return write_staged(sys.argv[2:])
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--output", type=Path, help="create a new raw GPT disk image")
    target.add_argument("--device", type=Path, help="prepare a whole Linux block device")
    parser.add_argument("--erase", action="store_true", help="authorize replacing the physical disk layout")
    for name in ("boot", "diskfs", "tools", "config", "sync", "python"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--root-sectors", type=int, default=2097152)
    parser.add_argument("--sudo", default="", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if bool(args.device) != args.erase:
        parser.error("--device and --erase must be used together")

    try:
        if args.device:
            device = args.device.resolve(strict=True)
            disk = inspect_disk(device.stat())
            print(f"Preparing {device} ({disk.capacity // MIB} MiB), replacing its partition table", flush=True)
            with tempfile.TemporaryDirectory(prefix="savanxp-prepare-") as scratch:
                work = Path(scratch)
                guid = stage(disk.capacity, work, args)
                install_device(device, disk, work, args.sudo)
            print(f"SxFS root: /dev/disk/by-partuuid/{guid}")
        else:
            output = args.output.absolute()
            if output.exists() or output.is_symlink():
                raise ValueError(f"output already exists: {output}")
            capacity = ROOT_START + args.root_sectors * SECTOR + MIB
            with tempfile.TemporaryDirectory(prefix=".savanxp-prepare-", dir=output.parent) as scratch:
                work = Path(scratch)
                staged = work / "disk.img"
                guid = stage(capacity, work, args)
                with staged.open("xb") as disk:
                    disk.truncate(capacity)
                    install(disk.fileno(), capacity, work, physical=False)
                os.link(staged, output)
            print(f"Prepared {output} (root PARTUUID {guid})")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"prepare-disk: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
