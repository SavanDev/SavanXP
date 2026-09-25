#!/usr/bin/env python3

"""Static checks for the always-present Media Player system entry point."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()

    source = (root / "subsystems/posix/userland/mediaplayer.c").read_text(encoding="utf-8")
    manifest = (root / "subsystems/posix/userland/mediaplayer.sxres").read_text(encoding="utf-8")
    user_programs = (root / "cmake/UserPrograms.cmake").read_text(encoding="utf-8")
    init = (root / "subsystems/posix/userland/init.c").read_text(encoding="utf-8")
    external_manifest = (
        root / "ports/ffmpeg/overlay/mediaplayer/mediaplayer-ffmpeg.sxres"
    ).read_text(encoding="utf-8")
    install_script = (root / "ports/ffmpeg/install.sh").read_text(encoding="utf-8")

    assert 'MEDIA_PLAYER_PORT "/disk/bin/mediaplayer-ffmpeg"' in source
    assert '"/disk/bin/mediaplayer"' not in source
    assert "FFmpeg support is not installed" in source
    assert "savanxp_stat" in source
    assert "SAVANXP_S_IFREG" in source
    assert "libav" not in source.lower()
    assert "exec(MEDIA_PLAYER_PORT" in source
    assert "category=Accessories" in manifest
    assert "mime_open=" in manifest
    assert "ext_open=" in manifest
    assert "icon=app-mediaplayer" in manifest
    assert "savanxp_program(NAME mediaplayer" in user_programs
    assert 'path = "/bin/mediaplayer"' in init
    assert "mediaplayer-availability" in init
    assert "category=" not in external_manifest
    assert "mime_open=" not in external_manifest
    assert "ext_open=" not in external_manifest
    assert 'BACKEND_NAME="mediaplayer-ffmpeg"' in install_script
    assert 'STAGE/bin/$BACKEND_NAME' in install_script
    for size in (16, 32):
        assert (root / f"assets/desktop/icons/{size}x{size}/app-mediaplayer.png").is_file()
    print("system Media Player layout: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
