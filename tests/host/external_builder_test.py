#!/usr/bin/env python3
"""Exercise the native external SDK builder's source-root include contract."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
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
    print("external builder source-root include: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
