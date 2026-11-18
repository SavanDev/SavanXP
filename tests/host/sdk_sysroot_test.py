#!/usr/bin/env python3
"""The SDK sysroot is self-contained (docs/roadmaps/open/SDK_V2_ROADMAP.md, phase 2).

Two checks:

- Every header in the sysroot's include/ compiles on its own with only that
  directory on the include path, and nothing it reads comes from outside it
  but the compiler's own freestanding headers. All of them also compile
  together, so no two define the same thing. A public header that includes one the
  allowlist (subsystems/posix/sdk/v1/public-headers.txt) leaves out fails here.
- A copy of the sysroot in a temporary directory, away from the repository,
  builds sdk/hello, sdk/gfxhello (--gui), sdk/nativetest (--native),
  sdk/sungui (--native --gui, on SUN's application frame) and a --heap-mib
  program with its own share/sdk/build-user.sh.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True, check=False, **kwargs)
    if result.returncode != 0:
        raise SystemExit(
            f"failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def check_headers(sysroot: Path, clang: str, scratch: Path) -> int:
    """Each header alone, in its own translation unit, so a header that needs
    another without including it is caught; then all of them together."""
    include = sysroot / "include"
    resource = Path(run([clang, "-print-resource-dir"]).stdout.strip()).resolve()
    headers = sorted(path.relative_to(include).as_posix() for path in include.rglob("*.h"))
    outside: set[str] = set()
    failures = []
    for index, header in enumerate(headers):
        probe = scratch / f"probe-{index}.c"
        probe.write_text(f'#include "{header}"\n', encoding="utf-8")
        depfile = scratch / f"probe-{index}.d"
        result = subprocess.run(
            [clang, "-target", "x86_64-unknown-none-elf", "-ffreestanding", "-msse", "-msse2",
             "-fsyntax-only", "-MD", "-MF", str(depfile), "-I", str(include), str(probe)],
            text=True, capture_output=True, check=False)
        if result.returncode != 0:
            failures.append(f"{header}:\n{result.stderr}")
            continue
        tokens = depfile.read_text(encoding="utf-8").replace("\\\n", " ").split()
        for token in tokens:
            if token.endswith(":") or token == str(probe):
                continue
            path = Path(token).resolve()
            if not (path.is_relative_to(include.resolve()) or path.is_relative_to(resource)):
                outside.add(f"{header} -> {path}")
    if failures:
        raise SystemExit("sysroot headers that do not compile on their own:\n" + "\n".join(failures))
    # And all of them together, so two headers cannot define the same thing:
    # sys/time.h and sys/select.h once both defined struct timeval.
    together = scratch / "probe-all.c"
    together.write_text("".join(f'#include "{header}"\n' for header in headers), encoding="utf-8")
    result = subprocess.run(
        [clang, "-target", "x86_64-unknown-none-elf", "-ffreestanding", "-msse", "-msse2",
         "-fsyntax-only", "-I", str(include), str(together)],
        text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise SystemExit("sysroot headers that conflict when included together:\n" + result.stderr)
    if outside:
        raise SystemExit("sysroot headers read files outside the sysroot:\n  " + "\n  ".join(sorted(outside)))
    return len(headers)


def check_standalone(sysroot: Path, root: Path, clang: str, linker: str, scratch: Path) -> None:
    copy = scratch / "sdk"
    shutil.copytree(sysroot, copy)
    sources = scratch / "src"
    for name in ("hello", "gfxhello", "nativetest", "sungui"):
        shutil.copytree(root / "sdk" / name, sources / name)
    environment = os.environ.copy()
    environment["SAVANXP_CLANG"] = clang
    environment["SAVANXP_LD"] = linker
    environment["SAVANXP_OUTPUT_ROOT"] = str(scratch / "out")
    builder = str(copy / "share" / "sdk" / "build-user.sh")
    builds = [
        ("hello", ["--source", str(sources / "hello")]),
        ("gfxhello", ["--source", str(sources / "gfxhello"), "--gui"]),
        ("nativetest", ["--source", str(sources / "nativetest"), "--native"]),
        ("sungui", ["--source", str(sources / "sungui"), "--native", "--gui"]),
        ("hello-heap", ["--source", str(sources / "hello"), "--heap-mib", "2"]),
    ]
    for name, arguments in builds:
        output = scratch / f"{name}.elf"
        run([builder, *arguments, "--name", name, "--no-install", "--output", str(output)],
            cwd=scratch, env=environment)
        if output.read_bytes()[:4] != b"\x7fELF":
            raise SystemExit(f"{name}: {output} is not an ELF image")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--ld", default="ld.lld")
    args = parser.parse_args()
    root = args.root.resolve()
    clang = shutil.which(args.clang) or args.clang
    linker = shutil.which(args.ld) or args.ld

    make_sdk = str(root / "tools" / "make-sdk.sh")
    environment = os.environ.copy()
    environment["SAVANXP_CLANG"] = clang
    run([make_sdk, "--quiet"], env=environment)
    sysroot = Path(run([make_sdk, "--print-path"], env=environment).stdout.strip())

    with tempfile.TemporaryDirectory(prefix="savanxp-sdk-sysroot.") as temporary:
        scratch = Path(temporary)
        count = check_headers(sysroot, clang, scratch)
        check_standalone(sysroot, root, clang, linker, scratch)
    print(f"sdk sysroot: {count} headers self-contained; a copy builds five programs: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
