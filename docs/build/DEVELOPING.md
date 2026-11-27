# Developing SavanXP

How to build, run and extend SavanXP from a checkout: the requirements, the
`build.sh` commands, the QEMU machines, booting real hardware, external apps,
the persistent volume and the layout of the tree. The build graph itself is
in [BUILD_CMAKE.md](BUILD_CMAKE.md), host packages and QEMU details in
[BUILD_LINUX.md](BUILD_LINUX.md), and the working rules for the repository in
[`AGENTS.md`](../../AGENTS.md).

## Quick start

On Linux, from a clean checkout:

```bash
./build.sh build       # configure, compile, stage SxFS and EFI artifacts
./build.sh run         # boot the staged image in QEMU
./build.sh smoke smoke # run the core QEMU smoke scenario
```

The native path uses tools from `PATH` (or CMake cache overrides). The
versioned ports are built independently, for example with
`./ports/doomgeneric/build.sh`, `./ports/ffmpeg/build.sh`,
`./ports/ccleste/build.sh` and `./ports/xash3d/build.sh`
([ports/README.md](../../ports/README.md)).

## Requirements

On Linux, install Clang/LLVM, CMake, Ninja, GNU Make, Python 3 with Pillow,
Perl, gperf, Git, QEMU, OVMF and xorriso, plus Meson and `pkg-config` for the
FFmpeg port. Perl and gperf generate parts of Web Viewer's engine libraries,
and Perl also builds the OpenSSL port, which Web Viewer needs
(`./ports/openssl/build.sh` before `./build.sh build`). The `https-smoke`
scenario and Web Viewer's visual check make their test certificates with the
host's `openssl` command, 3.4 or newer (`SAVANXP_HOST_OPENSSL` overrides it).
`build.sh` resolves these tools from `PATH` and fetches the pinned Limine
checkout when needed. Distribution package names and QEMU backend details are
in [BUILD_LINUX.md](BUILD_LINUX.md).

Python (with `Pillow`) is used on every build to convert checked-in PNG assets
into C headers and SXE resources. It never redraws the icons: the checked-in
PNGs are the source of the art.

CMake tool locations can be overridden in the cache or with the corresponding
`SAVANXP_*` environment variables.

## Building

```bash
./build.sh build
```

That command:

- compiles the kernel and the internal userland
- generates the `initramfs`
- prepares the EFI boot image
- creates `build/disk.img` if it does not exist yet
- syncs the internal contents onto the persistent volume

Important: a normal build must not recreate `build/disk.img` unconditionally.
The native build applies changes to a validated sibling candidate and installs
it atomically. The persistent image is kept across builds except on real
corruption or a format incompatibility.

To build without the test and diagnostic apps (keytest, gfxdemo, smoke, ...),
use `--no-test-apps`: those binaries stay out of the rootfs and the desktop menu
is built without their entries. The automation commands (`smoke`,
`windowd-smoke`, ...) always include them, because their harnesses depend on
them.

```bash
./build.sh build --no-test-apps
```

Generating a bootable ISO:

```bash
./build.sh iso
```

The ISO lands in `build/SavanXP.iso`. That command requires `xorriso` from
`PATH` (or `SAVANXP_XORRISO`) and uses the EFI tree the build already prepared.
It is a hybrid image: it boots on UEFI and on legacy BIOS (VirtualBox with EFI
off, SeaBIOS in QEMU), and `tools/iso_boot_test.py --iso build/SavanXP.iso`
boots it both ways in QEMU.
To keep the disk's data when booting under VirtualBox or another hypervisor,
attach `build/disk.img` as an extra disk as well.

## Running on real hardware

The ISO boots a real PC from a USB stick. First reached on 2026-11-06 on an
AMD desktop (Zen, 400-series chipset, UEFI): the system came up from the
stick and its USB keyboard and mouse drove the desktop. The machine needs an
`x86_64` CPU with NX, and Secure Boot turned off (Limine is not signed).

On Linux, write the ISO to the whole stick, not to a partition (this erases
it; check the device name with `lsblk` first):

