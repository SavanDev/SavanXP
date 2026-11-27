#!/usr/bin/env python3
"""The SDK sysroot is self-contained (docs/roadmaps/done/SDK_V2_ROADMAP.md, phase 2).

Two checks:

- Every header in the sysroot's include/ compiles on its own with only that
  directory on the include path, and nothing it reads comes from outside it
  but the compiler's own freestanding headers. All of them also compile
  together, so no two define the same thing. A public header that includes one the
  allowlist (subsystems/posix/sdk/v1/public-headers.txt) leaves out fails here.
- A copy of the sysroot in a temporary directory, away from the repository,
  builds sdk/hello, sdk/gfxhello (--gui), sdk/nativetest (--native),
  sdk/sungui (--native --gui, on SUN's application frame), a --heap-mib
  program and sdk/piesmoke (--pie --needs libmath.so.0.4, checked to be
  ET_DYN with the loader's PT_INTERP and the DT_NEEDED) with its own
  share/sdk/build-user.sh; and sdk/glsmoke, which includes <GL/gl.h>, twice:
  --pie with SxGL linked in, and --pie --needs libsxgl.so.0.4, checked to
  name the library.
- The same copy's sxcc builds static programs in one step and in two, in
  every profile, a SUN program and one linking -lsxgui; and CMake with its
  share/sdk/savanxp-toolchain.cmake builds sdk/cmakesmoke.
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
    for name in ("hello", "gfxhello", "nativetest", "sungui", "piesmoke", "glsmoke", "cmakesmoke"):
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
        ("piesmoke", ["--source", str(sources / "piesmoke"), "--pie", "--needs", "libmath.so.0.4"]),
        ("glsmoke", ["--source", str(sources / "glsmoke"), "--pie"]),
        ("glsmoke-needs", ["--source", str(sources / "glsmoke"), "--pie", "--needs", "libsxgl.so.0.4"]),
    ]
    for name, arguments in builds:
        output = scratch / f"{name}.elf"
        run([builder, *arguments, "--name", name, "--no-install", "--output", str(output)],
            cwd=scratch, env=environment)
        if output.read_bytes()[:4] != b"\x7fELF":
            raise SystemExit(f"{name}: {output} is not an ELF image")
    # The PIE one is ET_DYN, asks for the loader's path and names libmath.
    readelf = shutil.which("llvm-readelf") or "llvm-readelf"
    dump = run([readelf, "-h", "-l", "-d", str(scratch / "piesmoke.elf")]).stdout
    for expected in ("DYN (Shared object file)", "[Requesting program interpreter: /lib/ld.so.0.4]",
                     "Shared library: [libmath.so.0.4]"):
        if expected not in dump:
            raise SystemExit(f"piesmoke: '{expected}' missing from its headers:\n{dump}")
    # SxGL as a library: with --needs the gl units leave the program and the
    # library is a DT_NEEDED.
    dump = run([readelf, "-d", str(scratch / "glsmoke-needs.elf")]).stdout
    if "Shared library: [libsxgl.so.0.4]" not in dump:
        raise SystemExit(f"glsmoke-needs: libsxgl.so.0.4 missing from its dynamic section:\n{dump}")


def check_toolchain(scratch: Path, clang: str, linker: str) -> None:
    """sxcc and the CMake toolchain file, from the same copy of the sysroot."""
    copy = scratch / "sdk"
    sources = scratch / "src"
    sxcc = str(copy / "share" / "sdk" / "sxcc")
    environment = os.environ.copy()
    environment["SAVANXP_CLANG"] = clang
    environment.pop("SAVANXP_SDK_PROFILE", None)
    readelf = shutil.which("llvm-readelf") or "llvm-readelf"
    out = scratch / "sxcc"
    out.mkdir()
    # One step and two steps, every profile, a SUN program and an archive by -l.
    run([sxcc, "--sx-profile=nosse", str(sources / "hello" / "main.c"), "-o", str(out / "nosse")],
        env=environment)
    run([sxcc, "-c", str(sources / "hello" / "main.c"), "-o", str(out / "hello.o")], env=environment)
    run([sxcc, str(out / "hello.o"), "-o", str(out / "sse")], env=environment)
    run([sxcc, str(sources / "gfxhello" / "main.c"), "-lsxgui", "-o", str(out / "gui")], env=environment)
    run([sxcc, "--sx-native", str(sources / "nativetest" / "main.c"), "-o", str(out / "native")],
        env=environment)
    run([sxcc, "--sx-profile=pie", str(sources / "piesmoke" / "main.c"), "-o", str(out / "pie"),
         str(copy / "lib" / "shared" / "libmath.so.0.4")], env=environment)
    for name in ("nosse", "sse", "gui", "native"):
        if "EXEC (Executable file)" not in run([readelf, "-h", str(out / name)]).stdout:
            raise SystemExit(f"sxcc: {name} is not a static executable")
    dump = run([readelf, "-h", "-d", str(out / "pie")]).stdout
    if "DYN (Shared object file)" not in dump or "[libmath.so.0.4]" not in dump:
        raise SystemExit(f"sxcc: the pie profile did not link a PIE against libmath:\n{dump}")
    symbols = run([shutil.which("llvm-nm") or "llvm-nm", str(out / "native")]).stdout
    if " T sx_heap_pages_map" not in symbols:
        raise SystemExit("sxcc --sx-native did not link SUN's heap page source")

    # CMake with the toolchain file, as sdk/cmakesmoke/build.sh does.
    build = scratch / "cmake-build"
    run(["cmake", "-S", str(sources / "cmakesmoke"), "-B", str(build), "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={copy / 'share' / 'sdk' / 'savanxp-toolchain.cmake'}"],
        env=environment)
    run(["cmake", "--build", str(build)], env=environment)
    if "EXEC (Executable file)" not in run([readelf, "-h", str(build / "cmakesmoke")]).stdout:
        raise SystemExit("cmake: cmakesmoke is not a static executable")


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
        check_toolchain(scratch, clang, linker)
    print(f"sdk sysroot: {count} headers self-contained; a copy builds six programs with "
          "build-user.sh, five with sxcc and one with CMake: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
