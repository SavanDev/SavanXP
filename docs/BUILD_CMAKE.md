# Native i386 CMake build

This branch builds i386 only. The x86_64 system lives on `master`; dedicated
64-bit boot sources, Limine staging and the dual-architecture build graph have
been retired here. Unported generic kernel and userland sources remain available
for the [progressive port](I386_PORT.md).

## Commands and targets

```bash
./build.sh configure
./build.sh build
./build.sh userland
./build.sh kernel
./build.sh iso
./build.sh run
./build.sh debug --headless
./build.sh smoke
./build.sh smoke boot
./build.sh smoke processes
./build.sh smoke desktop
./build.sh smoke legacy-cpu
./build.sh test
./build.sh clean
```

The default build directory is `build/linux-i386`; output goes to `build`.
`build` compiles the kernel and userland and updates `disk.img`. `iso`, `run`
and `debug` also prepare the disk and build a GRUB2 BIOS ISO. The ISO does not
embed the disk; userland requires the attached IDE image. `run` launches
`qemu-system-i386` with SeaBIOS/i440FX, `qemu32`, standard VGA, PS/2 and one CPU.
`debug` adds `-s -S`. There is no UEFI firmware or Limine download.

Standard CMake targets are `savanxp`, `savanxp_kernel`, `savanxp_userland`,
`savanxp_disk`, `savanxp_iso`, `savanxp_run`, `savanxp_debug`, and `savanxp_tests`.
The older `savanxp_i386_*` userland/disk aliases remain available.
`SAVANXP_ARCH=x86_64` is rejected with a pointer to `master`.

```bash
cmake --preset linux
cmake --build --preset linux
```

Presets also select i386 and use a new build directory, avoiding old x86_64
caches. A stale cache explicitly set to x86_64 is rejected by direct CMake;
configure a separate directory rather than mixing architectures.

## Tools and overrides

Install Clang, LLD, LLVM tools, CMake 3.25+, Ninja, Python 3.10+ with Pillow,
Git, QEMU, `xorriso`, and GRUB2's `grub-mkrescue` and `i386-pc` platform files.
Tools are discovered from `PATH`, with CMake cache overrides such as
`SAVANXP_LD`, `SAVANXP_GRUB_MKRESCUE` and `SAVANXP_GRUB_I386_PC`.
Direct CMake accepts `Python3_EXECUTABLE`; `build.sh` passes its host interpreter
from `SAVANXP_PYTHON` (default `python3` from `PATH`).

Environment overrides include `SAVANXP_BUILD_DIR`, `SAVANXP_OUTPUT_ROOT`,
`SAVANXP_BUILD_TYPE`, `SAVANXP_INCLUDE_TEST_APPS`, `SAVANXP_JOBS`,
`SAVANXP_ACCEL`, `SAVANXP_HEADLESS`, and `SAVANXP_QEMU`.
`--legacy` remains a compatibility alias; i386 is already selected.
SMP values other than one and virtio are rejected until those features are ported.

## Regression checks

`./build.sh smoke` invokes `tools/run_smoke.py` and `tools/legacy_boot_test.py` against the freshly built
ISO and a snapshot of the normal disk. It checks boot, shell, keyboard,
concurrent CPU workers, spawn/wait/exit, shell restart, desktop glyphs, PS/2
buttons and cursor restoration. Logs/screenshots stay in a unique directory
under `build/smoke-logs`. `smoke --list` lists supported cases. Former x86_64
scenarios such as `sxgui-smoke` and `windowd-smoke` are unavailable until their
subsystems have been ported; the minimal desktop check does not prove SxGUI.

`./build.sh test` runs hosted regressions for reused modules, external ELF32
construction, and the normal build/run routing. Host audio or network tests do
not imply those drivers work in the i386 guest.

`tools/iso_boot_test.py --iso build/SavanXP.iso` checks the BIOS boot catalog
and diskless kernel startup. `tools/shoot.sh --scenario desktop` builds and
verifies the minimal desktop, saving screenshots under `build/shots`.

## Persistence and external programs

`cmake/userland-i386.cmake` stages the internal userland in `build/diskfs` and
uses `tools/sxfs_sync.py` to additively synchronize `build/disk.img`. Existing
valid images, unrelated external binaries, libraries and `/disk/games` survive.
Updates use a validated sibling candidate and atomic replacement; `clean` does
not remove the image. Existing ELF64 files are preserved as data, not executed.
The old `build/i386/disk-i386.img` is not selected or reset automatically.

`tools/build-user.sh` builds static ELF32 and installs below `/disk` (default
`/disk/bin/NAME`). It includes i386 `setjmp` and compiler division helpers.
Application dependencies still require the corresponding i386 syscall support.
`tools/run-user.sh` builds/installs and opens the shell; launch the program with
`run /disk/bin/NAME`.

`./tools/verify_doom_persistence.sh` builds and installs the canonical Doom port
as static ELF32 into an isolated copy, adds WAD/config/save data, then runs the
normal build and compares hashes. This checks preservation only. Doom's file,
audio and memory requirements are not yet implemented by the i386 guest.
