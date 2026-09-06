# SxGFX — hardening the 2D layer into a GDI32

> **Status: ALL THREE BATCHES COMPLETE, on master.** This document is the plan:
> what SxGFX is missing measured against the role
> [SYSTEM_LAYERING.md](SYSTEM_LAYERING.md) assigns it — **GDI32**, the 2D
> rasterization layer underneath SXGUI-C — and in what order to attack it.
>
> What batch 1 built:
>
> - **Painter primitives**: `sx_painter_set_pixel`, `sx_painter_hline`,
>   `sx_painter_vline`. SXGUI-C no longer defines its own `hline`/`vline`, nor
>   paints single pixels with 1×1 rects.
> - **Coordinate origin**: `sx_painter_push_origin`/`pop_origin` with their own
>   stack, applied exactly once per public call. The clip is now expressed in
>   local coordinates, and `sx_painter_clip_bounds()` is the supported way to
>   query it (reading `painter->clip_rect` by hand gives device coordinates).
> - **Brushes**: `sx_brush`, either solid or with a device-anchored 1-bit 8×8
>   pattern, plus `sx_painter_fill_rect_brush` and `draw_frame_brush`. The
>   dotted focus rect and the scrollbar trough came out of hand-written code.
> - **The `source_rect` clamp** in `draw_scaled_bitmap_nearest` (which was step
>   2 of the suggested order).
>
> **`sx_pen` did not land**: there is no consumer. Solid 1px lines are already
> covered by `hline`/`vline`, and the toolkit's only styled stroke is the focus
> rect, which is a frame with a brush. Adding the object with nobody to use it
> would be speculative API; it goes in when the first stroke with a width or a
> style shows up.
>
> Verification: `build.ps1 gfx2d-test` (43 pixel-exact checks against the real
> painter, on the host, without booting) plus `windowd-smoke`, `progman-smoke`,
> `filesapp-smoke`, `taskbar-smoke` and `cursor-repro`. The `files` scenario of
> `tools/shoot.ps1` produces **pixel-identical** captures before and after:
> batch 1 is a refactor with no change in appearance.
>
> The code in question is
> [savanxp/gfx2d.h](../subsystems/posix/sdk/v1/include/savanxp/gfx2d.h) (public
> API), [runtime/gfx2d.c](../subsystems/posix/sdk/v1/runtime/gfx2d.c) (the
> painter and the rect sets) and
> [runtime/gfx_impl.inc](../subsystems/posix/sdk/v1/runtime/gfx_impl.inc) (the
> raw primitives over the buffer, and text).
>
> **From the "Batch 1" section down, this document is the record of the
> original plan** — with the reasoning and the evidence exactly as they were.
> It is written in the future tense and cites lines of `sxgui.c` that batch 1
> itself already deleted (the `sxgui_hline`/`sxgui_vline`, the focus rect's
> pixel loops). It is kept that way because the value is the reasoning, not the
> status.
>
> What batch 2 built:
>
> - **`sx_region`** (2.1): a Y-banded region in canonical form, with **exact**
>   union/subtraction/intersection against a rect from a single engine.
>   `sx_painter_push_clip_region` uses it as a clip, sharing the stack with
>   `push_clip`. `windowd` composes against it: **one** call to
>   `wm_paint_layer` per layer instead of one per sub-rect.
> - **Fonts** (2.2): UTF-8 decoding (`gfx_utf8.inc`, shared by the POSIX SDK
>   and the native runtime the same way the font data is), the Noto table
>   reindexed **by codepoint** with ranges — Latin-1 plus typographic
>   punctuation and the euro sign — and `sx_painter_set_font` selecting between
>   `SX_FONT_UI` and `SX_FONT_MONO`, which is the `SelectObject(hFont)` that was
>   missing. The mono path gained its clipped blit, which did not exist.
>
> **A measurement that corrected an assumption:** `sx_rect_set` subtraction was
> already exact (it uses `push_raw`, which does not merge), so with a single
> damage source the region gains no area — 752 px either way. What over-covers
> is `sx_rect_set_add`, which merges by bounding box as soon as two dirty rects
> touch: with damage in three rects it is 198 px exact against 358. Both cases
> were kept as tests so nobody assumes it again.
>
> **UTF-8 is an enabler, not a fix:** today there is not a single non-ASCII
> literal in the UI, so nothing looked wrong. What changes is that now you *can*
> write "Configuración" without it coming out as two glyphs.
>
> What batch 3 built:
>
> - **Raster ops**: `sx_rop` (COPY/XOR/AND/OR/INVERT) over the brush. XOR is
>   the one that matters: applied twice it restores the exact destination, which
>   is what makes drag frames cheap.
> - **Geometry**: line (Bresenham, with a shortcut for straight ones),
>   polyline, scanline-filled polygon, ellipse (outline and fill) and rounded
>   rect. Everything leans on the painter's `hline`/`vline`/`set_pixel`, so the
>   clip, the region and the origin come out free and correct.
> - **Quality scaling**: `sx_painter_draw_scaled_bitmap` with a selectable
>   filter; bilinear in 8-bit fixed point (this layer is linked by every binary
>   and cannot depend on `-Sse`).
> - **Memory DC**: `sx_bitmap_create`/`destroy`, the missing
>   `CreateCompatibleBitmap`. `destroy` on a `wrap` bitmap is a deliberate
>   no-op.
> - **`sx_region_from_polygon`**: GDI's `PathToRegion`. It shares the scanline
>   with `fill_polygon`, so the region and the drawing describe exactly the same
>   shape — the condition for clipping against a figure without the edge
>   flickering. It closes the loop with the non-rectangular windows that
>   motivated 2.1.
>
> **A path recorder did not land** (`BeginPath`/`EndPath`/Bézier). With no
> consumer, and with the polygon already covering the useful engine, it would
> have been speculative API — the same criterion that left `sx_pen` out of
> batch 1.
>
> **A real bug found by the test, not by review:** the scanline crossing was
> computed from the vertex as the caller stored it, so the same diagonal
> traversed one way gave one `x` and the other way gave another — two polygons
> sharing an edge would overlap or leave a seam. The fix is to normalize the
> edge top to bottom. Along the way I tried floor and ceiling for the rounding:
> both break mirror symmetry. C's truncation is the correct one precisely
> because it is odd-symmetric, and there is now a test pinning that.
>
> **Consumers wired up, and the ones that were not:** the wallpaper moved to
> bilinear (it scales by arbitrary factors and is paid once per background
> change). Client surface scaling in the compositor stays on nearest on
> purpose: it is on the hot path and four samples per pixel do not go in there
> blind. `sxgui_paint_arrow` was left as is: its four-row stepped triangle is
> deliberate Win9x pixel art, not a shortcoming of the layer.
>
> Verification: `gfx2d-test` grows to **182 checks**, with the geometry also
> rendered to an image and looked at with human eyes. Live, the full smoke
> battery.

