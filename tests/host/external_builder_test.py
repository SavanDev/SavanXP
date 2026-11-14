#!/usr/bin/env python3
"""Exercise the native external SDK builder's source-root include contract."""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import tempfile
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--ld", default="ld.lld")
    args = parser.parse_args()
    root = args.root.resolve()
    clang = shutil.which(args.clang) or args.clang
    linker = shutil.which(args.ld) or args.ld
    with tempfile.TemporaryDirectory(prefix="savanxp-external-builder.") as temporary:
        source = Path(temporary)
        (source / "local.h").write_text("#define LOCAL_ANSWER 42\n", encoding="utf-8")
        (source / "main.c").write_text(
            "#include <local.h>\nint main(void) { return LOCAL_ANSWER; }\n",
            encoding="utf-8",
        )
        environment = os.environ.copy()
        environment["SAVANXP_OUTPUT_ROOT"] = str(source / "output")
        environment["SAVANXP_PYTHON"] = sys.executable
        environment.pop("SAVANXP_ARCH", None)
        environment["SAVANXP_CLANG"] = clang
        environment["SAVANXP_LD"] = linker
        result = subprocess.run(
            [
                str(root / "tools" / "build-user.sh"),
                "--source",
                str(source),
                "--name",
                "external-include-test",
                "--no-install",
            ],
            cwd=root,
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            raise SystemExit(
                f"external builder failed ({result.returncode})\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )
        binary = source / "output/external/external-include-test.elf"
        data = binary.read_bytes()
        assert data[:5] == b"\x7fELF\x01", "external builder must emit ELF32 by default"
        elf_type, machine = struct.unpack_from("<HH", data, 16)
        assert (elf_type, machine) == (2, 3), "expected static i386 ET_EXEC"
        entry, = struct.unpack_from("<I", data, 24)
        assert 0x08000000 <= entry < 0x09000000, hex(entry)
    print("external builder source-root include: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
