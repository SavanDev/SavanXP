# Building on Linux

`build.ps1` does not depend on Windows, but it is a PowerShell script and it
assumes a few tools that Linux distributions split across more packages than
you would expect. This page covers what `README.md` deliberately leaves out.

`tools/bootstrap.ps1` (the baked toolchain) is Windows-oriented, so on Linux
the usual path is to resolve every tool through `PATH` — step 3 of the
resolution order in the README.

## PowerShell itself

`build.ps1` needs `pwsh` on the `PATH`. Distributions do not always package it
(Arch does not ship it in the official repositories); the
`powershell-<version>-linux-x64.tar.gz` tarball from
[PowerShell/PowerShell](https://github.com/PowerShell/PowerShell/releases)
extracts directly, with no installer.

## Base packages

On Arch Linux:

```bash
pacman -S clang lld llvm qemu-system-x86 edk2-ovmf libisoburn python-pillow ninja
```

- `clang`/`lld`/`llvm`: the compiler, the linker (`ld.lld`) and the tools
  (`llvm-objcopy`, `llvm-readelf`) that `Add-SxeResources` uses to stamp
  `.sxmeta`/`.sxicon` resources.
- `libisoburn` is the package that ships the `xorriso` binary (the package name
  does not match the tool name).
- `ninja` is always required, whether or not the toolchain is baked: it is not
  part of any base package.
- Without `bootstrap.ps1` there is no `toolchain/toolchain.json` holding the
  OVMF paths, so `OVMF_CODE`/`OVMF_VARS` have to be set by hand — for example
  `/usr/share/edk2/x64/OVMF_CODE.4m.fd` and `.../OVMF_VARS.4m.fd`, the
  `edk2-ovmf` paths on Arch.

`python3` (or `python`) with Pillow is also required, on every platform:
`build.ps1` uses it on each build to generate the desktop art and convert the
cursor/icon PNGs into C headers.

## ISO builds need a C compiler

Outside Windows, `.\build.ps1 iso` also needs `make` and a `cc` on the `PATH`.
Limine's `v10.x-binary` branch only ships a prebuilt `limine.exe` for Windows,
so elsewhere the `limine` deployer (used for the BIOS boot path of the ISO) is
compiled once from `limine.c` with the Makefile in Limine's own repository.

## QEMU with a graphical window

For `.\build.ps1 run` / `debug`, Arch splits the QEMU backends into packages
separate from `qemu-system-x86`:

```bash
pacman -S qemu-ui-gtk qemu-ui-opengl qemu-audio-sdl libpulse
```

- Without `qemu-ui-gtk` the `-display gtk` backend that `Run-Qemu` uses does
  not exist (with plain `qemu-system-x86`, `-display help` lists only `none`).
- Without `qemu-audio-sdl`, asking for `-audiodev sdl,...` — which is what
  `run`/`debug` do — makes QEMU **segfault** instead of failing with a readable
  error.
- `libpulse` is what gives SDL2 a real audio backend to talk to the host audio
  server (on WSL2, the Pulse socket exposed by WSLg).

## QEMU with virtio devices

For `-Virtio`, Arch further splits every virtio display device into its own
package, outside `qemu-system-x86`:

```bash
pacman -S qemu-hw-display-virtio-vga qemu-hw-display-virtio-vga-gl \
  qemu-hw-display-virtio-gpu qemu-hw-display-virtio-gpu-pci \
  qemu-hw-display-virtio-gpu-pci-gl
```

Without `qemu-hw-display-virtio-vga`, the `-device virtio-vga` that `-Virtio`
builds does not exist and QEMU fails with "is not a valid device model name".

The nastiest case is missing only `qemu-hw-display-virtio-gpu` (without
`-pci`): the other packages stay installed, `-device virtio-vga,help` lists
properties as if nothing were wrong, but on actually starting the machine QEMU
**segfaults** in `virtio_instance_init_common` — the base module holding the
`virtio-gpu-device` type that the PCI frontends depend on is not loaded.

## Hardware acceleration

On Linux with VT-x/AMD-V, `-Accel kvm` runs the guest against `/dev/kvm` with
`-cpu host`. Unlike whpx it needs no CPU model workaround, because KVM
virtualizes the real CPU features instead of exposing a synthetic model.

```bash
pwsh ./build.ps1 run -Accel kvm
```