## Why GDI32 and not DirectX

Worth writing down because the question keeps coming back: SxGFX is **not** the
place to put a DirectX-style model (device, opaque resources, swapchain,
pipeline state). SXGUI-C is built on top of it and what it needs is
`fill_rect`, not `CreateDevice`. Turning SxGFX into a D3D breaks the role it
has been assigned.

Curiously the kernel is already more DirectX-like than SxGFX:
`GPU_IOC_IMPORT_SECTION` → `surface_id` is resource creation,
`PRESENT_SURFACE_BATCH` is a command list, and `savanxp_gpu_present_timeline`
(submitted/retired + `WAIT_PRESENT`) is a fence. If that model is ever wanted,
it goes in a new layer beside — not inside — SxGFX, talking directly to
`/dev/gpu0`. This document is about the other direction: making SxGFX a **good
GDI**.

## Batch 1 — what already hurts

These three have direct evidence in SXGUI-C's code: the toolkit is emulating by
hand things the layer below should be giving it.

### 1.1 The painter does not expose primitives that already exist

`gfx_pixel`, `gfx_hline`, `gfx_vline` and `gfx_frame` are implemented in
[gfx_impl.inc:755-833](../subsystems/posix/sdk/v1/runtime/gfx_impl.inc:755),
but no `sx_painter_*` wraps them. Since SXGUI-C needs the painter's clipping
and the painter only offers `fill_rect`, the toolkit ends up painting **single
pixels through the rectangle fill path**:

