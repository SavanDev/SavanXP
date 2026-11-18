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

Validation follows the same version sequence. The v0.4.0 and v0.4.1 targets
are assessed in emulation; physical-machine bring-up and validation begin
with the v0.4.2 target. Laptop-specific BIOS/E820, ATA, framebuffer, peripheral
and power work is deferred to that stage, not an extra requirement for closing
an earlier target. This does not defer reference features that can be exercised
in emulation, including SMP and virtio.

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
- The reference device and capacity features in suitable emulation for
  v0.4.0/v0.4.1, with physical-machine validation added at v0.4.2. A missing
  capability such as SMP or virtio remains a parity gap even when the eventual
  physical target cannot exercise it.

Record remaining gaps explicitly; passing the current minimal smoke is not
sufficient to close v0.4.0. Move to v0.4.1 only after the v0.4.0 parity review
has established that the target is met. Architecture-dependent differences
must be accounted for in that review rather than silently dropping features.

## Working baseline

The graph links the i386 boot/CPU, paging, process, console, input and syscall
implementation with the shared ATA, block, partition, filesystem, SxFS, VFS and
runtime modules. `cmake/userland-i386.cmake` builds the SDK runtime and static
the reference graphical `init`, compositor/WM and applications, shared graphics
libraries, `sh`/BusyBox and core tools, plus optional diagnostics. `init32`,
`sh32` and `desktop32` remain explicit diagnostic programs.
The default disk is `build/disk.img`, the ISO is `build/SavanXP.iso`, and the
CMake cache is `build/linux-i386`. See [BUILD_CMAKE.md](BUILD_CMAKE.md).

Private process maps/stacks, eager x87/SSE state, PIT preemption and spawn/wait/
exit are implemented. Sixty-four process slots use allocator-backed 4 KiB user pages
below the supervisor direct map; user faults terminate only their
process. ELF write permissions, unmapped holes and a stack guard are enforced;
supervisor mappings remain protected without NX. The reference descriptor model,
regular-file/directory operations, byte pipes, canonical TTY, polling and
`exec` are implemented; [I386_POSIX.md](I386_POSIX.md) defines their scope.
[I386_PROCESSES.md](I386_PROCESSES.md)
defines the exact current guarantees. Section views, growing heaps, demand-grown stacks and the embedded ELF32
library loader are usable; [I386_MEMORY.md](I386_MEMORY.md) defines their scope.
The reference window manager, compositor and SxGUI run on i386;
[I386_GRAPHICS.md](I386_GRAPHICS.md) defines their ownership, input and verification contracts.
[I386_SERVICES.md](I386_SERVICES.md) covers legacy device services, external
applications, optional LiveCD boot and their completed milestone 5 regressions.

## Ordered next milestones

These milestones order the work needed for v0.4.0 parity; each is an
intermediate dependency, not a smaller substitute for the final system.
Keep each milestone bootable and add an end-to-end regression before adding
the next dependency. Adapt the corresponding v0.4.0 implementation with minimal
changes, and record implementation/verification evidence as work lands.

