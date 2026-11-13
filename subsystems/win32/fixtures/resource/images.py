#!/usr/bin/env python3
"""Write the images resource.rc compiles into resource.exe, into a folder.

Small and odd-sized so row padding matters, one per format the loader reads:

- core1.bmp   BITMAPCOREHEADER, 1 bit, black and white (a monochrome bitmap)
- info1.bmp   BITMAPINFOHEADER, 1 bit, red and white (a colour bitmap)
- core4.bmp   BITMAPCOREHEADER, 4 bits, 16 colours
- info24.bmp  BITMAPINFOHEADER, 24 bits
- icon.ico    a 16x16 and a 32x32 image, 4 bits
- cursor.cur  one 32x32 image, 1 bit, hot spot (5, 7)

Deterministic: the same bytes every build.
"""

import struct
import sys
from pathlib import Path


def rows(width, height, bpp, pixel):
    """Bottom-up rows, padded to four bytes, of pixel(x, y) indices/colours."""
    out = bytearray()
    for y in reversed(range(height)):
        row = bytearray()
        if bpp == 24:
            for x in range(width):
                r, g, b = pixel(x, y)
                row += bytes((b, g, r))
        else:
            bits = 0
            count = 0
            for x in range(width):
                bits = (bits << bpp) | pixel(x, y)
                count += bpp
                if count == 8:
                    row.append(bits)
                    bits = count = 0
            if count:
                row.append(bits << (8 - count))
        while len(row) % 4:
            row.append(0)
        out += row
    return bytes(out)


def bmp(core, width, height, bpp, palette, pixel):
    if core:
        header = struct.pack("<IHHHH", 12, width, height, 1, bpp)
        table = b"".join(bytes((b, g, r)) for r, g, b in palette)
    else:
        header = struct.pack("<IiiHHIIiiII", 40, width, height, 1, bpp, 0, 0, 2835, 2835, len(palette), 0)
        table = b"".join(bytes((b, g, r, 0)) for r, g, b in palette)
    dib = header + table + rows(width, height, bpp, pixel)
    offset = 14 + len(header) + len(table)
    return struct.pack("<2sIHHI", b"BM", 14 + len(dib), 0, 0, offset) + dib


def icon_image(size, cursor_hotspot=None):
    """A DIB with doubled height: 4-bit colour (or 1-bit for a cursor) and
    the AND mask, a ring transparent outside."""
    bpp = 1 if cursor_hotspot else 4
    palette = [(0, 0, 0), (255, 255, 255)] if bpp == 1 else [
        (0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128), (0, 128, 128),
        (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255),
        (255, 0, 255), (0, 255, 255), (255, 255, 255)]
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, bpp, 0, 0, 0, 0, len(palette), 0)
    table = b"".join(bytes((b, g, r, 0)) for r, g, b in palette)
    centre = size / 2

    def inside(x, y):
        return (x - centre + 0.5) ** 2 + (y - centre + 0.5) ** 2 <= (size / 2) ** 2

    color = rows(size, size, bpp, lambda x, y: ((x + y) % len(palette)) if inside(x, y) else 0)
    mask = rows(size, size, 1, lambda x, y: 0 if inside(x, y) else 1)
    return header + table + color + mask


def ico(images, cursor=False):
    out = struct.pack("<HHH", 0, 2 if cursor else 1, len(images))
    offset = 6 + 16 * len(images)
    data = b""
    for size, image, hotspot in images:
        planes, bits = (hotspot if cursor else (1, 1 if cursor else 4))
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0 if cursor else 16, 0, planes, bits, len(image),
                           offset + len(data))
        data += image
    return out + data


def main():
    folder = Path(sys.argv[1])
    folder.mkdir(parents=True, exist_ok=True)
    folder.joinpath("core1.bmp").write_bytes(
        bmp(True, 13, 9, 1, [(0, 0, 0), (255, 255, 255)], lambda x, y: (x + 2 * y) % 3 == 0))
    folder.joinpath("info1.bmp").write_bytes(
        bmp(False, 11, 7, 1, [(255, 0, 0), (255, 255, 255)], lambda x, y: (x * y) % 4 == 1))
    folder.joinpath("core4.bmp").write_bytes(
        bmp(True, 9, 5, 4, [(i * 16, 255 - i * 16, (i * 37) % 256) for i in range(16)],
            lambda x, y: (x + 3 * y) % 16))
    folder.joinpath("info24.bmp").write_bytes(
        bmp(False, 7, 6, 24, [], lambda x, y: (x * 36, y * 40, (x * y * 9) % 256)))
    folder.joinpath("icon.ico").write_bytes(
        ico([(16, icon_image(16), None), (32, icon_image(32), None)]))
    folder.joinpath("cursor.cur").write_bytes(ico([(32, icon_image(32, (5, 7)), (5, 7))], cursor=True))


if __name__ == "__main__":
    main()
