#!/usr/bin/env python3
"""Play a Win32 fixture's input script into a QEMU guest over QMP.

The script format is the host harness's (docs/userland/WIN32.md, "Input
scripts"): actions in the main window's client coordinates. Here they become
absolute pointer moves, buttons and keys through `input-send-event`, so the
same file drives the fixture on Windows XP and on SavanXP. The guest needs an
absolute pointer (a USB or virtio tablet).

The client area is found on a screenshot by the fixture's marker: a 4x4
magenta square at its top-left corner. `window N` switches to the client
area of a dialog, whose marker is cyan (`window 0` is the main window).
`close` is Alt+F4, which both window managers turn into the frame's close;
`dump FILE [WxH]` saves the client area of a screenshot as a binary PPM, W x H
or the size given with --client.

    win32_input.py --socket qmp.sock --script input.txt --client 320x240 \\
                   [--dump-dir DIR] [--marker-timeout 60]
"""

from __future__ import annotations

import argparse
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qmp_client import QmpClient  # noqa: E402

MAGENTA = (255, 0, 255)
# The marker of each window a script switches to with "window N": the main
# window's, then the dialog's.
MARKERS = [MAGENTA, (0, 255, 255)]

# Windows virtual keys to QEMU key codes, for the keys scripts use.
VK_QCODES = {
    0x08: "backspace", 0x09: "tab", 0x0D: "ret", 0x10: "shift", 0x11: "ctrl", 0x12: "alt",
    0x1B: "esc", 0x20: "spc", 0x25: "left", 0x26: "up", 0x27: "right", 0x28: "down",
    **{0x30 + d: str(d) for d in range(10)},
    **{0x41 + i: chr(ord("a") + i) for i in range(26)},
    **{0x70 + i: f"f{i + 1}" for i in range(12)},
}

BUTTONS = {"left": "left", "right": "right", "middle": "middle"}


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    return read_ppm_bytes(path.read_bytes(), str(path))


def read_ppm_bytes(data: bytes, name: str = "image") -> tuple[int, int, bytes]:
    fields = []
    index = 0
    while len(fields) < 4:
        while data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b"#":
            index = data.index(b"\n", index) + 1
            continue
        end = index
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[index:end])
        index = end
    if fields[0] != b"P6" or fields[3] != b"255":
        raise ValueError(f"{name}: not an 8-bit binary PPM")
    width, height = int(fields[1]), int(fields[2])
    return width, height, data[index + 1:index + 1 + width * height * 3]


def write_ppm(path: Path, width: int, height: int, rgb: bytes) -> None:
    path.write_bytes(b"P6\n%d %d\n255\n" % (width, height) + rgb)


class Screen:
    def __init__(self, qmp: QmpClient) -> None:
        self.qmp = qmp
        self.scratch = Path(tempfile.mkdtemp(prefix="win32-input-"))

    def grab(self) -> tuple[int, int, bytes]:
        shot = self.scratch / "screen.ppm"
        self.qmp.execute("screendump", {"filename": str(shot)})
        return read_ppm(shot)


