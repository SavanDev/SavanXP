# SxGL — the 3D layer goes beside SxGFX, never inside it

> **Status: nothing is built, and nothing should be built until a port asks for
> it.** This document is not a plan in progress: it is the *shape* a 3D API
> would have to take here, written down while the reasoning is fresh, so that
> the day someone wants OpenGL the answer starts from a design instead of from
> a blank page — and so that nobody tries to grow it out of SxGFX in the
> meantime.
>
> The question that produced it: *if SxGFX is the closest thing to GDI, do we
> need a new API to implement OpenGL?* The answer is **yes**, and
> [SXGFX_ROADMAP.md](SXGFX_ROADMAP.md) had already committed to it for the
> DirectX-shaped version of the same question — "*if that model is ever wanted,
> it goes in a new layer beside, not inside, SxGFX*". This document is that
> sentence worked out.

## The answer in one line

**SxGL is a sibling of SxGFX, not a layer on top of it.** It consumes the
window's surface and the present path, and it gains SxGFX exactly zero new
functions.

## Why SxGFX cannot be the base

Not a matter of missing entry points. The model is incompatible in three places,
and each one has evidence in the tree. None of them is about the toolchain: the
in-tree userland has floating point now, and that changed nothing here — which
is the point. These are reasons about the model.

### 1. SxGFX has no state, and OpenGL *is* state

`sx_painter` carries a bitmap, a clip stack of 16, an origin stack and a font id
([gfx2d.h:110](../subsystems/posix/sdk/v1/include/savanxp/gfx2d.h:110)). Colour
travels as a loose `uint32_t` per call. That is immediate mode on purpose: its
consumer is SXGUI-C, and what a widget needs is `fill_rect`.

OpenGL is a state machine with a context — bound texture, matrix stacks, depth
func, blend func, cull mode, current colour and normal. Putting `glEnable` in
the painter does not extend SxGFX; it replaces the thing SxGFX is.

### 2. Everything 3D is missing, not merely incomplete

No depth buffer. No textures with filtering, wrap or perspective correction. No
matrix stack. And the blend is nailed to `SRC_OVER`
([gfx2d.c:3](../subsystems/posix/sdk/v1/runtime/gfx2d.c:3)) — the raster ops
that landed in batch 3 (`SX_ROP_XOR` and company) are GDI's ROPs, which is a
different thing from `glBlendFunc`.

### 3. The kernel has no 3D either

`savanxp_gpu_ioctl` ([syscall.h:371](../subsystems/posix/sdk/v1/include/savanxp/syscall.h:371))
is 21 ioctls of strictly 2D work: import a section as a surface, present dirty
rectangles, move a cursor plane, read a present timeline. `virtio_gpu` never
negotiates `VIRTIO_GPU_F_VIRGL` and there is no `CTX_CREATE` anywhere in the
tree.

**Consequence, and it is the one that shapes everything below: any OpenGL here
is a software rasterizer, 100% of it.** There is no hardware path to fall back
to and none to fall forward to.

## Where it goes

The Windows analogy is exact, and it is the one to hold on to:

| Role | SavanXP | Windows |
|---|---|---|
| 2D rasterization | **SxGFX** | GDI32 |
| Control toolkit | **SXGUI-C** | USER32 / comctl32 |
| 3D rasterization | **SxGL** *(does not exist)* | `opengl32.dll` |
| Context ↔ window binding | part of SxGL | WGL |
| Presentation | `gfx_present_region` | `SwapBuffers` |

