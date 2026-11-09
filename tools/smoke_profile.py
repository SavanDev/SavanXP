#!/usr/bin/env python3
"""Reset resolution and effects preferences on a disposable smoke image.

Smokes and visual scenarios run on a copy of build/disk.img, which is the
developer's persistent disk. Whatever the developer chose in Display
Properties lives in it -- a display.cfg with mode=1024x768, say -- and the
copy inherits it. Every coordinate the host drivers click on assumes the
default 1280x800, so a personal preference turned into failing smokes and
false positives (a find_text matching the black padding of a smaller screen).

So a smoke never sees the developer's display preferences: this removes them
from the copy, and the session falls back to its default mode. A scenario
that wants a specific resolution will write its own display.cfg on top of a
clean copy. Effects also start from their defaults, so pixel checks do not
inherit the developer's shadow choices.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# Where savanxp_settings_path (subsystems/posix/sdk/v1/runtime/posix.c) looks
# for display.cfg: the user's Settings folder, then loose in the profile (from
# before the folder), then the root of the volume, which is root's profile and
# also where settings lived before profiles existed. Effects use the current
# Settings path only, like cursor preferences.
DISPLAY_PREFERENCE_PATHS = (
    "users/user/Settings/display.cfg",
    "users/user/display.cfg",
    "display.cfg",
    "users/user/Settings/effects.cfg",
)

def reset_audio_preferences(sxfs_cli: Path, image: Path) -> None:
    """Isolate an audio acceptance test from settings on the developer's disk."""
    paths = [prefix+name+suffix for prefix in ("users/user/Settings/","users/user/","")
             for name in ("audio.cfg","audio-mixer.cfg") for suffix in ("",".previous")]
    result = subprocess.run([str(sxfs_cli),"rm","-f",str(image),*paths],capture_output=True,text=True)
    if result.returncode:
        raise RuntimeError(f"could not reset audio preferences of {image}: {(result.stderr or result.stdout).strip()}")


def reset_display_preferences(sxfs_cli: Path, image: Path) -> None:
    """Remove display/effects preferences from the disposable image."""
    removed = subprocess.run(
        [str(sxfs_cli), "rm", "-f", str(image), *DISPLAY_PREFERENCE_PATHS],
        text=True,
        capture_output=True,
        check=False,
    )
    if removed.returncode != 0:
        detail = (removed.stderr or removed.stdout).strip()
        raise RuntimeError(f"could not reset the display preferences of {image}: {detail}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sxfs-cli", required=True, type=Path)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    try:
        reset_display_preferences(args.sxfs_cli, args.image)
    except RuntimeError as error:
        print(f"smoke_profile: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
