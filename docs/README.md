# SavanXP documentation

Design documents, decisions, and specifications. `README.md` at the repository
root covers how to build and run; this directory explains how the system is put
together and why.

Anything that has to stay true for the future of the OS lives here, not in a
changelog entry. See the rules in [`AGENTS.md`](../AGENTS.md).

This branch pursues v0.4.0 parity on i386 with minimal visual and internal
changes, then advances to v0.4.1. Start with [I386_PORT.md](I386_PORT.md) for
the pinned baseline, completion criteria, current support and ordered roadmap. Other
subsystem documents describe the original system unless they explicitly record
i386 support; retained code is not necessarily part of the active build.

## Building

- [`BUILD_CMAKE.md`](BUILD_CMAKE.md) — the native Bash/CMake build graph, tools,
  targets, external applications, and persistent-image rules.
- [`BUILD_LINUX.md`](BUILD_LINUX.md) — host packages, QEMU setup, official
  ports, smoke tests, and KVM configuration.

## Architecture

- [`I386_PORT.md`](I386_PORT.md) — version parity targets and minimal-change policy,
  completion criteria, current i386 baseline, milestones and verification.
- [`I386_PORT_STUDY.md`](I386_PORT_STUDY.md) — evolving technical study of the
  port: problems, mechanisms, decisions, evidence and remaining parity gaps.

- [`SYSTEM_LAYERING.md`](SYSTEM_LAYERING.md) — the language layering: native
  POSIX SDK applications and the optional managed-app experiment.
- [`WM_SUBSYSTEM.md`](WM_SUBSYSTEM.md) — the window-manager subsystem, its
  client protocol, and compositor model.
- [`OWNED_WINDOWS.md`](OWNED_WINDOWS.md) — ownership rules for dialogs and
  other windows created by an application.
- [`SXGFX_ROADMAP.md`](SXGFX_ROADMAP.md) — hardening of the 2D rasterization
  layer and its planned role in the desktop stack.
- [`SXGL_ROADMAP.md`](SXGL_ROADMAP.md) — the shape and boundaries of a future
  3D API.
- [`SMP_ROADMAP.md`](SMP_ROADMAP.md) — multi-core startup, scheduler locking,
  and TLB shootdown work.
- [`REAL_HARDWARE.md`](REAL_HARDWARE.md) — the i386 porting route through a
  minimal graphical desktop in QEMU, its verification commands, and the
  remaining hardware gaps for a Compaq Evo N1020v.
- [`I386_PROCESSES.md`](I386_PROCESSES.md) — private i386 process contexts, PIT
  preemption, spawn/wait/exit semantics and the end-to-end regression.
- [`KERNEL_SECURITY.md`](KERNEL_SECURITY.md) — the kernel trust boundary and
  audit record.
- [`SECTIONS.md`](SECTIONS.md) — section objects and views: the grant model,
  execution as a view permission, W^X, the view and section budgets, and where
  shared library images belong.
- [`SHARED_LIBRARIES.md`](SHARED_LIBRARIES.md) — the shared-library subsystem: what
  is built and measured, the decisions and the bugs behind them, and the remaining
  work as ordered phases with the dependencies between them.
- [`NETWORKING.md`](NETWORKING.md) — TCP guarantees, retransmission, windows,
  and fault injection.
- [`SXFS_CHECK.md`](SXFS_CHECK.md) — the `fscheck` consistency report: what
  mounting does not validate, what a leak looks like from the guest, and why
  there is no repair yet.
- [`VIRTIO.md`](VIRTIO.md) — the virtio-pci transport and its MMIO rules.
- [`GRAPHICS_PERF.md`](GRAPHICS_PERF.md) — graphics measurements and the
  interpretation of the performance tables.
- [`TIME.md`](TIME.md) — kernel clocks, virtualization, and diagnosis.
- [`MEDIA_PLAYER.md`](MEDIA_PLAYER.md) — Media Player, the optional FFmpeg
  backend, playback timing, and the synchronization selftest.
- [`AUDIO_DAEMON.md`](AUDIO_DAEMON.md) — `audiod`, the mixing daemon: UDP
  loopback transport, the slice protocol, and who holds `/dev/audio0`.
- [`MIDI.md`](MIDI.md) — `libsxmidi`, the General MIDI synthesizer: the SMF
  parser, the built-in instrument bank, and how the Doom port plays music
  through it.
- [`SXMEDIA.md`](SXMEDIA.md) — **withdrawn.** What the multimedia layer was, what
  building it taught, and what has to exist before trying again: a way for one
  program to use a codec library it was not built with.
- [`SYSTEM_MONITORING.md`](SYSTEM_MONITORING.md) — process, memory, and system
  metrics exposed by the kernel.

## Formats

- [`SXE_FORMAT.md`](SXE_FORMAT.md) — the executable format, resource sections,
  generator, stamping, and runtime rules.
- [`SXFS_ROADMAP.md`](SXFS_ROADMAP.md) — the SxFS work left after format v2, the
  measured baseline to compare against, and which of it can land without moving
  the on-disk format.

## Third-party code

- [`THIRD_PARTY_ADOPTION.md`](THIRD_PARTY_ADOPTION.md) — the policy for adopting
  external code.
- [`THIRD_PARTY_PROVENANCE.md`](THIRD_PARTY_PROVENANCE.md) — origin, pinned
  version, and verified license for distributed third-party components.
