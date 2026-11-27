# VMware SVGA II — a display driver, and 3D after it

> **Status: planned, not started (2026-11-27). Phase 0 (research) is next.**
> This document is the order of work, written before the work, and the
> decisions that shape it.

The VMware SVGA II adapter (PCI `15ad:0405`) is the virtual GPU of VMware
Workstation and ESXi, of VirtualBox as **VMSVGA**, and of QEMU as
`-vga vmware`. On those machines SavanXP draws today on the firmware
framebuffer through `fb_gpu`: one mode, no hardware cursor, no 3D. VirtualBox
has no `virtio-gpu`, so nothing in the tree can do better there.

The work has two halves of very different cost:

- **2D** (phases 1–2): mode setting, presentation through the command FIFO,
  a hardware cursor and fences. A small, well documented device, testable in
  QEMU.
- **3D** (phases 4–8): the SVGA3D protocol as a second hardware backend for
  SxGL, beside virgl ([VIRGL_ROADMAP.md](VIRGL_ROADMAP.md)). QEMU does **not**
  implement SVGA3D (`hw/display/vmware_vga.c` is 2D only), so this half runs
  only on VirtualBox and VMware and needs a test harness that does not exist
  yet (phase 3). It costs more than virgl did: see "Why 3D costs more".

The 3D half does not start until phase 3's gate is passed.

## Decisions

- **One more `display::Driver`, no new API.** `vmsvga` registers in
  `kernel_main` with `display::register_driver()`, at a priority below
  `virtio_gpu` and above `fb_gpu`. It implements the `display::Backend` hooks
  that `virtio_gpu` already gives `compositord` (`set_mode`, `present_region`,
  `set_cursor`, `move_cursor`, `wait_present`, `emergency_console`), so
  `compositord` does not change. A hook the device cannot serve returns false,
  as with `fb_gpu`.
- **A missing or refused device boots exactly as before.** If the PCI ID is
  absent, the version handshake (`SVGA_REG_ID`) fails or the FIFO does not
  come up, `probe()` returns false and `fb_gpu` takes the firmware framebuffer,
  as it does today.
- **The emergency console survives.** A panic after `SVGA_REG_ENABLE` must
  still reach the screen: `emergency_console` either keeps drawing into VRAM
  with a synchronous `UPDATE` or turns SVGA mode off and returns to the legacy
  framebuffer. Phase 1 picks one and says which.
- **3D reuses the `/dev/gpu0` 3D ioctls.** `GPU_IOC_3D_GET_CAPSET` to
  `GPU_IOC_3D_TRANSFER`, the per-pid ownership and the cleanup on exit stay
  as they are. The capset identifies the protocol: SVGA3D gets an id of
  SavanXP's own, outside virtio's capset numbers, and SxGL picks its encoder
  from it. If phase 0 shows the ABI cannot carry what SVGA3D needs, the ABI
  change is written here first.
- **The kernel validates every 3D command buffer.** Unlike virgl, SVGA3D
  commands name host surfaces, contexts and guest memory regions (GMRs or
  MOBs) directly. A command buffer is parsed in the kernel and refused unless
  every id it names belongs to the calling process. Unknown commands are
  refused, not passed through.
- **Synchronous and readback first, as virgl.** Every 3D call is fenced and
  waited for; the frame is read back into the window surface and presented by
  the 2D path. Asynchronous submission or zero-copy only if phase 8 asks.
- **`--vmware` selects the machine.** `run` and `smoke` take `--vmware`,
  which swaps the default display for `-vga vmware`. Without it nothing
  changes.

## Why 3D costs more than virgl

- **Validation.** virgl's host validates the stream and resources are opaque
  ids, so `virtio_gpu` only carries bytes. SVGA3D leaves that to the guest
  kernel; Linux's `vmwgfx_execbuf.c`, which does it, is one of the largest files
  in that driver.
- **Shaders are binary.** virgl takes TGSI as text; SVGA3D takes D3D9
  bytecode (legacy) or VGPU10 bytecode (DX). SxGL's handful of shaders has to
  be assembled by hand in one of those formats.
- **No QEMU.** Every 3D phase is checked on VirtualBox or VMware, outside the
  CI that runs today.

