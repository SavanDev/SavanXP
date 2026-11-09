#!/usr/bin/env python3
"""Independently compare all installed packs against compositor scanout pixels."""
import argparse
import io
import struct
import time
from pathlib import Path
from PIL import Image
from qmp_client import QmpClient

ROLES = ('arrow', 'wait', 'text', 'move', 'resize_h', 'resize_v', 'unavailable', 'link', 'resize_nwse', 'resize_nesw')


def cur_image(path):
    data = path.read_bytes()
    entries = [struct.unpack_from('<BBBBHHII', data, 6+i*16)
               for i in range(struct.unpack_from('<H', data, 4)[0])]
    entry = next(e for e in entries if 0 < e[0] <= 32 and 0 < e[1] <= 32)
    width, height, _, _, hx, hy, length, offset = entry
    if data[offset:offset+8] == b'\x89PNG\r\n\x1a\n':
        return Image.open(io.BytesIO(data[offset:offset+length])).convert('RGBA')
    assert struct.unpack_from('<IiiHH', data, offset) == (40, width, height*2, 1, 32)
    pixels = Image.new('RGBA', (width, height))
    for y in range(height):
        for x in range(width):
            b, g, r, a = struct.unpack_from('4B', data, offset+40+((height-1-y)*width+x)*4)
            pixels.putpixel((x, y), (r, g, b, a))
    return pixels


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--socket', type=Path, required=True)
    parser.add_argument('--out-dir', type=Path, required=True)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    shot = (args.out_dir / 'cursor-packs.ppm').resolve()
    time.sleep(0.3)
    with QmpClient(args.socket) as qmp:
        qmp.screenshot(shot)
    image = Image.open(shot).convert('RGB')
    root = Path(__file__).resolve().parent.parent
    for theme, name in enumerate(('classic', 'retrosmart', 'hackneyed')):
        for column, role in enumerate(ROLES):
            cursor = cur_image(root / f'assets/desktop/cursors/{name}/{role}.cur')
            if name == 'classic' and column < 8:
                assert cursor.tobytes() == Image.open(root / f'assets/desktop/cursors/classic/source/{role}.png').convert('RGBA').tobytes()
            for row, background in enumerate((255, 0)):
                for y in range(64):
                    for x in range(64):
                        r,g,b,a = cursor.getpixel((x,y)) if x < cursor.width and y < cursor.height else (0,0,0,0)
                        expected = tuple((c*a+background*(255-a)+127)//255 for c in (r,g,b))
                        actual = image.getpixel((32+column*96+x, 32+theme*176+row*80+y))
                        assert actual == expected, (name, role, row, x, y, actual, expected)
    for point, expected in {
        (32,600):(0,0,0), (33,600):(255,255,255), (32,601):(18,52,86), (33,601):(237,203,169),
        (64,600):(0,0,0), (65,600):(18,52,86), (64,601):(18,52,86), (65,601):(18,52,86),
        (0,632):(255,255,255), (0,633):(237,203,169), (1,632):(18,52,86),
    }.items():
        assert image.getpixel(point) == expected, (point, image.getpixel(point), expected)
    for y in range(680,744):
        for x in range(900,964):
            assert image.getpixel((x,y)) == (128,128,128), ('hardware cursor residue', x, y)
    image.save(args.out_dir / 'cursor-packs.png')
    print('cursor-smoke: all 30 cursors match on light/dark backgrounds; original classic pixels, inversion and clipping checked')


if __name__ == '__main__':
    main()
