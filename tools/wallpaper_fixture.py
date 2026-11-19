"""Own constant-block RGB JPEGs, installed only on a disposable visual-test disk.

T.81 SOF0, unit quantizers, canonical DC categories 0..11, AC EOB, Adobe RGB.
No encoder dependency or third-party pixels. Large sources exercise wallpaper
reduction; small tiles give exact scanout colours independently of the decoder.
"""
from __future__ import annotations

import argparse
import struct
import subprocess
from pathlib import Path


PALETTE = ((224, 48, 80), (32, 208, 96), (48, 80, 224), (224, 208, 48))


def rgb_jpeg(width: int, height: int, *, photo: bool = False) -> bytes:
    def segment(marker: int, payload: bytes) -> bytes:
        return bytes((255, marker)) + struct.pack('>H', len(payload) + 2) + payload

    data = bytearray(b'\xff\xd8')
    data += segment(0xee, b'Adobe\x00\x64\x00\x00\x00\x00\x00')
    data += segment(0xdb, b'\x00' + bytes([1]) * 64)
    data += segment(0xc4, b'\x00' + bytes([0, 0, 0, 12]) + bytes(12) + bytes(range(12)) +
                    b'\x10\x01' + bytes(15) + b'\x00')
    data += segment(0xc0, struct.pack('>BHHB', 8, height, width, 3) + b'R\x11\x00G\x11\x00B\x11\x00')
    data += segment(0xda, b'\x03R\x00G\x00B\x00\x00\x3f\x00')
    pending = count = 0

    def bits(value: int, size: int) -> None:
        nonlocal pending, count
        for bit in range(size - 1, -1, -1):
            pending = (pending << 1) | ((value >> bit) & 1)
            count += 1
            if count == 8:
                data.append(pending)
                if pending == 255:
                    data.append(0)
                pending = count = 0

    predictors = [0, 0, 0]
    for by in range((height + 7) // 8):
        for bx in range((width + 7) // 8):
            colour = PALETTE[int(bx >= 256)] if photo else PALETTE[(by % 2) * 2 + bx % 2]
            for c, sample in enumerate(colour):
                dc = (sample - 128) * 8
                delta = dc - predictors[c]
                predictors[c] = dc
                category = abs(delta).bit_length()
                bits(category, 4)
                bits(delta if delta >= 0 else delta + (1 << category) - 1, category)
                bits(0, 1)
    if count:
        bits((1 << (8 - count)) - 1, 8 - count)
    data += b'\xff\xd9'
    return bytes(data)


def install_fixtures(cli: Path, image: Path, directory: Path) -> None:
    """Seed only the test copy; no files enter rootfs, diskfs or the build image."""
    directory.mkdir(parents=True, exist_ok=True)
    small = rgb_jpeg(16, 16)
    fixtures = {
        'system/wallpapers/00-I5 Photo.JpEg': rgb_jpeg(4097, 17, photo=True),
        'system/wallpapers/01-I5 RGB.JPG': small,
        'i5-picture.png': small,  # Deliberately mislabeled: content selects the decoder.
        'i5-broken.jpg': small[:-2],  # Header is valid; decode must still require EOI.
    }
    lines = ['mkdir\tsystem/wallpapers']
    for index, (target, contents) in enumerate(fixtures.items()):
        source = directory / f'{index}.jpg'
        source.write_bytes(contents)
        lines += [f'file\t{target}\t{source.resolve()}', f'owner\t{target}\t0\t0\t100644']
    manifest = directory / 'fixtures.manifest'
    manifest.write_text('\n'.join(lines) + '\n')
    subprocess.run([str(cli), 'apply', str(image), str(manifest)], check=True)
    subprocess.run([str(cli), 'rm', '-f', str(image), 'users/user/Settings/desktop.cfg',
                    'users/user/desktop.cfg', 'desktop.cfg'], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--sxfs-cli', required=True, type=Path)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--out-dir', required=True, type=Path)
    args = parser.parse_args()
    install_fixtures(args.sxfs_cli.resolve(), args.image.resolve(), args.out_dir)
