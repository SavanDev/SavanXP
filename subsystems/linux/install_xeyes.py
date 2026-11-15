#!/usr/bin/env python3
"""Install the pinned, unchanged Xeyes runtime into an existing SxFS image.

Run after a normal build. Files not owned by this profile are preserved.
No download, executable conversion, or recreation of the image is performed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]
MANIFEST = Path(__file__).with_name('profile') / 'xeyes-runtime.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=REPO / 'build/linux-profile/root')
    parser.add_argument('--image', type=Path, default=REPO / 'build/disk.img')
    parser.add_argument('--cli', type=Path, default=REPO / 'build/tools/sxfs-cli')
    parser.add_argument('--user', default='user', help='profile receiving the Start shortcut')
    args = parser.parse_args()
    if not args.user or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for c in args.user):
        parser.error('invalid user profile name')
    if not args.image.is_file():
        parser.error('build the existing SxFS image first')
    files = json.loads(MANIFEST.read_text())['files']
    for item in files:
        source = args.root / item['source']
        if not source.is_file() or hashlib.sha256(source.read_bytes()).hexdigest() != item['sha256']:
            parser.error(f"missing or unpinned runtime file: {source}")
    with tempfile.TemporaryDirectory(prefix='ssl-xeyes-install-') as temp:
        stage = Path(temp)
        for item in files:
            destination = stage / 'system/linux' / item['path']
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.root / item['source'], destination)
        # Install into the user's current menu too: /system/start seeds only
        # fresh profiles and must not resurrect other shortcuts a user removed.
        shortcut = 'target=/system/linux/usr/bin/xeyes\n'
        for path in ['system/start/Xeyes.sxlnk', f'users/{args.user}/Start Menu/Xeyes.sxlnk']:
            destination = stage / path; destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(shortcut)
        subprocess.run(['python3', str(REPO / 'tools/stage_licenses.py'), 'stage', '--root', str(REPO),
                        '--out', str(stage / 'system/licenses'), str(REPO / 'assets/licenses/linux-xeyes.notice'),
                        str(REPO / 'assets/licenses/linux-x11.notice')], check=True)
        subprocess.run(['python3', str(REPO / 'tools/sxfs_sync.py'), '--image', str(args.image),
                        '--cli', str(args.cli), '--source', str(stage)], check=True)
    print('Xeyes installed: original ELF, musl, 18 libraries, locale data and Start shortcut')


if __name__ == '__main__':
    main()
