#!/usr/bin/env python3
"""Launch the staged SavanXP image in QEMU on Linux."""

from __future__ import annotations

import argparse
import fcntl
import os
import secrets
import shutil
import socket
import struct
import subprocess
import sys
from contextlib import ExitStack
from pathlib import Path

from block_device import claim, configured_sudo, device_identity


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


DISK_BUSES = ("ahci", "ide", "nvme")
SXFS_SECTOR_SIZE = 512
SXFS_SUPERBLOCK_MAGIC = b"SXFS\0\0\0\0"

# A tablet is what a QEMU window wants (the host pointer maps straight onto
# it). Next to the PS/2 mouse it is wrong for relative scenarios: QEMU sends
# relative moves to the mouse but buttons to whichever pointer it last
# activated, and the tablet reports a click at its own position.
USB_POINTERS = ("tablet", "mouse")
# The USB host controller --usb puts the devices on. ehci is an ICH9 EHCI
# with its three UHCI companions, as on a Core 2 board: high-speed devices
# stay on EHCI, the rest are handed to a UHCI. ehci-ohci gives the same EHCI
# two OHCI companions instead, as on an AMD board. (QEMU's plain usb-ehci
# takes no companions.)
USB_HOSTS = ("xhci", "ehci", "ehci-ohci", "uhci", "ohci")
# The emulated IOMMU --iommu adds, with interrupt remapping, so the guest
# finds an IVRS (amd) or a DMAR (intel) in its ACPI tables
# (docs/kernel/IOMMU.md). "none" is the board without one.
IOMMUS = ("none", "amd", "intel")


