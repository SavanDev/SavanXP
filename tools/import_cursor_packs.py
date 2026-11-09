#!/usr/bin/env python3
"""Reproduce checked-in CUR packs from the classic PNGs and pinned release ZIPs.

Third-party CURs are copied verbatim. Only the first embedded CUR of wait.ani
is extracted; animation is deliberately outside the static compositor format.
"""
import argparse
import struct
import zipfile
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent / 'assets/desktop/cursors'
ROLES = ('arrow', 'wait', 'text', 'move', 'resize_h', 'resize_v',
         'unavailable', 'link', 'resize_nwse', 'resize_nesw')
SPOTS = ((3, 1), (9, 9), (3, 8), (9, 9), (9, 5), (5, 9), (9, 9), (5, 2))


def encode(image, spot):
    image = image.convert('RGBA')
    w, h = image.size
    stride = ((w + 31) // 32) * 4
    pixels, mask = bytearray(), bytearray()
    for y in reversed(range(h)):
        row = bytearray(stride)
        for x in range(w):
            r, g, b, a = image.getpixel((x, y))
            pixels.extend((b, g, r, a))
            if not a:
                row[x // 8] |= 0x80 >> (x % 8)
        mask.extend(row)
    dib = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 32, 0, len(pixels), 0, 0, 0, 0)
    payload = dib + pixels + mask
    return struct.pack('<HHHBBBBHHII', 0, 2, 1, w, h, 0, 0, *spot, len(payload), 22) + payload


def diagonal():
    # A white double arrow with a one-pixel black outline, matching the old pack.
    inside = {(x, y) for y in range(4, 15) for x in range(4, 15) if abs(x-y) <= 1}
    inside |= {(x, y) for y in range(2, 9) for x in range(2, 9) if x+y <= 10}
    inside |= {(18-x, 18-y) for x, y in tuple(inside) if x <= 8 and y <= 8}
    image = Image.new('RGBA', (19, 19))
    for x, y in inside:
        color = (255, 255, 255, 255) if all((x+dx, y+dy) in inside for dx, dy in
                ((-1, 0), (1, 0), (0, -1), (0, 1))) else (0, 0, 0, 255)
        image.putpixel((x, y), color)
    return image


def first_frame(data):
    assert data[:4] == b'RIFF' and data[8:12] == b'ACON'
    def chunks(start, end):
        while start + 8 <= end:
            kind, size = struct.unpack_from('<4sI', data, start)
            body, finish = start + 8, start + 8 + size
            assert finish <= end
            if kind == b'icon':
                return data[body:finish]
            if kind == b'LIST' and data[body:body+4] == b'fram':
                found = chunks(body+4, finish)
                if found:
                    return found
            start = finish + (size & 1)
    frame = chunks(12, len(data))
    assert frame and frame[:4] == b'\0\0\2\0'
    return frame


def import_zip(filename, directory, theme, names):
    out = ROOT / theme
    out.mkdir(exist_ok=True)
    with zipfile.ZipFile(filename) as source:
        for role, name in zip(ROLES, names):
            data = source.read(directory + '/' + name)
            if name.endswith('.ani'):
                data = first_frame(data)
            (out / (role + '.cur')).write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--retrosmart', type=Path)
    parser.add_argument('--hackneyed', type=Path)
    args = parser.parse_args()
    for role, spot in zip(ROLES, SPOTS):
        (ROOT / 'classic' / (role+'.cur')).write_bytes(encode(Image.open(ROOT / 'classic/source' / (role+'.png')), spot))
    nwse = diagonal()
    for role, image in (('resize_nwse', nwse), ('resize_nesw', nwse.transpose(Image.Transpose.FLIP_LEFT_RIGHT))):
        (ROOT / 'classic' / (role+'.cur')).write_bytes(encode(image, (9, 9)))
    if args.retrosmart:
        import_zip(args.retrosmart, 'retrosmart-xcursor-win-ish-classic', 'retrosmart',
            ('default.cur', 'wait.ani', 'text.cur', 'sizeall.cur', 'sizewe.cur', 'sizens.cur',
             'not-allowed.cur', 'pointer.cur', 'sizenwse.cur', 'sizenesw.cur'))
    if args.hackneyed:
        import_zip(args.hackneyed, 'Hackneyed-Windows-0.9.3/Standard', 'hackneyed',
            ('default.cur', 'wait.ani', 'text.cur', 'move.cur', 'ew-resize.cur', 'ns-resize.cur',
             'not-allowed.cur', 'pointer.cur', 'nwse-resize.cur', 'nesw-resize.cur'))


if __name__ == '__main__':
    main()