```c
sx_painter_fill_rect(painter, sx_rect_make(x, y, 1, 1), SXGUI_COLOR_TEXT);
```

It shows up in [sxgui.c:121-136](../subsystems/posix/sdk/v1/runtime/sxgui.c:121)
(the dotted focus rect), [:153](../subsystems/posix/sdk/v1/runtime/sxgui.c:153)
(the checkbox dither) and
[:1133](../subsystems/posix/sdk/v1/runtime/sxgui.c:1133). Every pixel pays for
a clip intersection, a clip against the bitmap and a call. And the toolkit
defines its own `sxgui_hline`/`sxgui_vline` over `fill_rect` in
[sxgui.c:7-15](../subsystems/posix/sdk/v1/runtime/sxgui.c:7).

**What to do:** `sx_painter_set_pixel`, `sx_painter_hline`,
`sx_painter_vline`, delegating to the raw primitives after applying the clip.
It is the cheapest fix in this document.

**Invariant to respect:** the new primitives have to be correct *per fragment*.
The comment in [gfx2d.c:288](../subsystems/posix/sdk/v1/runtime/gfx2d.c:288)
documents the cursor residue bug — `draw_frame` was tracing a border around
every dirty sub-rect — and that lesson applies to everything added here.

### 1.2 There are no pen or brush objects

Color travels as a loose `uint32_t` in every call. GDI has `HPEN` (width,
dotted, dashed) and `HBRUSH` (solid, hatch, pattern). SXGUI-C's dotted focus
rect and checkbox dither are, literally, pattern brushes made by hand pixel by
pixel.

**What to do:** an `sx_brush` with a solid color or a 1-bit 8×8 pattern, and an
`sx_pen` with width and style. The Win9x look comes from there instead of being
reimplemented in every widget.

### 1.3 There is no coordinate origin

GDI has `SetViewportOrgEx`. Here everything is absolute, so every widget
computes absolute coordinates by hand.

**What to do:** `sx_painter_push_origin(dx, dy)` / `pop_origin`, reusing the
same stack pattern the clip already has (`SX_PAINTER_CLIP_STACK_DEPTH`). It is
a prerequisite for nested widgets and for scrolling containers that should not
have to do the arithmetic by hand.

## Batch 2 — what changes structure

### 2.1 Clip by region, not by rectangle

`sx_painter` has a single `clip_rect` plus a stack of 16. But `sx_rect_set`
**already implements** rect sets with `sx_rect_set_subtract_rect`: the region
machinery is written and is not wired into the painter's clip.

GDI has `HRGN` with AND/OR/XOR/DIFF combination. Without it there are no
non-rectangular windows and no direct clip against the damage region.

Watch out for an existing simplification: `sx_rect_set_add`
([gfx2d.c:511](../subsystems/posix/sdk/v1/runtime/gfx2d.c:511)) merges by
bounding box on any overlap or adjacency, so two L-shaped rects become the
rectangle containing them. It over-covers. A banded region — like GDI's — is
exactly the upgrade that solves this and the clip at the same time.

### 2.2 Font object

There are **two** baked fonts and the choice is nailed into the name of the
function being called: `gfx_blit_text` (Noto, proportional, antialiased)
against `gfx_blit_text_mono` + `gfx_cell_width`
([gfx_impl.inc:912](../subsystems/posix/sdk/v1/runtime/gfx_impl.inc:912),
UniFont, the console). The painter only exposes the first, through
`sx_painter_draw_text`.

Worse: `gfx_noto_glyph(unsigned char c)`
([gfx_impl.inc:9](../subsystems/posix/sdk/v1/runtime/gfx_impl.inc:9)) indexes
by byte. A hard cap of 256 glyphs, **no Unicode**, no sizes, no bold or italic.
There is no equivalent of `SelectObject(hFont)`.

