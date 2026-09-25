# SavanXP documentation

Design documents, decisions, and specifications. `README.md` at the repository
root covers how to build and run; this directory explains how the system is put
together and why.

Anything that has to stay true for the future of the OS lives here, not in a
changelog entry. See the rules in [`AGENTS.md`](../AGENTS.md).

## Building

- [`BUILD_CMAKE.md`](BUILD_CMAKE.md) — the native Bash/CMake build graph, tools,
  targets, external applications, and persistent-image rules.
- [`BUILD_LINUX.md`](BUILD_LINUX.md) — host packages, QEMU setup, official
  ports, smoke tests, and KVM configuration.

## Architecture

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
- [`KERNEL_SECURITY.md`](KERNEL_SECURITY.md) — the kernel trust boundary and
  audit record.
- [`NETWORKING.md`](NETWORKING.md) — TCP guarantees, retransmission, windows,
  and fault injection.
- [`VIRTIO.md`](VIRTIO.md) — the virtio-pci transport and its MMIO rules.
- [`GRAPHICS_PERF.md`](GRAPHICS_PERF.md) — graphics measurements and the
  interpretation of the performance tables.
- [`TIME.md`](TIME.md) — kernel clocks, virtualization, and diagnosis.
- [`MEDIA_PLAYER.md`](MEDIA_PLAYER.md) — Media Player, the optional FFmpeg
  backend, playback timing, and the synchronization selftest.
- [`SXMEDIA.md`](SXMEDIA.md) — the proposed multimedia layer: the engine, the
  device sinks, and the backend registry that makes FFmpeg a fallback.
- [`SYSTEM_MONITORING.md`](SYSTEM_MONITORING.md) — process, memory, and system
  metrics exposed by the kernel.

## Formats

- [`SXE_FORMAT.md`](SXE_FORMAT.md) — the executable format, resource sections,
  generator, stamping, and runtime rules.

## Third-party code

- [`THIRD_PARTY_ADOPTION.md`](THIRD_PARTY_ADOPTION.md) — the policy for adopting
  external code.
- [`THIRD_PARTY_PROVENANCE.md`](THIRD_PARTY_PROVENANCE.md) — origin, pinned
  version, and verified license for distributed third-party components.
