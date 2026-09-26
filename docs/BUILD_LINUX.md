# Native host build

SavanXP's supported host workflow is Bash and CMake on Linux. `build.sh` is the
canonical entry point and CMake owns the target graph, tool discovery, image
staging, and host tests.

## Requirements

The host needs:

- Clang/LLVM (`clang`, `ld.lld`, `llvm-objcopy`, and `llvm-readelf`)
- CMake and Ninja
- Python 3 with Pillow
- Git
- QEMU with OVMF firmware
- `xorriso` for ISO targets
- GNU Make for the optional FFmpeg port

On Arch Linux, the core set is:

```bash
pacman -S clang lld llvm cmake ninja make python-pillow \
  qemu-system-x86 edk2-ovmf libisoburn
```

CMake resolves tools from `PATH` and accepts cache overrides when a distribution
uses non-standard locations. Set `SAVANXP_OUTPUT_ROOT` to move generated
products and the persistent image, and set `SAVANXP_BUILD_DIR` to select the
CMake binary directory.

## Basic workflow

From the repository root:

```bash
./build.sh configure
./build.sh build
./build.sh run
./build.sh test
```

The first configure downloads the pinned Limine checkout under `tools/limine`.
The normal build stages the internal userland and the EFI tree while preserving
an existing valid `build/disk.img`. The image is updated through a validated
sibling candidate and is replaced atomically only after validation succeeds.

## Official ports

Ports are independent from the base CMake target and use the same SxFS image:

```bash
./ports/doomgeneric/build.sh --wad ports/doomgeneric/wad/doom1.wad
./ports/ffmpeg/build.sh
./ports/ffmpeg/smoke.sh
./ports/ccleste/build.sh
```

`ccleste` downloads its pinned upstream archive and hash-verifies it, then
installs the game assets from that same archive under `/disk/games/celeste`.

The optional native Haxe experiment has its own entry point:

```bash
./subsystems/native/build.sh --name nativehello --source haxe --no-install
```

A port installer requires an existing valid image and the `sxfs-cli` produced by
the base build. It never resets the image.

## External SDK applications

The external application builder is independent from the in-tree userland
registry:

```bash
./tools/build-user.sh --source sdk/hello --name hello
./tools/build-user.sh --source sdk/multifile --name multifile --no-install
./tools/new-user-app.sh --name myapp
./tools/run-user.sh --source sdk/errdemo --name errdemo
```

Use `--sse` for the floating-point runtime, `--gui` for SXGUI, `--audio` for
the PCM mixer, and `--destination` for a path below `/disk`. Installation uses
`tools/sxfs_sync.py` and the same candidate/validation/rollback rules as the
base image builder.

## Smoke and visual tests

Smoke scenarios use the staged Bash/CMake image and a disposable copy of the
persistent disk:

```bash
./build.sh smoke --list
./build.sh smoke smoke
./build.sh smoke ffmpeg-smoke
./build.sh smoke taskbar-smoke
./build.sh smoke --smp 4
./tools/shoot.sh --scenario desktop
```

The runner stages one ASCII `/SMOKE` line in the initramfs, locks the real image,
boots QEMU, and records the command and serial output under `build/smoke-logs/`.
A normal build removes the temporary smoke specification from the image.

`tools/shoot.sh` speaks QMP over a Unix socket and uses the same scenario driver
as the media-player display assertion. The keyboard and taskbar smoke drivers
use `tools/qmp_client.py`.

## The persistent image may be larger than its filesystem

`build/disk.img` is allowed to be bigger than the SxFS volume inside it. The
kernel mounts an image whose file is larger than `superblock.total_sectors`
without complaint — it only requires `total_sectors <= device sectors` — and
ignores the tail. `sxfs-cli` follows the same rule: it rejects an image that is
*smaller* than the superblock declares, and accepts one that is larger.

That is the room a volume grow needs. Nothing has to be reformatted to make the
file bigger:

```bash
truncate -s 496M build/disk.img   # sparse: the tail costs no space yet
./build.sh build                   # keeps the size and keeps it sparse
```

Two rules keep that safe, and both are covered by
`tests/host/sxfs_compaction_test.py`:

- The rebuild path must preserve the file size. Safe compaction recreates the
  image from scratch at exactly `total_sectors`, so it would otherwise shrink a
  grown image back to 64 MiB and throw the room away.
- The rebuild path must preserve holes. `shutil.copy2` does not on Linux, and an
  ordinary sync copies the image before applying, so one build would turn the
  reserved tail into real bytes and every build after it would rewrite all of
  them.

### The 496 MiB ceiling is gone from the development path

It used to be the FAT16 staging. It no longer applies to `run` or the smokes,
because the development boot tree no longer carries the persistent image at all
— see the section above. The ceiling that binds there now is SxFS itself, at
64 MiB, which is the block-bitmap geometry rather than anything about staging.

The ISO is not limited either: `--efi-boot-image` builds a small El Torito FAT
image for the bootloader and leaves the rest of the tree on ISO9660, which has
no FAT16 ceiling.

## QEMU packages and firmware

Some distributions split QEMU's optional display and audio backends into
separate packages. On Arch, install the packages that match the installed QEMU
version when using virtio display, a graphical window, or SDL audio:

```bash
pacman -S qemu-hw-display-virtio-vga qemu-hw-display-virtio-gpu-pci \
  qemu-ui-gtk qemu-ui-opengl qemu-audio-sdl libpulse
```

The launcher accepts only `tcg` and `kvm` for acceleration:

```bash
./build.sh run --accel tcg
./build.sh run --accel kvm
```

KVM requires hardware virtualization support and uses the host CPU model. The
default `tcg` mode works without `/dev/kvm` and is the portable fallback.

### The 496 MiB ceiling is gone from the development path

`build/image` no longer contains `build/disk.img`. The persistent volume reaches
the system two different ways, and only one of them belongs to each path:

| Path | How `/disk` arrives | Boot config |
| --- | --- | --- |
| `run`, `debug`, smokes, `gpu-soak` | QEMU attaches `build/disk.img` as an ATA or virtio-blk disk | `boot/limine-dev.conf` |
| ISO / LiveCD | a Limine module, mounted as the `livecd` ramdisk | `boot/limine.conf` |

In the development path the module was pure overhead. `kernel_main.cpp` registers
it as a `livecd` ramdisk with priority 10, against ATA's 100 and virtio-blk's
110, and `fs::mount_any` takes the first device that claims the root — so Limine
loaded the whole image into RAM, the kernel enumerated it as a second device, and
nothing ever mounted it. Measured effect of dropping it:

```text
block: 2 device(s) ata0(rw) livecd(rw)   ->  block: 1 device(s) ata0(rw)
boot ready: 367 MiB usable               ->  boot ready: 431 MiB usable
```

and the staged EFI tree went from 542 MiB to 30 MiB, which is what lifts the
FAT16 limit for `run` and the smokes. The ISO keeps its own tree in
`build/iso-root`, where the module is the only source of storage there is.

The kernel does not depend on the module either way: `build_boot_info()` leaves
`disk_image_address` null when no such module came, and `kernel_main.cpp` checks
it before calling `ramdisk::attach_image`.

`tools/iso_boot_test.py` requires `livecd(rw)` and `/disk mounted` in the serial
log of both the BIOS and the UEFI ISO boots. Reaching `init` is not enough: an ISO
whose volume module went missing still hands off to `init` and then has no
storage at all.

The ISO carries its own tree in `build/iso-root`, and that tree gets its own
**compacted** copy of the volume. A grown development image has a sparse tail
that is reserved room and nothing else; copied into an ISO it becomes real zero
bytes, which is how a 64 MiB volume turns a 98 MB ISO into a 541 MB one.
`tools/iso_volume.py` stages the volume at exactly what the superblock declares
and leaves the development image untouched.

## Persistence checks

The repository provides a repeatable external-data regression:

```bash
./tools/verify_doom_persistence.sh \
  --wad ports/doomgeneric/wad/doom1.wad
```

It verifies the Doom executable, the WAD, configuration files, and savegames
across a normal rebuild. Never replace `build/disk.img` with a newly formatted
image as part of routine cleanup.
