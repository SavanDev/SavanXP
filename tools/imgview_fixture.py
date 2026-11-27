"""Deterministic BMP/PNG/JPEGs for Image Viewer's disposable smoke disks."""
from __future__ import annotations
import argparse
from pathlib import Path
import struct
import subprocess
import zlib
from wallpaper_fixture import rgb_jpeg, PALETTE


def pixels(width: int, height: int, cell: int) -> bytes:
    return b''.join(bytes(PALETTE[(y//cell%2)*2+x//cell%2]) for y in range(height) for x in range(width))


def png(width: int, height: int, data: bytes, alpha: bool = False) -> bytes:
    def chunk(name: bytes, body: bytes) -> bytes:
        return struct.pack('>I',len(body))+name+body+struct.pack('>I',zlib.crc32(name+body))
    stride=width*(4 if alpha else 3)
    rows=b''.join(b'\0'+data[y*stride:(y+1)*stride] for y in range(height))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,6 if alpha else 2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')


def bmp(width: int, height: int, data: bytes) -> bytes:
    stride=(width*3+3)&~3
    rows=b''.join(b''.join(data[(y*width+x)*3:(y*width+x)*3+3][::-1] for x in range(width))+bytes(stride-width*3)
                  for y in range(height-1,-1,-1))
    return b'BM'+struct.pack('<IHHI',54+len(rows),0,0,54)+struct.pack('<IiiHHIIiiII',40,width,height,1,24,0,len(rows),0,0,0,0)+rows


def progressive_jpeg(width: int, height: int) -> bytes:
    """The checkerboard of rgb_jpeg as SOF2: an interleaved DC scan at Al=1, its
    refinement, and one AC band per component carried by EOB runs."""
    def segment(marker: int, payload: bytes) -> bytes:
        return bytes((255, marker)) + struct.pack('>H', len(payload) + 2) + payload

    data = bytearray(b'\xff\xd8')
    data += segment(0xee, b'Adobe\x00\x64\x00\x00\x00\x00\x00')
    data += segment(0xdb, b'\x00' + bytes([1]) * 64)
    # DC categories 0..11 and AC EOB0..EOB14, each as its own 4-bit code.
    data += segment(0xc4, b'\x00' + bytes([0, 0, 0, 12]) + bytes(12) + bytes(range(12)) +
                    b'\x10' + bytes([0, 0, 0, 15]) + bytes(12) + bytes(r << 4 for r in range(15)))
    data += segment(0xc2, struct.pack('>BHHB', 8, height, width, 3) + b'R\x11\x00G\x11\x00B\x11\x00')
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

    def flush() -> None:
        nonlocal pending, count
        if count:
            bits((1 << (8 - count)) - 1, 8 - count)

    across, down = (width + 7) // 8, (height + 7) // 8
    blocks = [PALETTE[(by % 2) * 2 + bx % 2] for by in range(down) for bx in range(across)]
    data += segment(0xda, b'\x03R\x00G\x00B\x00\x00\x00\x01')
    predictors = [0, 0, 0]
    for colour in blocks:
        for c, sample in enumerate(colour):
            dc = ((sample - 128) * 8) >> 1
            delta = dc - predictors[c]
            predictors[c] = dc
            category = abs(delta).bit_length()
            bits(category, 4)
            bits(delta if delta >= 0 else delta + (1 << category) - 1, category)
    flush()
    data += segment(0xda, b'\x03R\x00G\x00B\x00\x00\x00\x10')
    for colour in blocks:
        for sample in colour:
            bits(((sample - 128) * 8) & 1, 1)
    flush()
    for name in b'RGB':
        data += segment(0xda, bytes((1, name, 0, 1, 63, 0)))
        remaining = len(blocks)
        while remaining:
            run = min(remaining, 32767)
            r = run.bit_length() - 1
            bits(r, 4)
            bits(run - (1 << r), r)
            remaining -= run
        flush()
    data += b'\xff\xd9'
    return bytes(data)


def install_fixtures(cli: Path, image: Path, directory: Path) -> None:
    directory.mkdir(parents=True,exist_ok=True)
    small=pixels(16,16,8); jpeg=rgb_jpeg(16,16)
    # Arithmetic coding stays unsupported: kept out of the browsed folder.
    arithmetic=bytearray(jpeg); arithmetic[arithmetic.index(b'\xff\xc0')+1]=0xc9
    files={
        '01-bitmap.BMP':bmp(16,16,small), '02-colour.png':png(16,16,small), '03-colour.JpEg':jpeg,
        '04-large.jpg':rgb_jpeg(4097,17,photo=True), '05-pan.bmp':bmp(1024,768,pixels(1024,768,64)),
        '06-progressive.jpg':progressive_jpeg(16,16), '07-broken.jpg':jpeg[:-2],
        '08-alpha.png':png(16,16,bytes((0,0,255,128))*256,True),
    }
    targets={f'imgview-test/{name}':data for name,data in files.items()}
    targets['imgview-unsupported/arithmetic.jpg']=bytes(arithmetic)
    lines=['mkdir\timgview-test','mkdir\timgview-unsupported']
    for target,data in targets.items():
        file=directory/target.replace('/','-'); file.write_bytes(data)
        lines += [f'file\t{target}\t{file.resolve()}',f'owner\t{target}\t0\t0\t100644']
    manifest=directory/'fixtures.manifest'; manifest.write_text('\n'.join(lines)+'\n')
    subprocess.run([str(cli),'apply',str(image),str(manifest)],check=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sxfs-cli',required=True,type=Path)
    parser.add_argument('--image',required=True,type=Path)
    parser.add_argument('--out-dir',required=True,type=Path)
    args=parser.parse_args()
    install_fixtures(args.sxfs_cli.resolve(),args.image.resolve(),args.out_dir)
