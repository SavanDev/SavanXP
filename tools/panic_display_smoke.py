#!/usr/bin/env python3
"""The panic text is what the screen shows (FB4, docs/roadmaps/open/FRAMEBUFFER_ROADMAP.md).

Runs at "smp: other cores stopped", the last line a panic prints. Before
panicking, panictest left a solid magenta frame in the DISPI buffer its
variant names (`PANICTEST display=... mode=WxH ready` in the serial log).
After the panic the console must fill the visible buffer with its own
background and text: no magenta may remain, and the screenshot has the size
of the mode the adapter scans out.
"""
import argparse
import os
import re
import time
from pathlib import Path

from PIL import Image

from qmp_client import QmpClient

MAGENTA = (255, 0, 255)
CONSOLE_BACKGROUND = (0x06, 0x0B, 0x10)
CONSOLE_TEXT = (0xD5, 0xF4, 0xDF)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--socket', type=Path, required=True)
    parser.add_argument('--out-dir', type=Path, required=True)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    serial = Path(os.environ['SAVANXP_SMOKE_SERIAL_LOG']).read_text(errors='replace')
    ready = re.search(r'PANICTEST display=(\w+) mode=(\d+)x(\d+) ready', serial)
    if ready is None:
        print('PANIC DISPLAY FAIL panictest never prepared the display')
        return 1
    variant, width, height = ready.group(1), int(ready.group(2)), int(ready.group(3))

    # The buffering panictest asked for must be the one it got, or the
    # variant tests nothing.
    modes = re.findall(r'fb_gpu: mode (\d+)x(\d+) pitch=\d+, double buffer (yes|no)', serial)
    if not modes or (int(modes[-1][0]), int(modes[-1][1])) != (width, height):
        print(f'PANIC DISPLAY FAIL no DISPI mode {width}x{height} in the log: {modes}')
        return 1
    expected_double = 'no' if variant == 'single' else 'yes'
    if modes[-1][2] != expected_double:
        print(f'PANIC DISPLAY FAIL {variant}: double buffer {modes[-1][2]}, expected {expected_double}')
        return 1

    shot = (args.out_dir / f'panic-{variant}.ppm').resolve()
    time.sleep(0.5)
    with QmpClient(args.socket) as qmp:
        qmp.screenshot(shot)
    image = Image.open(shot).convert('RGB')
    if image.size != (width, height):
        print(f'PANIC DISPLAY FAIL {variant}: screen is {image.size}, mode is {width}x{height}')
        return 1

    counts = {colour: 0 for colour in (MAGENTA, CONSOLE_BACKGROUND, CONSOLE_TEXT)}
    for colour, count in ((c, n) for n, c in image.getcolors(maxcolors=1 << 24)):
        if colour in counts:
            counts[colour] = count
    total = width * height
    if counts[MAGENTA] != 0:
        print(f'PANIC DISPLAY FAIL {variant}: {counts[MAGENTA]} of {total} pixels still show the pre-panic frame')
        return 1
    if counts[CONSOLE_BACKGROUND] < total * 0.9 or counts[CONSOLE_TEXT] == 0:
        print(f'PANIC DISPLAY FAIL {variant}: console not visible '
              f'(background {counts[CONSOLE_BACKGROUND]}, text {counts[CONSOLE_TEXT]} of {total})')
        return 1
    print(f'PANIC DISPLAY PASS {variant}: {width}x{height}, console background {counts[CONSOLE_BACKGROUND]} '
          f'and text {counts[CONSOLE_TEXT]} of {total} pixels')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
