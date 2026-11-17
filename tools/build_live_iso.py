#!/usr/bin/env python3
"""Build a disposable SxFS LiveCD module without copying the persistent disk."""
import argparse
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sectors", type=int, default=262144)
    args = parser.parse_args()
    if args.iso.resolve() == args.output.resolve():
        parser.error("the LiveCD must have a separate output path")
    xorriso = os.environ.get("SAVANXP_XORRISO", "xorriso")
    with tempfile.TemporaryDirectory(prefix="sx-live.", dir=args.output.parent) as tmp:
        tmp = Path(tmp)
        image = tmp / "live-disk.img"
        subprocess.run([sys.executable, str(Path(__file__).with_name("sxfs_sync.py")),
            "--source", str(args.source), "--cli", str(args.cli),
            "--image", str(image), "--sectors", str(args.sectors)], check=True)
        config = tmp / "grub.cfg"
        subprocess.run([xorriso, "-osirrox", "on", "-indev", str(args.iso),
            "-extract", "/boot/grub/grub.cfg", str(config)], check=True)
        config.chmod(0o600)
        text = config.read_text()
        needle = "    module2 /boot/initramfs.cpio initramfs\n"
        if text.count(needle) != 1:
            raise RuntimeError("unexpected GRUB module layout")
        config.write_text(text.replace(needle, needle + "    module2 /boot/live-disk.img sxfs\n"))
        output = tmp / "live.iso"
        subprocess.run([xorriso, "-indev", str(args.iso), "-outdev", str(output),
            "-boot_image", "any", "replay", "-map", str(config), "/boot/grub/grub.cfg",
            "-map", str(image), "/boot/live-disk.img", "-commit", "-end"], check=True)
        os.replace(output, args.output)
    print(f"i386 LiveCD: {args.output}")

if __name__ == "__main__":
    main()
