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

## Architecture

- [`SYSTEM_LAYERING.md`](SYSTEM_LAYERING.md) — **source of truth** for the
  language layering: what is written in C (platform and system apps) and what
  is written in Haxe on top of the VM (user apps). If another document
  contradicts this one, this one wins.
- [`WM_SUBSYSTEM.md`](WM_SUBSYSTEM.md) — extraction of the window manager into
  its own subsystem, following the NT 3.5 model: `windowd` as the WM, the shell
  as client processes, and the WM↔client protocol.
- [`SXGFX_ROADMAP.md`](SXGFX_ROADMAP.md) — the plan for hardening `sxgfx`, the
  2D rasterization layer, into the role `SYSTEM_LAYERING.md` assigns it: the
  GDI32 underneath SXGUI-C.
- [`SMP_ROADMAP.md`](SMP_ROADMAP.md) — the plan for running on more than one
  core: bringing up the APs, per-CPU state, a big kernel lock over the
  scheduler, TLB shootdown, and why splitting that lock should wait for
  threads.

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