## Phases

0. **Research.** Answer, with sources, before writing driver code:
   - Which 3D protocol each target host speaks: legacy SVGA3D (guest-defined
     surfaces, `SURFACE_DMA`, D3D9 shaders) or guest-backed objects with the
     DX commands (`SVGA3D_CMD_DX_*`, VGPU10 shaders), and on which VirtualBox
     versions and host OSes. Pick the protocol from that, and record what the
     choice leaves out.
   - Which capability bits (`SVGA_REG_CAPABILITIES`, `SVGA_FIFO_CAPABILITIES`,
     the 3D caps) QEMU, VirtualBox and VMware actually report, read from a
     running guest, not from the headers.
   - That VirtualBox still has no `virtio-gpu` in the versions we target, and
     what VMSVGA under VirtualBox does on a mode change and on a host window
     resize.
   - How the firmware framebuffer (GOP or VBE) maps onto BAR1 on each host, so
     the handover from `fb_gpu`'s mode is known before `SVGA_REG_ENABLE`.
   - The licenses of the references: the device headers in Mesa
     (`src/gallium/drivers/svga/include`), Linux's `vmwgfx`, VMware's SVGA
     developer kit. What may be adopted goes to
     [THIRD_PARTY_PROVENANCE.md](../../third-party/THIRD_PARTY_PROVENANCE.md).
   - How to drive VirtualBox unattended for phase 3: `VBoxManage` to create
     and start a headless VM from `build/disk.img` or the ISO, serial output
     to a file, `controlvm … screenshotpng`, power-off.

   The findings go into a hardware document under `docs/hardware/`, listed in
   `docs/README.md`; this roadmap is corrected where they contradict it.
1. **2D bring-up.** PCI probe, version handshake, FIFO setup, mode setting
   through `SVGA_REG_WIDTH/HEIGHT/BITS_PER_PIXEL`, presentation by copying into
   VRAM and `SVGA_CMD_UPDATE`. Boot log: `vmsvga: <version> vram=… fifo=…`. A
   `--vmware` smoke scenario that reaches the desktop, and `shoot.sh` on it.
2. **Cursor, fences, interrupts.** Hardware cursor
   (`SVGA_CMD_DEFINE_ALPHA_CURSOR`, with `compositord`'s software cursor as
   the fallback it already is), FIFO fences behind `wait_present` and the
   present timeline, the IRQ where the device offers `SVGA_CAP_IRQMASK`, and
   reset recovery. The same smoke plus `windowd-smoke` and `taskbar-smoke`
   under `--vmware`.
3. **VirtualBox harness, and the gate.** A tool that boots the image in a
   headless VirtualBox VM and collects serial output and a screenshot, used to
   run phases 1–2 there. The 3D half starts only if phase 0 found a protocol
   the targets implement, and a concrete need for acceleration on VirtualBox
   or VMware is stated here.
4. **3D transport.** 3D caps read and handed out as the capset, guest memory
   regions (GMR or MOB, per phase 0) backed by the caller's sections,
   contexts and surfaces per pid, cleanup on exit and on reset.
5. **The validator.** Command-buffer parsing in the kernel for exactly the
   commands SxGL emits, with host tests that feed it valid buffers and ones
   naming another process's ids, unknown commands and truncated lengths.
6. **A first frame.** A SUN test program clears a render target, reads it
   back and checks the pixels: the `nativetest` assertion, run through the
   phase 3 harness.
7. **SxGL backend.** An SVGA3D encoder beside the virgl one, the shaders in
   the chosen bytecode, then the rest of the subset (lighting, textures,
   blending, alpha test, fog, scissor, culling) against the same reference
   images and tolerance as the virgl backend.
8. **Measure.** `gears` and `nightwood` on software and SVGA3D, figures in
   [SXGL.md](../../desktop/SXGL.md) next to the virgl ones.

## Consumers

`compositord` and the desktop for phases 1–2, unchanged; `gears` (POSIX) and
`nightwood` (SUN) for the 3D half, unchanged too, since the backend is picked
when the SxGL context is created. `nightwood-smoke` is the regression on every
machine it can run on.
