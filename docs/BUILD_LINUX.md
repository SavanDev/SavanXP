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
as the media-player display assertion. The keyboard, pointer and taskbar smoke
drivers use `tools/qmp_client.py`.

A scenario whose device only exists on the paravirtualized machine declares it
in the catalog (`requires_virtio`) and the runner refuses to start without
`--virtio` rather than failing on a missing device. `pointer-smoke` is the one
today: it drives `virtio-tablet`, and the base machine has PS/2, which has no
absolute pointer to assert on.

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

### The v1 image does not mount

SxFS is at format version 2. A v1 image is rejected on sight, by both the kernel
and `sxfs-cli`:

```text
sxfs-cli: 'disk.img' no es una imagen SxFS valida: argumento/ruta invalida.
```

That is deliberate and not a bug. v2 moved the inode table from 64 to 1024
sectors and the block bitmap from 32 to 512, and every other LBA is derived from
those two numbers, so a v1 image would have its metadata read from offsets that
now mean something else. The superblock validator compares the on-disk geometry
against the compiled constants, which turns that into an explicit refusal.

There is no converter. To keep the contents of a v1 image, extract it with a v1
`sxfs-cli` and rebuild it into a v2 one. `build/disk.img` itself is simply
deleted and recreated by the next build.

### The geometry

| | v1 | v2 |
| --- | --- | --- |
| `SXFS_VERSION` | 1 | 2 |
| `SXFS_MAX_INODES` | 256 | 4096 |
| `SXFS_INODE_TABLE_SECTORS` | 64 | 1024 |
| `SXFS_BLOCK_BITMAP_SECTORS` | 32 | 512 |
| `data_lba` | 197 | 3077 |
| volume ceiling | 64 MiB | 1 GiB |

The development image is 1 GiB, and it costs about 30 MiB of real disk: the
volume is created sparse, so the unused tail is a hole and costs nothing until
the filesystem writes into it. `sxfs-cli create` used to write real zeros, which
at this size would have been 1 GiB of disk and 1 GiB copied on every build.

The ceiling is the block bitmap: every bit is a sector, so 512 sectors of bitmap
address 2 Mi sectors. The inode bitmap stays at one sector because 4096 inodes
are exactly its 4096 bits.

The cost is metadata, and it is not free. Two things to know:

**Capacity.** With the default 64 MiB volume, usable data drops from 63.9 MiB to
62.5 MiB, because the metadata grew from 97 to 1537 sectors. A bigger volume
recovers that and more.

**Commits got 16 times more expensive.** The journal copies the whole metadata
and rewrites it, so commit cost tracks metadata size, not volume size. Measured
on the core smoke with `fscheck`:

```text
commits             19
bytes_written       29933056
bytes_per_commit    1575424
```

That is 28.5 MiB of PIO writes per smoke, against 1.8 MiB in v1, and the smoke's
wall clock went from about 7.4 s to 16.5 s. It is affordable but it is the wall
for the next round of growth, and the fix is a delta journal: log which blocks
changed instead of copying everything. The counters above exist so that decision
rests on a measurement.

### The development machine and the LiveCD have different volumes

They are different on purpose, and the sizes are independent:

| | Volume | Where it lives |
| --- | --- | --- |
| `run`, `debug`, smokes, `gpu-soak` | 1 GiB | a disk QEMU attaches |
| ISO / LiveCD | 256 MiB | RAM, as a Limine module |

The LiveCD volume is smaller because it is loaded into memory, so every byte of
it is also a byte of guest RAM. Measured on the ISO boot with a 512 MiB machine:

```text
block: 1 device(s) livecd(rw)
boot ready: 219 MiB usable, 2 MiB reclaimable
```

The development machine, which attaches the volume as a disk instead, has 1 GiB
of RAM and reports 938 MiB usable with the same kernel and the same filesystem.

The ISO tree gets its own copy of the volume at the LiveCD size rather than a
copy of the development image. `tools/iso_volume.py` extracts, recreates at the
requested size and applies, which is what shrinking a filesystem means, and it
fails with a clear message if the contents do not fit. Two consequences worth
knowing: the ISO is about 286 MB, because a sparse 256 MiB volume becomes 256 MiB
of real bytes in ISO9660, and growing the LiveCD volume past the development one
is refused rather than silently producing a larger file.

## Persistence checks

The repository provides a repeatable external-data regression:

```bash
./tools/verify_doom_persistence.sh \
  --wad ports/doomgeneric/wad/doom1.wad
```

It verifies the Doom executable, the WAD, configuration files, and savegames
across a normal rebuild. Never replace `build/disk.img` with a newly formatted
image as part of routine cleanup.