**What to do, in order:** (a) decode UTF-8 → codepoint on the text path; (b) an
opaque `sx_font` the painter selects, with the two current fonts as the first
two instances; (c) only then, variants and sizes.

## Batch 3 — GDI gaps that are missing entirely

- **Raster ops.** There is no SRCCOPY/SRCINVERT/PATINVERT: the blend is nailed
  to SRC_OVER (`sx_blend_bgra8888_over_rgb`,
  [gfx2d.c:3](../subsystems/posix/sdk/v1/runtime/gfx2d.c:3)). XOR is what makes
  drag rubber-bands and focus rects cheap, and today they are emulated pixel by
  pixel.
- **Geometry.** There is no diagonal line, circle, ellipse, polygon or rounded
  rectangle. Bresenham plus a midpoint ellipse is about 80 lines.
- **Quality scaling.** `sx_painter_draw_scaled_bitmap_nearest` is the only
  option and the filter is in the name. GDI has
  `SetStretchBltMode(HALFTONE)`; bilinear when shrinking icons and wallpapers
  is visible to the naked eye.
- **Memory DC.** Only `sx_bitmap_wrap` exists. GDI has `CreateCompatibleDC` +
  `CreateCompatibleBitmap`, which is the canonical recipe for flicker-free
  painting; today every app does its own malloc and builds the
  `savanxp_fb_info` by hand.
- **Paths.** `BeginPath`/`EndPath` and regions derived from paths. Lowest
  priority: it is not blocking anything.

## Hardening what is already there

**`draw_scaled_bitmap_nearest` does not validate `source_rect` against the
source bitmap.** The destination is clipped (`target_rect`), but `source_x` and
`source_y` are derived from a `source_rect` supplied by the caller, of which
only emptiness is checked
([gfx2d.c:378](../subsystems/posix/sdk/v1/runtime/gfx2d.c:378)). A
`source_rect` exceeding the source dimensions, or with negative `x`/`y`, reads
outside the buffer.

Today it is **latent, not an active bug**: the three in-tree callers
(`desktop_wallpaper.c:356`, `progman.c:228`, `windowd_render.c:385`) pass the
full source rect. But it is public SDK API and the clamp is four lines.

## What is right and must not be broken

- **Antialiased text** through per-pixel coverage (`kNotoCoverage`). GDI32 took
  years to get that; no refactor should lose it.
- **Correct per-fragment clipping**, with the reasoning documented in
  [gfx2d.c:288](../subsystems/posix/sdk/v1/runtime/gfx2d.c:288). It is an
  invariant won the hard way against a real repaint bug.
- **`sx_rect_set` overflow collapses to a deliberate superset**
  ([gfx2d.c:558](../subsystems/posix/sdk/v1/runtime/gfx2d.c:558)): it
  over-paints, never under-paints, and it is documented. The fixed capacity of
  64 and the clip stack of 16 degrade safely.
- **The `memcpy` fast path** in `sx_painter_blit_bitmap` when source and
  destination share a full width
  ([gfx2d.c:308](../subsystems/posix/sdk/v1/runtime/gfx2d.c:308)).

## Suggested order

1. ~~**Batch 1 complete** (1.1 + 1.3 + 1.2, in that order).~~ **Done.**
   Additive, no kernel changes, and it deleted SXGUI-C code.
2. ~~**The `source_rect` clamp.**~~ **Done**, together with batch 1.
3. ~~**2.1 (regions).**~~ **Done.** It absorbed `sx_rect_set_add`'s
   over-coverage and left the base ready for non-rectangular windows.
4. ~~**2.2 (fonts).**~~ **Done**, including the change in
   `tools/font/genfont.py` to bake by codepoint ranges.
5. ~~**Batch 3, on demand.**~~ **Done**, except the path recorder, which still
   has no consumer and therefore did not go in.

Verification: all of this falls under the headless toolkit preview on the host
— the toolchain's `clang` with stubs rendering to PNG — and under
`windowd-smoke`. New primitives should arrive with an image comparison before
touching SXGUI-C.
