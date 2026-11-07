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


DISK_BUSES = ("ahci", "ide")

# A tablet is what a QEMU window wants (the host pointer maps straight onto
# it). Next to the PS/2 mouse it is wrong for relative scenarios: QEMU sends
# relative moves to the mouse but buttons to whichever pointer it last
# activated, and the tablet reports a click at its own position.
USB_POINTERS = ("tablet", "mouse")

# The window QEMU shows. GTK is the default: it scales and has menus. Its pointer
# grab cannot confine the host pointer on a Wayland session, so a game that
# captures the pointer (Nightwood's mouse look) sees the mouse leave the window;
# SDL locks the pointer through its relative mouse mode and keeps it in.
# SAVANXP_QEMU_DISPLAY picks one (docs/build/BUILD_LINUX.md#the-qemu-window).
QEMU_DISPLAYS = {
    "gtk": "gtk,grab-on-hover=on,show-cursor=off,window-close=on,zoom-to-fit=off",
    "sdl": "sdl,window-close=on",
}


def qemu_display() -> str:
    name = os.environ.get("SAVANXP_QEMU_DISPLAY", "gtk")
    if name not in QEMU_DISPLAYS:
        raise SystemExit(
            f"unknown SAVANXP_QEMU_DISPLAY {name!r}; expected one of {', '.join(QEMU_DISPLAYS)}")
    return QEMU_DISPLAYS[name]


def qemu_display_arg(*, headless: bool, gl: bool) -> str:
    if headless:
        return "egl-headless" if gl else "none"
    display = qemu_display()
    return display + ",gl=on" if gl else display


