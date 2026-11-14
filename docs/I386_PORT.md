# Progressive i386 port

The `legacy` branch targets i386 exclusively. `master` maintains the original
x86_64 system. Normal commands build and run ELF32 through GRUB2/Multiboot2
and legacy BIOS; there is no parallel x86_64 boot target here.

The [porting study](I386_PORT_STUDY.md) records the evolution of the branch:
the problems encountered, mechanisms involved, decisions taken and evidence
behind them. This document defines the target and orders the remaining work.

## Version parity is the objective

The current goal is to run **SavanXP v0.4.0 on 32-bit x86 with the smallest
possible visual, behavioral and internal changes**, ideally none wherever the
architecture permits. The reference is the immutable `v0.4.0` tag, commit
`8ece07f4dae90812155a541951cbf8b3701bd92d`, where this branch diverged. Compare
against that version's source, assets, documentation and behavior rather than
against the moving tip of `master`.

Once v0.4.0 parity is reached, the next target becomes **v0.4.1** (tag
`v0.4.1`, commit `1e7e59c6a845423dfa125ec4868352aa6f8af82f`). Repeat this
version-by-version process for later targets. New upstream releases do not
advance the active target automatically or excuse unfinished v0.4.0 behavior.
The baseline version is a compatibility target, not a claim that the current
i386 prototype has already reached it.

### Change policy

- Port and reuse the reference implementation wherever possible. Preserve
  subsystem boundaries, algorithms, protocols, persistent formats, APIs,
  applications, artwork and interaction behavior when they do not depend on
  the architecture. Prefer localized adaptations over replacement systems.
- Restrict necessary differences to concrete architectural or hardware needs:
  boot handoff, protected-mode CPU state, pointer width, calling conventions,
  ELF32 structures/relocations and the 32-bit memory layout. Document each
  unavoidable difference and its reason; do not use the port to redesign the
  desktop or unrelated internals.
- Treat `sh32`, `desktop32`, static process backing and the static Doom build as
  temporary bring-up steps. Their tests establish progress, not final parity.
  Restore the reference shell, desktop, process/memory facilities and shared
  library integration as their dependencies become available.
- Treat retained generic kernel, POSIX, SDK, application, port and vendor
  sources as the implementation to port. Their presence alone does not mean
  they are linked or usable on i386. Existing design documents describe the
  reference system unless they explicitly record i386 support.

### Reaching a parity target

Assess the reference version feature by feature, recording what is working,
what is still missing and any justified architectural differences. Completion
requires evidence for all applicable reference capabilities, including:

- The same graphical session, applications, assets, layout and interactions,
  checked with comparable screenshots and input scenarios.
- Equivalent shell, process, filesystem, SDK, library and application behavior,
  including external programs and persistent data across rebuilds.
- The reference internal subsystem model, adapted to the CPU/ABI rather than
  replaced by permanent prototype shortcuts, with its relevant regressions.
- The reference device and capacity features on supported hardware or suitable
  emulation. A missing capability such as SMP or virtio remains a parity gap
  even when the first physical target cannot exercise it.

Record remaining gaps explicitly; passing the current minimal smoke is not
sufficient to close v0.4.0. Move to v0.4.1 only after the v0.4.0 parity review
has established that the target is met. Architecture-dependent differences
must be accounted for in that review rather than silently dropping features.

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

These milestones order the work needed for v0.4.0 parity; each is an
intermediate dependency, not a smaller substitute for the final system.
Keep each milestone bootable and add an end-to-end regression before adding
the next dependency. Adapt the corresponding v0.4.0 implementation with minimal
changes, and record implementation/verification evidence as work lands.

1. **Fault containment and memory foundations.** Terminate a faulting user
   process and reap it without killing the shell or siblings. Port the reference
   allocator, allocation failure handling and user-page protection to replace
   static image reservations. Keep supervisor mappings protected on CPUs without NX.
2. **File descriptors and the POSIX base.** Port per-process descriptor tables,
   regular-file read/write/seek/stat/readdir, console ownership, pipes and
   descriptor inheritance. Bring up the reference shell/BusyBox incrementally,
   then restore its v0.4.0 behavior with process cleanup and persistence tests.
3. **Memory objects and ELF32 shared libraries.** Port section objects and
   views, heap growth and ELF32 relocations/loader semantics. Check 32-bit
   layouts, overflow, ownership and failure cleanup. Adapt the existing loader
   design to ELF32 structures and relocations.
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
   Port the reference SMP and optional virtio support after the single-CPU base
   is reliable; they remain part of the parity assessment.

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
