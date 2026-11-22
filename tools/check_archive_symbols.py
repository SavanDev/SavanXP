#!/usr/bin/env python3
"""Fail when static archives ask the program for a symbol outside a list.

A library built for SUN programs may only call C functions the runtime
defines without a system call (docs/desktop/WEBVIEWER_ENGINE.md). The archives'
undefined symbols, less what any of them defines, must all be in --allow.
"""
import argparse
import pathlib
import subprocess
import sys


def symbols(nm: str, archive: str, defined: bool) -> set[str]:
    flag = "--defined-only" if defined else "--undefined-only"
    out = subprocess.run([nm, flag, "--format=just-symbols", archive],
                         check=True, capture_output=True, text=True).stdout
    return {line.strip() for line in out.splitlines()
            if line.strip() and not line.endswith(":")}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--allow", required=True, help="comma-separated symbols")
    parser.add_argument("--stamp", required=True)
    parser.add_argument("archives", nargs="+")
    args = parser.parse_args()
    allowed = {name for name in args.allow.split(",") if name}
    defined: set[str] = set()
    undefined: set[str] = set()
    for archive in args.archives:
        defined |= symbols(args.nm, archive, True)
        undefined |= symbols(args.nm, archive, False)
    unexpected = sorted(undefined - defined - allowed)
    if unexpected:
        print(f"{args.name}: the libraries ask the program for more than SUN's "
              "runtime gives without a system call:", file=sys.stderr)
        for name in unexpected:
            print(f"  {name}", file=sys.stderr)
        return 1
    pathlib.Path(args.stamp).write_text("checked\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
