# SavanXP documentation

Design documents, decisions, and specifications. `README.md` at the repository
root covers how to build and run; this directory explains how the system is put
together and why.

Anything that has to stay true for the future of the OS lives here, not in a
changelog entry. See the rules in [`AGENTS.md`](../AGENTS.md).

## Build — `build/`

- [`BUILD_CMAKE.md`](build/BUILD_CMAKE.md) — the native Bash/CMake build graph, tools,
  targets, external applications, and persistent-image rules.
- [`BUILD_LINUX.md`](build/BUILD_LINUX.md) — host packages, QEMU setup, official
  ports, smoke tests, KVM configuration, and the GitHub Actions CI.

## Kernel — `kernel/`

- [`KERNEL_NATIVE_API.md`](kernel/KERNEL_NATIVE_API.md) — the NT-style kernel API:
  the native dispatcher boundary, the slice plan, and what stays invariant.
- [`KERNEL_SECURITY.md`](kernel/KERNEL_SECURITY.md) — the kernel trust boundary and
  audit record.
- [`SECTIONS.md`](kernel/SECTIONS.md) — section objects and views: the grant model,
  execution as a view permission, W^X, the view and section budgets, and where
  shared library images belong.
- [`SHARED_LIBRARIES.md`](kernel/SHARED_LIBRARIES.md) — the shared-library subsystem: what
  is built and measured, the decisions and the bugs behind them, and the remaining
  work as ordered phases with the dependencies between them.
- [`SMP_ROADMAP.md`](kernel/SMP_ROADMAP.md) — multi-core startup, scheduler locking,
  and TLB shootdown work.
- [`VIRTIO.md`](kernel/VIRTIO.md) — the virtio-pci transport and its MMIO rules.
- [`REAL_HARDWARE.md`](kernel/REAL_HARDWARE.md) — what stops SavanXP from booting real
  2003-era hardware: the 64-bit-only blockers, the driver gaps QEMU hides, and
  the routes for reaching a Compaq Evo N1020v. The Compaq-specific work (the
  i386 port) belongs to the `legacy` branch.
- [`TIME.md`](kernel/TIME.md) — kernel clocks, virtualization, and diagnosis; plus the
  calendar date: the CMOS RTC, manual setting (`date`, `SET_REALTIME`), network
  sync (`ntpsync`, SNTP), and the taskbar clock with its calendar popup.

## Userland — `userland/`

- [`SYSTEM_LAYERING.md`](userland/SYSTEM_LAYERING.md) — the layering: the platform,
  apps on the POSIX SDK, and when an app may be a SUN program instead.
- [`SUN.md`](userland/SUN.md) — SUN, the Savan Userland Native API: what it is
  next to Sx and POSIX, its pieces, the rules that grow it into our own Win32,
  the Win32 map, and the slices the Command Prompt drives.
- [`NATIVE_APPS.md`](userland/NATIVE_APPS.md) — how to write a SUN program:
  headers, connecting a window, manifests, and the worked examples.
- [`NATIVE_HEAP.md`](userland/NATIVE_HEAP.md) — `malloc` for native programs: the
  allocator's replaceable page source, what a native process has today, the plan.
- [`SYSTEM_MONITORING.md`](userland/SYSTEM_MONITORING.md) — process, memory, and system
  metrics exposed by the kernel.
- [`SXE_FORMAT.md`](userland/SXE_FORMAT.md) — the executable format, resource sections,
  generator, stamping, and runtime rules.

## Desktop and graphics — `desktop/`

- [`DESKTOP_ARCHITECTURE.md`](desktop/DESKTOP_ARCHITECTURE.md) — the target split
  between `compositord`, `windowd` and `desktop` (NT model), window roles,
  global hotkeys, the Task List removal, the move onto the native API, and the
  phases to get there.
- [`WM_SUBSYSTEM.md`](desktop/WM_SUBSYSTEM.md) — the window-manager subsystem, its
  client protocol, and compositor model.
- [`OWNED_WINDOWS.md`](desktop/OWNED_WINDOWS.md) — ownership rules for dialogs and
  other windows created by an application.
- [`SXGFX_ROADMAP.md`](desktop/SXGFX_ROADMAP.md) — hardening of the 2D rasterization
  layer and its planned role in the desktop stack.
- [`SXGL_ROADMAP.md`](desktop/SXGL_ROADMAP.md) — the shape and boundaries of a future
  3D API.
- [`GRAPHICS_PERF.md`](desktop/GRAPHICS_PERF.md) — graphics measurements and the
  interpretation of the performance tables.
- [`UI_FONTS.md`](desktop/UI_FONTS.md) — the UI, caption and console faces, the
  rule that every UI font is 1-bit, and how to change a face.
- [`CONSOLE.md`](desktop/CONSOLE.md) — the Command Prompt: the console host (SUN)
  and `sh` as two programs, their handles and control channel, line editing,
  Ctrl+C without signals, and how the smokes open programs through it.
- [`PAINT.md`](desktop/PAINT.md) — Paint, the Accessories sketchbook: toolbox
  layout without widgets, the 8 tools, 8-deep undo, BMP/PNG files, and the
  selftest plus visual scenario.
- [`WEBVIEWER.md`](desktop/WEBVIEWER.md) — Web Viewer, the in-tree browser prototype:
  its rendering contract, the CSS subset, and what each milestone leaves out.

## Media — `media/`

- [`MEDIA_PLAYER.md`](media/MEDIA_PLAYER.md) — Media Player, the optional FFmpeg
  backend, playback timing, and the synchronization selftest.
- [`AUDIO_DAEMON.md`](media/AUDIO_DAEMON.md) — `audiod`, the mixing daemon: UDP
  loopback transport, the slice protocol, and who holds `/dev/audio0`.
- [`MIDI.md`](media/MIDI.md) — `libsxmidi`, the General MIDI synthesizer: the SMF
  parser, the built-in instrument bank, and how the Doom port plays music
  through it.
- [`SXMEDIA.md`](media/SXMEDIA.md) — **withdrawn.** What the multimedia layer was, what
  building it taught, and what has to exist before trying again: a way for one
  program to use a codec library it was not built with.

## Filesystem — `fs/`

- [`SXFS_CHECK.md`](fs/SXFS_CHECK.md) — the `fscheck` consistency report: what
  mounting does not validate, what a leak looks like from the guest, and why
  there is no repair yet.
- [`SXFS_JOURNAL.md`](fs/SXFS_JOURNAL.md) — the sparse metadata journal: format,
  the write order that makes a commit crash-safe, and the snapshot invariant that
  tells the driver which sectors changed.
- [`SXFS_ROADMAP.md`](fs/SXFS_ROADMAP.md) — the SxFS work left after format v2, the
  measured baseline to compare against, and which of it can land without moving
  the on-disk format.

## Networking — `net/`

- [`NETWORKING.md`](net/NETWORKING.md) — TCP guarantees, retransmission, windows,
  fault injection, and the minimal DNS client.

## Ports — `ports/`

- [`XASH3D_ROADMAP.md`](ports/XASH3D_ROADMAP.md) — porting Xash3D FWGS: what the
  engine needs from the platform, and the order that makes it pay for `dlopen` and
  SxGL without pulling in threads or SDL.

## Third-party code — `third-party/`

- [`THIRD_PARTY_ADOPTION.md`](third-party/THIRD_PARTY_ADOPTION.md) — the policy for adopting
  external code.
- [`THIRD_PARTY_PROVENANCE.md`](third-party/THIRD_PARTY_PROVENANCE.md) — origin, pinned
  version, and verified license for distributed third-party components.
