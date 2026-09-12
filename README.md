<p align="center">
  <img src="assets/brand/logo.png" alt="SavanXP" width="160">
</p>

# SavanXP

SavanXP is an experimental operating system for `x86_64 + UEFI`, with the
`Limine` bootloader, its own kernel in `C/C++`, and a workflow built to be
developed and tested from native Windows with `PowerShell`.

It boots to a working graphical session with its own window manager, a userland
shell, a persistent volume mounted at `/disk`, a minimal POSIX base, built-in
apps, and support for external apps compiled against the SDK in this repo.

![The SavanXP desktop: Program Manager, the About window and Doom running in
a window, over the window manager's own compositor](docs/images/desktop.png)

Current version: `v0.3.4` &middot; [Changelog](CHANGELOG.md) &middot;
[MIT license](LICENSE)

## Quick start

On Windows, from a clean checkout:

```powershell
.\tools\bootstrap.ps1   # bake a local toolchain (clang, qemu, xorriso, ninja)
.\build.ps1 build       # build kernel, userland and the disk image
.\build.ps1 run         # boot it in QEMU
```

That is the whole loop. Everything below is detail you only need when you want
it.

On Linux, skip `bootstrap.ps1` and see [Building on Linux](docs/BUILD_LINUX.md).

## What works today

- `x86_64` kernel booting over UEFI via Limine.
- Framebuffer console and early serial output.
- Userland with `ELF64` processes, syscalls and a preemptive scheduler.
- Shell with `pipes`, redirections and basic builtins.
- NT 3.5-style graphical session: its own window manager (`windowd`), the
  wallpaper and Program Manager as clients, and a Task List (Ctrl+Esc).
- A taskbar listing the open windows, as a WM client of its own, with an ES/EN
  keyboard layout selector.
- Built-in apps: Files, Notepad, Shell, Add/Remove Programs, System Properties
  and a Task Manager with live CPU and memory per process
  ([how it measures](docs/SYSTEM_MONITORING.md)).
- Persistent `SxFS` volume mounted at `/disk`.
- POSIX base and SDK v1 for compiling external applications.
- `sxgfx` 2D graphics layer for surfaces, painter and rect sets.
- Initial support for network, audio, input, GPU and storage.

## Requirements

The recommended path is to bake a self-contained local toolchain:

```powershell
.\tools\bootstrap.ps1
```

That downloads pinned versions (LLVM/Clang with `ld.lld`, `llvm-objcopy` and
`llvm-readelf`; QEMU with the OVMF firmware it ships; `xorriso` for generating
ISOs; and `ninja`) into `toolchain/` (git-ignored) and writes the
`toolchain/toolchain.json` manifest that `build.ps1` consumes. The versions are
pinned in `tools/toolchain.lock.json`; updating a tool means editing that file.
`xorriso` can be skipped with `-SkipXorriso`, and `ninja` with `-SkipNinja`, if
you already have them.

`build.ps1` contains no paths from any particular machine: it resolves each
tool in this order and keeps the first one that exists.

1. explicit environment variable override
   (`SAVANXP_CLANG`, `SAVANXP_CLANGXX`, `SAVANXP_LD`, `SAVANXP_OBJCOPY`,
   `SAVANXP_READELF`, `SAVANXP_QEMU`, `SAVANXP_XORRISO`, `SAVANXP_NINJA`,
   `OVMF_CODE` / `OVMF_VARS`)
2. the toolchain baked into `toolchain/`
3. the system `PATH`

That is why `bootstrap.ps1` is optional: if you already have `clang++`,
`ld.lld`, `llvm-objcopy`, `llvm-readelf`, `ninja` and `qemu-system-x86_64` on
the `PATH`, the build works all the same. `git` is also required on the `PATH`.
`build.ps1` automatically downloads Limine's `v10.x-binary` branch if it is not
present in `tools/limine`.

`python3` (or `python`) with `Pillow` installed (`pip install Pillow`) is
required too: `build.ps1` uses it on every build to generate the desktop art
and convert the cursor/icon PNGs into C headers
(`tools/gen_desktop_source_art.py`, `tools/gen_cursor_asset.py`,
`tools/gen_desktop_icon_assets.py`). It is not part of the toolchain baked by
`bootstrap.ps1`.

For anything outside Windows — PowerShell itself, distribution packages, QEMU
backends, virtio devices — see [Building on Linux](docs/BUILD_LINUX.md).

## Building

```powershell
.\build.ps1 build
```

That command:

- compiles the kernel and the internal userland
- generates the `initramfs`
- prepares the EFI boot image
- creates `build/disk.img` if it does not exist yet
- syncs the internal contents onto the persistent volume

Important: a normal build must not recreate `build/disk.img` unconditionally.
The persistent image is kept across builds except on real corruption or a
format incompatibility.

To build without the test and diagnostic apps (keytest, gfxdemo, smoke, ...),
use `-NoTestApps`: those binaries stay out of the rootfs and the desktop menu
is built without their entries. The automation commands (`smoke`,
`windowd-smoke`, ...) always include them, because their harnesses depend on
them.

```powershell
.\build.ps1 build -NoTestApps
```

Generating a bootable ISO:

```powershell
.\build.ps1 iso
```

The ISO lands in `build/SavanXP.iso`. That command requires `xorriso` —
resolved through `SAVANXP_XORRISO`, `toolchain/toolchain.json` or the `PATH` —
and uses the EFI tree the build already prepared. To keep `/disk` data when
booting under VirtualBox or another hypervisor, attach `build/disk.img` as an
extra disk as well.

## Running

```powershell
.\build.ps1 run
```

By default this uses TCG (software emulation). With Hyper-V enabled on Windows,
`-Accel whpx` accelerates the boot through the Windows Hypervisor Platform; on
Linux with VT-x/AMD-V, `-Accel kvm` does the same against `/dev/kvm`:

```powershell
.\build.ps1 run -Accel whpx
.\build.ps1 run -Accel kvm
```

Note: under whpx, `-cpu max` / `-cpu host` crash OVMF with a #GP in PlatformPei
right at boot (WHPX cannot back the very recent CPU features those models
expose to the guest). That is why `-Accel whpx` forces `-cpu qemu64`, which
boots fine. `-Accel kvm` does not have that problem and uses `-cpu host`.

The QEMU machine is assembled with "base" hardware by default: standard VGA,
PS/2 mouse and keyboard, AC'97 audio, IDE disk and an rtl8139 NIC — the same
set VirtualBox emulates. This way the kernel exercises the fallback backends
(`fb_gpu`, `ps2`, `ac97`, `ata`, `rtl8139`) without leaving QEMU. Bringing the
machine up with paravirtualized devices (virtio-vga, virtio-tablet,
virtio-keyboard, virtio-sound, virtio-blk, virtio-net) has to be asked for
explicitly:

```powershell
.\build.ps1 run -Virtio
```

The switch applies to every command that launches QEMU (`run`, `debug`, the
smokes and `gpu-soak`). The audio harnesses that measure one specific driver
(`ac97-count`, `virtio-count`, ...) force their device and ignore it.

Other available targets:

```powershell
.\build.ps1 debug
.\build.ps1 smoke
.\build.ps1 windowd-smoke
.\build.ps1 gpu-soak
.\build.ps1 clean
```

- `run` starts QEMU with a graphical session and serial output on the terminal.
- `debug` keeps the boot flow oriented towards debugging.
- `smoke` runs an automated headless test and leaves logs in `build/`.
- `windowd-smoke` exercises the graphical compositor.
- `gpu-soak` stresses the GPU presentation path.
- `clean` removes build artifacts and can force the environment to be recreated
  on the next build.

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

```powershell
.\build.ps1 build
.\tools\build-user.ps1 -Source .\sdk\hello\main.c -Name hello
.\build.ps1 run
```

Inside SavanXP:

```text
which hello
hello
```

There is also a wrapper that compiles, installs and boots the system in one
step:

```powershell
.\tools\run-user.ps1 -Source .\sdk\errdemo\main.c -Name errdemo
```

Included examples:

- `sdk/hello`
- `sdk/errdemo`
- `sdk/fsdemo`
- `sdk/gfxhello`
- `sdk/doomgeneric`

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

The repo workflow protects this persistence: a `.\build.ps1 build` must not
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
- `sdk/`: examples, tooling and external ports
- `tools/`: host-side scripts and development utilities
- `vendor/`: third-party dependencies

## Documentation

- [`docs/README.md`](docs/README.md) — index of every design document
- [`docs/BUILD_LINUX.md`](docs/BUILD_LINUX.md) — building outside Windows
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
