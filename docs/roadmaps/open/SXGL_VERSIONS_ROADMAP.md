# SxGL versions — from a subset to OpenGL, one version at a time

> **Status: step 0 done (2026-11-26); step 1 in progress: lighting and mipmaps done.** It replaces the rule
> SxGL grew under — "the subset grows only when a consumer asks" — with a
> target: the versions of OpenGL itself, 1.1 first. `<GL/gl.h>`,
> `libsxgl.so.0.4` and `sxgl_get_proc_address` exist;
> [SXGL.md](../../desktop/SXGL.md) points here for the target, and step 3 of
> [XASH3D_ROADMAP.md](XASH3D_ROADMAP.md) consumes this roadmap.

SxGL was built as a documented subset of OpenGL 1.1 fixed-function
([SXGL_ROADMAP.md](../done/SXGL_ROADMAP.md)), and each batch was paid for by a
program in the tree: `gears`, Nightwood, then the GPU backend
([VIRGL_ROADMAP.md](VIRGL_ROADMAP.md)). That was the right way to build it,
because every call it has is exercised. It is the wrong way to keep growing
it. A program written elsewhere against OpenGL does not ask SxGL what it has.
It asks `glGetString(GL_VERSION)`, and from then on it assumes everything that
version promises. A subset shaped by three consumers fails the fourth in ways
none of the three could predict.

This document sets the target and the order. SxGL keeps its name, its two
backends and its context calls; what changes is what it promises.

## The decisions

### OpenGL's version numbers, not SxGL's own

SxGL reports and targets OpenGL versions: 1.1, 1.2, 1.3, 1.4, 1.5. There is no
"SxGL 2". Direct3D could number itself because it was its own API; SxGL's API
*is* OpenGL, and the programs that use it read GL's numbers and GL's extension
names. A second numbering would be a table nobody outside the tree reads.

What SxGL owns — the context calls (`sxgl_context_create`, `_resize`,
`_backend`, `sxgl_swap_buffers`, the debug sink, the stats), SxGL's WGL — is
versioned the way the rest of the platform is, by its soname, and only if it
ever changes incompatibly.

### Extensions are the increments, versions are the milestones

This is how OpenGL itself grew: a capability shipped as an extension, and a
version later made a set of them core. SxGL does the same.

- A piece of a later version lands on its own as its **real extension name**
  (`GL_ARB_multitexture`, `GL_SGIS_generate_mipmap`, `GL_EXT_texture3D`) and is
  listed in `GL_EXTENSIONS` once it works on both backends. Pieces may land out
  of order when a consumer needs one early.
- `GL_VERSION` moves to a version only when **everything** that version
  requires is in, on both backends, with its tests. Never before.
- The rule of the subset survives: **what is absent is absent.** An entry point
  that is not implemented is not exported, and a token it does not support
  raises `GL_INVALID_ENUM` and says so in the debug sink. A version string that
  claims more than is there costs somebody else a day of debugging.

### What the specification lets SxGL leave at zero

"Complete" means what the specification requires, not every feature it
describes. Several capabilities are properties of the pixel format, and a
conformant implementation may offer formats without them. SxGL uses that
freedom, and says so in `SXGL.md` instead of leaving it to be found:

| Capability | Required? | SxGL |
|---|---|---|
| Colour index mode | No: an implementation may offer RGBA visuals only | Not provided |
| Accumulation buffer | No: zero accumulation bits is conformant | Zero bits, until a consumer asks |
| Auxiliary buffers | No | None |
| Stencil buffer | No, but most 3D programs want 8 bits | 8 bits, in step 1 |
| Destination alpha | No | None, as today |
| Multisampling (1.3) | No: `GL_SAMPLE_BUFFERS` may be 0 | Zero buffers |
| Non-power-of-two textures | No: only from 2.0 | Powers of two, as today |

The minimum limits are the specification's, not ours: at least 8 lights,
6 user clip planes, a projection and a texture stack 2 deep, an attribute
stack 16 deep, textures up to at least 64 (SxGL keeps 1024).

### Both backends, the software one first

Every capability works in software and on the GPU before it counts. The
software rasterizer is the reference — it runs on every machine and in the
host test — and the GPU backend is compared against it with `sxgltest`, the
way the existing scenes already are. A capability the GPU backend cannot take
falls back to software for that draw, which is correct and slow, and is
written down.

The span loop keeps its pipeline object. New fragment features join the
generic instantiation first; a specialization is added when a measured
consumer draws with that combination, not before.

### How a program reaches it

Today SxGL is part of the static user runtime and its header is
`<sxgfx/sxgl.h>`. A program written for OpenGL expects `<GL/gl.h>`, a library
it can `dlopen`, and a function that returns entry points by name (that is how
`ref_gl` resolves every call). Step 0 adds all three; the static runtime keeps
working for the programs that link it today.

