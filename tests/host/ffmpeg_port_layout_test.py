#!/usr/bin/env python3
"""Validate the pinned FFmpeg port layout without downloading/building FFmpeg."""

from __future__ import annotations

import argparse
import ast
import re
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

    expected_overlay = {
        "gen_icons.py",
        "icon.png",
        "icons.inc",
        "mediaplayer.c",
        "mediaplayer-ffmpeg.sxres",
        "playback.c",
        "playback.h",
        "selftest.c",
        "selftest.h",
    }
    overlay = port / "overlay" / "mediaplayer"
    actual_overlay = {path.name for path in overlay.iterdir() if path.is_file()}
    assert actual_overlay == expected_overlay
    # The decode engine left the overlay: it is the SDK's runtime/sxmedia.c, and
    # what the port keeps is the one file that tells a library how to be a
    # backend. media.c and media.h are gone, so nothing in the port is
    # FFmpeg-shaped except the backend.
    backend_dir = port / "overlay" / "sxmedia"
    assert {path.name for path in backend_dir.iterdir() if path.is_file()} == {
        "sxmedia_ffmpeg.c",
        "sxmedia_ffmpeg.h",
    }
    backend_source = (backend_dir / "sxmedia_ffmpeg.c").read_text(encoding="utf-8")
    assert "sx_media_backend_ops" in backend_source
    assert "sxmedia_ffmpeg_register" in backend_source
    assert "claim_source" in backend_source and "claim_stream" in backend_source
    # The front end and the selftest talk to the engine and to the registry and
    # never to a library. This is the assertion that would have caught the
    # engine leaking back into the window, and it checks the dependency rather
    # than the prose: a comment may name the library to explain why the modes
    # share a binary, but no include and no call may.
    for name in ("mediaplayer.c", "playback.c", "selftest.c", "playback.h"):
        # Comments may name what the code used to use; the code may not. A
        # comment explaining the seam is the documentation for it.
        text = (overlay / name).read_text(encoding="utf-8")
        code = "\n".join(
            line for line in text.splitlines()
            if not line.lstrip().startswith(("*", "/*", "//"))
        )
        assert "#include <libav" not in code, f"{name} includes a library header"
        assert "av_log_set_level" not in code, f"{name} calls a library function"
        for symbol in ("AVFrame", "AVCodec", "AVFormatContext", "AVPacket", "SwsContext", "SwrContext"):
            assert symbol not in code, f"{name} names the library type {symbol}"
        assert "struct media" not in code, f"{name} still uses the old engine type"
        # `sx_media_open` contains "media_open", so the old call has to be
        # matched with a lookbehind rather than a substring test.
        assert not re.search(r"(?<!sx_)\bmedia_(open|close|seek|read_audio|scale_video)\b", code), (
            f"{name} still calls the old engine"
        )
    # ...and they do go through the engine rather than around it.
    for name in ("mediaplayer.c", "playback.c", "selftest.c"):
        text = (overlay / name).read_text(encoding="utf-8")
        assert "savanxp/sxmedia.h" in text or "sx_media_" in text, f"{name} bypasses the engine"
    backend_manifest = (overlay / "mediaplayer-ffmpeg.sxres").read_text(encoding="utf-8")
    assert "category=" not in backend_manifest
    assert "mime_open=" not in backend_manifest
    assert "ext_open=" not in backend_manifest
    assert "name=FFmpeg Media Player Backend" in backend_manifest
    install_script = (port / "install.sh").read_text(encoding="utf-8")
    assert 'BACKEND_NAME="mediaplayer-ffmpeg"' in install_script
    assert 'cp "$STAMPED" "$STAGE/bin/$BACKEND_NAME"' in install_script
    assert 'cp "$STAMPED" "$STAGE/bin/mediaplayer"' not in install_script
    assert 'SAVANXP_DISK_IMAGE' in install_script
    assert 'SAVANXP_SXFS_CLI' in install_script
    smoke_script = (port / "smoke.sh").read_text(encoding="utf-8")
    assert "mediaplayer-selftest" in smoke_script
    assert "mediaplayer-display" in smoke_script
    assert "build.sh" in smoke_script
    link_script = (port / "link.sh").read_text(encoding="utf-8")
    assert '"$OUTPUT_ROOT/external/mediaplayer.elf"' in link_script
    assert len(list((overlay / "icons").glob("*.png"))) == 10
    assert {path.name for path in (port / "tests").glob("*.py")} == {
        "make-tone.py",
        "make-clip.py",
        "make-avclip.py",
        "qmp_display.py",
    }
    ast.parse((port / "tests" / "qmp_display.py").read_text(encoding="utf-8"))

    # The vtable is initialised positionally, so a field added to
    # `sx_media_backend_ops` shifts every entry after it and the result is a
    # provider whose converters are the wrong functions -- or a compile error
    # that only shows up when a port is relinked, which is not something the host
    # suite does. This is not a substitute for compiling the port; it is the only
    # check that can run without libav, and it is what would have caught the two
    # whole-source slots being left out of the FFmpeg backend.
    backend_source = (port / "overlay" / "sxmedia" / "sxmedia_ffmpeg.c").read_text(encoding="utf-8")
    initialiser = backend_source.split("kFFmpegOps = {", 1)[1].split("};", 1)[0]
    # Comments come out first: an English comma inside a block comment is not an
    # initialiser entry, and counting one turns a correct table into a failure.
    initialiser = re.sub(r"/\*.*?\*/", " ", initialiser, flags=re.DOTALL)
    initialiser = re.sub(r"//[^\n]*", " ", initialiser)
    ops_entries = [entry for entry in initialiser.split(",") if entry.strip()]
    header = (root / "subsystems/posix/sdk/v1/include/savanxp/sxmedia.h").read_text(encoding="utf-8")
    declared_ops = header.split("struct sx_media_backend_ops {", 1)[1].split("\n};", 1)[0]
    fields = [line for line in declared_ops.splitlines() if "(*" in line]
    assert len(fields) == len(ops_entries), (
        f"sx_media_backend_ops has {len(fields)} entries and the FFmpeg backend "
        f"initialises {len(ops_entries)}; the positional table is misaligned"
    )

    for name in ("build.sh", "all.sh", "fetch.sh", "runtime.sh", "configure.sh", "make.sh", "link.sh", "install.sh", "smoke.sh"):
        script = port / name
        assert script.is_file()
        assert script.stat().st_mode & 0o111
        run(["bash", "-n", str(script)], root)

    print("ffmpeg port layout: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
