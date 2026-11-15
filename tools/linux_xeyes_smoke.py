#!/usr/bin/env python3
"""Compare the unchanged Xeyes against Linux, resize, close, relaunch and reboot."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import time
from PIL import Image
from qmp_client import QmpClient
from win32_input import Screen
from linux_xclient_smoke import wait_serial

ROOT = Path(__file__).resolve().parent.parent
POSITIONS = [(75, 50), (0, 0), (149, 99), (100, 20), (400, 300), (620, 10), (10, 460)]


def reference(i: int) -> Image.Image:
    x, y = POSITIONS[i]
    # L0 captures the X root, including the one-pixel X border. SUN chrome
    # replaces that border; compare the original 150x100 client area only.
    return Image.open(ROOT / f'subsystems/linux/profile/expected/screens/xeyes-{i}-{x}x{y}.png').convert('RGB').crop((1, 1, 151, 101))


def verify_image(image: Path, cli: Path) -> bool:
    with tempfile.TemporaryDirectory(prefix='ssl-xeyes-verify-') as temp:
        owners = Path(temp) / 'ownership.tsv'
        subprocess.run([str(cli), 'extract', str(image), temp, str(owners)], check=True, stdout=subprocess.DEVNULL)
        modes = {parts[1]: int(parts[4], 8) & 0o777 for line in owners.read_text().splitlines() if len(parts := line.split('\t')) == 5 and parts[0] == 'owner'}
        root = Path(temp) / 'system/linux'
        if not (root / 'usr/bin/xeyes').is_file():
            return False
        for item in json.loads((ROOT / 'subsystems/linux/profile/xeyes-runtime.json').read_text())['files']:
            path = root / item['path']
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256'] or modes.get('system/linux/' + item['path']) != item['mode']:
                raise AssertionError(f'installed Xeyes runtime mismatch: {item["path"]}')
    print('linux-xeyes: installed ELF, interpreter, 18 libraries and locale hashes PASS', flush=True)
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--socket', type=Path, required=True)
    parser.add_argument('--out-dir', type=Path, required=True)
    args, _ = parser.parse_known_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    log = Path(os.environ['SAVANXP_SMOKE_SERIAL_LOG'])
    with QmpClient(args.socket, timeout=10) as qmp:
        screen = Screen(qmp)
        sw, sh, _ = screen.grab()

        def move(x: int, y: int) -> None:
            qmp.send_input([
                {'type': 'abs', 'data': {'axis': 'x', 'value': math.ceil(x * 32767 / (sw - 1))}},
                {'type': 'abs', 'data': {'axis': 'y', 'value': math.ceil(y * 32767 / (sh - 1))}},
            ])

        def click(x: int, y: int) -> None:
            move(x, y)
            qmp.send_input([{'type': 'btn', 'data': {'button': 'left', 'down': True}}])
            qmp.send_input([{'type': 'btn', 'data': {'button': 'left', 'down': False}}])

        def locate() -> tuple[int, int]:
            # Locate the two eye whites by their exact fixed ellipse shape;
            # pupils move, so the horizontal white run is on the top half.
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline:
                width, height, rgb = screen.grab()
                image = Image.frombytes('RGB', (width, height), rgb)
                # Unique 2-white-eye signature in the first 30 rows of content.
                for y in range(20, height - 100):
                    row = rgb[y * width * 3:(y + 1) * width * 3]
                    target = reference(0).crop((0, 12, 150, 13)).tobytes()
                    at = row.find(target)
                    if at >= 0 and at % 3 == 0:
                        return at // 3, y - 12
                time.sleep(.1)
            raise AssertionError('Xeyes did not draw both eyes')

        def capture(name: str, x: int, y: int, expected: Image.Image, cursor=None) -> None:
            if cursor:
                helper = Path(os.environ.get('SAVANXP_BUILD_DIR', ROOT / 'build/linux')) / 'savanxp-linux-cursor-reference'
                result = subprocess.run([str(helper), str(ROOT / 'assets/desktop/cursors/classic/arrow.cur'),
                    str(expected.width), str(expected.height), str(cursor[0] - x), str(cursor[1] - y)],
                    input=expected.tobytes(), capture_output=True, check=True)
                expected = Image.frombytes('RGB', expected.size, result.stdout)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                width, height, rgb = screen.grab()
                image = Image.frombytes('RGB', (width, height), rgb)
                crop = image.crop((x, y, x + expected.width, y + expected.height))
                if crop.tobytes() == expected.tobytes():
                    image.save(args.out_dir / f'{name}.png')
                    print(f'linux-xeyes: {name} matches Linux pixel for pixel', flush=True)
                    return
                image.save(args.out_dir / f'{name}-failed.png')
                time.sleep(.1)
            raise AssertionError(f'{name}: differs from Linux')

        for boot in range(2):
            for cycle in range(2):
                if cycle:
                    wait_serial(log, 'LINUX XEYES READY 2', 30, start=next_start)
                x, y = locate()
                # Move to a reproducible origin, leaving room for the full
                # recorded outside-window pointer path on every screen.
                move(x + 40, y - 12)
                qmp.send_input([{'type': 'btn', 'data': {'button': 'left', 'down': True}}])
                time.sleep(.1); move(104, 52); time.sleep(.1)
                qmp.send_input([{'type': 'btn', 'data': {'button': 'left', 'down': False}}])
                x, y = locate()
                for i, (rx, ry) in enumerate(POSITIONS):
                    # Linux's reference top-left outer border is at 0,0;
                    # its content origin is 1,1. Preserve local coordinates.
                    px, py = x + rx - 1, y + ry - 1
                    move(px, py)
                    time.sleep(.1)
                    capture(f'boot{boot}-run{cycle}-pointer{i}', x, y, reference(i), (px, py))
                move(x + 151, y + 101)
                qmp.send_input([{'type': 'btn', 'data': {'button': 'left', 'down': True}}])
                time.sleep(.1); move(x + 211, y + 131); time.sleep(.1)
                qmp.send_input([{'type': 'btn', 'data': {'button': 'left', 'down': False}}])
                move(x + 399, y + 299)
                expected = Image.open(ROOT / 'subsystems/linux/profile/expected/screens/xeyes-resized-400x300.png').convert('RGB')
                capture(f'boot{boot}-run{cycle}-resized', x, y, expected)
                start = log.stat().st_size
                next_start = start
                click(x + 202, y - 12)
                wait_serial(log, 'linux-xeyes: cycle clean', 30, start=start)
            if not boot:
                wait_serial(log, 'LINUX XEYES RESTART READY', 30)
                start = log.stat().st_size
                qmp.execute('set-action', {'reboot': 'reset'})
                qmp.execute('system_reset')
                wait_serial(log, 'LINUX XEYES READY 1', 90, start=start)
        wait_serial(log, 'LINUX XEYES PASS', 30)
    print('linux-xeyes: pointer path, resize, close, relaunch and restart PASS', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
