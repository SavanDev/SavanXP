# SavanXP documentation

Design documents, decisions and specifications. `README.md` at the repository
root covers how to build and run; this directory covers how the system is put
together and why.

Anything that has to stay true for the future of the OS lives here, not in a
changelog entry. See the rules in [`AGENTS.md`](../AGENTS.md).

## Building

- [`BUILD_LINUX.md`](BUILD_LINUX.md) — building and running outside Windows:
  PowerShell itself, distribution packages, QEMU display and audio backends,
  virtio devices, KVM.
- [`WINDOWS_BOOTSTRAP.md`](WINDOWS_BOOTSTRAP.md) — what `tools/bootstrap.ps1`
  bakes on Windows and what it deliberately does not: the embedded Python,
  why Visual Studio Build Tools cannot be pinned like the rest of the
  toolchain, and the path-quoting bug a space in the Windows profile name
  used to trigger.

## Architecture

- [`SYSTEM_LAYERING.md`](SYSTEM_LAYERING.md) — **source of truth** for the
  language layering: everything SavanXP ships is C on the POSIX SDK, and the
  managed app layer (Haxe on a VM) is **deferred until after v1.0** — why, what
  it would have to look like if it ever arrives (a selective port, UWP-style,
  beside the native apps and never replacing them), and what that means for work
  done today. If another document contradicts this one, this one wins.
- [`WM_SUBSYSTEM.md`](WM_SUBSYSTEM.md) — extraction of the window manager into
  its own subsystem, following the NT 3.5 model: `windowd` as the WM, the shell
  as client processes, and the WM↔client protocol.
- [`SXGFX_ROADMAP.md`](SXGFX_ROADMAP.md) — the plan for hardening `sxgfx`, the
  2D rasterization layer, into the role `SYSTEM_LAYERING.md` assigns it: the
  GDI32 underneath SXGUI-C.
- [`SXGL_ROADMAP.md`](SXGL_ROADMAP.md) — the shape a 3D API would have to take:
  why OpenGL cannot grow out of SxGFX, where SxGL binds to `windowd` instead,
  and which GL subset is worth implementing. Batch 0 landed (`/bin/gears`);
  SxGL itself is not built.
- [`SMP_ROADMAP.md`](SMP_ROADMAP.md) — the plan for running on more than one
  core: bringing up the APs, per-CPU state, a big kernel lock over the
  scheduler, TLB shootdown, and why splitting that lock should wait for
  threads.

- [`NETWORKING.md`](NETWORKING.md) — what the TCP path guarantees and what it
  deliberately does not: retransmission with backoff, reassembly by extents,
  the advertised window, why the retransmission clock is the poll path, and the
  fault injector that makes any of it testable.

- [`VIRTIO.md`](VIRTIO.md) — what the virtio-pci transport asks of every
  paravirtualized driver: why a 64-bit MMIO field is touched in two 32-bit
  halves, why QEMU forgives an access VirtualBox answers with a guru
  meditation, and how to read that crash back to a line of the kernel.

- [`GRAPHICS_PERF.md`](GRAPHICS_PERF.md) — how the graphics pipeline is
  measured and what it costs: every field of the `windowd-stats` line, which
  load measures the display path and which only measures the harness, the
  scheduler fix that made present ~470x faster, the numbers across backends,
  and why asynchronous present buys nothing on one core.

- [`TIME.md`](TIME.md) — which clock the kernel reads for what: the tick
  counter as CPU accounting and nothing else, why the wall clock cannot be the
  TSC inside a hypervisor (host time vs. virtual time), the ACPI PM timer rule
  and its 32-bit condition, what a runaway clock did to the audio feed, and the
  three lines that diagnose a clock problem.

- [`MEDIA_PLAYER.md`](MEDIA_PLAYER.md) — how Media Player plays a file on one
  thread: the pull-based FFmpeg engine, accurate seek, why the clock is the
  wall clock and not the audio device, the driver latency it compensates and
  why pause, seek and stalls restart the audio stream, what the sync selftest
  proves and what it cannot, and the roadmap (audio position, threads, asm).

- [`SYSTEM_MONITORING.md`](SYSTEM_MONITORING.md) — what the system reports about
  itself and how to read it: the per-process tick and memory counters the kernel
  exports, why a percentage only exists between two samples, why resident memory
  is walked instead of counted, where processor identity comes from, and which
  Task Manager tabs are missing and what each would require first.

## Formats

- [`SXE_FORMAT.md`](SXE_FORMAT.md) — the SXE executable format: icons and
  metadata carried by the binary itself as non-alloc ELF sections. The
  canonical definition is [`include/sxe/sxe_format.h`](../include/sxe/sxe_format.h);
  this document explains the reasoning around it.

## Third-party code

- [`THIRD_PARTY_ADOPTION.md`](THIRD_PARTY_ADOPTION.md) — the policy: which
  categories of adoption exist and what each one requires.
- [`THIRD_PARTY_PROVENANCE.md`](THIRD_PARTY_PROVENANCE.md) — the registry: one
  entry per third-party bit that SavanXP actually distributes, with origin,
  pinned version and verified license.