def usb_host_devices(usb_host: str) -> tuple[list[str], str]:
    """The -device arguments of a USB host controller and its bus name."""
    if usb_host == "xhci":
        return ["-device", "qemu-xhci,id=xhci"], "xhci.0"
    if usb_host == "uhci":
        return ["-device", "piix3-usb-uhci,id=uhci"], "uhci.0"
    if usb_host == "ohci":
        return ["-device", "pci-ohci,id=ohci,num-ports=4"], "ohci.0"
    if usb_host == "ehci":
        devices = ["-device", "ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on"]
        for index in range(3):
            devices += [
                "-device",
                f"ich9-usb-uhci{index + 1},masterbus=ehci.0,firstport={index * 2},"
                f"addr=1d.{index},multifunction=on",
            ]
        return devices, "ehci.0"
    if usb_host == "ehci-ohci":
        devices = ["-device", "ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on"]
        for index in range(2):
            devices += [
                "-device",
                f"pci-ohci,masterbus=ehci.0,firstport={index * 3},num-ports=3,"
                f"addr=1d.{index},multifunction=on",
            ]
        return devices, "ehci.0"
    raise SystemExit(f"unknown USB host {usb_host!r}; expected one of {', '.join(USB_HOSTS)}")

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
    capture_fixture: bool = False,
    wav_path: Path | None = None,
    disk_bus: str | None = None,
    extra_disks: tuple[tuple[Path, str], ...] = (),
    second_ahci: bool = False,
    usb: bool = False,
    usb_stick: Path | None = None,
    network: bool = True,
    ps2: bool = True,
    usb_pointer: str = "tablet",
    usb_hub: bool = False,
    usb_host: str = "xhci",
    iommu: str = "none",
    rng: bool = True,
    disk_fds: tuple[int, ...] = (),
    disk_snapshot: bool = False,
) -> list[str]:
    """Build the common QEMU command used by normal and smoke launches.

    Without --virtio the disk image goes on the q35 AHCI controller (DMA), on
    isa-ide (PIO) when disk_bus or SAVANXP_DISK_BUS is "ide", or on an NVMe
    controller when it is "nvme" (docs/kernel/NVME.md). With no disk
    image the machine has no disk at all, which is how the rescue system boots.

    gl implies virtio and swaps virtio-vga for virtio-vga-gl, which offers
    virgl to the guest (docs/roadmaps/open/VIRGL_ROADMAP.md). It needs a GL
    display: gl=on on the window, egl-headless when headless.

    extra_disks are (image, serial) pairs put on the next AHCI ports, for the
    scenarios that need a disk of their own to write (Disk Management's). The
    serial is how the guest tells them from the system's disk. second_ahci puts
    them on an AHCI controller of their own instead, for a machine with two HBAs.

    usb adds a USB host controller (xHCI unless usb_host says otherwise, see
    USB_HOSTS) with a keyboard and a pointer on it (a tablet, or a relative
    mouse with usb_pointer="mouse"); usb_stick adds a raw image as a USB
    mass-storage device on the same controller
    (docs/roadmaps/done/USB_ROADMAP.md). SAVANXP_USB_TRACE=PATH logs QEMU's
    usb_xhci_* trace events to PATH.

    iommu adds an emulated AMD or Intel IOMMU (IOMMUS). QEMU wants it before
    any other device, and under KVM its interrupt remapping needs the split
    irqchip. QEMU's amd-iommu translates emulated devices' DMA only with
    dma-remap=on. The board also gets QEMU's edu device, whose DMA engine the
    kernel's iommu-selftest points inside and outside the identity domain; its
    DMA mask is widened so that it does not clip the address that must fault.

    rng adds virtio-rng-pci, the host's entropy for the kernel generator
    (docs/kernel/RANDOM.md). Without it the CPU also loses RDRAND and RDSEED,
    so the guest has no credited source at all and must say so.
    """
    if iommu not in IOMMUS:
        raise SystemExit(f"unknown IOMMU {iommu!r}; expected one of {', '.join(IOMMUS)}")
    virtio = virtio or gl
    disk_bus = disk_bus or os.environ.get("SAVANXP_DISK_BUS", "ahci")
    if disk_bus not in DISK_BUSES:
        raise SystemExit(f"unknown disk bus {disk_bus!r}; expected one of {', '.join(DISK_BUSES)}")
    args_list = [
        "qemu-system-x86_64",
        "-machine",
        "q35,pcspk-audiodev=audio0" + ("" if ps2 else ",i8042=off")
        + (",kernel-irqchip=split" if iommu != "none" and accel == "kvm" else ""),
        "-accel",
        accel,
        "-m",
        "1024M",
        "-smp",
        str(smp),
        "-cpu",
        ("host" if accel == "kvm" else "max") + ("" if rng else ",-rdrand,-rdseed"),
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
        "-serial",
        f"file:{serial_path}" if serial_path is not None else "stdio",
        "-debugcon",
        f"file:{debug_log}",
        "-global",
        "isa-debugcon.iobase=0xe9",
    ]
    if iommu == "amd":
        args_list += ["-device", "amd-iommu,intremap=on,dma-remap=on"]
    elif iommu == "intel":
        args_list += ["-device", "intel-iommu,intremap=on"]
    args_list += ["-netdev", "user,id=net0"] if network else ["-nic", "none"]
    if rng:
        args_list += ["-device", "virtio-rng-pci"]
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

    disk_path = str(disk_image).replace(",", ",,")
    drive = f"if=none,id=svdisk,media=disk,format=raw,file={disk_path}"
    if disk_fds:
        for fd in disk_fds:
            args_list += ["-add-fd", f"fd={fd},set=0"]
        drive = "if=none,id=svdisk,media=disk,format=raw,file=/dev/fdset/0,file.driver=host_device"
        if disk_snapshot:
            drive += ",snapshot=on"
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
            *(["-device", "virtio-net-pci,netdev=net0"] if network else []),
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
        elif disk_bus == "nvme":
            disk_devices = [
                "-device",
                "nvme,drive=svdisk,serial=SXNVME0",
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
        if extra_disks and disk_bus == "ide":
            raise SystemExit("extra disks go on the AHCI controller: run without SAVANXP_DISK_BUS=ide")
        if second_ahci and extra_disks:
            disk_devices += ["-device", "ahci,id=svahci"]
        for index, (path, serial) in enumerate(extra_disks):
            bus = f"svahci.{index}" if second_ahci else f"ide.{2 + index}"
            disk_devices += [
                "-drive",
                f"if=none,id=svextra{index},media=disk,format=raw,file={path}",
                "-device",
                f"ide-hd,drive=svextra{index},bus={bus},serial={serial}",
            ]
        args_list += [
            *(["-device", "rtl8139,netdev=net0"] if network else []),
            *disk_devices,
            "-device",
            "VGA,edid=on,xres=1280,yres=800",
        ]
    host_devices, usb_bus = usb_host_devices(usb_host)
    if usb or usb_stick is not None:
        args_list += host_devices
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
            args_list += ["-device", f"usb-hub,bus={usb_bus},port=1,id=usbhub"]
        args_list += [
            "-device",
            f"usb-kbd,bus={usb_bus},id=usbkbd{hub_ports[0]}",
            "-device",
            f"usb-{usb_pointer},bus={usb_bus},id=usb{usb_pointer}{hub_ports[1]}",
        ]
    if usb_stick is not None:
        args_list += [
            "-drive",
            f"if=none,id=usbstick,media=disk,format=raw,file={usb_stick}",
            "-device",
            f"usb-storage,bus={usb_bus},drive=usbstick,id=usbstorage,serial=SXUSB0",
        ]
    if wav_path is not None:
        audio1 = f"wav,id=audio1,path={wav_path}"
    else:
        audio1 = "none,id=audio1" if headless else "sdl,id=audio1"
    if capture_fixture:
        audio1 = ("alsa,id=audio1,in.dev=savanxp_fixture_input,out.dev=savanxp_fixture_output,"
                  "in.frequency=48000,out.frequency=48000,in.channels=2,out.channels=2,"
                  "in.format=s16,out.format=s16,in.try-poll=off,out.try-poll=off,"
                  "in.buffer-length=10000,out.buffer-length=10000,"
                  "in.period-length=10000,out.period-length=10000,timer-period=10000")
    if audio_device == "multi":
        if not capture_fixture:
            raise ValueError("multi audio requires the deterministic capture fixture")
        for suffix in ("2","3"):
            args_list += ["-audiodev",audio1.replace("id=audio1",f"id=audio{suffix}")
                          .replace("savanxp_fixture_input",f"savanxp_fixture_input{suffix}")
                          .replace("savanxp_fixture_output",f"savanxp_fixture_output{suffix}")]
        args_list += ["-audiodev","none,id=audio4","-audiodev","none,id=audio5"]
        devices = ("ich9-intel-hda,id=hda", "hda-duplex,bus=hda.0,cad=0,audiodev=audio1",
                   "hda-duplex,bus=hda.0,cad=2,audiodev=audio2", "ich9-intel-hda,id=hda2",
                   "hda-duplex,bus=hda2.0,cad=0,audiodev=audio3", "AC97,audiodev=audio4",
                   "virtio-sound-pci,audiodev=audio5,streams=2")
    elif audio_device == "hda":
        if wav_path is not None and not capture_fixture:
            audio1 += ",out.frequency=48000"
        devices = ("ich9-intel-hda,id=hda", "hda-duplex,bus=hda.0,audiodev=audio1")
    elif audio_device == "virtio" or (audio_device == "auto" and virtio):
        devices = ("virtio-sound-pci,audiodev=audio1,streams=2",)
    else:
        devices = ("AC97,audiodev=audio1",)
    args_list += ["-audiodev", audio1]
    for device in devices:
        args_list += ["-device", device]
    if iommu != "none":
        # Last, so that every other device keeps the slot it has without it.
        args_list += ["-device", "edu,dma_mask=0xffffffffffffffff"]
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


def claim_disk_fds(path: Path, writable: bool, resources: ExitStack) -> tuple[int, ...]:
    fd = resources.enter_context(claim(path, writable=writable))
    if not writable:
        return (fd,)
    # QEMU also requests a read-only descriptor for probes and reopening.
    read_fd = os.open(f"/proc/self/fd/{fd}", os.O_RDONLY)
    resources.callback(os.close, read_fd)
    return fd, read_fd


def check_sxfs(path: Path, fd: int) -> None:
    # Match either superblock. The guest validates the volume at mount time.
    header = os.pread(fd, 2 * SXFS_SECTOR_SIZE, 0)
    if not (header.startswith(SXFS_SUPERBLOCK_MAGIC) or
            header.startswith(SXFS_SUPERBLOCK_MAGIC, SXFS_SECTOR_SIZE)):
        raise ValueError(f"{path} does not contain a raw SxFS volume. "
                         "Select the SxFS partition on a partitioned disk.")


def peer_uid(connection: socket.socket) -> int:
    _, uid, _ = struct.unpack("iII", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
    return uid


def receive_disk_fds(connection: socket.socket, writable: bool, identity: str,
                     resources: ExitStack) -> tuple[int, ...]:
    modes = (os.O_RDWR, os.O_RDONLY) if writable else (os.O_RDONLY,)
    message, fds, flags, _ = socket.recv_fds(connection, 1, len(modes))
    for fd in fds:
        resources.callback(os.close, fd)
        os.set_inheritable(fd, False)
    if message != b"D" or flags & socket.MSG_CTRUNC or len(fds) != len(modes):
        raise ValueError("the device opener did not return the expected descriptors")
    for fd, mode in zip(fds, modes):
        if (fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE) != mode or device_identity(os.fstat(fd)) != identity:
            raise ValueError("the device opener returned a different device or access mode")
    return tuple(fds)


def sudo_disk_fds(path: Path, writable: bool, sudo: str, resources: ExitStack) -> tuple[int, ...]:
    identity = device_identity(path.stat())
    address = "savanxp-qemu-" + secrets.token_hex(16)
    command = [sudo, "--", sys.executable, "-E", "-S", "-B", str(Path(__file__).resolve()),
               "--send-disk-fds", str(path), identity, address]
    if writable:
        command.append("--disk-write")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind("\0" + address)
        listener.listen(1)
        print(f"Requesting sudo to open QEMU disk device {path}", flush=True)
        result = subprocess.run(command, check=False)
        if result.returncode:
            raise ValueError(f"sudo could not open the device (exit {result.returncode})")
        listener.settimeout(2)
        connection, _ = listener.accept()
        with connection:
            connection.settimeout(2)
            if peer_uid(connection) != 0:
                raise ValueError("the device opener is not running as root")
            return receive_disk_fds(connection, writable, identity, resources)


def send_disk_fds(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Open a QEMU disk device and pass it to the launcher.")
    parser.add_argument("path", type=Path)
    parser.add_argument("identity")
    parser.add_argument("address")
    parser.add_argument("--disk-write", action="store_true")
    args = parser.parse_args(argv)
    try:
        uid = int(os.environ.get("SUDO_UID", "0"))
        if os.geteuid() != 0 or uid <= 0:
            raise ValueError("the device opener must be started by sudo")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection, ExitStack() as resources:
            connection.connect("\0" + args.address)
            if peer_uid(connection) != uid:
                raise ValueError("the QEMU launcher is not owned by the sudo user")
            fds = claim_disk_fds(args.path, args.disk_write, resources)
            if device_identity(os.fstat(fds[0])) != args.identity:
                raise ValueError(f"{args.path} changed while waiting for sudo")
            check_sxfs(args.path, fds[0])
            socket.send_fds(connection, [b"D"], fds)
    except (OSError, ValueError) as error:
        parser.exit(1, f"QEMU device opener: {error}\n")
    return 0


def open_disk_device(path: Path, writable: bool, resources: ExitStack, sudo: str = "") -> tuple[int, ...]:
    """Hold the exclusive device open until QEMU exits."""
    try:
        try:
            with ExitStack() as direct:
                fds = claim_disk_fds(path, writable, direct)
                resources.callback(direct.pop_all().close)
        except PermissionError:
            if not sudo or os.geteuid() == 0:
                raise
            fds = sudo_disk_fds(path, writable, sudo, resources)
        check_sxfs(path, fds[0])
        return fds
    except (OSError, ValueError) as error:
        raise SystemExit(f"cannot open QEMU disk device {path}: {error}") from error


def main() -> int:
    if sys.argv[1:2] == ["--send-disk-fds"]:
        return send_disk_fds(sys.argv[2:])
    parser = argparse.ArgumentParser()
    parser.add_argument("--image-root", required=True, type=Path)
    disk = parser.add_mutually_exclusive_group(required=True)
    disk.add_argument("--disk-image", type=Path, help="a regular SxFS image file")
    disk.add_argument("--disk-device", type=Path, help="an existing raw SxFS block device")
    parser.add_argument("--disk-write", action="store_true",
                        help="write to --disk-device instead of using a temporary overlay")
    parser.add_argument("--ovmf-code", type=Path)
    parser.add_argument("--ovmf-vars", type=Path)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    parser.add_argument("--smp", type=int, default=2)
    parser.add_argument("--virtio", action="store_true")
    parser.add_argument("--gl", action="store_true",
                        help="virtio machine with virtio-vga-gl (virgl); implies --virtio")
    parser.add_argument("--disk-bus", choices=DISK_BUSES,
                        help="disk controller without --virtio (default: SAVANXP_DISK_BUS or ahci)")
    parser.add_argument("--no-network", action="store_true", help="omit every NIC and network backend")
    parser.add_argument("--no-rng", action="store_true",
                        help="omit virtio-rng and the CPU's RDRAND and RDSEED")
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
    parser.add_argument("--usb-host", choices=USB_HOSTS, default="xhci",
                        help="the USB host controller --usb uses (default: xhci)")
    parser.add_argument("--iommu", choices=IOMMUS, default="none",
                        help="add an emulated AMD or Intel IOMMU (default: none)")
    args = parser.parse_args()
    if not 1 <= args.smp <= 32:
        parser.error("--smp must be between 1 and 32")
    if args.disk_write and args.disk_device is None:
        parser.error("--disk-write requires --disk-device")

    qemu = find_qemu()
    if not qemu:
        raise SystemExit("qemu-system-x86_64 was not found in PATH")
    image_root = args.image_root.resolve()
    disk_image = (args.disk_device or args.disk_image).resolve()
    with ExitStack() as resources:
        if args.disk_device is not None:
            sudo = configured_sudo(image_root.parent / "tools")
            disk_fds = open_disk_device(disk_image, args.disk_write, resources, sudo)
            mode = "persistent writes" if args.disk_write else "temporary overlay"
            print(f"QEMU disk device: {disk_image} ({mode})", flush=True)
        else:
            if not disk_image.is_file():
                raise SystemExit(f"QEMU disk image {disk_image} is not a regular file; "
                                 "use --disk-device for a block device")
            disk_fds = ()
            lock_path = disk_image.with_name(disk_image.name + ".lock")
            lock = resources.enter_context(lock_path.open("a+"))
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        return launch_qemu(qemu, args, image_root, disk_image, disk_fds)


def launch_qemu(qemu: str, args: argparse.Namespace, image_root: Path,
                disk_image: Path, disk_fds: tuple[int, ...]) -> int:
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
        network=not args.no_network,
        rng=not args.no_rng,
        audio_device=args.audio_device,
        extra_disks=((args.extra_disk.resolve(), "SXBLANK0"),) if args.extra_disk else (),
        usb=args.usb,
        usb_stick=args.usb_stick.resolve() if args.usb_stick else None,
        usb_pointer=args.usb_pointer,
        usb_host=args.usb_host,
        iommu=args.iommu,
        disk_fds=disk_fds,
        disk_snapshot=not args.disk_write,
    )
    args_list[0] = qemu

    print("qemu:", " ".join(args_list), flush=True)
    return subprocess.run(args_list, pass_fds=disk_fds, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
