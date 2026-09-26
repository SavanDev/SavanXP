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

### The 496 MiB ceiling is the EFI FAT16 image, not SxFS

`build/image` is the staged EFI tree, and QEMU mounts it as a FAT16 filesystem
(`-drive file=fat:rw:...`). `CMakeLists.txt` copies `build/disk.img` into that
tree as `boot/disk.img`, so growing the image also grows the directory QEMU has
to fit into FAT16.

FAT16 as QEMU implements it tops out just under 512 MiB, and the tree carries
the initramfs and the kernel on top of the image, so **496 MiB is the practical
limit for the file, not 512 MiB**:

```text
qemu-system-x86_64: -drive file=fat:rw:.../build/image,format=raw:
  Directory does not fit in FAT16 (capacity 516.06 MB)
```

Past that point the smoke fails at QEMU startup with no serial log, and the
runner discards QEMU's stderr, so the cause is invisible from the log. A larger
persistent volume therefore needs a different staging path — a prebuilt FAT32
image file, or shipping the volume outside the EFI tree — before the SxFS
geometry is even the limiting factor.

Note that this ceiling and the SxFS one are independent: SxFS itself cannot
exceed 64 MiB at the current block-bitmap geometry, and the FAT16 staging caps
the file well above that. Both have to move.

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

## Persistence checks

The repository provides a repeatable external-data regression:

```bash
./tools/verify_doom_persistence.sh \
  --wad ports/doomgeneric/wad/doom1.wad
```

It verifies the Doom executable, the WAD, configuration files, and savegames
across a normal rebuild. Never replace `build/disk.img` with a newly formatted
image as part of routine cleanup.