```bash
sudo dd if=build/SavanXP.iso of=/dev/sdX bs=4M conv=fsync oflag=direct status=progress
```

On Windows, Rufus does the same in its DD image mode. Then boot the stick
from the firmware's boot menu. The Limine menu waits 3 seconds and offers two
more entries: **boot log on screen** shows the kernel log instead of the
splash, the only way to see where a boot stops on a machine without a serial
port, and **no USB** leaves the USB controllers to the firmware.

The live system runs from memory and keeps nothing across reboots. It does
see the machine's SATA, NVMe and USB disks (NVMe ones read-only unless the
boot entry carries `nvme-rw`, [NVME.md](../kernel/NVME.md)). At boot it only
reads them, with one exception: a disk holding an SxFS volume is mounted as
`/` instead of the live system, and mounting replays its journal. After that,
only Disk Management writes to a disk, and only when asked to. What a
machine needs, and what is known about each hardware target, is in
[REAL_HARDWARE.md](../hardware/REAL_HARDWARE.md).

## Running in QEMU

```bash
./build.sh run
./build.sh run --accel kvm
```

The native launcher uses TCG by default and KVM when requested.

To keep a personal SxFS copy on a USB drive, copy `build/disk.img` to a file
on the drive and run `./build.sh run --disk-image /path/to/personal.img`.
An existing raw SxFS volume can also be attached with `--disk-device`.
See [Personal volumes in QEMU](BUILD_CMAKE.md#personal-volumes-in-qemu).

`./build.sh prepare-disk` can make a fresh UEFI disk image or prepare a
dedicated USB drive. See [Preparing an installed disk](BUILD_CMAKE.md#preparing-an-installed-disk)
for the device and port installation commands.

The QEMU machine is assembled with "base" hardware by default: standard VGA,
PS/2 mouse and keyboard, AC'97 audio, an AHCI (SATA) disk and an rtl8139 NIC —
the same set VirtualBox emulates. This way the kernel exercises the fallback
backends (`fb_gpu`, `ps2`, `ac97`, `ahci`, `rtl8139`) without leaving QEMU.
`SAVANXP_DISK_BUS=ide` puts the disk on legacy IDE instead, for the `ata`
driver, and `SAVANXP_DISK_BUS=nvme` on an NVMe controller, for the `nvme`
driver. Bringing the machine up with paravirtualized devices (virtio-vga,
virtio-tablet, virtio-keyboard, virtio-sound, virtio-blk, virtio-net) has to
be asked for explicitly:

```bash
./build.sh run --virtio
```

The switch applies to every command that launches QEMU (`run`, `debug`, the
smokes and `gpu-soak`). The audio harnesses that measure one specific driver
(`ac97-count`, `virtio-count`, ...) force their device and ignore it.

`--gl` (or `SAVANXP_GL=ON`) is the virtio machine with `virtio-vga-gl`, which
offers virgl to the guest. It needs a QEMU built with virglrenderer and a GL
display (`egl-headless` for the smokes). The kernel logs `virtio-gpu: virgl
capset=...` when it gets one; 3D work is tracked in
[VIRGL_ROADMAP.md](../roadmaps/open/VIRGL_ROADMAP.md).

`--usb` (or `SAVANXP_USB=ON`) adds an xHCI controller with a USB keyboard and
tablet, on either machine. The `usb-*-smoke` scenarios drive the USB stack
against it, with a machine that has no PS/2 controller at all
([USB_ROADMAP.md](../roadmaps/done/USB_ROADMAP.md)).

`--iommu amd|intel` (or `SAVANXP_IOMMU`) adds QEMU's emulated IOMMU, and
`--boot-options "..."` (or `SAVANXP_BOOT_OPTIONS`) appends kernel boot options
to the development image's command line. `--iommu amd --boot-options iommu`
boots with AMD-Vi translating ([IOMMU.md](../kernel/IOMMU.md)); with a smoke
scenario, both apply on top of what the scenario asks for.

Other available targets:

```bash
./build.sh debug
./build.sh smoke
./build.sh windowd-smoke
./build.sh gpu-soak
./build.sh clean
```

- `run` starts QEMU with a graphical session and serial output on the terminal.
- `debug` keeps the boot flow oriented towards debugging.
- `smoke` runs an automated headless test and leaves logs in `build/`;
  `./build.sh smoke --list` names every scenario.
- `windowd-smoke` exercises the graphical compositor.
- `gpu-soak` stresses the GPU presentation path.
- `clean` removes generated build outputs while preserving the persistent disk
  image and external artifacts.
- `tools/shoot.sh --scenario desktop` performs the native visual/QMP check on a
  disposable image copy.

## First boot

The system enters `init` and then `sh` as the main shell. To see the base state
of the system from inside the guest:

```text
sysinfo
df
ls /
```

Useful commands in the current userland: `sh`, `sysinfo`, `df`, `windowd`,
`keytest`, `mousetest`, `gputest`, `ping`, `netinfo`, `beep` and `audiotest`.

The basic file utilities are small in-tree programs too
(`subsystems/posix/userland/coreutils`): `ls`, `cat`, `echo`, `mkdir [-p]`,
`rm [-rf]`, `mv`, `cp [-r]`, `ps`, `true`, `false` and `sleep`.

## External apps

The recommended flow for testing your own programs does not require rebuilding
the `initramfs`. External apps are compiled against the SDK and installed
straight into `build/disk.img`, normally under `/bin`.

```bash
./build.sh build
./tools/build-user.sh --source sdk/hello/main.c --name hello
./build.sh run
```

Use `--destination /bin/NAME` (or another path below `/`) when the
program should not use the default `/bin/NAME` location. A SUN program,
built with `--native`, installs as `/bin/NAME.sxe`; the shell runs it as
`NAME` too ([SXE_FORMAT.md](../userland/SXE_FORMAT.md#sun-programs-are-named-sxe)).

Inside SavanXP:

```text
which hello
hello
```

There is also a wrapper that compiles, installs and boots the system in one
step:

```bash
./tools/run-user.sh --source sdk/hello/main.c --name hello
```

Included examples: `sdk/hello`, `sdk/fsdemo`, `sdk/gfxhello` and
`ports/doomgeneric`.

## Persistence

SavanXP uses a persistent disk image at `build/disk.img`, mounted as `/`
inside the system through `SxFS`. It keeps files across reboots, external
binaries installed in `/bin`, and assets and persistent data under `/`.

Inside the guest:

```text
echo hello > /notes.txt
sync
cat /notes.txt
```

The repo workflow protects this persistence: `./build.sh build` must not
delete external applications that are already installed, nor persistent assets
such as the ones `doomgeneric` uses.

## Repository layout

- `arch/`: architecture-specific code
- `kernel/`: kernel and base subsystems
- `subsystems/posix/`: POSIX layer, SDK and main userland
- `subsystems/win32/`, `subsystems/linux/`: the Win32 and Linux binary
  compatibility subsystems
- `system/`: native (SUN) programs: the platform services (`compositord`,
  `windowd`, `sessiond`, `elevationd`), the Command Prompt, Disk Management,
  Task Manager and Nightwood
- `libsxfs/`: portable core of the `SxFS` filesystem and `sxfs-cli`, the host
  tool that does all disk image writing
- `rootfs/`: contents of the `initramfs`, the rescue system published under
  `/rescue`
- `diskfs/`: initial contents of the persistent volume
- `sdk/`: SDK examples
- `ports/`: versioned official third-party ports with independent build flows
- `tools/`: host-side scripts and development utilities
- `vendor/`: third-party dependencies

## Documentation

- [`docs/README.md`](../README.md) — index of every design document
- [BUILD_CMAKE.md](BUILD_CMAKE.md) — native build graph and commands
- [BUILD_LINUX.md](BUILD_LINUX.md) — native host requirements and QEMU setup
- [SYSTEM_LAYERING.md](../userland/SYSTEM_LAYERING.md) — which layer is
  written in which language, and why
- [`AGENTS.md`](../../AGENTS.md) — working rules for this repository
  (changelog format, persistence guarantees, minimum verification)
