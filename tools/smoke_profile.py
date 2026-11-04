#!/usr/bin/env python3
"""Reset the user's display preferences on a disposable smoke image.

Smokes and visual scenarios run on a copy of build/disk.img, which is the
developer's persistent disk. Whatever the developer chose in Display
Properties lives in it -- a display.cfg with mode=1024x768, say -- and the
copy inherits it. Every coordinate the host drivers click on assumes the
default 1280x800, so a personal preference turned into failing smokes and
false positives (a find_text matching the black padding of a smaller screen).

So a smoke never sees the developer's display preferences: this removes them
from the copy, and the session falls back to its default mode. A scenario
that wants a specific resolution will write its own display.cfg on top of a
clean copy.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# Where savanxp_settings_path (subsystems/posix/sdk/v1/runtime/posix.c) looks
# for display.cfg: the profile of each account in its passwd table, then the
# root of the volume, which is root's profile and also where settings lived
# before profiles existed.
DISPLAY_PREFERENCE_PATHS = (
    "users/user/display.cfg",
    "display.cfg",
)


def reset_display_preferences(sxfs_cli: Path, image: Path) -> None:
    """Remove the display preferences from `image`. Missing files are fine."""
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
