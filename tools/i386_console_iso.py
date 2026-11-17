"""Derive an explicit console diagnostic boot from the exact supplied GRUB ISO."""
from pathlib import Path
import os
import subprocess


def make_console_iso(source: Path, destination: Path) -> Path:
    config = destination / "grub.cfg"
    iso = destination / "console.iso"
    xorriso = os.environ.get("SAVANXP_XORRISO", "xorriso")
    log_path = destination / "console-iso.log"
    with log_path.open("w") as log:
        subprocess.run([xorriso, "-osirrox", "on", "-indev", str(source),
                        "-extract", "/boot/grub/grub.cfg", str(config)],
                       stdout=log, stderr=log, check=True)
        config.chmod(0o600)
        text = config.read_text()
        needle = "multiboot2 /boot/savanxp-kernel.elf"
        if needle not in text:
            raise ValueError("the supplied ISO has no supported i386 GRUB entry")
        config.write_text(text.replace(needle, needle + " console-init"))
        # Replay the existing BIOS boot records; preserve the exact kernel,
        # CPIO and GRUB modules without discovering a second GRUB installation.
        result = subprocess.run([xorriso, "-indev", str(source), "-outdev", str(iso),
                                 "-boot_image", "any", "replay",
                                 "-map", str(config), "/boot/grub/grub.cfg",
                                 "-commit", "-end"], stdout=log, stderr=log)
        if result.returncode:
            log.flush()
            raise RuntimeError(f"console ISO failed: {log_path.read_text()[-3000:]}")
    return iso
