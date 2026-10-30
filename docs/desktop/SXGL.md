# SxGL — the subset, call by call

SxGL is the platform's 3D rasterizer: a software implementation of a
documented subset of OpenGL 1.1 fixed-function. Why it exists, why it sits
beside SxGFX and not on top of it, and how it was built are in
[SXGL_ROADMAP.md](SXGL_ROADMAP.md). This document is the reference: what is
in, what is not, and where SxGL deliberately differs from the specification.

| | |
|---|---|
| Header | `include/sxgfx/sxgl.h` |
| Source | `system/gfx/sxgl.c` (runtime group `gl` in `units.txt`) |
| Host test | `tests/host/sxgl_test.cpp`, `./build.sh smoke sxgl-test` |
| Consumers | `/bin/gears` (POSIX), `/bin/nightwood.sxe` (SUN) |

## Where it lives and who presents

SxGL is platform code, like SxGFX and SXCHROME
([PLATFORM_GRAPHICS.md](PLATFORM_GRAPHICS.md)): it makes no system call. It
asks the runtime for `malloc`, `free`, `memset`, `memcpy` and libm, and nothing
else, so the same object links into POSIX programs, SUN programs and the host
test.

It is part of the static user runtime (group `gl`), the way libm is, and every
in-tree program can call it. The userland links with `--gc-sections`, so a
program that never calls `gl*` carries none of it. An external program built
with `tools/build-user.sh --sse` gets it too; without `--sse` it does not,
because the rasterizer is floating point.

**SxGL never presents.** `sxgl_swap_buffers` returns the rectangle written
since the previous swap and starts a new frame; the program presents that
rectangle through its own subsystem:

```c
/* POSIX */
sxgl_swap_buffers(gl, &damage);
gfx_present_region(&gfx, gfx.pixels, damage.x, damage.y, damage.width, damage.height);

/* SUN */
sxgl_swap_buffers(gl, &damage);
rect = (struct savanxp_gpu_dirty_rect){damage.x, damage.y, damage.width, damage.height};
sxnwm_submit_rects(&wm, &rect, 1);
```

That keeps the rule of the roadmap — no second present path — true by
construction: SxGL has no way to reach the compositor.

## The context (SxGL's WGL)

| Call | Does |
|---|---|
| `sxgl_context_create(bitmap)` | A context over the caller's `sx_bitmap` (`BGRX8888` or `BGRA8888`). The pixels stay the caller's; the context allocates its depth buffer at the same size. Viewport and scissor start as the whole bitmap. |
| `sxgl_context_resize(ctx, bitmap)` | After a window resize: new bitmap, depth buffer reallocated and cleared, viewport and scissor reset to the whole bitmap. Between frames only. Fails (and keeps the old size) when out of memory. |
| `sxgl_make_current`, `sxgl_get_current` | One current context per process; SxGL is single-threaded. |
| `sxgl_swap_buffers(ctx, &damage)` | See above. |
| `sxgl_set_debug_sink(ctx, fn, user)` | The debug layer: every error is reported at the call, by name and with the reason (`glTexImage2D: GL_INVALID_VALUE (size is not a power of two up to 1024)`). `glGetError` keeps GL's sticky semantics either way. Programs route it to the serial line (POSIX) or `sxn_log` (SUN). |
| `sxgl_get_stats`, `sxgl_reset_stats` | Triangles submitted and rasterized, fragments written: the numbers behind triangles per second. |

The answers to the roadmap's open questions:

- **The depth buffer is per context**, reallocated by `sxgl_context_resize`.
- **A GL window may share its client area with SXGUI-C or SxGFX drawing.** The
  colour buffer is the whole window; a program confines GL to part of it with
  `glViewport` plus `glScissor` (the scissor is what keeps `glClear` off the
  rest), and `sxgl_swap_buffers` reports only what GL wrote. `gears` does
  exactly that.
- **Resize mid-frame is not a state SxGL can be in.** The program applies the
  resize event, calls `sxgl_context_resize` before it draws again, and draws the
  next frame at the new size. The rule for the program is the one DXGI's
  `ResizeBuffers` has: never resize with a frame half drawn.

## The subset

**Geometry.** `glBegin`/`glEnd` with `GL_TRIANGLES`, `GL_TRIANGLE_STRIP`,
`GL_TRIANGLE_FAN`, `GL_QUADS`, `GL_QUAD_STRIP`, `GL_POLYGON`; `glVertex2f`,
`glVertex3f(v)`, `glColor3f/4f(v)`, `glColor3ub/4ub`, `glNormal3f(v)`,
`glTexCoord2f(v)`. Client arrays: `glEnableClientState`/`glDisableClientState`,
`glVertexPointer` (2–4 floats), `glColorPointer` (3–4 floats or bytes),
`glNormalPointer` (floats), `glTexCoordPointer` (2 floats), `glDrawArrays`,
`glDrawElements` (byte, short and int indices). **No points and no lines.**

**Transform.** `glMatrixMode` (`GL_MODELVIEW`, 32 deep; `GL_PROJECTION`, 4
deep), `glLoadIdentity`, `glLoadMatrixf`, `glMultMatrixf`, `glPushMatrix`,
`glPopMatrix`, `glTranslatef`, `glScalef`, `glRotatef`, `glFrustum`, `glOrtho`,
`glViewport`, `glDepthRange`. No texture matrix.

