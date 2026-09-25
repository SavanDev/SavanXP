#!/usr/bin/env python3
"""Write the optional init smoke specification into the staged rootfs."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--command", required=True)
    args = parser.parse_args()
    try:
        encoded = args.command.encode("ascii", errors="strict")
    except UnicodeEncodeError as exc:
        raise SystemExit("smoke command must contain only ASCII characters") from exc
    if not args.command or len(encoded) > 63 or "\x00" in args.command or "\n" in args.command or "\r" in args.command:
        raise SystemExit("smoke command must be one non-empty ASCII line of at most 63 bytes")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(args.command + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
