#!/usr/bin/env python3
"""Validate the pinned ccleste port layout without downloading the archive."""

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
    parser.add_argument("--port", required=True, type=Path)
    args = parser.parse_args()
    port = args.port.resolve()
    root = port.parent.parent

    metadata = {}
    for line in (port / "UPSTREAM").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            metadata[key] = value
    assert metadata["name"] == "ccleste"
    assert metadata["commit"] == "261d96f15af430b8111abc7a5250229246654f52"
    assert metadata["tree"] == "8a5143facf7c8a6818817b7c33e4956e3a0caa8b"
    assert metadata["archive_sha256"] == "2f6c7f753d1c8d0ba13bbc144a2a3ab9d3942ed4333e4f7f56e058e3f3677069"
    assert metadata["archive_size"] == "1539189"
    # ccleste ships no license file. The pin has to keep saying so, because the
    # provenance registry entry depends on this value.
    assert metadata["license"] == "Unlicensed"
    assert metadata["patch_count"] == "0"
    assert metadata["archived"] == "true"

    # The engine is downloaded, never vendored: an unlicensed tree must not be
    # able to appear in the repository by accident.
    assert not (port / "source").exists()
    assert not (port / "patches").exists()

    expected_overlay = {
        "ccleste.sxres",
        "ccleste_savanxp.c",
        "ccleste_savanxp_assets.c",
        "ccleste_savanxp_assets.h",
        "icon.png",
    }
    assert {path.name for path in (port / "overlay").iterdir() if path.is_file()} == expected_overlay

    manifest = (port / "overlay" / "ccleste.sxres").read_text(encoding="utf-8")
    assert "category=Games" in manifest
    assert "launch_flags=fullscreen" in manifest
    assert "data_dir=/disk/games/celeste" in manifest
    assert "mime_open=" not in manifest
    assert "ext_open=" not in manifest

    env = (port / "env.sh").read_text(encoding="utf-8")
    # The engine units are the whole upstream build, and none of them may be an
    # SDL host frontend: sdl12main.c is what this port replaces.
    assert "ENGINE_SOURCES=(celeste.c)" in env
    assert "SAVANXP_DISK_IMAGE" not in env
    assert "-msse -msse2" in env or ("-msse" in env and "-msse2" in env)
    assert "SX_HEAP_SIZE" in env

    build_script = (port / "build.sh").read_text(encoding="utf-8")
    assert "SAVANXP_DISK_IMAGE" in build_script
    assert "SAVANXP_SXFS_CLI" in build_script
    assert "sxfs_sync.py" in build_script
    # The music is out of scope, so the installer must not ship the ogg tracks.
    assert "*.ogg" not in build_script
    assert "mus*.ogg" not in build_script
    assert "snd*.wav" in build_script
    # The upstream frontend is sdl12main.c and this port replaces it, so it must
    # never reach a copy or compile list. The comment in build.sh names it; the
    # staging line must not.
    assert 'cp "$SRC/celeste.c" "$SRC/celeste.h" "$SRC/tilemap.h" "$BUILD_SRC/"' in build_script
    staged = [line for line in build_script.splitlines() if "sdl12main" in line]
    assert all(line.lstrip().startswith("#") for line in staged), staged

    for name in ("build.sh", "fetch.sh", "env.sh"):
        script = port / name
        assert script.is_file(), name
        assert script.stat().st_mode & 0o111, name
        run(["bash", "-n", str(script)], root)

    print("ccleste port layout: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