1. **Fault containment and memory foundations — completed.** User exceptions
   terminate and reap their process without killing the shell or siblings.
   The reference physical allocator backs ELF32 pages and page tables; failed
   loads roll back, exit reclaims pages, and 4 KiB permissions/guards protect
   user and supervisor memory without NX. `./build.sh smoke memory` exercises
   partial-load `ENOMEM` on 32 MiB. [Mechanism and evidence](I386_PORT_STUDY.md#7-fault-containment-and-allocator-backed-user-pages).
   Milestone 3 extends the original virtual window and restores section
   objects, heap growth and stack-demand behavior; milestone 4 raises the
   original six-slot process bound to the reference 64. This closes the milestone, not v0.4.0 parity.
2. **File descriptors and the POSIX base — completed.** Per-process descriptor
   tables and shared I/O objects support files/directories, canonical console
   input, byte pipes, standard-channel inheritance, polling, `exec` and process
   cleanup. The reference console `sh` and all eleven v0.4.0 BusyBox applets run
   as static ELF32, including pipelines and redirection. `smoke posix` checks
   behavior and failed-pipeline recovery; `tools/verify_posix_persistence.py`
   checks guest writes across a normal rebuild and a cold boot of an isolated disk.
   Milestone 4 restores the reference process bound and event/timer/semaphore
   waits; full signal parity remains outstanding. Milestone 6 restores idle processes
   and their CPU accounting. [Contracts and limits](I386_POSIX.md).
3. **Memory objects and ELF32 shared libraries — completed.**
   Reference section objects/views, growing heaps, private/shared fork and
   demand-grown stacks support ELF32 PIE and the embedded dependency loader.
   `smoke shared` and `smoke shared-memory` check native layouts/relocations,
   ownership, budgets and partial-load/ENOMEM cleanup. Full smoke, host tests and
   POSIX/Doom persistence checks passed. [Mechanism and limits](I386_MEMORY.md).

   **SDK prerequisite 3.1 — completed.** Native limits,
   64-bit conversions and integer formatted I/O now follow the i386 data model.
   `sdkint32`/`sdkintpie` exercise static and shared runtime calls in the shared
   smokes. [SDK contract and tests](I386_SDK.md).
4. **Graphics stack and full desktop — completed.** The default boot runs the
   reference init, compositor/WM, taskbar, Program Manager and applications as
   ELF32, using shared SxGFX/Gfx2D/SxGUI libraries. Surface ownership, waitable
   events, input draining/wheel packets and 64 process slots support the unchanged
   rendering/client code. `windowd-smoke` covers capacity, reconnect and fullscreen;
   `sxgui-smoke` checks editor/taskbar glyphs and input. `desktop32` remains an
   explicit diagnostic. [Contracts and limits](I386_GRAPHICS.md).
5. **Device services and applications — completed.** Reference PCI/CPU discovery,
   CMOS clock, AC'97 audio, RTL8139 networking, ACPI power and legacy device nodes
   run on i386. ATA/MBR/GPT and the optional LiveCD ramdisk have guest regressions;
   the existing PS/2 path supports the restored applications. FFmpeg/Media Player,
   Ccleste, shared-MIDI Doom and optional native Haxe clients pass runtime checks,
   alongside memory, graphics and persistence gates. [Contracts and evidence](I386_SERVICES.md).
   Optional virtio backends remain in milestone 6; physical-machine coverage
   begins with the v0.4.2 target.
6. **Capacity and remaining reference parity in emulation — in progress.** Port the
   reference SMP and optional virtio support, and assess performance, power
   mechanisms and the remaining internal/visual gaps against v0.4.0 in QEMU.
   Complete the feature-by-feature parity review before advancing to v0.4.1.
   Physical-machine bring-up is reserved for the v0.4.2 target.

   **6.1 — Virtio integration completed.** The reference PCI transport, block, network,
   sound, GPU, tablet and keyboard drivers are linked on i386. The optional
   `--virtio` profile retains the same desktop and applications;
   [I386_VIRTIO.md](I386_VIRTIO.md) defines its gates and architecture boundary.
   **6.2 — SMP and interrupt infrastructure in progress.** AP startup, private
   TSS/stacks, targeted IPIs and the GPU MSI-X boundary are verified in QEMU.
   User processes now run on APs under the reference global lock, with local
   timers, deferred remote termination and lazy kernel-TLB synchronization.
   Per-CPU ring-3 idle processes, dynamic init ownership and idle/busy accounting
   now follow the reference. Retained kernel waits are timer-preemptible and
   resume on their original CPU. Same-CPU syscall-return wakeup handoff now
   follows the reference. The port now uses the reference MADT/IOAPIC router
   for PS/2 and the ACPI SCI, with a PIC fallback. The vendored AML interpreter
   resolves root-bus PCI INTx through `_PRT` and link `_CRS`. The BSP timer and
   the remaining internal and visual parity review are still open.
   [Contracts and evidence](I386_SMP.md).
   The remaining internal/visual parity review follows. These intermediate
   results do not complete milestone 6 or the v0.4.0 target.

The detailed laptop analysis and earlier completed milestones are in
[REAL_HARDWARE.md](REAL_HARDWARE.md); its physical-machine work is preparation
for the v0.4.2 target. The list above is the current branch plan, not a claim
that those later subsystems have been ported.

## Migration and verification rules

Do not reset a valid disk when changing architectures. Add the new internal
ELF32 programs to the same SxFS volume and retain unrelated external binaries,
libraries, games, configs and saves. Retained ELF64 files fail ELF32 validation;
rebuild individual apps rather than erasing the image. Earlier experimental
`build/i386/disk-i386.img` stays untouched and is not imported automatically.

Run `./build.sh build`, `./build.sh smoke`, `./build.sh smoke memory`,
`./build.sh smoke shared-memory`, `./build.sh test`, and
`./tools/verify_doom_persistence.sh` for build/persistence changes. Run
`python3 tools/verify_posix_persistence.py` for changes to guest file I/O,
descriptor ownership or the POSIX base. The default
smoke checks the i386 shell, memory/libraries, preemption and lifecycle before
booting the real session for SxGUI glyph/input assertions. Console checks use an
explicit disposable `console-init` ISO; normal boot starts graphical init.
Unsupported scenarios fail explicitly until their subsystem is ported. A
successful loader-only smoke is not evidence of drawing.
