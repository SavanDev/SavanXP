#!/usr/bin/env python3
"""Validate the pinned DoomGeneric port layout without building the port."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
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
    parser.add_argument("--port", required=True, type=Path)
    args = parser.parse_args()
    port = args.port.resolve()

    metadata = {}
    for line in (port / "UPSTREAM").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            metadata[key] = value
    assert metadata["commit"] == "fc601639494e089702a1ada082eb51aaafc03722"
    assert metadata["tree"] == "4b8c10b05138583636e5425131242b7212023ef7"
    assert metadata["archive_sha256"] == "6a5879c5f686199f0156ea8abcdaab820350c3a74aff211e81558f8675c8e2d5"
    assert metadata["archive_size"] == "3090394"
    assert metadata["license"] == "GPL-2.0"
    assert metadata["source_tree_sha256"] == "4cdeac88cd3db3b31e54ea975dd54d00f00939d64dcded73f0d73d19cd2ff5cf"

    source = port / "source" / "doomgeneric"
    run(
        [
            "python3",
            str(port.parent.parent / "tools" / "source_tree_digest.py"),
            "--root",
            str(source),
            "--expected",
            metadata["source_tree_sha256"],
        ],
        port.parent.parent,
    )
    assert source.is_dir()
    assert (port / "source" / "LICENSE").is_file()
    source_list = [
        line.strip()
        for line in (port / "sources.txt").read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    assert len(source_list) == 79
    assert len(set(source_list)) == len(source_list)
    assert all((source / relative).is_file() for relative in source_list)
    assert not any(Path(relative).name.startswith("doomgeneric_savanxp") for relative in source_list)

    expected_overlay = {
        "doomgeneric.sxres",
        "doomgeneric_savanxp.c",
        "doomgeneric_savanxp_audio.c",
        "icon.png",
        "net_stubs.c",
        "savanxp_compat.c",
        "savanxp_compat.h",
    }
    assert {path.name for path in (port / "overlay").iterdir() if path.is_file()} == expected_overlay
    script = (port / "build.sh").read_text(encoding="utf-8")
    assert "SAVANXP_OUTPUT_ROOT" in script
    assert "SAVANXP_DISK_IMAGE" in script
    assert "SAVANXP_SXFS_CLI" in script
    assert (port / "build.sh").stat().st_mode & 0o111
    assert (port / "wad").is_dir()

    with tempfile.TemporaryDirectory(prefix="doom-port-patches-") as temporary:
        assembled = Path(temporary) / "src"
        shutil.copytree(source, assembled)
        run(["git", "init", "-q"], assembled)
        for patch in sorted((port / "patches").glob("*.patch")):
            run(["git", "apply", "--check", "--whitespace=nowarn", str(patch)], assembled)
            run(["git", "apply", "--whitespace=nowarn", str(patch)], assembled)

    run(["bash", "-n", str(port / "build.sh")], port.parent.parent)
    print("doom port layout: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
