#!/usr/bin/env python3
"""Regression test for additive SxFS synchronization and safe compaction."""

from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sxfs_checksum(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def assert_pending_journal_rejected(cli: Path, image: Path, root: Path) -> None:
    pending = root / "pending.img"
    pending.write_bytes(image.read_bytes())
    data = bytearray(pending.read_bytes())
    sequence = struct.unpack_from("<I", data, 16)[0]
    header = bytearray(struct.pack("<8sIIII", b"SXJN\0\0\0\0", 0, sequence + 1, 1, 97))
    struct.pack_into("<I", header, 8, sxfs_checksum(header))
    data[2 * 512 : 3 * 512] = header
    pending.write_bytes(data)
    result = subprocess.run(
        [str(cli), "check", str(pending)], cwd=root, text=True, capture_output=True, check=False
    )
    if result.returncode == 0:
        raise RuntimeError("pending journal was accepted by sxfs-cli check")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--sync", required=True, type=Path)
    args = parser.parse_args()
    cli = args.cli.resolve()
    sync = args.sync.resolve()

    with tempfile.TemporaryDirectory(prefix="savanxp-sxfs-compaction-") as temporary:
        root = Path(temporary)
        seed = root / "seed"
        source = root / "source"
        seed.mkdir()
        source.mkdir()
        names = ("external", "a", "b", "c", "d", "e", "f", "g", "h")
        payload = b"savanxp-compaction-payload\n" * 1200
        for name in names:
            (seed / f"{name}.bin").write_bytes(payload)
        new_payload = b"new-build-file\n" * 12000
        (source / "new.bin").write_bytes(new_payload)
        manifest = root / "seed.manifest"
        manifest.write_text(
            "\n".join(f"file\t{name}.bin\t{seed / (name + '.bin')}" for name in names) + "\n",
            encoding="utf-8",
        )

        image = root / "disk.img"
        run([str(cli), "create", str(image), "1000"], root)
        run([str(cli), "apply", str(image), str(manifest)], root)
        run([str(cli), "rm", str(image), "a.bin", "c.bin", "e.bin", "g.bin"], root)
        before_blocked_sync = digest(image)
        blocked = subprocess.run(
            [
                sys.executable,
                str(sync),
                "--image",
                str(image),
                "--source",
                str(source),
                "--cli",
                str(cli),
                "--sectors",
                "1000",
                "--no-compact",
            ],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        if blocked.returncode != 3 or digest(image) != before_blocked_sync:
            raise RuntimeError("a blocked candidate apply modified the persistent image")

        capacity_source = root / "capacity-source"
        capacity_source.mkdir()
        (capacity_source / "too-large.bin").write_bytes(b"x" * 450000)
        capacity_image = root / "capacity.img"
        capacity_image.write_bytes(image.read_bytes())
        before_capacity_failure = digest(capacity_image)
        capacity = subprocess.run(
            [
                sys.executable,
                str(sync),
                "--image",
                str(capacity_image),
                "--source",
                str(capacity_source),
                "--cli",
                str(cli),
                "--sectors",
                "1000",
            ],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        if capacity.returncode != 3 or digest(capacity_image) != before_capacity_failure:
            raise RuntimeError("real capacity exhaustion replaced the persistent image")

        sync_result = run(
            [
                sys.executable,
                str(sync),
                "--image",
                str(image),
                "--source",
                str(source),
                "--cli",
                str(cli),
                "--sectors",
                "1000",
            ],
            root,
        )
        if "compactacion segura completada" not in (sync_result.stdout + sync_result.stderr):
            raise RuntimeError("fragmentation test did not exercise safe compaction")

        extracted = root / "extracted"
        run([str(cli), "extract", str(image), str(extracted)], root)
        for name in ("external", "b", "d", "f", "h"):
            expected = seed / f"{name}.bin"
            actual = extracted / f"{name}.bin"
            if digest(expected) != digest(actual):
                raise RuntimeError(f"preserved file differs: {name}")
        if digest(source / "new.bin") != digest(extracted / "new.bin"):
            raise RuntimeError("new build file differs after compaction")
        assert_pending_journal_rejected(cli, image, root)
        if list(root.glob(f"{image.name}.compact-*")) or list(root.glob(f"{image.name}.pre-compact")):
            raise RuntimeError("successful compaction left a staging or recovery file")

    print("sxfs compaction regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
