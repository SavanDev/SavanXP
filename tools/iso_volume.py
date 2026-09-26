#!/usr/bin/env python3
"""Stage the persistent volume for the ISO at exactly its filesystem size.

The development image is deliberately allowed to be larger than the filesystem
inside it, so there is room to grow the volume later. An ISO has no use for that
room: the volume travels as a Limine module, and a sparse tail copied into the
ISO tree turns into real zero bytes. Measured on a 64 MiB volume inside a 512 MiB
file, the ISO went from 98 MB to 541 MB, all of it padding.

So the ISO tree gets its own copy, truncated to what the superblock declares.
The development image is never modified.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def total_sectors(cli: Path, image: Path) -> int:
    result = run([str(cli), "info", str(image)])
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise SystemExit(f"iso-volume: sxfs-cli info failed: {detail}")
    for line in reversed(result.stdout.splitlines()):
        try:
            sectors = int(line.strip())
        except ValueError:
            continue
        if sectors > 0:
            return sectors
    raise SystemExit("iso-volume: sxfs-cli info did not report a sector count")


def copy_preserving_holes(source: Path, destination: Path) -> None:
    """Same extent walk as tools/sxfs_sync.py, so the copy stays cheap."""
    size = source.stat().st_size
    with source.open("rb") as reader, destination.open("wb") as writer:
        offset = 0
        while offset < size:
            try:
                reader.seek(offset, os.SEEK_DATA)
            except OSError:
                break
            data_start = reader.tell()
            try:
                reader.seek(data_start, os.SEEK_HOLE)
            except OSError:
                break
            data_end = min(reader.tell(), size)
            reader.seek(data_start)
            remaining = data_end - data_start
            while remaining > 0:
                chunk = reader.read(min(remaining, 1 << 20))
                if not chunk:
                    break
                writer.write(chunk)
                remaining -= len(chunk)
            offset = data_end
        writer.truncate(size)
        writer.flush()
        os.fsync(writer.fileno())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    args = parser.parse_args()

    image = args.image.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    sectors = total_sectors(args.cli.resolve(), image)
    declared = sectors * 512
    copy_preserving_holes(image, output)

    image_size = image.stat().st_size
    if image_size > declared:
        os.truncate(output, declared)
        with output.open("rb") as handle:
            os.fsync(handle.fileno())
        trimmed = declared
    else:
        trimmed = output.stat().st_size

    check = run([str(args.cli), "check", str(output)])
    if check.returncode != 0:
        detail = (check.stderr or check.stdout).strip()
        output.unlink(missing_ok=True)
        raise SystemExit(f"iso-volume: the staged volume did not validate: {detail}")

    if trimmed != declared:
        print(f"iso-volume: unexpected staged size {trimmed}, expected {declared}", file=sys.stderr)
        return 1
    print(
        f"iso-volume: staged {trimmed} bytes ({sectors} sectors)"
        + (f" from a {image_size}-byte image" if image_size > declared else ""),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
