#!/usr/bin/env python3
"""Check the compositor's Wine cursor grid against original CUR bitmap data."""
import argparse
import struct
import time
from pathlib import Path
from PIL import Image
from qmp_client import QmpClient

NAMES = ('normal', 'wait', 'ibeam', 'sizeall', 'sizewe', 'sizens', 'no', 'hand')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--socket', type=Path, required=True)
    parser.add_argument('--out-dir', type=Path, required=True)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    shot = (args.out_dir / 'wine-cursors.ppm').resolve()
    time.sleep(0.3)
    with QmpClient(args.socket) as qmp:
        qmp.screenshot(shot)
    image = Image.open(shot).convert('RGB')
    root = Path(__file__).resolve().parent.parent
    for column, name in enumerate(NAMES):
        data = (root / f'assets/desktop/cursors/wine/ocr_{name}.cur').read_bytes()
        # Wine's first entry is its 64px 32-bit DIB; derive expected colours
        # directly from its bottom-up bitmap, independent of our C decoder.
        offset = struct.unpack_from('<I', data, 18)[0]
        assert struct.unpack_from('<IiiHH', data, offset) == (40, 64, 128, 1, 32)
        for row, background in enumerate((255, 0)):
            for y in range(64):
                for x in range(64):
                    b, g, r, a = struct.unpack_from('4B', data, offset + 40 + ((63-y)*64+x)*4)
                    expected = tuple((c*a + background*(255-a)+127)//255 for c in (r,g,b))
                    actual = image.getpixel((32+column*96+x, 32+row*96+y))
                    assert actual == expected, (name, row, x, y, actual, expected)
    for point, expected in {
        (32, 256): (0, 0, 0), (33, 256): (255, 255, 255),
        (32, 257): (18, 52, 86), (33, 257): (237, 203, 169),
        (64, 256): (0, 0, 0), (65, 256): (18, 52, 86),
        (64, 257): (18, 52, 86), (65, 257): (18, 52, 86),
        (0, 288): (255, 255, 255), (0, 289): (237, 203, 169),
        (1, 288): (18, 52, 86),
    }.items():
        assert image.getpixel(point) == expected, (point, image.getpixel(point), expected)
    for y in range(400, 464):
        for x in range(900, 964):
            assert image.getpixel((x, y)) == (128, 128, 128), ('hardware cursor residue', x, y)
    image.save(args.out_dir / 'wine-cursors.png')
    print('cursor-smoke: all eight Wine cursors match on light and dark backgrounds; inversion and clipping checked')

if __name__ == '__main__':
    main()
