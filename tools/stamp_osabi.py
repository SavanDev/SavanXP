#!/usr/bin/env python3
"""Stamp EI_OSABI on an ELF so the kernel runs it as a native (Sx) process.

The value comes from SXN_ELF_OSABI_NATIVE in the neutral ABI header, the same
source tools/build-user.sh --native reads, so the two cannot disagree.

It also enforces the SUN program rule (docs/userland/SXE_FORMAT.md, "SUN
programs are named .sxe"): the image carries a .sxmeta section and its name on
disk ends in .sxe. The kernel never checks either, so the build has to.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

SXE_SUFFIX = ".sxe"


def native_osabi(header: Path) -> int:
    match = re.search(r"^#define\s+SXN_ELF_OSABI_NATIVE\s+0x([0-9A-Fa-f]+)", header.read_text(), re.MULTILINE)
    if match is None:
        raise SystemExit(f"stamp_osabi: {header} defines no SXN_ELF_OSABI_NATIVE")
    return int(match.group(1), 16)


def section_names(data: bytes) -> set[str]:
    """Section names of a little-endian ELF64, read from its section headers."""
    if len(data) < 64 or data[4] != 2 or data[5] != 1:
        raise SystemExit("stamp_osabi: only little-endian ELF64 images are supported")
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    if shoff == 0 or shnum == 0 or shstrndx >= shnum or shoff + shnum * shentsize > len(data):
        return set()
    strtab_header = shoff + shstrndx * shentsize
    strtab_offset, strtab_size = struct.unpack_from("<QQ", data, strtab_header + 0x18)
    strtab = data[strtab_offset : strtab_offset + strtab_size]
    names: set[str] = set()
    for index in range(shnum):
        name_offset = struct.unpack_from("<I", data, shoff + index * shentsize)[0]
        end = strtab.find(b"\0", name_offset)
        if end >= 0:
            names.add(strtab[name_offset:end].decode("ascii", "replace"))
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--abi-header", required=True, type=Path)
    parser.add_argument(
        "--installed-name",
        required=True,
        help="the file name the program will have on disk, which must end in .sxe",
    )
    args = parser.parse_args()

    if not args.installed_name.endswith(SXE_SUFFIX) or args.installed_name == SXE_SUFFIX:
        raise SystemExit(
            f"stamp_osabi: a native program is named <name>.sxe on disk, not '{args.installed_name}'"
        )
    data = bytearray(args.binary.read_bytes())
    if len(data) < 16 or data[:4] != b"\x7fELF":
        raise SystemExit(f"stamp_osabi: {args.binary} is not an ELF")
    if ".sxmeta" not in section_names(data):
        raise SystemExit(
            f"stamp_osabi: {args.installed_name} has no .sxmeta; a native program is an SXE image"
        )
    data[7] = native_osabi(args.abi_header)
    args.binary.write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
