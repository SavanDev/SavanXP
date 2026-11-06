# VirGL — a hardware backend for SxGL

> **Status: phases 1–3 done (the kernel side); phase 4 (the SxGL backend) is
> next.** This document is the order of work, written before the work, and the
> decisions that shape it.

SxGL is a software rasterizer ([SXGL.md](../../desktop/SXGL.md)) because the
kernel had no 3D: `virtio_gpu` never negotiated `VIRTIO_GPU_F_VIRGL`
([SXGL_ROADMAP.md](../done/SXGL_ROADMAP.md), "The kernel has no 3D either").
This roadmap adds the kernel side and then a second SxGL backend on top of it.
The software rasterizer stays: it is the path on every machine without virgl
(the firmware framebuffer, `virtio-vga` without GL, real hardware) and the
reference the GPU output is checked against.

## Decisions

- **SxGL encodes the virgl stream itself.** No Mesa. A small encoder in the
  tree emits the Gallium commands virgl understands, and SxGL's fixed-function
  pipeline maps to a handful of TGSI shaders written as text. The GL subset is
  small and documented; the encoder only has to cover what it uses.
- **Readback, not zero-copy.** The application renders into a host resource
  and `TRANSFER_FROM_HOST_3D` copies the result into its window surface, which
  is then presented by the 2D path that exists today. `compositord` does not
  change. The cost is one copy per frame, and it is measured, not assumed.
- **The 3D ioctls live on `/dev/gpu0`, outside the display session.** Today
  only the session owner (`compositord`) may use `/dev/gpu0`. The 3D ioctls are
  exempt: any process may open the device and create a virgl context of its
  own. The context belongs to the process and the kernel destroys it, with
  every resource it owns, when the process exits.
- **Every 3D call is synchronous.** Each command is fenced and the ioctl
  returns when the host has finished it, so a readback has landed when
  `GPU_IOC_3D_TRANSFER` returns. Asynchronous submission comes only if the
  measurements in phase 6 ask for it.
- **Resources are backed by the caller's sections**, as `import_surface` already
  does: the readback lands in memory the application maps, with no copy in the
  kernel.
- **SxGL makes the ioctls itself.** The GPU backend opens `/dev/gpu0`; the
  software rasterizer underneath stays syscall-free, so the host test still
  links it.
- **`--gl` selects the machine.** `run`, `smoke` and `shoot` take `--gl`, which
  implies `--virtio` and swaps `virtio-vga` for `virtio-vga-gl` with a GL
  display (`egl-headless` when headless). Without `--gl` nothing changes and the
  host needs no virglrenderer.

## The kernel interface

Seven ioctls on `/dev/gpu0`, `GPU_IOC_3D_GET_CAPSET` to `GPU_IOC_3D_TRANSFER`,
declared with their structs in `include/abi/savanxp_abi.h`. They skip the
display-session check; ownership is by pid. A context's virtio id is its slot
+ 1 (16 slots); a resource's is `0x10000` + its slot (256 slots), far above the
2D ids. A command buffer is at most `SAVANXP_GPU_3D_MAX_SUBMIT_BYTES` (one
command slot). The wire values the tree sends are in
`include/sxgfx/virgl_protocol.h`, written from the protocol and grown only with
what is used.

A device reset (recovery) destroys every host object. The tables keep the
entries, marked lost: every later call on them fails with `SX_EIO`, the owner
destroys them, and the boot log says `virtio-gpu: reset lost N 3d context(s)`.
Nothing is replayed; the caller falls back to software.

**Known limitation: the wait spins.** A fenced 3D command is waited for the way
2D commands are, spinning with the kernel lock held, for up to 2 s instead of
100 ms. `smp::wait_for_interrupt()` would release the lock, but the driver has
never run two calls at once and is not ready to be re-entered mid-command.
Phase 6 measures what the spin costs before anything changes.

## Phases

1. **Negotiation and capsets.** `virtio_gpu` asks for `VIRTIO_GPU_F_VIRGL`
   when the device offers it, reads the capsets with `GET_CAPSET_INFO` and
   `GET_CAPSET`, keeps the best one (virgl2 over virgl) and reports
   `SAVANXP_GPU_INFO_FLAG_3D`. Boot log: `virtio-gpu: virgl capset=…`. A device
   without virgl boots exactly as before.
2. **3D ioctls.** Context create/destroy, `RESOURCE_CREATE_3D` backed by a
   section, attach to the context, `SUBMIT_3D` of a command buffer copied from
   the caller, `TRANSFER_TO_HOST_3D`/`TRANSFER_FROM_HOST_3D`, a fence to wait
   on, and the capset handed to user space. Per-process ownership and cleanup.
3. **A first frame.** A SUN test program creates a render target, clears it
   through `SUBMIT_3D`, reads it back and checks the pixels; it is the
   `nativetest` assertion and a `--gl` smoke scenario.
4. **SxGL backend: clears and triangles.** The virgl encoder, the shaders for
   flat and smooth colour, vertex buffers, depth. Backend chosen at context
   creation; software when the flag is absent.
5. **SxGL backend: the rest of the subset.** Lighting, textures, blending,
   alpha test, fog, scissor, culling, against the host test's reference images
   with a stated tolerance.
6. **Measure.** `gears` and `nightwood` on both backends, figures recorded in
   [SXGL.md](../../desktop/SXGL.md) next to the software ones.

## Consumers

`gears` (POSIX) and `nightwood` (SUN) are the programs SxGL exists for. The GPU
backend has to arrive under them without a change to either: the backend is
picked when the context is created. `nightwood-smoke` is the regression on
both machines.
