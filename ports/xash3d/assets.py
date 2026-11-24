"""Stage user-supplied Half-Life assets for the SavanXP port."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import struct


def local_files(directory: Path):
    for path in sorted(directory.rglob("*")):
        if path.is_file() and not path.is_symlink():
            yield path


def pack_sounds(valve: Path, archive: Path) -> int:
    records = []
    with archive.open("wb") as output:
        output.write(b"PACK" + bytes(8))
        for asset in local_files(valve / "sound"):
            name = os.fsencode(asset.relative_to(valve).as_posix())
            if len(name) >= 56:
                raise ValueError(f"PAK name is too long: {asset.relative_to(valve)}")
            offset = output.tell()
            with asset.open("rb") as source:
                shutil.copyfileobj(source, output)
            records.append(struct.pack("<56sII", name, offset, output.tell() - offset))
        if not 0 < len(records) <= 65536:
            raise ValueError(f"expected 1 to 65536 sound files, found {len(records)}")
        offset = output.tell()
        if offset + 64 * len(records) > 0x7fffffff:
            raise ValueError("sound PAK exceeds the engine's signed 32-bit format")
        output.writelines(records)
        output.seek(4)
        output.write(struct.pack("<II", offset, 64 * len(records)))
    return len(records)


def stage_assets(valve: Path, stage: Path) -> None:
    metadata = [valve / name for name in ("liblist.gam", "gameinfo.txt")]
    if not valve.is_dir() or not any(path.is_file() and not path.is_symlink() for path in metadata):
        raise ValueError(f"{valve} is not an unpacked Half-Life valve directory")

    target = stage / "games/xash3d/valve"
    target.mkdir(parents=True, exist_ok=True)
    excluded = {"cl_dlls", "dlls", "hw", "save", "sound"}
    for path in local_files(valve):
        relative = path.relative_to(valve)
        if relative.parts[0].lower() in excluded or path.name.startswith("."):
            continue
        if path.suffix.lower() in {".dll", ".exe", ".so", ".dylib"}:
            continue
        if any(char in relative.as_posix() for char in "\t\r\n"):
            raise ValueError(f"SxFS cannot install this asset name: {str(relative)!r}")
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)

    if next(local_files(valve / "sound"), None) is not None:
        index = 0
        archive = target / "savanxp-sounds.pak"
        while archive.exists():
            index += 1
            archive = target / f"savanxp-sounds-{index}.pak"
        count = pack_sounds(valve, archive)
        print(f"xash3d: packed {count} sound files into {archive.name}", flush=True)

    # SxFS assigns files under the profile to the desktop user. Xash3D can
    # write saves and settings there while mounting the installed data as rodir.
    (stage / "users/user/games/xash3d/valve").mkdir(parents=True, exist_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("valve", type=Path)
    parser.add_argument("stage", type=Path)
    args = parser.parse_args()
    try:
        stage_assets(args.valve, args.stage)
    except (OSError, ValueError) as error:
        parser.exit(1, f"xash3d: {error}\n")


if __name__ == "__main__":
    main()
