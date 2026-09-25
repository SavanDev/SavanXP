#!/usr/bin/env python3
"""Validate the optional native/Haxe boundary without running Haxe."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def run(command: list[str], cwd: Path) -> None:
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    native = root / "subsystems" / "native"

    assert (root / "include" / "abi" / "savanxp_native_abi.h").is_file()
    assert (root / "kernel" / "native_syscall_dispatch.inc").is_file()
    assert not (native / "sdk" / "include" / "savanxp_native_abi.h").exists()
    assert not (native / "kernel" / "syscall_dispatch.inc").exists()
    process = (root / "kernel" / "process.cpp").read_text(encoding="utf-8")
    assert "include/abi/savanxp_native_abi.h" in process
    assert "native_syscall_dispatch.inc" in process
    generator = (root / "tools" / "gen_sxe_resources.py").read_text(encoding="utf-8")
    assert '"include", "abi", "savanxp_native_abi.h"' in generator
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "include/abi/savanxp_native_abi.h" in cmake
    assert "add_subdirectory(subsystems/native)" not in cmake
    assert "find_program(HAXE)" not in cmake

    metadata = {}
    for line in (native / "UPSTREAM").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            metadata[key] = value
    assert metadata["haxe_version"] == "4.3.7"
    assert len(metadata["haxe_commit"]) == 40
    assert len(metadata["reflaxe_commit"]) == 40
    assert len(metadata["reflaxe_cpp_commit"]) == 40

    script = native / "build.sh"
    script_text = script.read_text(encoding="utf-8")
    assert "SAVANXP_OUTPUT_ROOT" in script_text
    assert "SAVANXP_DISK_IMAGE" in script_text
    assert "SAVANXP_SXFS_CLI" in script_text
    assert script.stat().st_mode & 0o111
    run(["bash", "-n", str(script)], root)
    assert (native / "haxe" / "nativehello.sxres").is_file()
    assert (native / "haxe-gui" / "nativegui.sxres").is_file()
    assert (native / "haxe-sxgui" / "sxguiapp.sxres").is_file()
    assert (native / "sdk" / "include" / "savanxp_native.h").is_file()
    print("native port layout: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
