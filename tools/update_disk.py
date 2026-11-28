#!/usr/bin/env python3
"""Update a SavanXP installation made by prepare-disk, keeping the user's files.

The GPT is never written. The SxFS root is synced the way the build syncs
build/disk.img (build-owned files replaced or retired, everything else kept),
then the EFI partition is rebuilt for the root GUID already in the table.
"""

import argparse
import os
import stat
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace

import prepare_disk
import sxfs_sync
from block_device import claim, device_identity
from prepare_disk import ESP_MIB, ESP_START, MIB, ROOT_START, SECTOR

ESP_TYPE = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
SXFS_TYPE = uuid.UUID("277c8079-900e-477e-99d8-4fee1496d370")
SXFS_MAGIC = b"SXFS\0\0\0\0"


@dataclass(frozen=True)
class Layout:
    root_guid: str
    root_size: int


def read_layout(fd: int, capacity: int) -> Layout:
    """Accept only the layout prepare-disk writes, and return its root partition."""
    redo = "prepare it again with prepare-disk --erase"
    header = sxfs_sync.read_device(fd, SECTOR, SECTOR)
    if header[:8] != b"EFI PART":
        raise ValueError(f"the target has no GPT; {redo}")
    size, crc = struct.unpack_from("<II", header, 12)
    if not 92 <= size <= SECTOR or zlib.crc32(header[:16] + bytes(4) + header[20:size]) != crc:
        raise ValueError("the primary GPT header is damaged")
    current, backup = struct.unpack_from("<QQ", header, 24)
    if current != 1 or backup != capacity // SECTOR - 1:
        raise ValueError(f"the GPT does not describe a disk of this size; {redo}")
    table_lba, count, item_size, table_crc = struct.unpack_from("<QIII", header, 72)
    if item_size < 128 or count < 2 or count * item_size > MIB:
        raise ValueError("the GPT partition array is not valid")
    entries = sxfs_sync.read_device(fd, count * item_size, table_lba * SECTOR)
    if zlib.crc32(entries) != table_crc:
        raise ValueError("the GPT partition array is damaged")

    def entry(index: int) -> tuple[uuid.UUID, uuid.UUID, int, int]:
        raw = entries[index * item_size:index * item_size + 48]
        first, last = struct.unpack_from("<QQ", raw, 32)
        return uuid.UUID(bytes_le=raw[:16]), uuid.UUID(bytes_le=raw[16:32]), first, last

    esp_type, _, esp_first, esp_last = entry(0)
    root_type, root_guid, root_first, root_last = entry(1)
    if (esp_type != ESP_TYPE or esp_first * SECTOR != ESP_START or
            (esp_last + 1) * SECTOR != ROOT_START or
            root_type != SXFS_TYPE or root_first * SECTOR != ROOT_START or root_last < root_first):
        raise ValueError(f"the partitions are not the SavanXP installed layout; {redo}")
    root_size = (root_last - root_first + 1) * SECTOR
    if root_size > sxfs_sync.VOLUME_LIMIT or ROOT_START + root_size > capacity:
        raise ValueError(f"the SxFS root partition does not fit the layout; {redo}")
    if sxfs_sync.read_device(fd, len(SXFS_MAGIC), ROOT_START) != SXFS_MAGIC:
        raise ValueError("the root partition holds no SxFS volume")
    return Layout(str(root_guid), root_size)


def write_esp(fd: int, work: Path) -> None:
    """Replace the EFI partition, putting the previous one back if a write fails."""
    original = work / "esp-before.img"
    with original.open("xb") as backup:
        backup.write(sxfs_sync.read_device(fd, ESP_MIB * MIB, ESP_START))
    try:
        sxfs_sync.write_volume(fd, work / "esp.img", ESP_START)
    except (OSError, KeyboardInterrupt) as error:
        print(f"update-disk: writing the EFI partition failed: {error}. Restoring it.", file=sys.stderr)
        sxfs_sync.write_volume(fd, original, ESP_START)
        raise OSError("the EFI partition was restored but the root is already updated; "
                      "run update-disk again before booting this disk") from error


def update(fd: int, capacity: int, target: Path, args: argparse.Namespace, recovery: Path) -> int:
    layout = read_layout(fd, capacity)
    print(f"Updating {target} (SxFS root PARTUUID {layout.root_guid})", flush=True)
    with tempfile.TemporaryDirectory(prefix="savanxp-update-") as scratch:
        work = Path(scratch)
        # Stage the boot files first, so a failure here leaves the disk untouched.
        esp = prepare_disk.stage_esp(work, args, layout.root_guid)
        if esp.stat().st_size != ESP_MIB * MIB:
            raise ValueError("the staged EFI partition does not match the layout")
        sync_args = SimpleNamespace(reset=False, no_compact=False, sectors=layout.root_size // SECTOR)
        result = sxfs_sync.sync_block_volume(sync_args, target, fd, layout.root_size, args.diskfs,
                                             args.tools / "sxfs-cli", recovery, base=ROOT_START)
        if result != 0:
            print("update-disk: the disk was not changed", file=sys.stderr)
            return result
        write_esp(fd, work)
    print(f"Updated {target}")
    return 0


def update_image(image: Path, args: argparse.Namespace, recovery: Path) -> int:
    fd = os.open(image, os.O_RDWR | os.O_NOFOLLOW)
    try:
        status = os.fstat(fd)
        if not stat.S_ISREG(status.st_mode):
            raise ValueError(f"{image} is not a regular disk image file")
        return update(fd, status.st_size, image, args, recovery)
    finally:
        os.close(fd)


def update_device(device: Path, args: argparse.Namespace, recovery: Path) -> int:
    disk = prepare_disk.inspect_disk(device.stat())
    identity = device_identity(device.stat())
    if args.device_identity is not None and identity != args.device_identity:
        raise ValueError(f"{device} changed since the update began")
    with ExitStack() as resources:
        try:
            fd = resources.enter_context(claim(device, writable=True))
        except PermissionError as error:
            if args.device_identity is not None or os.geteuid() == 0:
                raise
            if not args.sudo:
                raise PermissionError(f"write access to {device} requires sudo or device permissions") from error
            print(f"Requesting sudo to open {device}", flush=True)
            command = [args.sudo, "--", sys.executable, "-E", "-S", "-B", str(Path(__file__).resolve()),
                       *sys.argv[1:], "--device-identity", identity, "--recovery-root", str(recovery)]
            return subprocess.run(command, check=False).returncode
        if device_identity(os.fstat(fd)) != identity:
            raise ValueError(f"{device} changed since the update began")
        if args.device_identity is not None:
            # The exclusive descriptor stays usable after dropping root: reading,
            # staging, validation and writing all run as the calling user.
            sxfs_sync.drop_sudo_privileges()
        return update(fd, disk.capacity, device, args, recovery)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--image", type=Path, help="an installed raw GPT disk image")
    target.add_argument("--device", type=Path, help="an installed whole Linux block device")
    for name in ("boot", "diskfs", "tools", "config"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--sudo", default="", help=argparse.SUPPRESS)
    parser.add_argument("--device-identity", help=argparse.SUPPRESS)
    parser.add_argument("--recovery-root", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.device_identity is not None and args.device is None:
        parser.error("the privileged device opener requires --device")
    recovery = (args.recovery_root or Path(os.environ.get(
        "SAVANXP_DEVICE_RECOVERY_DIR", args.tools.parent / "device-recovery"))).resolve()
    try:
        if args.device:
            return update_device(args.device.resolve(strict=True), args, recovery)
        return update_image(args.image.resolve(strict=True), args, recovery)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"update-disk: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
