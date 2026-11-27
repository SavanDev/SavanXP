#!/usr/bin/env python3
"""The host half of the win32-richedit smoke scenario (tools/smoke_catalog.py).

At "WIN32 RICHEDIT READY" the guest runs "richedit.exe input" under windowd.
This plays the fixture's input script over QMP and compares the control with
a selection with the harness's golden image. The guest
compares what the control told its parent with Windows XP's.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from win32_window_smoke import play_fixture  # noqa: E402

if __name__ == "__main__":
    sys.exit(play_fixture("win32-richedit", "richedit/input.txt", (400, 300),
                          ["richedit-selection"]))
