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


def install_fixtures(cli: Path, image: Path, directory: Path) -> None:
    directory.mkdir(parents=True,exist_ok=True)
    small=pixels(16,16,8); jpeg=rgb_jpeg(16,16)
    progressive=bytearray(jpeg); progressive[progressive.index(b'\xff\xc0')+1]=0xc2
    files={
        '01-bitmap.BMP':bmp(16,16,small), '02-colour.png':png(16,16,small), '03-colour.JpEg':jpeg,
        '04-large.jpg':rgb_jpeg(4097,17,photo=True), '05-pan.bmp':bmp(1024,768,pixels(1024,768,64)),
        '06-progressive.jpg':bytes(progressive), '07-broken.jpg':jpeg[:-2],
        '08-alpha.png':png(16,16,bytes((0,0,255,128))*256,True),
    }
    lines=['mkdir\timgview-test']
    for name,data in files.items():
        file=directory/name; file.write_bytes(data)
        lines += [f'file\timgview-test/{name}\t{file.resolve()}',f'owner\timgview-test/{name}\t0\t0\t100644']
    manifest=directory/'fixtures.manifest'; manifest.write_text('\n'.join(lines)+'\n')
    subprocess.run([str(cli),'apply',str(image),str(manifest)],check=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sxfs-cli',required=True,type=Path)
    parser.add_argument('--image',required=True,type=Path)
    parser.add_argument('--out-dir',required=True,type=Path)
    args=parser.parse_args()
    install_fixtures(args.sxfs_cli.resolve(),args.image.resolve(),args.out_dir)