def build_qemu_args(
    *,
    image_root: Path,
    disk_image: Path | None,
    code: Path,
    vars_copy: Path,
    debug_log: Path,
    accel: str,
    smp: int,
    virtio: bool,
    headless: bool,
    debug: bool,
    gl: bool = False,
    monitor_path: Path | None = None,
    serial_path: Path | None = None,
    qmp_path: Path | None = None,
    audio_device: str = "auto",
    wav_path: Path | None = None,
    disk_bus: str | None = None,
    extra_disks: tuple[tuple[Path, str], ...] = (),
    usb: bool = False,
    usb_stick: Path | None = None,
    ps2: bool = True,
    usb_pointer: str = "tablet",
    usb_hub: bool = False,
) -> list[str]:
    """Build the common QEMU command used by normal and smoke launches.

    Without --virtio the disk image goes on the q35 AHCI controller (DMA), or
    on isa-ide (PIO) when disk_bus or SAVANXP_DISK_BUS is "ide". With no disk
    image the machine has no disk at all, which is how the rescue system boots.

    gl implies virtio and swaps virtio-vga for virtio-vga-gl, which offers
    virgl to the guest (docs/roadmaps/open/VIRGL_ROADMAP.md). It needs a GL
    display: gl=on on the window, egl-headless when headless.

    extra_disks are (image, serial) pairs put on the next AHCI ports, for the
    scenarios that need a disk of their own to write (Disk Management's). The
    serial is how the guest tells them from the system's disk.

    usb adds an xHCI controller with a keyboard and a pointer on it (a tablet,
    or a relative mouse with usb_pointer="mouse"); usb_stick
    adds a raw image as a USB mass-storage device on the same controller
    (docs/roadmaps/done/USB_ROADMAP.md). SAVANXP_USB_TRACE=PATH logs QEMU's
    usb_xhci_* trace events to PATH.
    """
    virtio = virtio or gl
    disk_bus = disk_bus or os.environ.get("SAVANXP_DISK_BUS", "ahci")
    if disk_bus not in DISK_BUSES:
        raise SystemExit(f"unknown disk bus {disk_bus!r}; expected one of {', '.join(DISK_BUSES)}")
    args_list = [
        "qemu-system-x86_64",
        "-machine",
        "q35,pcspk-audiodev=audio0" + ("" if ps2 else ",i8042=off"),
        "-accel",
        accel,
        "-m",
        "1024M",
        "-smp",
        str(smp),
        "-cpu",
        "host" if accel == "kvm" else "max",
        "-audiodev",
        "none,id=audio0" if headless else "sdl,id=audio0",
        "-display",
        qemu_display_arg(headless=headless, gl=gl),
        "-rtc",
        "base=localtime",
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={code}",
        "-drive",
        f"if=pflash,format=raw,file={vars_copy}",
        # The boot drive: QEMU's virtual FAT over the staging tree, read-only,
        # under a throwaway overlay. The guest sees a writable disk (/boot,
        # docs/roadmaps/open/FAT_ROADMAP.md) and its writes vanish at exit: they never
        # reach build/image, and vvfat's own write-back never runs.
        "-drive",
        f"file=fat:{image_root},format=raw,snapshot=on",
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
    if virtio and extra_disks:
        # virtio_blk drives a single device (vblk0) today.
        raise SystemExit("extra disks need the AHCI machine: run without --virtio")
    if virtio:
        disk_devices = [] if disk_image is None else [
            "-drive",
            drive,
            "-device",
            "virtio-blk-pci,drive=svdisk",
        ]
        args_list += [
            "-device",
            "virtio-net-pci,netdev=net0",
            *disk_devices,
            "-device",
            ("virtio-vga-gl" if gl else "virtio-vga") + ",xres=1280,yres=800",
            "-device",
            "virtio-tablet-pci",
            "-device",
            "virtio-keyboard-pci",
        ]
    else:
        if disk_bus == "ide":
            disk_devices = [
                "-device",
                "isa-ide,id=svide",
                "-device",
                "ide-hd,drive=svdisk,bus=svide.0",
            ]
        else:
            # Port 1 of the board's AHCI controller; port 0 holds the FAT boot
            # drive (`-drive file=fat:...` defaults to the first AHCI port).
            disk_devices = [
                "-device",
                "ide-hd,drive=svdisk,bus=ide.1",
            ]
        if disk_image is None:
            disk_devices = []
        else:
            disk_devices = ["-drive", drive, *disk_devices]
        if extra_disks and disk_bus != "ahci":
            raise SystemExit("extra disks go on the AHCI controller: run without SAVANXP_DISK_BUS=ide")
        for index, (path, serial) in enumerate(extra_disks):
            disk_devices += [
                "-drive",
                f"if=none,id=svextra{index},media=disk,format=raw,file={path}",
                "-device",
                f"ide-hd,drive=svextra{index},bus=ide.{2 + index},serial={serial}",
            ]
        args_list += [
            "-device",
            "rtl8139,netdev=net0",
            *disk_devices,
            "-device",
            "VGA,edid=on,xres=1280,yres=800",
        ]
    if usb or usb_stick is not None:
        args_list += ["-device", "qemu-xhci,id=xhci"]
        trace_path = os.environ.get("SAVANXP_USB_TRACE")
        if trace_path:
            args_list += ["-trace", f"enable=usb_xhci_*,file={trace_path}"]
    if usb:
        if usb_pointer not in USB_POINTERS:
            raise SystemExit(f"unknown USB pointer {usb_pointer!r}; expected one of {', '.join(USB_POINTERS)}")
        # Behind the hub the devices name their port: 1.1 is port 1 of the hub
        # on root port 1.
        hub_ports = (",port=1.1", ",port=1.2") if usb_hub else ("", "")
        if usb_hub:
            args_list += ["-device", "usb-hub,bus=xhci.0,port=1,id=usbhub"]
        args_list += [
            "-device",
            f"usb-kbd,bus=xhci.0,id=usbkbd{hub_ports[0]}",
            "-device",
            f"usb-{usb_pointer},bus=xhci.0,id=usb{usb_pointer}{hub_ports[1]}",
        ]
    if usb_stick is not None:
        args_list += [
            "-drive",
            f"if=none,id=usbstick,media=disk,format=raw,file={usb_stick}",
            "-device",
            "usb-storage,bus=xhci.0,drive=usbstick,id=usbstorage,serial=SXUSB0",
        ]
    if wav_path is not None:
        audio1 = f"wav,id=audio1,path={wav_path}"
    else:
        audio1 = "none,id=audio1" if headless else "sdl,id=audio1"
    if audio_device == "hda":
        if wav_path is not None:
            audio1 += ",out.frequency=48000"
        devices = ("ich9-intel-hda,id=hda", "hda-duplex,bus=hda.0,audiodev=audio1")
    elif audio_device == "virtio" or (audio_device == "auto" and virtio):
        devices = ("virtio-sound-pci,audiodev=audio1,streams=2",)
    else:
        devices = ("AC97,audiodev=audio1",)
    args_list += ["-audiodev", audio1]
    for device in devices:
        args_list += ["-device", device]
    if debug:
        args_list += ["-s", "-S"]
    return args_list


def prepare_image(
    *,
    image_root: Path,
    disk_image: Path | None,
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
    parser.add_argument("--smp", type=int, default=2)
    parser.add_argument("--virtio", action="store_true")
    parser.add_argument("--gl", action="store_true",
                        help="virtio machine with virtio-vga-gl (virgl); implies --virtio")
    parser.add_argument("--disk-bus", choices=DISK_BUSES,
                        help="disk controller without --virtio (default: SAVANXP_DISK_BUS or ahci)")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--qmp-path", type=Path, help="QMP Unix socket path")
    parser.add_argument("--monitor-path", type=Path, help="QEMU HMP Unix socket path")
    parser.add_argument("--serial-log", type=Path, help="write guest serial output here")
    parser.add_argument("--audio-device", choices=("auto", "ac97", "virtio", "hda"),
                        default="auto", help="sound device (hda selects Intel HDA with an analog codec)")
    parser.add_argument("--vars-copy", type=Path, help="writable OVMF variable copy")
    parser.add_argument("--extra-disk", type=Path,
                        help="a second disk image (serial SXBLANK0) for Disk Management to write")
    parser.add_argument("--usb", action="store_true",
                        help="add an xHCI controller with a USB keyboard and tablet")
    parser.add_argument("--usb-pointer", choices=USB_POINTERS, default="tablet",
                        help="the pointer --usb adds (default: tablet)")
    parser.add_argument("--usb-stick", type=Path,
                        help="attach this raw image as a USB mass-storage device")
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
        gl=args.gl,
        disk_bus=args.disk_bus,
        qmp_path=args.qmp_path,
        monitor_path=args.monitor_path,
        serial_path=args.serial_log,
        audio_device=args.audio_device,
        extra_disks=((args.extra_disk.resolve(), "SXBLANK0"),) if args.extra_disk else (),
        usb=args.usb,
        usb_stick=args.usb_stick.resolve() if args.usb_stick else None,
        usb_pointer=args.usb_pointer,
    )
    args_list[0] = qemu

    print("qemu:", " ".join(args_list), flush=True)
    lock_path = disk_image.with_name(disk_image.name + ".lock")
    with lock_path.open("a+") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        return subprocess.run(args_list, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