`opengl32.dll` is **not** built on GDI. It asks the `HDC` for a pixel format,
writes its own colour buffer, and presents. The relationship here is the same
one, and it extends
[SYSTEM_LAYERING.md](SYSTEM_LAYERING.md#analogy-to-fix-the-mental-model)'s table
with one row rather than changing any of its existing ones.

## The seam: what SxGL would actually consume

This is the part worth getting right, because it is where a wrong design would
start duplicating the platform.

A GUI client already opens its surface with `gfx_open`, which maps the section
`windowd` handed it, **creates a private section of its own**, and hands the app
a plain `uint32_t*` to draw into
([gfx_impl.inc:272](../subsystems/posix/sdk/v1/runtime/gfx_impl.inc:272)).
`gfx_present_region` then waits for the compositor to be idle, copies the dirty
region private → shared, and submits a dirty-rect batch
([gfx_impl.inc:498](../subsystems/posix/sdk/v1/runtime/gfx_impl.inc:498)).

So the whole binding is three facts:

- **The GL colour buffer is that private buffer.** No allocation of its own, no
  copy, and `sx_bitmap_wrap`
  ([gfx2d.h:191](../subsystems/posix/sdk/v1/include/savanxp/gfx2d.h:191)) already
  describes it as an `sx_bitmap` — which means SXGUI-C chrome and GL content can
  share one buffer with a painter clipped to the client area. That is the
  `glViewport`-inside-a-window case for free, and the reason not to invent a
  surface type.
- **`SwapBuffers` is `gfx_present_region`**, with the GL viewport as the dirty
  rect. Vsync, throttling and the wait for the compositor are already solved
  there; SxGL must not grow a second present path.
- **The depth buffer is SxGL's own allocation** and never leaves the process. It
  is the one buffer the platform knows nothing about.

Everything else SxGL needs — transform, clip, raster, texture — is its own code.
The only SxGFX function it is likely to call at all is
`sx_painter_blit_bitmap`, and only for the layouts where GL renders off-screen
first.

## The two costs, stated plainly

**It no longer leaves the image — this cost was paid elsewhere.** Floats used to
mean `-Sse`, and `-Sse` meant `tools/build-user.ps1` and a binary installed into
`/disk/bin` instead of the image's `/bin`. The in-tree userland now compiles
with `-msse2` and links the libm, so a `float` no longer decides where a binary
lives — see
[SYSTEM_LAYERING.md](SYSTEM_LAYERING.md#an-in-tree-app-has-floating-point). The
OS side was already in place: the kernel saves and restores FPU/SSE per process
(`fxsave64`/`fxrstor64`) and `crt0.S` aligns `rsp` to 16 before the first call.

What survives of this cost is size, and it is a smaller one: the userland links
with `--gc-sections`, so a consumer of SxGL pulls in the entry points it calls
and not the whole rasterizer. The alternative — a fixed-point rasterizer,
Quake-style — bought an in-image `libGL` at the price of a GL whose semantics
are its own invention, and it is no longer buying anything. It is not OpenGL; do
not call it OpenGL. **Recommendation: real floats and conformance to a subset.**

**Where the code lives.** `sxgl.c` would be a runtime module of the SDK like
`runtime/math.c`, which every in-tree program already links, and
`savanxp/sxgl.h` a normal header. The layering rule it has to respect is no
longer about the FPU: SxGL binds to the window surface and
`gfx_present_region`, never to the GPU behind the compositor's back.

## Which OpenGL

The subset is the first real decision, and it is bigger than the rasterizer.

- **GL ES 2 / core profile** needs a GLSL compiler. That is a separate project,
  larger than everything else in this document put together, and it buys nothing
  a software rasterizer can exploit.
- **A GL 1.x fixed-function subset** — matrix stacks, vertex arrays (and
  `glBegin`/`glEnd` if a port wants it), one texture unit, depth test, alpha
  blend, flat and Gouraud shading — is a few thousand lines, is what
  TinyGL-class implementations demonstrate, and is contemporary with the Win9x
  era the rest of the system imitates.

**Recommendation: GL 1.1 fixed-function, documented as a subset, with the
unimplemented entry points absent rather than present-and-lying.** A
`glGetString(GL_VERSION)` that claims more than is there costs a day of somebody
else's debugging.

## What DirectX is good for here

Choosing OpenGL for the public API does not mean ignoring the other tradition.
The rule that keeps both useful: **DirectX is worth copying inside SxGL and
below it, never at its public surface.** GL outside, because that is what ports
speak; D3D-shaped internals, because that is what makes a software rasterizer
fast.

### The layer below is already D3D-shaped

Not an analogy — read the structs. The client surface header carries
`submit_sequence`, `retired_sequence` and `composed_sequence`
([syscall.h:728](../subsystems/posix/sdk/v1/include/savanxp/syscall.h:728)), and
every `savanxp_gpu_dirty_rect_batch` carries its own `submit_sequence`. That is
a fence over a swapchain with numbered command lists.
`savanxp_gpu_present_timeline` (`submitted`/`retired`/`pending_count`) is
`GetCompletedValue`, and `GPU_IOC_WAIT_PRESENT` is `SetEventOnCompletion`.
OpenGL has no vocabulary for any of it — `SwapBuffers` is opaque and `ARB_sync`
arrived fifteen years later.

The immediate use is the resize question at the end of this document. DXGI
answers it (`ResizeBuffers`, and you do not resize with frames in flight) and
D3D9's *device lost* is the cautionary tale of what happens when the answer is
discovered in production instead of written down. **Settle it before batch 1,
not during.**

One boundary this does not move: the fence is there to be **read**, not to
submit around. SxGL may consult the timeline to avoid blocking; it may not step
past `gfx_present_region`.

### Pipeline state objects — the one idea worth taking early

The known cost of GL is revalidating state on every draw. In a software
rasterizer it is worse, because it becomes an `if (depth_test)`, an
`if (texturing)` and an `if (blending)` *per pixel*, inside the span loop. D3D10
solved it by freezing state into an immutable object and paying the cost once.

Here that means hashing the state that affects the inner loop and selecting a
specialized span routine — the manual version of what Quake and TinyGL did.
It is invisible from the API: `glEnable` on the outside, constants in the loop.
**This is why it is part of batch 2 rather than an optimization to retrofit:
retrofitting it means rewriting the rasterizer, which is the one piece nobody
wants to write twice.**

### Immutable resource descriptions

A `D3D11_TEXTURE2D_DESC` is fixed at creation; a GL texture is mutable at any
time, which forces the sampler to re-check format and size. Internally,
`glTexImage2D` can mean "build a new internal resource with a normalized format,
precomputed strides and whatever layout the sampler wants, then swap the
pointer". GL semantics preserved, constants in the loop again. It costs nothing
to plan for: the upload formats are the ones SxGFX already speaks.

### A debug layer, not `glGetError`

`glGetError` is global, sticky and deferred — a poor design even for its time.
D3D's debug layer validates at the call and names it. In a system where you
cannot attach a graphics debugger that matters more, not less, and it is cheap:
a compile-time `SXGL_VALIDATE` writing to `/dev/serial`, the channel
`windowd-stats` already uses precisely so that measuring does not corrupt what
is being measured ([GRAPHICS_PERF.md](GRAPHICS_PERF.md#why-devserial-and-not-stderr)).

### And the precedent: WARP

Microsoft ships a **conformant** software rasterizer. It is the argument behind
the recommendation above — software, but real and conformant to a subset, rather
than approximately GL.

### What not to bring

- **COM, `HRESULT`, interface refcounting.** This is C, and the SDK is plain
  structs.
- **A D3D-shaped public API.** Ports speak GL; nobody is going to arrive with a
  D3D port. The value of GL here is the body of code that already exists, not
  the design.
- **Command lists and deferred contexts.** They solve submission parallelism to
  a GPU. There is neither a GPU nor threads.
- **Descriptor heaps and the binding model.** They solve a hardware problem this
  system does not have.

## Suggested order

### Batch 0 — the consumer, before any API

The rule this repository already applies: `sx_pen` and the path recorder stayed
out of SxGFX because nothing was going to call them. Same criterion, and it is
stricter here because the surface is far larger. **The consumer lands first** —
a gears demo — rasterizing by hand, and it doubles as the specification of the
subset: whatever it calls is batch 1.

**It goes where the system's own test apps go, not in `ports/`.** An in-tree
program in `subsystems/posix/userland/`, registered in `build.ps1` with
`Test = $true` and listed in the Diagnostics group next to Gfx Demo — the 2D
rendering test that `tools/shoot.ps1` already drives for `windowd-stats`. A
gears demo is its 3D sibling and belongs beside it. `ports/` is gitignored and
disposable, which is right for a throwaway port and wrong for the program that
defines the API: this one has to be versioned, has to break the build when SxGL
breaks it, and has to be reachable by the smoke harness from day one.

That this is possible at all is recent. While the in-tree userland compiled
without floating point, anything doing 3D maths had to be an external app in
`/disk/bin`, and `ports/` was the only place it could start from. Since the
userland moved to SSE2 that constraint is gone, and the consumer can be an
ordinary test app of the image.

### Batch 1 — the context and the swapchain, with no triangles

`sxgl_context_create` over an existing `savanxp_gfx_context`, colour buffer
wrapped as an `sx_bitmap`, depth buffer allocated, `sxgl_make_current`,
`glViewport`, `glClear`, `glClearColor`, `sxgl_swap_buffers`. The result is a
window that clears to a colour and presents. **It is worth landing as its own
step**: it proves the seam against `windowd` — resize, damage, the wait for the
compositor — before any of it is entangled with rasterization.

### Batch 2 — the fixed-function pipeline, over a state object

Matrix stacks (`GL_MODELVIEW`/`GL_PROJECTION`), the transform, clipping against
the near plane, the viewport map, backface culling, the triangle rasterizer with
a depth test, and flat plus Gouraud shading.

**The rasterizer is built against an internal state object from the first line,
not against the GL state directly.** The draw path collapses the state that
affects the inner loop into one immutable descriptor, hashes it, and selects the
span routine for it; `glEnable` and company invalidate that descriptor instead
of being read per pixel. It is D3D's PSO, described above, and it is in this
batch for one reason: adding it later means rewriting the rasterizer, and the
rasterizer is the piece nobody wants to write twice. Starting with two or three
specializations and a generic fallback is enough — what must exist from day one
is the seam that lets a fourth be added without touching the pipeline.

This is also where the correctness work is: the fill rule has to be consistent,
for exactly the reason batch 3 of SxGFX found the hard way — a shared edge
rasterized from two triangles must not overlap or leave a seam. **Reuse that
lesson, not that code: the polygon scanline in `gfx2d.c` is integer and 2D, and
a 3D rasterizer needs subpixel precision and interpolated attributes.**

### Batch 3 — texturing

One texture unit, `glTexImage2D` in the formats SxGFX already speaks
(`SX_PIXEL_FORMAT_BGRX8888`/`BGRA8888`, so uploading an `sx_bitmap` is free),
nearest and bilinear, wrap and clamp, and perspective-correct interpolation
per span rather than per pixel. Mipmaps only if a port asks.

### Batch 4 — blending, fog, and the state that is still missing

`glBlendFunc` for the common pairs, alpha test, fog, scissor. Driven by the
ports, one at a time, the same way batch 3 of SxGFX was.

## Verification

The pattern is already established and should be copied, not reinvented:
`tests/host/gfx2d_test.cpp` compiles the real painter on the **host** and
asserts pixels without booting, behind `build.ps1 gfx2d-test` (182 checks).

A `build.ps1 sxgl-test` alongside it is the right instrument for the whole of
batches 2–4: a rasterizer is exactly the kind of code where a rendered image
plus a hash catches regressions a human eye would sign off on. Batch 1 is the
part that cannot be tested on the host — it is the seam with `windowd`, so it
belongs in a smoke scenario with `tools/shoot.ps1`, driving the batch 0 demo the
same way the existing scenarios drive Gfx Demo.

Add one measurement no 2D test needed: **triangles and fragments per second on
the host**, recorded per batch. A software rasterizer without a number attached
to it accumulates slowdowns nobody notices until a port is unusable.

## What must not happen

- **SxGFX must not gain a single function for this.** If SxGL seems to need one,
  the seam is in the wrong place — recheck it against the three facts of the
  seam.
- **No second present path.** `gfx_present_region` throttles against the
  compositor; a GL loop that submits around it will look faster and be wrong.
- **No `libGL` in the image.** That is the floating-point rule, and it is not
  negotiable without changing `Get-UserCompileFlags` for everybody.
- **No hardware backend until the kernel has one.** Accelerating this means
  virgl in `virtio_gpu`, 3D ioctls next to the 21 that exist, and a command
  submission path — a kernel project with its own roadmap, which SxGL would sit
  on top of later. Designing SxGL's internals "for when the GPU arrives" is the
  speculative-API mistake in a more expensive form.
- **SxGL is not part of the platform.** Like Doom and unlike `windowd`, it is
  something apps consume. Nothing in the kernel, `compositord`, `windowd` or
  SXGUI-C may come to depend on it.

## Open questions

Worth answering before batch 1, not during it:

- **Does a GL window compose with SXGUI-C chrome, or own its client area?** The
  seam supports both; the answer decides whether `sxgl_swap_buffers` presents
  the viewport or the whole window.
- **Is the depth buffer per context or per window?** Per context is the GL
  answer; per window is cheaper on resize.
- **What happens on resize mid-frame?** `gfx_apply_resize_event` already exists
  for 2D clients; SxGL has to reallocate two buffers and reset the viewport, and
  the failure mode has to be defined rather than discovered.
