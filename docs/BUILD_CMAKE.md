# Native CMake build

SavanXP uses one host build graph: Bash invokes CMake, and CMake owns the
kernel, internal userland, initramfs, persistent SxFS image, EFI staging, ISO,
and host-test targets. Tool locations are discovered from `PATH` and can be
overridden in the CMake cache or through the documented `SAVANXP_*`
environment variables.

## Requirements

The host needs Clang/LLVM, CMake, Ninja, Python 3 with Pillow, Git, QEMU, OVMF
firmware, `xorriso`, and GNU Make for the optional FFmpeg port. On Arch the core
package set is:

```bash
pacman -S clang lld llvm cmake ninja make python-pillow \
  qemu-system-x86 edk2-ovmf libisoburn
```

`build.sh` fetches the pinned Limine checkout when `tools/limine` is missing.
The first configure must therefore go through `build.sh` unless Limine has
already been populated.

## Commands

From the repository root:

```bash
./build.sh configure
./build.sh build
./build.sh build --no-test-apps
./build.sh kernel
./build.sh userland
./build.sh iso
./build.sh run
./build.sh debug
./build.sh smoke smoke
./build.sh smoke kbd-smoke
./build.sh smoke taskbar-smoke
./build.sh smoke tcp-smoke
./build.sh smoke ffmpeg-smoke
./build.sh gpu-soak --gpu-soak-iterations 96
./build.sh test
./tools/shoot.sh --scenario desktop
```

A catalog scenario may also be used as a direct command, for example
`./build.sh windowd-smoke`. The smoke runner stages one line in the initramfs,
boots a disposable copy of the persistent image, waits for the scenario's serial
token or host-side QMP assertion, and retains logs under `build/smoke-logs/`.
The real image is locked and is never reset by a smoke run.

The equivalent manual CMake entry points are:

```bash
cmake --preset linux
cmake --build --preset linux
```

CMake uses `build/linux/` as its binary directory while generated products and
the persistent image remain under the shared `build/` root. The default target
is `savanxp`; `savanxp_iso` adds the hybrid BIOS/UEFI ISO. `build.sh clean`
removes generated CMake outputs but preserves `build/disk.img` and
`build/external/`.

## External applications

The external POSIX builder is deliberately separate from the in-tree CMake
userland registry:

```bash
./build.sh build
./tools/build-user.sh --source sdk/hello/main.c --name hello
./tools/build-user.sh --source path/to/app --name app \
  --destination /disk/bin/app
```

`--destination` is rooted at `/disk`; the default is `/disk/bin/NAME`. The
builder accepts a single C/assembly file or a directory, stamps SXE resources,
and installs through `tools/sxfs_sync.py` unless `--no-install` is selected.

`tools/new-user-app.sh` creates a new application from `sdk/template`, and
`tools/run-user.sh` builds, installs, and boots an external application in one
step.

## Layout

- `CMakeLists.txt` defines the target graph and host-tool requirements.
- `cmake/UserPrograms.cmake` is the explicit in-tree userland registry.
- `tools/mkinitramfs.py` writes the newc cpio archive.
- `tools/run_smoke.py` runs one QEMU smoke scenario and records serial output.
- `tools/qmp_client.py` provides the keyboard and taskbar QMP drivers.
- `tools/taskbar_smoke.py` and `tools/tcp_echo_server.py` provide host-side
  scenario drivers.
- `tools/smoke_catalog.py` keeps commands, tokens, and callbacks declarative.
- `tools/build-user.sh` builds optional external POSIX applications.
- `tools/new-user-app.sh` creates a new application from the SDK template.
- `tools/sxfs_sync.py` creates or additively updates `build/disk.img` under a
  lock, validates both sides, compacts when needed, and atomically retries.
- `tools/stamp_sxe.py` stamps and verifies generated SXE sections.
- `tools/run_qemu.py` launches the staged image with OVMF and optional virtio.
- `tools/shoot.sh` runs visual desktop scenarios through a Unix QMP socket.
- `ports/doomgeneric/build.sh` builds the official Doom port independently.
- `ports/ffmpeg/build.sh` preserves FFmpeg's configure and GNU Make flow; its
  optional backend installs as `/disk/bin/mediaplayer-ffmpeg`, separate from
  the always-built launcher.
- `subsystems/native/build.sh` is the optional native Haxe builder.
- `tools/limine/` is the ignored, pinned bootloader checkout.

The Haxe subsystem and third-party ports remain outside the base CMake target.
They have independent build flows and must not become boot-time dependencies of
the system image.

## Persistent image safety

A normal build never removes `build/disk.img`. An additive apply runs on a full
sibling candidate; the original is replaced only after the candidate passes
validation. If the allocator reports no contiguous run, the synchronizer
extracts reachable data, removes build-produced files from the carryover copy,
rebuilds a same-size staging image, validates it, and retries with a recovery
copy available. A failed apply or replacement leaves the original image
available; a failed apply must be treated as suspect before reuse.

The `sxfs-cli check` and `extract` paths reject pending journals, dirty
superblocks, invalid directory entries, aliases, cycles, and unreachable
allocated inodes rather than dropping data.

The regression case is external data, not only the freshly generated rootfs:

```bash
./tools/verify_doom_persistence.sh \
  --wad ports/doomgeneric/wad/doom1.wad
```

The check confirms that `/disk/bin/doomgeneric` and the WAD remain present after
a normal rebuild.
