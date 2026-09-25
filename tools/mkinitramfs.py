#!/usr/bin/env python3
"""Write a deterministic newc cpio archive for the SavanXP initramfs."""

from __future__ import annotations

import argparse
import os
from pathlib import Path


def align4(stream) -> None:
    while stream.tell() & 3:
        stream.write(b"\0")


def write_entry(stream, name: str, data: bytes | None, mode: int) -> None:
    name_bytes = name.encode("ascii")
    name_size = len(name_bytes) + 1
    file_size = len(data) if data is not None else 0
    # All inode numbers are zero: the archive is consumed as a tree, not as a
    # filesystem that needs stable inode identity across rebuilds.
    fields = (
        0,
        mode,
        0,
        0,
        1,
        0,
        file_size,
        0,
        0,
        0,
        0,
        name_size,
        0,
    )
    stream.write(b"070701")
    stream.write(b"".join(f"{value:08x}".encode("ascii") for value in fields))
    stream.write(name_bytes)
    stream.write(b"\0")
    align4(stream)
    if data:
        stream.write(data)
        align4(stream)


def collect_entries(root: Path) -> list[tuple[str, Path]]:
    entries: list[tuple[str, Path]] = []
    for current, directories, files in os.walk(root):
        current_path = Path(current)
        directories.sort()
        files.sort()
        for name in directories + files:
            path = current_path / name
            relative = path.relative_to(root).as_posix()
            entries.append((relative, path))
    entries.sort(key=lambda item: item[0])
    return entries


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source = args.source.resolve()
    if not source.is_dir():
        raise SystemExit(f"initramfs: source directory does not exist: {source}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as stream:
        for relative, path in collect_entries(source):
            if path.is_symlink():
                raise SystemExit(f"initramfs: symlinks are not supported: {path}")
            if path.is_dir():
                write_entry(stream, relative, None, 0o040755)
                continue
            if not path.is_file():
                raise SystemExit(f"initramfs: unsupported filesystem entry: {path}")
            mode = 0o100755 if relative.startswith("bin/") else 0o100644
            write_entry(stream, relative, path.read_bytes(), mode)
        write_entry(stream, "TRAILER!!!", None, 0)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
