#!/usr/bin/env python3
"""The host half of the win32-controls smoke scenario (tools/smoke_catalog.py).

At "WIN32 CONTROLS READY" the guest runs controls.exe under windowd. This
plays the fixture's input script over QMP and compares the window with a
drop-down list open and with a context menu open with the harness's golden
images. The guest compares the messages with Windows XP's.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from win32_window_smoke import play_fixture  # noqa: E402

if __name__ == "__main__":
    sys.exit(play_fixture("win32-controls", "controls/input.txt", (400, 300),
                          ["controls-dropdown", "controls-menu"]))
