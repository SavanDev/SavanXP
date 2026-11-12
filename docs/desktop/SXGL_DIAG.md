# SxGL Diagnostic Tool

`sxgldiag` (Programs ▸ System ▸ SxGL Diagnostic Tool) is the place to look
when a 3D program draws slowly, wrongly or not at all. It is laid out like the
classic diagnostic sheets of its era: tabs to read, a Notes box that says what
the page found, Next Page to walk them in order and a button that saves it all
as text. It is a SUN program drawn with SXGUI-C
([NATIVE_APPS.md](../userland/NATIVE_APPS.md)), and SxGL's only consumer whose
job is to report on SxGL itself ([SXGL.md](SXGL.md)).

| | |
|---|---|
| Source | `system/sxgldiag/sxgldiag.c`, `sxgldiag.sxres` |
| Visual check | `./tools/shoot.sh --scenario sxgldiag` |
| Without a window | `sxgldiag --selftest` (below) |

## What each page reads

Nothing on the sheet is a constant the program assumes. Every figure comes from
the system at the moment the page is drawn:

- **System**: `sxn_system_info` (bootloader, firmware, processor, memory, the
  SxFS volume, uptime, the boot framebuffer), the release from
  `shared/version.h` and the build number, and SxGL's `GL_VERSION`. Uptime and
  free memory are read again every second.
- **Display**: `gpu0`'s own queries, opened read-only: `GPU_IOC_GET_INFO` (the
  adapter, the mode, the 3D flag), `GPU_IOC_GET_CONNECTOR_PROPERTIES` (the
  largest mode it would try, "Mode limit") and, when the device has 3D,
  `GPU_IOC_3D_GET_CAPSET` (the virgl capset and its version). The SxGL box and
  the "Available" lines come from live contexts, below.
- **Renderer**: one column per backend. At start the tool creates a 16×16
  context with `SXGL_CONTEXT_SOFTWARE` and another with `SXGL_CONTEXT_GPU`, and
  asks each for `glGetString` and the `glGetIntegerv` limits. A GPU context
  that `sxgl_context_backend` reports as software means the machine has no 3D
  device, or the device refused: the column then says "Fell back to software"
  and the GPU test is disabled. That check is the only honest one; the 3D flag
  alone says the device negotiated virgl, not that SxGL can draw with it.

## The test

**Test software** and **Test GPU** draw the same scene for five seconds into a
208×156 bitmap of the tool's own, which the page shows as it runs: a cube with
one light, colour material, a 64×64 `GL_MODULATE` texture, depth test and
back-face culling, so one run touches transform, lighting, texturing and the
per-fragment path of either backend. The run reports frames, frames per second
and `sxgl_get_stats`' triangles in the Notes.

It then draws one more frame in a fixed pose that shows three faces, and checks
it: the centre has to be cube and the corner the clear colour (0, 0, 0.5)
within a tolerance the GPU's rounding fits. A run also fails if `glGetError`
reports anything or the debug sink (`sxgl_set_debug_sink`) received a message;
the first such message is what the Note quotes, and every one goes to the
system log. The frame rate includes painting the sheet and presenting it, as a
program's would, so it compares backends and machines, not rasterizers in
isolation; the rasterizer alone is measured by `./build.sh smoke sxgl-test`.

## Saving

**Save All Information...** writes every page and every Note as plain text to
`/users/user/SxGLDiag.txt` through `sxn_write_file`, so the report fits in
`SXN_WRITE_FILE_MAX` (4 KiB) and is truncated past it. The status line under
the tabs says where it went, or the error.

## `--selftest`

`sxgldiag --selftest` opens no window: it runs the test for one second with the
software backend and, when the GPU backend works, with it too, logs each
result line (`Software test passed: ...`) and then `SXGLDIAG SELFTEST PASS` or
`FAIL`, and exits 0 only if every run drew the expected frame. It is how the
GPU path is checked under `./build.sh run --gl`, where QEMU's `egl-headless`
display cannot take the screenshots `shoot.sh` needs.

## What it leaves out, on purpose

- **No date and time.** SUN has no wall-clock call yet; the sheet shows uptime
  instead of inventing a clock.
- **No sound, input or network pages.** Those belong to the Sound control
  panel, Task Manager and their own tools; this one is about drawing.
- **No vendor or renderer string from virgl.** The capset carries one in some
  versions at an offset this tool would have to guess; it reports the capset id
  and version, which it can read for certain.