There is no GLX, EGL or WGL. Context creation stays SxGL's own: a program, or
the port's backend, calls `sxgl_context_create` and presents with its
subsystem, exactly as now. SDL, if it ever arrives
([XASH3D_ROADMAP.md](XASH3D_ROADMAP.md#step-4--sdl-as-its-own-project)), is the
layer that would hide that from ports.

### Consumers and tests

Each version is proved by a conformance test written against the
specification, per entry point and per state, in the host test
(`tests/host/sxgl_test.cpp`) and in `sxgltest` for the GPU comparison. A test
written from the specification is a legitimate consumer: a capability no
longer waits for a port to need it. Ports and programs still matter: they are
how a version is shown to be usable, not only conformant, and `ref_gl` is the
first.

## The order

Each step names its acceptance condition. A step without one is a wish.

### Step 0 — the binding, before any new capability — **done**

- `<GL/gl.h>` (and `<GL/glext.h>` for the extension tokens), in the SDK, for
  POSIX and SUN alike. `<sxgfx/sxgl.h>` stays for the context calls.
- A shared `libsxgl` the loader can `dlopen`, next to the static runtime group.
- `sxgl_get_proc_address(name)`: the entry point by name, or null for one that
  is not implemented, so a program can tell "absent" from "present".
- `GL_EXTENSIONS` built from one table, so an extension is advertised from the
  same place its entry points are registered.
- The conformance harness the later steps add to.
- With the step's commit, [SXGL.md](../../desktop/SXGL.md) stops calling
  SxGL a subset and points here for the target, and step 3 of
  [XASH3D_ROADMAP.md](XASH3D_ROADMAP.md) is rewritten: `ref_gl` consumes this
  roadmap's steps instead of deciding what SxGL grows.

**Done when:** a program built with `tools/build-user.sh` includes
`<GL/gl.h>`, `dlopen`s `libsxgl`, resolves every call it draws with through
`sxgl_get_proc_address`, and renders the same frame as a statically linked
twin; and a name SxGL does not implement resolves to null. Both are a
`nativetest` or smoke assertion.

It does. `sdk/glsmoke` (`./build.sh smoke gl-smoke`) is that program: it
draws one scene through the library and through the SxGL linked into it, and
the two frames are equal. The library is opened without `RTLD_GLOBAL`, so its
relocations find its own SxGL before the program's and the two copies keep
separate contexts. `sunlibtest` maps `libsxgl` by `DT_NEEDED` from SUN, which had no run-time
loading then; it has since ([sxn/sxnlibrary.h](../../kernel/SHARED_LIBRARIES.md#run-time-loading-in-sun)). The host test `sxgl-conformance-test` is the harness:
each case names the section of the specification it checks, and its binding
cases read `<GL/gl.h>` and require every declaration to resolve, so a
function cannot be declared without being registered.

### Step 1 — OpenGL 1.1, complete

This is the first honest version, and the largest step: it is everything the
subset left out. Today's string, `"1.1 SxGL subset"`, already claims 1.1;
there is no lower number that would be true either, so it stays until this
step closes and then loses the word "subset". The gaps, measured against the
header and `SXGL.md`:

| Area | Missing today |
|---|---|
| Primitives | `GL_POINTS`, `GL_LINES`, `GL_LINE_STRIP`, `GL_LINE_LOOP`; `glPointSize`, `glLineWidth`, `glLineStipple`; `glPolygonMode`, `glPolygonStipple`, `glPolygonOffset`; edge flags |
| Vertex specification | The rest of the `glVertex`/`glColor`/`glNormal`/`glTexCoord` families (`d`, `i`, `s`, `b`, `ub`, `us`, 1–4 components); `glRect`; `glArrayElement`, `glInterleavedArrays`, `glEdgeFlagPointer`; every array type and size GL 1.1 allows |
| Transform | The texture matrix; the `d` variants of the matrix calls; user clip planes (`glClipPlane`, at least 6); `glTexGen` (object linear, eye linear, sphere map) |
| Lighting | **Done:** specular and `GL_SHININESS`, 8 lights, two-sided lighting, the local viewer, `GL_NORMALIZE` honoured, every `glLight`/`glLightModel`/`glMaterial` variant and their `glGet` |
| Textures | **Done:** mipmaps, levels in `glTexImage2D` and the four mipmap minification filters. **Missing:** 1D textures; borders; proxy textures; `glCopyTexImage`/`glCopyTexSubImage`; `glGetTexImage`; `GL_BLEND` environment and its colour; priorities and residency; the remaining internal formats and pixel types; `glTexParameterf/fv` |
| Pixels | `glRasterPos`, `glBitmap`, `glDrawPixels`, `glReadPixels`, `glCopyPixels`, `glPixelStore`, `glPixelTransfer`, `glPixelMap`, `glPixelZoom` |
| Fragment and framebuffer | Stencil test and operations; `glColorMask`; `glLogicOp`; dithering (accepted); `glDrawBuffer`/`glReadBuffer` |
| Display lists | `glNewList`, `glEndList`, `glCallList(s)`, `glGenLists`, `glDeleteLists`, `glIsList`, `glListBase` |
| Evaluators | `glMap1`/`glMap2`, `glEvalCoord`, `glEvalPoint`, `glMapGrid`, `glEvalMesh` |
| Selection and feedback | `glRenderMode`, `glSelectBuffer`, `glFeedbackBuffer`, the name stack, `glPassThrough` |
| State | `glPushAttrib`/`glPopAttrib`, `glPushClientAttrib`/`glPopClientAttrib`; every `glGet` (`glGetBooleanv`, `glGetDoublev`, `glGetPointerv`, and the per-object queries); `glIsTexture` |

Mipmaps are on this list, not a later one: the mipmap filters are GL 1.0
core, and only their automatic generation waits for 1.4. That removes the
"no mipmaps" entry from SXGL.md's deliberate differences. The other entries
there are reviewed the same way: one-material lighting goes with two-sided
lighting; "normals always normalized" goes too. Normalizing every normal is
a legal result, but a program that leaves `GL_NORMALIZE` off and scales its
modelview gets lighting from SxGL that no other implementation would give it,
and the target is to behave like the others. SxGL lights with the normal as
transformed, and normalizes only when `GL_NORMALIZE` is enabled, which is off
by default as GL says.

Inside the step, the order is what programs written for GL use most, so the
step is useful before it is finished: lighting and mipmaps, points and lines,
polygon offset, colour mask and stencil, the pixel calls, the texture matrix
and texgen, display lists, the attribute stacks; evaluators and selection and
feedback last. A consumer may pull an item forward.

**Done when:** every entry point and token of OpenGL 1.1 that the pixel
format choices above do not exclude is implemented on both backends, the
conformance tests cover each, and `GL_VERSION` reads `"1.1 SxGL"`.

### Step 2 — OpenGL 1.2

`glDrawRangeElements`; 3D textures (`glTexImage3D`, `glTexSubImage3D`,
`glCopyTexSubImage3D`); the packed pixel types; `GL_SEPARATE_SPECULAR_COLOR`;
`GL_RESCALE_NORMAL`; texture LOD control (`GL_TEXTURE_MIN_LOD`, `_MAX_LOD`,
`_BASE_LEVEL`, `_MAX_LEVEL`). `GL_BGRA` and `GL_CLAMP_TO_EDGE` are already in.
The imaging subset (`GL_ARB_imaging`) is optional in 1.2 and is left out.

**Done when:** the same as step 1, for 1.2.

### Step 3 — OpenGL 1.3

Multitexture (at least two units: `glActiveTexture`,
`glClientActiveTexture`, `glMultiTexCoord`); cube maps; the compressed texture
API (`glCompressedTexImage*`; the core has no required format, so S3TC is a
separate decision); the `GL_ADD`, `GL_COMBINE` and `GL_DOT3` environments;
`GL_CLAMP_TO_BORDER`; transpose matrices; the multisample API with zero
sample buffers.

Multitexture is the piece most likely to be pulled forward, as
`GL_ARB_multitexture`: `ref_gl` and most engines of the era prefer it for
lightmaps.

**Done when:** the same as step 1, for 1.3.

### Step 4 — OpenGL 1.4

Automatic mipmap generation; `glBlendFuncSeparate`, `glBlendColor`,
`glBlendEquation`; depth textures and shadow comparison; the fog coordinate;
secondary colour; `glMultiDrawArrays`/`glMultiDrawElements`; point
parameters; `GL_INCR_WRAP`/`GL_DECR_WRAP`; `GL_MIRRORED_REPEAT`; texture LOD
bias; the window position calls.

**Done when:** the same as step 1, for 1.4.

### Step 5 — OpenGL 1.5

Buffer objects (`glGenBuffers`, `glBindBuffer`, `glBufferData`,
`glMapBuffer`…), occlusion queries, and the extra shadow functions. It closes
fixed-function OpenGL.

**Done when:** the same as step 1, for 1.5. This roadmap moves to
`roadmaps/done/` then.

### Beyond 1.5 — not planned

OpenGL 2.0 requires GLSL: a shader compiler, and a software rasterizer that
runs its output. It is a separate project, larger than everything above, and
this document does not commit to it. The decision is taken when step 5 closes,
with the consumers that exist then.

## Who else reaches it

- **POSIX and SUN** get every step at once: SxGL is platform code and both
  link it.
- **Win32** (`opengl32.dll`) and **Linux** (`libGL.so.1`) programs are not in
  this roadmap. Each would be a thin layer in its subsystem over the same
  SxGL, with its own context calls (WGL, GLX), and each is a gap to write down
  in its subsystem's document when a consumer appears.

## What not to do

- **Do not raise `GL_VERSION` early.** A version is a promise to programs
  that will never read this document.
- **Do not number SxGL separately from OpenGL.**
- **Do not land a capability on one backend.** Software first, the GPU
  compared against it, both before it is advertised.
- **Do not export a stub.** An entry point that exists and does nothing is
  worse than one that is absent.
- **Do not start GLSL to unblock a 1.x feature.** Nothing in 1.x needs it.