**Clipping and rasterization.** Every triangle is clipped against the six
planes of the view volume in clip space, so geometry crossing the near plane
is cut, not dropped. Window coordinates are snapped to 1/16 pixel and
rasterized with the **top-left fill rule**: a mesh covers every pixel once,
never twice and never zero times, which the host test asserts by drawing
meshes additively. Depth, `1/w` and the texture coordinates over `w` are
interpolated in screen space; colour and fog are interpolated linearly
(Gouraud). Texture coordinates are perspective-correct, divided every 16
pixels and interpolated linearly in between.

**Lighting.** `GL_LIGHTING` with up to four lights (`GL_LIGHT0`..`GL_LIGHT3`):
directional or positional, ambient and diffuse, constant/linear/quadratic
attenuation, spot cutoff and exponent (`glLightf`, `glLightfv`).
`glLightModelfv(GL_LIGHT_MODEL_AMBIENT)`. Materials: `glMaterialfv` with
`GL_AMBIENT`, `GL_DIFFUSE`, `GL_AMBIENT_AND_DIFFUSE`, `GL_EMISSION`;
`GL_COLOR_MATERIAL` and `glColorMaterial`. Per vertex. `glShadeModel(GL_FLAT)`
uses the provoking vertex GL names (the last of a triangle, the first of a
polygon).

**Per fragment, in order.** Depth test (`glDepthFunc`, all eight functions;
`glDepthMask`), texturing, alpha test (`glAlphaFunc`, all eight), fog
(`GL_LINEAR`, `GL_EXP`, `GL_EXP2`; `glFogf/i/fv`), blending (`glBlendFunc`, all
GL 1.1 factors), and the scissor (`glScissor`, `GL_SCISSOR_TEST`). Culling:
`GL_CULL_FACE`, `glCullFace`, `glFrontFace`.

**Textures.** `glGenTextures`, `glDeleteTextures`, `glBindTexture`,
`glTexImage2D` and `glTexSubImage2D` in `GL_RGBA`, `GL_RGB`, `GL_BGRA`,
`GL_LUMINANCE` and `GL_ALPHA`, unsigned bytes only, powers of two up to 1024.
`GL_BGRA` (`GL_EXT_bgra`, the only advertised extension) is what an
`sx_bitmap` already is, so uploading one is a copy. `glTexParameteri` with
`GL_NEAREST`/`GL_LINEAR` filters and `GL_REPEAT`/`GL_CLAMP`/`GL_CLAMP_TO_EDGE`.
`glTexEnvi` with `GL_MODULATE`, `GL_REPLACE` and `GL_DECAL`. One texture unit.

**Queries.** `glGetError`, `glGetString` (`GL_VERSION` is `"1.1 SxGL
subset"`), `glGetIntegerv`/`glGetFloatv` for the viewport, the matrix mode, the
two matrices, the stack depths, `GL_MAX_LIGHTS`, `GL_MAX_TEXTURE_SIZE` and the
bound texture; `glIsEnabled`. `glFlush` and `glFinish` return at once, because
every call has completed when it returns. `glHint` is accepted and ignored, as
the specification allows.

## Where it differs from GL 1.1, on purpose

Each of these is a decision, not an omission found later:

- **What is absent is absent.** A capability, parameter or mode outside the
  subset raises `GL_INVALID_ENUM` (and says so in the debug sink). Nothing is
  accepted and silently ignored except hints.
- **No mipmaps.** `glTexImage2D` takes level 0 only, and the mipmap
  minification filters are refused. For that reason a new texture starts with
  `GL_LINEAR` for both filters, not GL's `GL_NEAREST_MIPMAP_LINEAR`, which would
  leave every texture incomplete. Minification versus magnification is decided
  per triangle, from the texel area it covers against its pixel area.
- **Normals are always normalized**, as if `GL_NORMALIZE` were on. Enabling it
  is accepted and changes nothing.
- **One material for both faces.** There is no two-sided lighting:
  `glMaterialfv(GL_BACK, ...)` alone is refused rather than applied to the front.
- **Fog uses the eye-space depth** as its distance, which the specification
  permits and which keeps fog still while the view turns.
- **No alpha in the colour buffer.** Destination alpha reads as 1, as on a GL
  visual with zero alpha bits.

## Inside: the pipeline object

The span loop never reads GL state. The first triangle drawn after any change
freezes the state that matters to the loop (depth test and function, texture
and environment, alpha test, blend factors, fog colour) into a pipeline
object, and that object picks a span routine compiled for exactly those
features: D3D10's pipeline state object, behind GL's `glEnable`. The routines
are one inline function, `sxgl_span_core`, instantiated with constant feature
masks, so each `if (features & ...)` folds away at compile time.

The specializations are the combinations the consumers in the tree draw with:
flat and Gouraud over a depth buffer (`gears`), textured, fogged and lit with
alpha-tested cut-outs and additive glow (the forest game). Anything else runs
the generic instantiation, which reads the functions from the pipeline object
at run time: correct, and slower. Adding a specialization is one line in
`g_sxgl_specializations`; nothing else in the pipeline changes.

## Measured

`./build.sh smoke sxgl-test` prints, at 640×480, triangles and fragments per
second for a lit scene and a textured, fogged one. On the development host
(native, `-O2`) the first figures were:

| Scene | Triangles/s | Fragments/s |
|---|---|---|
| Lit, flat colour, depth | ~1.2 M | ~29 M |
| Textured, fogged, depth | ~0.86 M | ~20 M |

Record the figure again whenever the rasterizer changes. Under QEMU with TCG
expect an order of magnitude less; that, not the host figure, is what sizes a
game's viewport.
