#!/usr/bin/env python3
"""The host half of the win32-drop smoke scenario (tools/smoke_catalog.py).

At "WIN32 DROP READY" the guest shows a SUN window painted yellow, the drag
source, and "clip.exe drop", whose client area starts with a magenta marker
(docs/desktop/DRAG_AND_DROP.md). This presses the left button on the yellow
window, which starts a drag of two files, moves the pointer over the child
of clip.exe's window at the same client points as fixtures/clip/drop.txt,
and releases it there. The guest checks what clip.exe printed and that the
source heard the drop take a copy.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qmp_client import QmpClient  # noqa: E402
import win32_input  # noqa: E402

YELLOW = (255, 255, 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args, _ = parser.parse_known_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    with QmpClient(args.socket, timeout=10.0) as qmp:
        screen = win32_input.Screen(qmp)
        cx, cy, width, height = win32_input.wait_for_marker(screen, 30.0)
        sx, sy, _w, _h = win32_input.wait_for_marker(screen, 30.0, YELLOW)
        print(f"win32-drop: source at {sx},{sy}, target client area at {cx},{cy} on a {width}x{height} screen",
              flush=True)
        player = win32_input.Player(qmp, screen, (0, 0), (width, height), (width, height), args.out_dir)
        # The source is 16x16 and asks for its drag on the press, which also
        # raises it: the drop points must still be clip.exe's.
        start = (sx + 6, sy + 6)
        player.move(*start)
        time.sleep(0.5)
        player.button("left", True)
        time.sleep(1.0)
        width, _height, rgb = screen.grab()
        for x, y in ((cx + 130, cy + 70), (cx + 180, cy + 100)):
            if tuple(rgb[(y * width + x) * 3:(y * width + x) * 3 + 3]) == YELLOW:
                print(f"win32-drop: the raised source covers the drop point {x},{y}", flush=True)
                player.button("left", False)
                return 1
        # The pointer travels over to the target in steps, as a hand would.
        for point in ((cx + 130, cy + 70), (cx + 180, cy + 100)):
            for step in range(1, 9):
                player.move(start[0] + (point[0] - start[0]) * step // 8,
                            start[1] + (point[1] - start[1]) * step // 8)
                time.sleep(0.1)
            start = point
            time.sleep(0.5)
        player.button("left", False)
        time.sleep(1.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
