#!/usr/bin/env python3
"""Stage the persistent volume for the ISO at the size a LiveCD should carry.

The ISO and the development machine deliberately have different volumes. The
development image is the full-size one, because there is no reason for it to be
small. The ISO carries a smaller volume on purpose: it is a bootable demo, it has
to fit on optical media, and it is loaded into RAM as a Limine module, so every
byte of it is also a byte of guest memory.

The ISO tree therefore gets its own rebuilt copy rather than the development
image. Two things follow from that, and both used to be wrong:

- A grown development image has a sparse tail that is reserved room. Copied
  into the ISO it becomes real zero bytes: a 64 MiB volume in a 512 MiB file
  turned a 98 MB ISO into a 541 MB one.
- A volume of a different size is not the same volume. Shrinking a filesystem
  means rebuilding it, so this extracts, recreates at the requested size, and
  applies, which is exactly what a compaction does.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SECTOR_SIZE = 512


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(detail: str) -> None:
    raise SystemExit(f"iso-volume: {detail}")


def total_sectors(cli: Path, image: Path) -> int:
    result = run([str(cli), "info", str(image)])
    if result.returncode != 0:
        fail(f"sxfs-cli info failed: {(result.stderr or result.stdout).strip()}")
    for line in reversed(result.stdout.splitlines()):
        try:
            sectors = int(line.strip())
        except ValueError:
            continue
        if sectors > 0:
            return sectors
    fail("sxfs-cli info did not report a sector count")


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
            # Mismo offset en el destino: sin esto los datos se escriben en
            # secuencia y la copia se corrompe con cualquier hueco intermedio.
            writer.seek(data_start)
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


def build_manifest(root: Path) -> Path:
    """A manifest of everything under `root`, directories before their contents."""
    entries = []
    for path in sorted(root.rglob("*"), key=lambda item: (len(item.parts), item.as_posix())):
        relative = path.relative_to(root).as_posix()
        if path.is_dir():
            entries.append(f"mkdir\t{relative}")
        else:
            entries.append(f"file\t{relative}\t{path}")
    manifest = root.parent / "iso-volume.manifest"
    manifest.write_text("\n".join(entries) + "\n", encoding="utf-8")
    return manifest


def stage(cli: Path, image: Path, output: Path, sectors: int) -> None:
    source_sectors = total_sectors(cli, image)
    if source_sectors == sectors:
        # Same size: a sparse copy is enough, and it is the cheap path.
        copy_preserving_holes(image, output)
        print(f"iso-volume: copied {sectors} sectors as-is", flush=True)
        return

    if source_sectors < sectors:
        fail(
            f"the development image is {source_sectors} sectors and the ISO asks for "
            f"{sectors}; growing a volume is a separate operation, not a copy"
        )

    with tempfile.TemporaryDirectory(prefix="savanxp-iso-volume.") as temporary:
        workspace = Path(temporary)
        extract = workspace / "extract"
        rebuilt = workspace / "rebuilt.img"
        result = run([str(cli), "extract", str(image), str(extract)])
        if result.returncode != 0:
            fail(f"extract failed: {(result.stderr or result.stdout).strip()}")
        manifest = build_manifest(extract)
        result = run([str(cli), "create", str(rebuilt), str(sectors)])
        if result.returncode != 0:
            fail(f"create at {sectors} sectors failed: {(result.stderr or result.stdout).strip()}")
        result = run([str(cli), "apply", str(rebuilt), str(manifest)])
        if result.returncode != 0:
            fail(
                f"the volume does not fit in {sectors} sectors: "
                f"{(result.stderr or result.stdout).strip()}"
            )
        # sxfs-cli create already leaves the image sparse; copy across so the
        # move into the tree stays a plain file operation.
        copy_preserving_holes(rebuilt, output)

    print(
        f"iso-volume: rebuilt a {source_sectors}-sector volume as {sectors} sectors",
        flush=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument(
        "--sectors",
        type=int,
        default=524288,
        help="Sector count of the LiveCD volume (default 524288 = 256 MiB).",
    )
    args = parser.parse_args()

    image = args.image.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if args.sectors <= 0:
        fail("--sectors must be positive")
    stage(args.cli.resolve(), image, output, args.sectors)

    expected = args.sectors * SECTOR_SIZE
    actual = output.stat().st_size
    if actual != expected:
        output.unlink(missing_ok=True)
        fail(f"staged {actual} bytes, expected {expected}")

    check = run([str(args.cli), "check", str(output)])
    if check.returncode != 0:
        detail = (check.stderr or check.stdout).strip()
        output.unlink(missing_ok=True)
        fail(f"the staged volume did not validate: {detail}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
