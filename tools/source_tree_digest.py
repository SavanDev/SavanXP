#!/usr/bin/env python3
"""Compute a deterministic digest for a vendored source tree."""

from __future__ import annotations

import argparse
import hashlib
import os
import stat
from pathlib import Path


def tree_digest(root: Path) -> str:
    digest = hashlib.sha256()
    paths = sorted(
        (path for path in root.rglob("*") if path.is_file() or path.is_symlink()),
        key=lambda path: path.relative_to(root).as_posix(),
    )
    for path in paths:
        relative = path.relative_to(root).as_posix().encode("utf-8")
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode):
            kind = b"l"
            payload = os.readlink(path).encode("utf-8")
        elif stat.S_ISREG(info.st_mode):
            kind = b"f"
            executable = bool(info.st_mode & 0o111)
            payload = (b"x" if executable else b"-") + path.read_bytes()
        else:
            raise ValueError(f"unsupported source entry: {path}")
        digest.update(len(relative).to_bytes(4, "big"))
        digest.update(relative)
        digest.update(kind)
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--expected")
    args = parser.parse_args()
    root = args.root.resolve()
    if not root.is_dir():
        parser.error(f"source tree does not exist: {root}")
    actual = tree_digest(root)
    if args.expected and actual != args.expected:
        print(f"source tree digest mismatch: expected {args.expected}, got {actual}")
        return 1
    print(actual)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
