#!/usr/bin/env python3
"""Stamp EI_OSABI on an ELF so the kernel runs it as a native (Sx) process.

The value comes from SXN_ELF_OSABI_NATIVE in the neutral ABI header, the same
source tools/build-user.sh --native reads, so the two cannot disagree.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def native_osabi(header: Path) -> int:
    match = re.search(r"^#define\s+SXN_ELF_OSABI_NATIVE\s+0x([0-9A-Fa-f]+)", header.read_text(), re.MULTILINE)
    if match is None:
        raise SystemExit(f"stamp_osabi: {header} defines no SXN_ELF_OSABI_NATIVE")
    return int(match.group(1), 16)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--abi-header", required=True, type=Path)
    args = parser.parse_args()

    data = bytearray(args.binary.read_bytes())
    if len(data) < 16 or data[:4] != b"\x7fELF":
        raise SystemExit(f"stamp_osabi: {args.binary} is not an ELF")
    data[7] = native_osabi(args.abi_header)
    args.binary.write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
