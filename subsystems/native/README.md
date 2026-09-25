# Native Haxe subsystem

The native subsystem is an optional, standalone experiment. It is not a
dependency of the base SavanXP build and is not installed in the default
`disk.img`. The base kernel keeps only the small native ABI compatibility layer
needed to recognize and route native executables.

The current AOT path is:

```text
Haxe -> reflaxe.CPP -> C++17 -> freestanding native ELF
```

The ELF is marked with `EI_OSABI=0x53` and is routed through the native syscall
dispatcher. The experiment remains single-threaded, assembly-free, and limited
to the validation applications described below.

## Build

The maintained Linux entry point is standalone and does not add a CMake target:

```bash
./subsystems/native/build.sh --name nativehello --source haxe --no-install
./subsystems/native/build.sh --all --no-install
./subsystems/native/build.sh --all --install
./subsystems/native/build.sh --install --no-compact
```

`--install` stages only the selected native binaries and uses
`tools/sxfs_sync.py`, including its lock, candidate validation, atomic
replacement, rollback, and compaction safeguards. It requires an existing valid
`build/disk.img` and `build/tools/sxfs-cli`; it never passes `--reset`.

The Bash entry point is the maintained implementation.

## Toolchain and dependencies

`UPSTREAM` records the Haxe release and the exact reflaxe commits. The first
build downloads those repositories into the ignored work directory
`build/ports/haxe/deps/`; subsequent builds reuse the pinned checkouts and
reject dirty trees.

Required host tools are:

- Haxe `4.3.7`;
- `clang`, `clang++`, and `ld.lld`;
- Git;
- Python/Pillow, `llvm-objcopy`, and `llvm-readelf` when SXE resources are
  generated.

The build reuses the POSIX `crt0.S` and `linker.ld`. It does not link the Haxe
compiler or generated C++ into the base userland target.

## ABI boundary

The kernel-facing contract is now neutral base infrastructure:

- `include/abi/savanxp_native_abi.h` — syscall numbers, shared structures,
  ABI version, and the native ELF OSABI value.
- `kernel/native_syscall_dispatch.inc` — the base kernel dispatcher.

The optional userland SDK remains under `subsystems/native/sdk/`:

- `sdk/include/savanxp_native.h` — `sxn_*` runtime API;
- `sdk/include/cxxstd/` — mini freestanding C++ library;
- `sdk/runtime/` — syscall shims, heap, GUI, text, filesystem, and entry glue.

The neutral ABI header is also used by `include/kernel/elf.hpp` and
`tools/gen_sxe_resources.py`, so the loader, resource generator, and standalone
builder cannot silently disagree about `0x53`.

## Applications

- `haxe/` — `nativehello`, validating classes, `String`, `Array`, `Null<T>`,
  `Map`, floating point, and native syscalls.
- `haxe-gui/` — `nativegui`, a compositor client using keyboard and pointer
  events.
- `haxe-sxgui/` — `sxguiapp`, an interactive SXGUI-style validation client.
- `test/guihost.c` and `test/sxguihost.c` — optional host-side GUI harnesses.

Each application has a small `.sxres` manifest with `subsystem=native`, so
`gen_sxe_resources.py` and `stamp_sxe.py` can stamp launcher metadata. These
files are external `/disk/bin` artifacts; they are not copied into `rootfs` or
`diskfs` by the base build.

## Repository layout

```text
build.sh                         standalone Linux entry point
UPSTREAM                         Haxe/reflaxe pins
haxe/                            nativehello source and SXE manifest
haxe-gui/                        nativegui source and SXE manifest
haxe-sxgui/                      sxguiapp source and SXE manifest
haxe-support/                    reflaxe compiler support macros
haxe-std-fixes/                  cross-target Haxe standard-library fixes
haxe-toolkit/                    validation-only GUI helpers
sdk/                             native userland runtime and mini C++ SDK
include/abi/                     neutral kernel/userland ABI
kernel/                          native syscall dispatcher
```

The root `CMakeLists.txt` intentionally has no `find_program(HAXE)`,
`add_subdirectory(subsystems/native)`, or native application target.

## Validation and persistence

The native build can be checked without touching the image:

```bash
./subsystems/native/build.sh --name nativehello --source haxe --no-install
llvm-readelf -h build/native/nativehello.elf | grep 'OS/ABI'
```

After installation, the normal Linux build must preserve all external port
assets, including:

```text
/disk/bin/nativehello
/disk/bin/mediaplayer
/disk/bin/mediaplayer-ffmpeg
/disk/bin/doomgeneric
/disk/games/doom/doom1.wad
```

The native experiment is intentionally not part of the base image or its
release guarantees until the managed-layer policy is revisited.
