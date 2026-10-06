#!/usr/bin/env python3
"""Validate the pinned FFmpeg port layout without downloading/building FFmpeg.

The port ships one library and no program; the Media Player itself lives in
``subsystems/posix/userland/mediaplayer/`` and is built by the system tree.
This test guards both halves of that split.
"""

from __future__ import annotations

import argparse
import ast
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
    assert metadata["tag"] == "n7.1.1"
    assert metadata["commit"] == "db69d06eeeab4f46da15030a80d539efb4503ca8"
    assert metadata["tree"] == "94e0dc384ffe4b562568294cf3846e47cb0260d6"
    assert metadata["archive_sha256"] == "733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1"
    assert metadata["archive_size"] == "11019500"
    assert metadata["license"] == "LGPL-2.1-or-later"
    assert metadata["patch_count"] == "0"
    assert not (port / "source").exists()
    assert (port / "LICENSE.md").is_file()
    assert (port / "COPYING.LGPLv2.1").is_file()

    # The port builds a library and nothing else: no overlay, no program stamped
    # into /disk/bin. The player moved into the tree (ada896f), so the port must
    # not carry an overlay directory again.
    assert not (port / "overlay").exists()
    install_script = (port / "install.sh").read_text(encoding="utf-8")
    assert 'LIBRARY="$OUTPUT_ROOT/external/libffmpeg.so.0.4"' in install_script
    assert 'cp "$LIBRARY" "$STAGE/lib/libffmpeg.so.0.4"' in install_script
    assert "$STAGE/bin" not in install_script
    assert "BACKEND_NAME" not in install_script
    assert "SAVANXP_DISK_IMAGE" in install_script
    assert "SAVANXP_SXFS_CLI" in install_script
    link_script = (port / "link.sh").read_text(encoding="utf-8")
    assert "-o libffmpeg.so.0.4" in link_script
    assert "-Wl,-soname,libffmpeg.so.0.4" in link_script
    assert "subsystems/posix/userland/mediaplayer/" in link_script
    assert "mediaplayer.elf" not in link_script
    assert "-l:libmath.so.0.4" in link_script
    smoke_script = (port / "smoke.sh").read_text(encoding="utf-8")
    assert "mediaplayer-selftest" in smoke_script
    assert "mediaplayer-display" in smoke_script
    assert "build.sh" in smoke_script

    # The player is the tree's, with its own manifest: capabilities (category,
    # mime_open, ext_open) and an icon that travels with the sources.
    player = root / "subsystems" / "posix" / "userland" / "mediaplayer"
    expected_player = {
        "gen_icons.py",
        "icon.png",
        "icons.inc",
        "media.c",
        "media.h",
        "mediaplayer.c",
        "mediaplayer.sxres",
        "playback.c",
        "playback.h",
        "selftest.c",
        "selftest.h",
    }
    actual_player = {path.name for path in player.iterdir() if path.is_file()}
    assert actual_player == expected_player
    assert not (player / "mediaplayer-ffmpeg.sxres").exists()
    manifest = (player / "mediaplayer.sxres").read_text(encoding="utf-8")
    assert "name=Media Player" in manifest
    assert "mime_open=" in manifest
    assert "ext_open=" in manifest
    assert "category=Accessories" in manifest
    assert "icon_file=icon.png" in manifest
    assert len(list((player / "icons").glob("*.png"))) == 10

    assert {path.name for path in (port / "tests").glob("*.py")} == {
        "make-tone.py",
        "make-clip.py",
        "make-avclip.py",
        "qmp_display.py",
    }
    ast.parse((port / "tests" / "qmp_display.py").read_text(encoding="utf-8"))

    for name in ("build.sh", "all.sh", "fetch.sh", "runtime.sh", "configure.sh", "make.sh", "link.sh", "install.sh", "smoke.sh"):
        script = port / name
        assert script.is_file()
        assert script.stat().st_mode & 0o111
        run(["bash", "-n", str(script)], root)

    print("ffmpeg port layout: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
