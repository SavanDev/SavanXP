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

The Win32 subsystem's DLLs and test fixtures are PE32 images: Clang compiles
them for `i686-pc-windows-msvc` (a target every LLVM build includes) and
`lld-link`, from the same `lld` package as `ld.lld`, links them, and `llvm-rc`,
from the `llvm` package, compiles the fixtures' resource scripts. CMake finds
them on `PATH` as `SAVANXP_LLD_LINK` and `SAVANXP_LLVM_RC`. Neither the Windows
SDK nor MinGW is used; see [WIN32.md](../userland/WIN32.md#build).

The `hda-capture`, `virtio-capture`, `audio-devices` and `sound-capture-smoke` scenarios also
require QEMU's ALSA audio backend and ALSA library plugins (`alsa-lib` on Arch).
They generate a deterministic virtual microphone through the ALSA file/null
plugins, without accessing the host microphone or speakers. The normal build
and other smoke scenarios do not require ALSA.

The static musl fixtures of the Linux subsystem link against the pinned
Alpine root that `subsystems/linux/profile/profile.sh setup` fetches by hash
(it needs `bwrap` and `curl`). CMake looks for it at
`SAVANXP_LINUX_MUSL_ROOT`, `build/linux-profile/root` by default. Without it
those fixtures are not built and `linux-hello` reports them as skipped; see
[LINUX.md](../userland/LINUX.md#how-it-is-tested).

Python is the `python3` command on `PATH`, the same one you run, and it
must have Pillow. A versioned interpreter earlier in `PATH` (a uv or pyenv
`python3.X` without Pillow) is not picked over it. To use another
interpreter, pass `-DPython3_EXECUTABLE=/path/to/python3` at configure.

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

### Preparing an installed disk

`prepare-disk` builds a fresh GPT disk with a FAT32 EFI partition and a 1 GiB
SxFS root. The EFI partition contains Limine, the kernel and the initramfs.
The installed boot entry identifies its root by GPT partition UUID. The
command builds the SDK and populates the root from the base system tree.

```bash
./build.sh prepare-disk --output build/savanxp-installed.img
```

The image is a standalone UEFI hard disk for QEMU with OVMF. To prepare a
dedicated USB drive instead, select the **whole disk**, unmount all its
volumes in Linux, and make sure your account has write access to the device.
`--erase` authorizes replacing its partition table and the filesystems in the
new layout. Use the stable device link, without a partition suffix.

```bash
./build.sh prepare-disk --device /dev/disk/by-id/usb-YOUR_DEVICE --erase
```

This requires 512-byte logical sectors and a disk between 1122 MiB and 2 TiB,
exclusive of the upper bound. Boot the drive
from the PC's 64-bit UEFI menu with Secure Boot off. For development QEMU
sessions, use the SxFS partition path printed by `prepare-disk`.

```bash
ROOT_VOLUME=/dev/disk/by-partuuid/YOUR_ROOT_UUID
./build.sh run --disk-device "$ROOT_VOLUME" --disk-write
```

### Installing ports to a personal volume

`SAVANXP_DISK_IMAGE` selects the destination for the official ports and
`tools/build-user.sh`. It accepts an existing SxFS image file or the SxFS
partition. Close QEMU before installing to a device and use an account with
write access to that partition.

```bash
SAVANXP_DISK_IMAGE="$ROOT_VOLUME" ./ports/doomgeneric/build.sh --wad /path/to/doom1.wad
```

The port installer stages and validates its update before writing the
partition. It holds the partition exclusively and keeps a recovery image on
the build host during the write, under `build/device-recovery/` by default.
On an I/O failure it attempts to restore the old volume. If restoration fails
or the installer is interrupted, the recovery image is available for offline
restoration. `SAVANXP_DEVICE_RECOVERY_DIR` can place it on another persistent
host filesystem. The port destination defaults to the usual `build/disk.img`.

### Personal volumes in QEMU

`run` and `debug` can build the local kernel and initramfs and boot with an
existing SxFS root of your choice. For a USB drive with a mounted FAT volume,
shut down the development VM and copy its image to a file on the drive once.
The guest's writes go to the copy. This example uses `/media/usb` as the
mount point.

```bash
./build.sh build
cp -i build/disk.img /media/usb/savanxp.img
./build.sh run --disk-image /media/usb/savanxp.img
```

For direct block access, use a USB drive already formatted with raw SxFS or
select its SxFS partition. Use a stable link from `/dev/disk/by-id` (including
the partition suffix if needed), since device names like `/dev/sde` can
change. Unmount the volume in Linux before opening it and make sure your
account has permission to read the device. Guest writes go to a temporary
overlay. Add `--disk-write` and grant device write access to keep the changes.
The launcher claims the device exclusively for the lifetime of QEMU.

```bash
./build.sh run --disk-device /dev/disk/by-id/usb-YOUR_DEVICE
./build.sh run --disk-device /dev/disk/by-id/usb-YOUR_DEVICE --disk-write
```

Both options use the applications and libraries already on the selected
volume. To refresh the internal userland on a personal **image file**, close
QEMU and run the same additive update as the normal build.

```bash
./build.sh build
python3 tools/sxfs_sync.py --image /media/usb/savanxp.img \
    --source build/diskfs --cli build/tools/sxfs-cli
```

## External applications

The external POSIX builder is deliberately separate from the in-tree CMake
userland registry:

```bash
./build.sh build
./tools/build-user.sh --source sdk/hello/main.c --name hello
./tools/build-user.sh --source path/to/app --name app \
  --destination /bin/app
```

`--destination` is a path on the disk, which is the guest's root (`/disk/...` is still accepted); the default is `/bin/NAME`. The
builder accepts a single C/assembly file or a directory, stamps SXE resources,
and installs through `tools/sxfs_sync.py` unless `--no-install` is selected.

The builder compiles against the SDK sysroot, `build/sdk/<MAJOR.MINOR>`, and
never the repository's tree. `tools/make-sdk.sh` assembles the sysroot: the
`savanxp_sdk` target runs it on every `./build.sh build`, and `build-user.sh`
runs it before every build. It rebuilds only when an input changed. The layout
is in [the SDK README](../../subsystems/posix/sdk/v1/README.md#the-sysroot).

`tools/new-user-app.sh` creates a new application from `sdk/template`, and
`tools/run-user.sh` builds, installs, and boots an external application in one
step.

### The runtime's units: one manifest

Which source files make up the POSIX userland runtime is written once, in
`subsystems/posix/sdk/v1/runtime/units.txt`: one `<group> <path>` line per
unit, the path from the repository root. Every build reads it:

| Consumer | Groups |
|---|---|
| CMake (`SAVANXP_USER_RUNTIME_SOURCES`) | `core math gui audio midi` |
| `tools/make-sdk.sh` | `core gui audio setjmp` without SSE, plus `math gl` with it, prebuilt into the sysroot. `tools/build-user.sh` links `core setjmp`, plus `math gl` (`--sse`), `audio` (`--audio`) and `gui` (`--gui`) |
| `ports/doomgeneric/build.sh` | `core math audio setjmp`, from the sysroot's PIE variant |
| `ports/ccleste/build.sh` | `core audio math setjmp`, from the sysroot's PIE variant |
| `ports/ffmpeg/runtime.sh` | `core math setjmp` into `libsavanxp.a`, `gui` into `libsxgui.a` |

Which runtime units each system library replaces is written once too, in
`subsystems/posix/sdk/v1/runtime/libraries.txt`. CMake turns it into
`SAVANXP_LIBRARY_REPLACES_<library>`, the units an in-tree PIE program's
`DEPENDS` leave out. `tools/make-sdk.sh` ships the libraries in the sysroot's
`lib/shared/`, with the objects each replaces, for `build-user.sh --pie
--needs`.

The SUN runtime has a manifest of the same format, `system/runtime/units.txt`
(`core`: the heap's page source; `gui`: the application frame and the file
dialog). CMake links all of it into every NATIVE program
(`SAVANXP_SUN_RUNTIME_SOURCES`, read by `savanxp_manifest_units`), and
`tools/make-sdk.sh` prebuilds it into the sysroot as `sun-core` and `sun-gui`.
`build-user.sh --native` links the first, and adds the second with `--gui`.

The scripts read it through `tools/lib/runtime_units.sh`
(`savanxp_runtime_units ROOT GROUP...`), which fails on an unknown group;
CMake re-configures when the manifest changes. Moving or adding a runtime unit
is an edit to the manifest and nothing else. Per-unit compile flags (the
`-fno-builtin` on `math.c`) stay in the script that needs them, keyed by file
name. `--heap-mib` no longer recompiles `posix.c`: the bootstrap arena is weak,
and the program links a strong one.

## Version and build number

The system shows its version as `<MAJOR>.<MINOR>.<PATCH> (Build <build>)`, for
example `0.4.2 (Build 588)`: on the boot screen, the kernel log, the desktop
stamp, About and the console banner. `sysinfo` prints them as two lines,
`version v0.4.2` and `build 588`, so scripts can read each one.

- **The version** lives in `include/shared/version.h` and is the single source
  of truth. Master always carries the *next* version: the release commit
  (`release(vX.Y.Z): ...`, tagged `vX.Y.Z`) is the last commit with that
  number, and the commit right after it bumps `SAVANXP_VERSION_PATCH` (or
  MINOR/MAJOR when the next release is planned as one). Every build checks
  this: if `vX.Y.Z` is already tagged on an earlier commit, it prints a CMake
  warning asking for the bump. The whole release procedure is in
  [RELEASING.md](RELEASING.md).
- **The build number** is the commit count of `HEAD` (`git rev-list --count
  HEAD`). It grows with every commit and the same code always gets the same
  number; uncommitted changes do not change it. Without git it is `0`, and a
  shallow clone gives a smaller count, so release images are built from a full
  clone.
- The target `savanxp_build_number` runs `cmake/GenBuildNumber.cmake` on every
  build and writes `build/generated/build_number.h` (`SAVANXP_BUILD_NUMBER`,
  `SAVANXP_BUILD_STRING`, `SAVANXP_FULL_VERSION_STRING`,
  `SAVANXP_FULL_VERSION_NUMBER`) only when the number changed, so only the
  files that include it recompile, once per commit.
- `version.h` stays free of the build number on purpose: it is part of what
  external programs compile against, and `build_number.h` exists only in the
  system's own build tree.

## Layout

- `CMakeLists.txt` defines the target graph and host-tool requirements.
- `cmake/UserPrograms.cmake` is the explicit in-tree userland registry. A
  program registered with `NATIVE` runs on the native (Sx) API: it must be
  `STATIC` (the shared-library loader speaks POSIX), and after the SXE
  resources `tools/stamp_osabi.py` marks it with `EI_OSABI` from
  `include/abi/savanxp_native_abi.h`. Its file is `<name>.sxe`, staged as
  `/bin/<name>.sxe` ([SXE_FORMAT.md](../userland/SXE_FORMAT.md#sun-programs-are-named-sxe)).
  `compositord` is one.
- `tools/mkinitramfs.py` writes the newc cpio archive.
- `tools/run_smoke.py` runs one QEMU smoke scenario and records serial output.
- `tools/qmp_client.py` provides the keyboard and taskbar QMP drivers.
- `tools/taskbar_smoke.py` and `tools/tcp_echo_server.py` provide host-side
  scenario drivers.
- `tools/smoke_catalog.py` keeps commands, tokens, and callbacks declarative.
- `tools/build-user.sh` builds optional external applications: POSIX, or SUN
  with `--native`, installed as `/bin/<name>.sxe`.
- `tools/make-sdk.sh` assembles the SDK sysroot under `build/sdk/`;
  `tools/lib/sdk_flags.sh` holds the compile flags it shares with
  `build-user.sh`.
- `tools/lib/runtime_units.sh` reads the runtime manifest for the builds
  outside CMake.
- `tools/new-user-app.sh` creates a new application from the SDK template.
- `tools/sxfs_sync.py` creates or additively updates `build/disk.img` under a
  lock, validates both sides, compacts when needed, and atomically retries.
  Installing `bin/<name>.sxe` retires `bin/<name>`, and the reverse.
- `tools/stamp_sxe.py` stamps and verifies generated SXE sections.
- `tools/stamp_osabi.py` marks a `NATIVE` program's ELF as native, and refuses
  one whose name on disk lacks `.sxe` or whose ELF lacks `.sxmeta`.
- `tools/run_qemu.py` launches the staged image with OVMF and optional virtio.
  Without virtio the disk goes on the q35 AHCI controller, on `isa-ide` with
  `--disk-bus ide` / `SAVANXP_DISK_BUS=ide`, or on an NVMe controller with
  `nvme`; every launcher honours the variable.
- `tools/shoot.sh` runs visual desktop scenarios through a Unix QMP socket.
- `ports/doomgeneric/build.sh` builds the official Doom port independently.
- `ports/ffmpeg/build.sh` preserves FFmpeg's configure and GNU Make flow and
  installs the shared library `/lib/libffmpeg.so.0.4` and no program; the
  Media Player is built by the tree from `subsystems/posix/userland/mediaplayer/`.
- `tools/limine/` is the ignored, pinned bootloader checkout.

Third-party ports remain outside the base CMake target.
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

The `sxfs-cli check` and `extract` paths reject invalid directory entries,
aliases, cycles, and unreachable allocated inodes rather than dropping data.
An image left with a pending journal or a superblock not marked clean (QEMU
killed mid-write) is read as recovered, without modifying it; `apply` and `rm`
persist that recovery on the candidate before changing it, and `sxfs-cli
repair` handles what does not validate
([SXFS_CHECK.md](../fs/SXFS_CHECK.md)).

The regression case is external data, not only the freshly generated rootfs:

```bash
./tools/verify_doom_persistence.sh \
  --wad ports/doomgeneric/wad/doom1.wad
```

The check confirms that `/bin/doomgeneric` and the WAD remain present after
a normal rebuild.
