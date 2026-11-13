#!/usr/bin/env python3
"""The host half of the win32-dialog smoke scenario (tools/smoke_catalog.py).

At "WIN32 DIALOG READY" the guest runs dialog.exe under windowd. This plays
the fixture's input script over QMP, menus and dialogs included, and
compares the window with a menu open and the Settings dialog with the
harness's golden images. The guest compares the messages with Windows XP's.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from win32_window_smoke import play_fixture  # noqa: E402

if __name__ == "__main__":
    sys.exit(play_fixture("win32-dialog", "dialog/input.txt", (360, 240), ["dialog-menu", "dialog-settings"]))
