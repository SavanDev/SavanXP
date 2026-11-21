#!/usr/bin/env python3
"""Check the SUBSYSTEM record tools/gen_sxe_resources.py stamps.

SUBSYSTEM is the informational mirror of EI_OSABI (docs/userland/SXE_FORMAT.md),
and the build decides EI_OSABI. So:

- a --native-program without a .sxres gets SUBSYSTEM native (0x53), and a
  plain --program gets posix (0);
- a .sxres that declares the subsystem the build matches is accepted;
- a .sxres that declares the other one is a build error.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

TAG_SUBSYSTEM = 0x0202
NATIVE = 0x53
POSIX = 0


def subsystem_of(blob: bytes) -> int | None:
    if blob[:4] != b"SXME":
        raise AssertionError("not a .sxmeta blob")
    header_bytes, blob_bytes, count = struct.unpack_from("<HII", blob, 6)
    offset = header_bytes
    for _ in range(count):
        tag, _flags, length = struct.unpack_from("<HHI", blob, offset)
        payload = blob[offset + 8 : offset + 8 + length]
        if tag == TAG_SUBSYSTEM:
            return payload[0]
        offset += 8 + ((length + 3) & ~3)
    if offset != blob_bytes:
        raise AssertionError("records do not add up to blob_bytes")
    return None


def generate(root: Path, work: Path, manifests: dict[str, str], programs: list[str],
             native: list[str]) -> subprocess.CompletedProcess[str]:
    sources = work / "src"
    output = work / "out"
    sources.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    for name, text in manifests.items():
        (sources / f"{name}.sxres").write_text(text)
    command = [sys.executable, str(root / "tools/gen_sxe_resources.py"),
               "--project-root", str(root), "--manifest-dir", str(sources),
               "--output-dir", str(output), "--build-id", "test"]
    for name in programs:
        command += ["--program", name]
    for name in native:
        command += ["--native-program", name]
    return subprocess.run(command, text=True, capture_output=True, check=False)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    root = parser.parse_args().root.resolve()
    failures = []

    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory) / "defaults"
        result = generate(root, work, {"declared": "subsystem=native\n"},
                          ["sunapp", "posixapp", "declared"], ["sunapp", "declared"])
        if result.returncode != 0:
            failures.append(f"defaults: generator failed: {result.stderr.strip()}")
        else:
            for name, expected in (("sunapp", NATIVE), ("posixapp", POSIX), ("declared", NATIVE)):
                got = subsystem_of((work / "out" / f"{name}.sxmeta").read_bytes())
                if got != expected:
                    failures.append(f"{name}: SUBSYSTEM {got}, expected {expected:#x}")

        for name, text, native in (("liar", "subsystem=posix\n", ["liar"]),
                                   ("liar", "subsystem=native\n", [])):
            work = Path(directory) / f"mismatch-{len(native)}"
            result = generate(root, work, {name: text}, [name], native)
            if result.returncode == 0:
                failures.append(f"{text.strip()} on a {'native' if native else 'posix'} build was accepted")

    for failure in failures:
        print(f"sxe-resources-test: FAIL {failure}", file=sys.stderr)
    if failures:
        return 1
    print("sxe-resources-test: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
