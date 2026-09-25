<p align="center">
  <img src="assets/brand/logo.png" alt="SavanXP" width="160">
</p>

# SavanXP

SavanXP is an experimental operating system for `x86_64 + UEFI`, with the
`Limine` bootloader, its own kernel in `C/C++`, and a native Bash/CMake
workflow.

It boots to a working graphical session with its own window manager, a userland
shell, a persistent volume mounted at `/disk`, a minimal POSIX base, built-in
apps, and support for external apps compiled against the SDK in this repo.

![The SavanXP desktop: Program Manager, the About window and Doom running in
a window, over the window manager's own compositor](docs/images/desktop.png)

Current version: `v0.3.4` &middot; [Changelog](CHANGELOG.md) &middot;
[MIT license](LICENSE)

## Quick start

On Linux, from a clean checkout:

```bash
./build.sh build       # configure, compile, stage SxFS and EFI artifacts
./build.sh run         # boot the staged image in QEMU
./build.sh smoke smoke # run the core QEMU smoke scenario
```

The native path uses tools from `PATH` (or CMake cache overrides); see
[Building with Bash and CMake](docs/BUILD_CMAKE.md). The versioned ports are
built independently with `./ports/doomgeneric/build.sh` and
`./ports/ffmpeg/build.sh`. The optional Haxe AOT experiment has
`./subsystems/native/build.sh`.

## What works today

- `x86_64` kernel booting over UEFI via Limine.
- Framebuffer console and early serial output.
- Userland with `ELF64` processes, syscalls and a preemptive scheduler.
- Shell with `pipes`, redirections and basic builtins.
- NT 3.5-style graphical session: its own window manager (`windowd`), the
  wallpaper and Program Manager as clients, and a Task List (Ctrl+Esc).
- A taskbar listing the open windows, as a WM client of its own, with an ES/EN
  keyboard layout selector.
- Built-in apps: Files, Notepad, Shell, Add/Remove Programs, System Properties,
  Media Player and a Task Manager with live CPU and memory per process
  ([how it measures](docs/SYSTEM_MONITORING.md)).
- Persistent `SxFS` volume mounted at `/disk`.
- POSIX base and SDK v1 for compiling external applications.
- `sxgfx` 2D graphics layer for surfaces, painter and rect sets.
- Initial support for network, audio, input, GPU and storage.

## Requirements

On Linux, install Clang/LLVM, CMake, Ninja, GNU Make, Python 3 with Pillow,
Git, QEMU, OVMF and xorriso. `build.sh` resolves these tools from `PATH` and
fetches the pinned Limine checkout when needed. Distribution package names and
QEMU backend details are in [Building on Linux](docs/BUILD_LINUX.md).

Python (with `Pillow`) is used on every build to generate desktop art and
convert PNG assets into C headers (`tools/gen_desktop_source_art.py`,
`tools/gen_cursor_asset.py`, `tools/gen_desktop_icon_assets.py`).

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
To keep `/disk` data when booting under VirtualBox or another hypervisor,
attach `build/disk.img` as an extra disk as well.

## Running

```bash
./build.sh run
./build.sh run --accel kvm
```

The native launcher uses TCG by default and KVM when requested.

The QEMU machine is assembled with "base" hardware by default: standard VGA,
PS/2 mouse and keyboard, AC'97 audio, IDE disk and an rtl8139 NIC — the same
set VirtualBox emulates. This way the kernel exercises the fallback backends
(`fb_gpu`, `ps2`, `ac97`, `ata`, `rtl8139`) without leaving QEMU. Bringing the
machine up with paravirtualized devices (virtio-vga, virtio-tablet,
virtio-keyboard, virtio-sound, virtio-blk, virtio-net) has to be asked for
explicitly:

```bash
./build.sh run --virtio
```

The switch applies to every command that launches QEMU (`run`, `debug`, the
smokes and `gpu-soak`). The audio harnesses that measure one specific driver
(`ac97-count`, `virtio-count`, ...) force their device and ignore it.

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
- `smoke` runs an automated headless test and leaves logs in `build/`.
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
ls /disk
```

Useful commands in the current userland:

- `sh`
- `sysinfo`
- `df`
- `windowd`
- `keytest`
- `mousetest`
- `gputest`
- `ping`
- `netinfo`
- `beep`
- `audiotest`

Several basic utilities also come out of the `busybox` multicall binary, for
example `ls`, `cat`, `echo`, `mkdir`, `rm`, `mv`, `cp`, `ps`, `true`, `false`
and `sleep`.

## External apps

The recommended flow for testing your own programs does not require rebuilding
the `initramfs`. External apps are compiled against the SDK and installed
straight into `build/disk.img`, normally under `/disk/bin`.

```bash
./build.sh build
./tools/build-user.sh --source sdk/hello/main.c --name hello
./build.sh run
```

Use `--destination /disk/bin/NAME` (or another path below `/disk`) when the
program should not use the default `/disk/bin/NAME` location.

Inside SavanXP:

```text
which hello
hello
```

There is also a wrapper that compiles, installs and boots the system in one
step:

```bash
./tools/run-user.sh --source sdk/errdemo/main.c --name errdemo
```

Included examples:

- `sdk/hello`
- `sdk/errdemo`
- `sdk/fsdemo`
- `sdk/gfxhello`
- `ports/doomgeneric`

## Persistence

SavanXP uses a persistent disk image at `build/disk.img`, mounted as `/disk`
inside the system through `SxFS`.

This makes it possible to:

- keep files across reboots
- install external binaries in `/disk/bin`
- store assets and persistent data under `/disk`

Inside the guest:

```text
echo hello > /disk/notes.txt
sync
cat /disk/notes.txt
```

The repo workflow protects this persistence: `./build.sh build` must not
delete external applications that are already installed, nor persistent assets
such as the ones `doomgeneric` uses.

## Repository layout

- `arch/`: architecture-specific code
- `kernel/`: kernel and base subsystems
- `subsystems/posix/`: POSIX layer, SDK and main userland
- `libsxfs/`: portable core of the `SxFS` filesystem and `sxfs-cli`, the host
  tool that does all disk image writing
- `rootfs/`: contents of the `initramfs`
- `diskfs/`: initial contents of the persistent volume
- `sdk/`: SDK examples
- `ports/`: versioned official third-party ports with independent build flows
- `tools/`: host-side scripts and development utilities
- `vendor/`: third-party dependencies

## Documentation

- [`docs/README.md`](docs/README.md) — index of every design document
- [`docs/BUILD_CMAKE.md`](docs/BUILD_CMAKE.md) — native build graph and commands
- [`docs/BUILD_LINUX.md`](docs/BUILD_LINUX.md) — native host requirements and QEMU setup
- [`docs/SYSTEM_LAYERING.md`](docs/SYSTEM_LAYERING.md) — which layer is written
  in which language, and why
- [`AGENTS.md`](AGENTS.md) — working rules for this repository (changelog
  format, persistence guarantees, minimum verification)

## Project status

SavanXP is well past the minimal-boot stage. Today it offers a coherent base to
keep building on:

- its own kernel and userland
- a usable initial desktop
- a graphics path under a compositor
- real persistence over `/disk`
- support for ports and external applications

It is still an experimental system, with APIs and subsystems in flux, but it is
already aiming to be a consistent, demonstrable working base.
