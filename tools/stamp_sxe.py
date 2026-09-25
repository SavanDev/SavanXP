#!/usr/bin/env python3
"""Stamp generated SXE sections into an ELF and verify they stay non-alloc."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> int:
    print(f"sxe: {message}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--resource-dir", required=True, type=Path)
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("--readelf", required=True)
    args = parser.parse_args()

    binary = args.binary.resolve()
    sections: list[tuple[str, Path]] = []
    for section, suffix in ((".sxmeta", ".sxmeta"), (".sxicon", ".sxicon")):
        path = args.resource_dir / f"{args.name}{suffix}"
        if path.is_file():
            sections.append((section, path.resolve()))
    if not sections:
        return 0

    temporary = binary.with_name(binary.name + ".sxe-tmp")
    temporary.unlink(missing_ok=True)
    command = [args.objcopy]
    for section, path in sections:
        command.append(f"--add-section={section}={path}")
    command.extend((str(binary), str(temporary)))
    try:
        result = subprocess.run(command, text=True, capture_output=True, check=False)
    except OSError as exc:
        return fail(f"could not execute objcopy: {exc}")
    if result.returncode != 0:
        if result.stdout:
            print(result.stdout, file=sys.stderr, end="")
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        temporary.unlink(missing_ok=True)
        return fail(f"objcopy failed for {args.name}")

    try:
        details = subprocess.run(
            [args.readelf, "--section-details", str(temporary)],
            text=True,
            capture_output=True,
            check=False,
        )
    except OSError as exc:
        temporary.unlink(missing_ok=True)
        return fail(f"could not execute readelf: {exc}")
    if details.returncode != 0:
        temporary.unlink(missing_ok=True)
        return fail(f"readelf failed for {args.name}")

    lines = details.stdout.splitlines()
    name_pattern = re.compile(r"^\s*\[\s*\d+\]\s+(\.\S+)\s*$")
    flags_pattern = re.compile(r"^\s*\[([0-9a-fA-F]+)\]:")
    flags_by_name: dict[str, int] = {}
    for index, line in enumerate(lines):
        match = name_pattern.match(line)
        if not match:
            continue
        section_name = match.group(1)
        for flag_line in lines[index + 1 :]:
            flag_match = flags_pattern.match(flag_line)
            if flag_match:
                flags_by_name[section_name] = int(flag_match.group(1), 16)
                break

    for section, _ in sections:
        flags = flags_by_name.get(section)
        if flags is None:
            temporary.unlink(missing_ok=True)
            return fail(f"{section} missing after stamping {args.name}")
        if flags & 0x2:
            temporary.unlink(missing_ok=True)
            return fail(f"{section} is SHF_ALLOC for {args.name}")

    shutil.copymode(binary, temporary)
    os.replace(temporary, binary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
