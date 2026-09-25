#!/usr/bin/env python3
"""Launch the staged SavanXP image in QEMU on Linux."""

from __future__ import annotations

import argparse
import fcntl
import os
import shutil
import subprocess
from pathlib import Path


def find_qemu() -> str | None:
    override = os.environ.get("SAVANXP_QEMU")
    if override:
        candidate = shutil.which(override)
        if candidate:
            return candidate
        path = Path(override)
        if path.is_file() and os.access(path, os.X_OK):
            return str(path)
        return None
    return shutil.which("qemu-system-x86_64")


def find_ovmf(name: str) -> Path | None:
    candidates = [
        os.environ.get(f"OVMF_{name.upper()}"),
        f"/usr/share/edk2/x64/OVMF_{name.upper()}.4m.fd",
        f"/usr/share/edk2-ovmf/x64/OVMF_{name.upper()}.fd",
        f"/usr/share/OVMF/OVMF_{name.upper()}.fd",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None


def build_qemu_args(
    *,
    image_root: Path,
    disk_image: Path,
    code: Path,
    vars_copy: Path,
    debug_log: Path,
    accel: str,
    smp: int,
    virtio: bool,
    headless: bool,
    debug: bool,
    monitor_path: Path | None = None,
    serial_path: Path | None = None,
    qmp_path: Path | None = None,
    audio_device: str = "auto",
    wav_path: Path | None = None,
) -> list[str]:
    """Build the common QEMU command used by normal and smoke launches."""
    args_list = [
        "qemu-system-x86_64",
        "-machine",
        "q35,pcspk-audiodev=audio0",
        "-accel",
        accel,
        "-m",
        "256M",
        "-smp",
        str(smp),
        "-cpu",
        "host" if accel == "kvm" else "max",
        "-audiodev",
        "none,id=audio0" if headless else "sdl,id=audio0",
        "-display",
        "none" if headless else "gtk,grab-on-hover=on,show-cursor=off,window-close=on,zoom-to-fit=off",
        "-rtc",
        "base=localtime",
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={code}",
        "-drive",
        f"if=pflash,format=raw,file={vars_copy}",
        "-drive",
        f"file=fat:rw:{image_root},format=raw",
        "-netdev",
        "user,id=net0",
        "-serial",
        f"file:{serial_path}" if serial_path is not None else "stdio",
        "-debugcon",
        f"file:{debug_log}",
        "-global",
        "isa-debugcon.iobase=0xe9",
    ]
    if monitor_path is not None:
        args_list += ["-no-reboot", "-no-shutdown"]
        args_list += [
            "-monitor",
            f"unix:{monitor_path},server=on,wait=off",
        ]
    if qmp_path is not None:
        args_list += [
            "-qmp",
            f"unix:{qmp_path},server=on,wait=off",
        ]

    drive = f"if=none,id=svdisk,media=disk,format=raw,file={disk_image}"
    if virtio:
        args_list += [
            "-device",
            "virtio-net-pci,netdev=net0",
            "-drive",
            drive,
            "-device",
            "virtio-blk-pci,drive=svdisk",
            "-device",
            "virtio-vga,xres=1280,yres=800",
            "-device",
            "virtio-tablet-pci",
            "-device",
            "virtio-keyboard-pci",
        ]
    else:
        args_list += [
            "-device",
            "rtl8139,netdev=net0",
            "-drive",
            drive,
            "-device",
            "isa-ide,id=svide",
            "-device",
            "ide-hd,drive=svdisk,bus=svide.0",
            "-device",
            "VGA,edid=on,xres=1280,yres=800",
        ]
    if wav_path is not None:
        audio1 = f"wav,id=audio1,path={wav_path}"
    else:
        audio1 = "none,id=audio1" if headless else "sdl,id=audio1"
    sound_device = "virtio-sound-pci,audiodev=audio1,streams=2" if (
        audio_device == "virtio" or (audio_device == "auto" and virtio)
    ) else "AC97,audiodev=audio1"
    args_list += [
        "-audiodev",
        audio1,
        "-device",
        sound_device,
    ]
    if debug:
        args_list += ["-s", "-S"]
    return args_list


def prepare_image(
    *,
    image_root: Path,
    disk_image: Path,
    ovmf_code: Path | None,
    ovmf_vars: Path | None,
    vars_copy: Path | None = None,
    debug_log: Path | None = None,
) -> tuple[Path, Path]:
    code = ovmf_code or find_ovmf("CODE")
    variables = ovmf_vars or find_ovmf("VARS")
    if not code or not variables:
        raise SystemExit(
            "OVMF was not found; set OVMF_CODE and OVMF_VARS or pass --ovmf-code/--ovmf-vars"
        )
    image_root = image_root.resolve()
    disk_image = disk_image.resolve()
    vars_copy = vars_copy or image_root.parent / "OVMF_VARS.fd"
    debug_log = debug_log or image_root.parent / "debugcon.log"
    vars_copy.parent.mkdir(parents=True, exist_ok=True)
    debug_log.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(variables, vars_copy)
    debug_log.unlink(missing_ok=True)
    return code, vars_copy


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image-root", required=True, type=Path)
    parser.add_argument("--disk-image", required=True, type=Path)
    parser.add_argument("--ovmf-code", type=Path)
    parser.add_argument("--ovmf-vars", type=Path)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    parser.add_argument("--smp", type=int, default=1)
    parser.add_argument("--virtio", action="store_true")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--qmp-path", type=Path, help="QMP Unix socket path")
    parser.add_argument("--monitor-path", type=Path, help="QEMU HMP Unix socket path")
    parser.add_argument("--serial-log", type=Path, help="write guest serial output here")
    parser.add_argument("--vars-copy", type=Path, help="writable OVMF variable copy")
    args = parser.parse_args()
    if not 1 <= args.smp <= 32:
        parser.error("--smp must be between 1 and 32")

    qemu = find_qemu()
    if not qemu:
        raise SystemExit("qemu-system-x86_64 was not found in PATH")
    image_root = args.image_root.resolve()
    disk_image = args.disk_image.resolve()
    code, vars_copy = prepare_image(
        image_root=image_root,
        disk_image=disk_image,
        ovmf_code=args.ovmf_code,
        ovmf_vars=args.ovmf_vars,
        vars_copy=args.vars_copy,
    )
    args_list = build_qemu_args(
        image_root=image_root,
        disk_image=disk_image,
        code=code,
        vars_copy=vars_copy,
        debug_log=args.image_root.resolve().parent / "debugcon.log",
        accel=args.accel,
        smp=args.smp,
        virtio=args.virtio,
        headless=args.headless,
        debug=args.debug,
        qmp_path=args.qmp_path,
        monitor_path=args.monitor_path,
        serial_path=args.serial_log,
    )
    args_list[0] = qemu

    print("qemu:", " ".join(args_list), flush=True)
    lock_path = disk_image.with_name(disk_image.name + ".lock")
    with lock_path.open("a+") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        return subprocess.run(args_list, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
