# Progressive i386 port

The `legacy` branch targets i386 exclusively. `master` maintains the original
x86_64 system. Normal commands build and run ELF32 through GRUB2/Multiboot2
and legacy BIOS; there is no parallel x86_64 boot target here.

The working kernel is in `arch/x86`. Generic kernel, POSIX, SDK, application,
port and vendor sources that have not crossed over remain as porting material.
Their presence does not mean they are linked or usable on i386. Existing design
documents describe the source system unless a document explicitly records i386
support. Preserve useful protocols and file formats while adapting pointer
width, memory ownership, interrupts and ABI details to 32 bits.

## Working baseline

The graph links the i386 boot/CPU, paging, process, console, input and syscall
implementation with the shared ATA, block, partition, filesystem, SxFS, VFS and
runtime modules. `cmake/userland-i386.cmake` builds the SDK runtime and static
`init32`, `sysinfo`, `sh32`, `desktop32`, plus optional `keyecho`/`multitest32`.
The default disk is `build/disk.img`, the ISO is `build/SavanXP.iso`, and the
CMake cache is `build/linux-i386`. See [BUILD_CMAKE.md](BUILD_CMAKE.md).

Private process maps/stacks, eager x87/SSE state, PIT preemption and spawn/wait/
exit are implemented. There are six static slots with 16 MiB of image space
each; user faults still halt the whole guest. [I386_PROCESSES.md](I386_PROCESSES.md)
defines the exact current guarantees. The full window manager and dynamic
library loader are not linked into this kernel.

## Ordered next milestones

Each milestone must remain bootable and have an end-to-end regression before
adding the next dependency. Update this checklist with implementation and
verification evidence as work lands.

1. **Fault containment and memory foundations.** Terminate a faulting user
   process and reap it without killing the shell or siblings. Add an allocator,
   allocation failure handling and finer user-page protection; replace static
   image reservations. Keep supervisor mappings protected on CPUs without NX.
2. **File descriptors and the POSIX base.** Port per-process descriptor tables,
   regular-file read/write/seek/stat/readdir, console ownership, pipes and
   descriptor inheritance. Restore a useful shell/BusyBox subset with explicit
   tests of process cleanup and persistent data.
3. **Memory objects and ELF32 shared libraries.** Port section objects and
   views, heap growth and ELF32 relocations/loader semantics. Check 32-bit
   layouts, overflow, ownership and failure cleanup; do not reuse ELF64 parsing.
4. **Graphics stack and full desktop.** Port the GPU/surface APIs, SxGUI,
   compositor/window manager and client protocols; then restore taskbar,
   Program Manager and applications. Keep the minimal desktop as a regression
   until the full session passes glyph/input/ownership checks.
5. **Device services and applications.** Port audio, networking, ACPI/power
   and remaining storage/input backends with guest regressions. Restore Doom,
   Ccleste, FFmpeg and the other applications as their dependencies become ready.
   The static Doom build currently verifies installation/persistence only.
6. **Physical hardware and capacity.** Validate BIOS/E820, ATA and framebuffer
   variants on the target laptop, then address suspend/power and performance.
   Revisit SMP and optional virtio only after the single-CPU base is reliable.

The detailed laptop analysis and earlier completed milestones are in
[REAL_HARDWARE.md](REAL_HARDWARE.md); the list above is the current branch plan,
not a claim that those later subsystems have been ported.

## Migration and verification rules

Do not reset a valid disk when changing architectures. Add the new internal
ELF32 programs to the same SxFS volume and retain unrelated external binaries,
libraries, games, configs and saves. Retained ELF64 files fail ELF32 validation;
rebuild individual apps rather than erasing the image. Earlier experimental
`build/i386/disk-i386.img` stays untouched and is not imported automatically.

Run `./build.sh build`, `./build.sh smoke`, `./build.sh test`, and
`./tools/verify_doom_persistence.sh` for build/persistence changes. The default
smoke checks the current i386 shell, preemption, lifecycle and minimal desktop
against the new binaries. Unsupported x86_64 smoke scenarios fail explicitly;
restore them alongside the subsystem they actually verify. When SxGUI lands,
restore `sxgui-smoke` and its glyph assertions before declaring the toolkit
usable. A successful loader-only smoke is not evidence of drawing.
