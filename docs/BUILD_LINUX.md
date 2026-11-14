# Linux host setup for i386

The supported workflow on this branch is Bash/CMake targeting 32-bit x86.
See [the build commands and overrides](BUILD_CMAKE.md) and
[the porting roadmap](I386_PORT.md).

## Dependencies

Install Clang/LLVM (`clang`, `clang++`, `ld.lld`, `llvm-objcopy`,
`llvm-readelf`), CMake, Ninja, Python 3 with Pillow, Git, QEMU with
`qemu-system-i386`, `xorriso`, and GRUB2 with `grub-mkrescue` and the `i386-pc`
platform files. OVMF, Limine and a hosted 32-bit libc are not needed. Clang
cross-compiles freestanding ELF32 while host tests use the host ABI.

On Arch Linux:

```bash
pacman -S clang lld llvm cmake ninja python-pillow git qemu-system-x86 libisoburn grub
```

On Debian/Ubuntu:

```bash
apt install clang lld llvm cmake ninja-build python3-pil git qemu-system-x86 xorriso grub-common grub-pc-bin mtools
```

Package names may differ by distribution. CMake discovers host tools from
`PATH`; optional paths can be set through cache variables. Use a Python
interpreter with Pillow, for example `SAVANXP_PYTHON=python3 ./build.sh build`.

## Build and boot

```bash
./build.sh build
./build.sh run
./build.sh smoke
./build.sh test
```

QEMU uses TCG by default. `./build.sh run --accel kvm` requires access to
`/dev/kvm` and a host capable of running the configured 32-bit CPU model.
The kernel is currently single-CPU; virtio and the full x86_64 device set are
porting work. `./build.sh debug` starts QEMU paused for GDB on port 1234.

Normal boot opens `sh32`. `desktop` launches the minimal desktop, `multitest`
runs process regressions, and `run /disk/bin/NAME` launches an external ELF32.
An existing SxFS disk is updated without discarding external files; old ELF64
programs must be rebuilt before they can run on this branch.

For a low-level headless check after `./build.sh iso`:

```bash
python3 tools/legacy_boot_test.py --iso build/SavanXP.iso \
  --disk-image build/disk.img --expect-user --expect-processes \
  --expect-desktop build/linux-i386/desktop32 --artifacts /tmp/savanxp-i386
```

This harness uses a disk snapshot. Attach `build/disk.img` as an IDE disk when
using the ISO in another hypervisor; the ISO alone boots the kernel but does
not contain userland. Physical laptop support remains subject to the gaps in
[REAL_HARDWARE.md](REAL_HARDWARE.md).
