# Porting a DRM driver from OpenBSD

> **Status: not planned (2026-11-17).** Recorded so the path, its price and
> its order are known if a need for it appears. The research is in
> [NATIVE_GRAPHICS.md](../../hardware/NATIVE_GRAPHICS.md); the path that is
> planned is [NATIVE_DISPLAY_ROADMAP.md](NATIVE_DISPLAY_ROADMAP.md).

The other way to native graphics: bring Linux's DRM drivers in the form
OpenBSD carries them, on a layer that gives them the Linux kernel interfaces
they are written against. It is two projects, the layer and the driver, and
the layer is the larger and comes first.

## When to start

Only when both hold:

- **A goal that native drivers cannot reach**: hardware 3D on metal, through
  Mesa, or display features on a family where native mode setting is not
  realistic (Vega and RDNA graphics), such as several monitors or hotplug.
- **The kernel prerequisites exist for their own reasons** (DP0 below), or a
  decision is taken to build them for this.

A sharper desktop on today's machines is not such a goal: level 1 of
[NATIVE_DISPLAY_ROADMAP.md](NATIVE_DISPLAY_ROADMAP.md) gives tear-free flips
and a hardware cursor without any of this.

## Decisions, if it starts

- **The source is OpenBSD's `sys/dev/pci/drm`**, pinned by commit under
  `vendor/openbsd/drm/` with its `OPENBSD_COMMIT`, `SHA256SUMS` and `LICENSE`,
  as `vendor/openbsd/azalia/` is. Not FreeBSD's `drm-kmod`, which lives
  outside base on a LinuxKPI tied to FreeBSD's kernel; not NetBSD's `drm2`,
  an older import with atomic mode setting turned off in `amdgpu`.
- **The layer is SavanXP's own.** OpenBSD's `include/linux/` headers and
  `drm_linux.c` are the list of what is needed and the reference for its
  semantics; they map onto uvm, `bus_dma`, taskq and `timeout(9)`, so they are
  rewritten, not compiled. Whatever carries over is a selective port (ISC).
- **The driver files are adopted unmodified where possible**, with OpenBSD's
  patches, so following a new OpenBSD drop stays a merge. Each file's notice
  is checked before it enters the tree.
- **`i915` first, `amdgpu` last.** `i915` is about a quarter of the size of
  `amdgpu`, needs no firmware for display on the targets' generations, is
  documented by Intel, and serves three of the targets. `amdgpu` comes only
  for the family of a machine at hand, compiled down to it.
- **Compiled as C.** The kernel build already compiles C (uACPI, FatFs).
- **`fb_gpu` keeps the boot path.** The DRM driver takes the display over
  once the root is mounted, since its firmware files live there, and gives it
  back to the firmware framebuffer on failure.

## DP0 — the kernel prerequisites

Each is useful beyond DRM, and each belongs to an existing plan rather than
to this one:

| Need | Today | Where it belongs |
| --- | --- | --- |
| Worker contexts that may sleep; workqueues, delayed work, timers | None; uACPI's deferred work runs synchronously | [SMP_ROADMAP.md](SMP_ROADMAP.md) phase 5, or kernel worker threads of their own |
| Sleeping locks and waits with timeouts (`mutex`, `ww_mutex`, `rwsem`, completions, RCU) | One global lock; no sleeping on the system-call stack | The same |
| MSI and MSI-X for any driver | Only inside `virtio_pci` | [DRIVER_FRAMEWORK_ROADMAP.md](DRIVER_FRAMEWORK_ROADMAP.md#interrupts-and-polling) |
| Coherent DMA, scatter-gather lists | The physical page allocator | D4 in [DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md) |
| Write-combining mappings with a known PAT state | Not audited | FB2 in [FRAMEBUFFER_ROADMAP.md](FRAMEBUFFER_ROADMAP.md) |
| Firmware files | None | F4 in [DRIVER_FRAMEWORK_ROADMAP.md](DRIVER_FRAMEWORK_ROADMAP.md#f4--firmware-files-and-the-usb-client-api) |
| Floating point in the kernel (`amdgpu` on DCN only) | General registers only | A state-management design of its own |
| Buffer objects mapped into processes and filled on faults | Sections and views; no driver supplies pages | [SECTIONS.md](../../kernel/SECTIONS.md), an extension to design |
| An I2C layer for DDC | None | With the first driver that reads EDID |

**Size:** XL. **Done when:** each item has landed in its own plan, with the
consumer that plan names.

## Phases

| Phase | Result | Size | Prerequisites |
| --- | --- | --- | --- |
| DP0 | The kernel prerequisites above | XL | Their own plans |
| DP1 | The Linux interface layer: the subset of `include/linux/` that the DRM core, TTM, the scheduler and `i915` use, each interface with host tests | XL | DP0 |
| DP2 | The DRM core, the KMS helpers, GEM and fbdev emulation compiled; a trivial driver over the firmware framebuffer proves the layer | L | DP1 |
| DP3 | `i915` on Haswell (the [Core i5-4440](../../hardware/CORE_I5_4440.md)): mode setting through a `display::Backend` adapter (`set_mode`, cursor, present events), take-over and hand-back | L–XL | DP2 |
| DP4 | Buffer objects and command submission reachable from userland | L | DP3 |
| DP5 | Mesa's `crocus` and `libdrm` in userland, behind SxGL or as GL of their own | XL | DP4 |
| DP6 | `amdgpu` for one family (Vega 10): Display Core with DCE 12 only, the SMU, sixteen firmware files through the security processor | XL | DP2, F4 |
| — | Following OpenBSD: 6.6.12, 6.12.12 and 6.18.16 arrived in 26 months | Ongoing | DP2 |

**First increment, if it starts:** DP1's inventory: which Linux interfaces
the DRM core, TTM, the scheduler and `i915` call, with a count per header,
taken from OpenBSD's tree at a pinned commit. It writes no code and turns
"the layer is XL" into a number.

## What makes it hard to verify

- QEMU emulates neither Intel nor AMD GPUs. CI-style runs need VFIO
  passthrough of a second GPU in a Linux host; otherwise every check is on
  metal.
- Once the driver owns the display, a hang is a black screen. A serial port
  or a working network driver (HW-1) has to be there first.

## Open questions

- **How a driver that sleeps fits SDF.** SDF's callbacks never block, and
  blocking is a capability the kernel host lacks until it has worker
  contexts that may sleep; display is not planned as an IUD, because it is on
  the boot path
  ([DRIVER_FRAMEWORK_ROADMAP.md](DRIVER_FRAMEWORK_ROADMAP.md#the-execution-model-serialized-and-non-blocking)).
  A DRM driver either waits for those contexts, or becomes the first display
  driver that is not on the boot path.
- **Which userland interface.** A SavanXP device of its own, or Linux's
  DRM ioctls so that a Mesa built for Linux could run under
  [SSL](../../userland/LINUX.md).
- **Firmware policy.** Where vendor firmware lives in the image, whether the
  ISO carries it, and how its licenses are recorded next to the MIT tree.
