#!/usr/bin/env python3
"""Check the install layout and offline SxFS updates using disposable files."""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib
from contextlib import nullcontext
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_disk
import sxfs_sync


def check_gpt(image: Path) -> str:
    with image.open("rb") as disk:
        def at(lba: int, count: int = 1) -> bytes:
            disk.seek(lba * 512)
            return disk.read(count * 512)

        last = image.stat().st_size // 512 - 1
        assert at(0)[450] == 0xEE
        for lba, alternate in ((1, last), (last, 1)):
            header = at(lba)
            assert header[:8] == b"EFI PART"
            size, crc = struct.unpack_from("<II", header, 12)
            assert zlib.crc32(header[:16] + bytes(4) + header[20:size]) == crc
            assert struct.unpack_from("<QQ", header, 24) == (lba, alternate)
            table_lba, count, item_size, table_crc = struct.unpack_from("<QIII", header, 72)
            entries = at(table_lba, count * item_size // 512)
            assert zlib.crc32(entries) == table_crc
            assert uuid.UUID(bytes_le=entries[:16]) == uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
            assert uuid.UUID(bytes_le=entries[128:144]) == uuid.UUID("277c8079-900e-477e-99d8-4fee1496d370")
            guid = str(uuid.UUID(bytes_le=entries[144:160]))
        esp = at(prepare_disk.ESP_START // 512, prepare_disk.ESP_MIB * 2048)
        assert esp[82:90] == b"FAT32   "
        assert f"root=PARTUUID={guid}".encode() in esp
        assert at(prepare_disk.ROOT_START // 512)[:8] == b"SXFS\0\0\0\0"
        return guid


def check_port_writes(cli: Path, root: Path, capacity: int, work: Path) -> None:
    stage = work / "port"
    save = stage / "games/demo/save.txt"
    save.parent.mkdir(parents=True)
    save.write_bytes(b"saved")
    args = SimpleNamespace(reset=False, no_compact=False)
    recovery = work / "recovery"
    disk = Path("fixture-partition")
    fd = os.open(root, os.O_RDWR)
    try:
        assert sxfs_sync.sync_block_volume(args, disk, fd, capacity, stage, cli, recovery) == 0
        stage = work / "second-port"
        binary = stage / "bin/demo"
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"demo")
        before = root.read_bytes()
        write_volume = sxfs_sync.write_volume

        def write_then_fail(fd: int, image: Path) -> None:
            if image.name == "candidate.img":
                os.pwrite(fd, bytes(512), 0)
                raise OSError("injected write failure")
            write_volume(fd, image)

        with patch.object(sxfs_sync, "write_volume", side_effect=write_then_fail):
            assert sxfs_sync.sync_block_volume(args, disk, fd, capacity, stage, cli, recovery) == 1
        assert root.read_bytes() == before
        assert not list(recovery.iterdir())
        assert sxfs_sync.sync_block_volume(args, disk, fd, capacity, stage, cli, recovery) == 0
        extracted = work / "files"
        subprocess.run([cli, "extract", root, extracted], check=True)
        assert (extracted / "games/demo/save.txt").read_bytes() == b"saved"
        assert (extracted / "bin/demo").read_bytes() == b"demo"
    finally:
        os.close(fd)


def check_device_elevation(work: Path) -> None:
    device = Path("/dev/test-disk")
    target = prepare_disk.DiskTarget(123, 2 << 30, 456)
    sudo = "/test/path/sudo"
    with (patch.object(prepare_disk, "claim", side_effect=PermissionError("denied")),
          patch.object(prepare_disk, "install") as install,
          patch.object(subprocess, "run") as run):
        prepare_disk.install_device(device, target, work, sudo)
        run.assert_called_once_with(
            [sudo, "--", sys.executable, "-E", "-S", "-B", str(ROOT / "tools/prepare_disk.py"),
             "--write-staged", str(device), "123", str(2 << 30), "456", str(work)], check=True)
        install.assert_not_called()

    with (patch.object(prepare_disk, "claim", return_value=nullcontext(11)),
          patch.object(os, "fstat"), patch.object(prepare_disk, "inspect_disk", return_value=target),
          patch.object(prepare_disk, "rescan"), patch.object(prepare_disk, "install") as install,
          patch.object(subprocess, "run") as run):
        prepare_disk.install_device(device, target, work, sudo)
        install.assert_called_once_with(11, target.capacity, work, physical=True)
        run.assert_not_called()

        install.side_effect = PermissionError("write denied after opening the device")
        try:
            prepare_disk.install_device(device, target, work, sudo)
        except PermissionError:
            run.assert_not_called()
        else:
            raise AssertionError("a write failure should leave without retrying as root")

    replacement = prepare_disk.DiskTarget(target.number, target.capacity, target.sequence + 1)
    with (patch.object(prepare_disk, "claim", return_value=nullcontext(11)),
          patch.object(os, "fstat"), patch.object(prepare_disk, "inspect_disk", return_value=replacement),
          patch.object(prepare_disk, "install") as install):
        try:
            prepare_disk.install_device(device, target, work, sudo)
        except ValueError as error:
            assert "changed since staging began" in str(error)
            install.assert_not_called()
        else:
            raise AssertionError("a replaced disk must be rejected")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tools", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="savanxp-install-test-") as temporary:
        work = Path(temporary)
        check_device_elevation(work)
        boot = work / "boot-tree"
        for name in ("EFI/BOOT/BOOTX64.EFI", "boot/kernel.elf", "boot/initramfs.cpio"):
            file = boot / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(name)
        base = work / "base"
        (base / "bin").mkdir(parents=True)
        (base / "bin/init").write_text("fixture")
        sectors = 32768
        image = work / "installed.img"
        command = [sys.executable, ROOT / "tools/prepare_disk.py", "--output", image,
                   "--boot", boot, "--diskfs", base, "--tools", args.tools,
                   "--config", ROOT / "boot/limine-installed.conf", "--sync", ROOT / "tools/sxfs_sync.py",
                   "--python", sys.executable, "--root-sectors", str(sectors), "--sudo", work / "no-sudo"]
        output = subprocess.check_output(command, text=True)
        assert check_gpt(image) in output
        again = subprocess.run(command, text=True, capture_output=True)
        assert again.returncode != 0 and "output already exists" in again.stderr
        root = work / "root.img"
        with image.open("rb") as disk:
            disk.seek(prepare_disk.ROOT_START)
            root.write_bytes(disk.read(sectors * 512))
        check_port_writes(args.tools / "sxfs-cli", root, sectors * 512, work)
    print("prepare-disk and port target: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