def find_marker(width: int, height: int, rgb: bytes, color: tuple[int, int, int] = MAGENTA) -> tuple[int, int] | None:
    """The top-left corner of a 4x4 square of the colour, or None."""
    magenta = bytes(color)
    start = 0
    while True:
        at = rgb.find(magenta, start)
        if at < 0:
            return None
        start = at + 1
        if at % 3 != 0:
            continue
        x, y = (at // 3) % width, (at // 3) // width
        if x + 4 > width or y + 4 > height:
            continue
        square = all(rgb[((y + dy) * width + x + dx) * 3:((y + dy) * width + x + dx) * 3 + 3] == magenta
                     for dy in range(4) for dx in range(4))
        left_clear = x == 0 or rgb[(y * width + x - 1) * 3:(y * width + x) * 3] != magenta
        top_clear = y == 0 or rgb[((y - 1) * width + x) * 3:((y - 1) * width + x) * 3 + 3] != magenta
        if square and left_clear and top_clear:
            return x, y


def wait_for_marker(screen: Screen, timeout: float, color: tuple[int, int, int] = MAGENTA) -> tuple[int, int, int, int]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        width, height, rgb = screen.grab()
        found = find_marker(width, height, rgb, color)
        if found is not None:
            return found[0], found[1], width, height
        time.sleep(0.5)
    raise TimeoutError("the fixture's marker never appeared")


class Player:
    def __init__(self, qmp: QmpClient, screen: Screen, origin: tuple[int, int], display: tuple[int, int],
                 client: tuple[int, int], dump_dir: Path, pace: float = 0.06) -> None:
        self.qmp = qmp
        self.screen = screen
        self.origin = origin
        self.display = display
        self.client = client
        self.dump_dir = dump_dir
        self.pace = pace
        self.main_origin = origin

    def move(self, x: int, y: int) -> None:
        sx = min(max(self.origin[0] + x, 0), self.display[0] - 1)
        sy = min(max(self.origin[1] + y, 0), self.display[1] - 1)
        self.qmp.send_input([
            {"type": "abs", "data": {"axis": "x", "value": sx * 32767 // (self.display[0] - 1)}},
            {"type": "abs", "data": {"axis": "y", "value": sy * 32767 // (self.display[1] - 1)}},
        ])

    def button(self, name: str, down: bool) -> None:
        self.qmp.send_input([{"type": "btn", "data": {"down": down, "button": BUTTONS[name]}}])

    def key(self, vk: int, down: bool) -> None:
        if vk not in VK_QCODES:
            raise ValueError(f"no QEMU key code for virtual key {vk:#x}")
        self.qmp.key(VK_QCODES[vk], down)

    def dump(self, name: str, size: tuple[int, int] | None = None) -> None:
        width, _height, rgb = self.screen.grab()
        x0, y0 = self.origin
        cw, ch = size or self.client
        rows = [rgb[((y0 + y) * width + x0) * 3:((y0 + y) * width + x0 + cw) * 3] for y in range(ch)]
        self.dump_dir.mkdir(parents=True, exist_ok=True)
        write_ppm(self.dump_dir / Path(name).name, cw, ch, b"".join(rows))

    def play(self, script: Path) -> None:
        for number, line in enumerate(script.read_text().splitlines(), 1):
            words = line.split()
            if not words or words[0].startswith("#"):
                continue
            verb, args = words[0], words[1:]
            if verb == "move":
                self.move(int(args[0]), int(args[1]))
            elif verb in ("down", "up"):
                self.move(int(args[1]), int(args[2]))
                time.sleep(self.pace / 2)
                self.button(args[0], verb == "down")
            elif verb in ("keydown", "keyup"):
                self.key(int(args[0], 0), verb == "keydown")
            elif verb == "wait":
                time.sleep(int(args[0]) / 1000)
                continue
            elif verb == "close":
                self.key(0x12, True)
                time.sleep(self.pace)
                self.key(0x73, True)
                time.sleep(self.pace)
                self.key(0x73, False)
                time.sleep(self.pace)
                self.key(0x12, False)
            elif verb == "window":
                index = int(args[0])
                if index == 0:
                    self.origin = self.main_origin
                else:
                    x, y, _w, _h = wait_for_marker(self.screen, 20.0, MARKERS[index])
                    self.origin = (x, y)
                continue
            elif verb == "dump":
                time.sleep(0.5)
                self.dump(args[0], tuple(int(v) for v in args[1].split("x")) if len(args) > 1 else None)
            else:
                raise ValueError(f"{script}:{number}: not an action: {line!r}")
            time.sleep(self.pace)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--script", type=Path, required=True)
    parser.add_argument("--client", required=True, help="the client area's size, WxH")
    parser.add_argument("--dump-dir", type=Path, default=Path("."))
    parser.add_argument("--marker-timeout", type=float, default=60.0)
    args = parser.parse_args()
    client = tuple(int(v) for v in args.client.split("x"))
    with QmpClient(args.socket, timeout=10.0) as qmp:
        screen = Screen(qmp)
        x, y, width, height = wait_for_marker(screen, args.marker_timeout)
        print(f"win32-input: client area at {x},{y} on a {width}x{height} screen", flush=True)
        Player(qmp, screen, (x, y), (width, height), client, args.dump_dir).play(args.script)
    return 0


if __name__ == "__main__":
    sys.exit(main())
