# Paint: the sketchbook

`/bin/paint` ("Paint") is the in-tree Accessories drawing app, in the spirit
of the WinXP one: toolbox on the left, colour strip on top, canvas over a
status bar. In-tree demo in `subsystems/posix/userland/paint.c` (about 1800 lines),
built as PIE against `libgfx2d`, `libsxgui`, `libsxgfx` and `libsximg`.

This document pins what it draws, what it opens and saves, and what proves
it — so the app can grow tools without rediscovering the ownership rules.

## Layout: chrome without widgets

None of the chrome are widgets: SXGUI has no toolbar or palette, so the
toolbox and the colour strip paint in `on_paint` and hit-test in
`on_pointer`, like the Mines grid in `mines.c`. The only widget in the
window is the status label. Tool selection reads sunken, like a button left
pressed.

The canvas is an `sx_bitmap` the app owns: the WM keeps no backing store, so
there is no Expose — the app blits the original on every `on_paint`. A new
document is 512x384; an opened image keeps its own size (up to 512x384, which
the window layout is sized for), and the canvas frame shrinks to fit it. Declares `mime_open=image/bmp,image/png` and `ext_open=.bmp,.png`
in `paint.sxres`, but that is capability, not default: association policy
lives in the registry (SXE phase 5), as in Notepad.

The status bar reads `<name> | <tool> | undo <n>`, with a `*` while dirty.

## Tools

| Tool | Acts as |
|---|---|
| Pencil | 1 px stroke |
| Brush | Ellipse stamps along the drag, radius 1..16 (`+`/`-` size it, default 4) |
| Eraser | Paints the background colour |
| Picker | Reads the pixel under the cursor |
| Fill | Exact-match 4-connected flood fill under the cursor: iterative, with a visited set, so it always fits and always terminates |
| Line, Rectangle, Ellipse | 1 px opaque shapes |

Everything is 1 px and opaque: no pen, no alpha, no flood fill in SXGFX
itself (see [`SXGFX_ROADMAP.md`](SXGFX_ROADMAP.md)).

## Undo

Every stroke that changes the document pushes one of 8 full-document
snapshots first (`Edit > Undo`, Ctrl+Z). Pencil, brush and eraser push on
button down; shapes push on release, and not at all when a rectangle or
ellipse has no width or height; the bucket pushes only when the pixel under it
is another colour; the picker never does. Ring of document-sized buffers; the
oldest is reused when full, and all of them are dropped when the document
changes size. The selftest below proves push, pop, and empty-pop.

## Files: BMP in, BMP out, PNG through `libsximg`

- **Save:** 24-bit `BI_RGB` BMP at the document's size — a 54-byte header
  plus padded rows, bottom-up.
- **Never BMP bytes under a `.png` name.** There is no PNG encoder, so
  `paint_save_bmp` refuses any path ending in `.png` (any case). Save on a
  document opened from a PNG opens Save As with the same name as `.bmp`;
  Save As refuses a `.png` name and keeps the dialog open. A failed Save As
  leaves the document's name unchanged.
- **Load BMP:** same subset back, bottom-up or top-down (negative height);
  anything else is a rejection, not a guess.
- **Load PNG:** through `libsximg.so.0.4` (`savanxp/img.h`), composited over
  white; images larger than 512x384 are refused.

## Verification

Two proofs, at two levels:

- `./build.sh smoke paint-smoke` runs `/bin/paint --selftest` headless — no
  window, no WM — through the same functions the UI uses (`paint_shape`,
  `paint_save_bmp`, `paint_load_image`), never a parallel API: pencil pixel,
  line endpoint, rect corner, brush stamp, picker (plus out-of-bounds),
  undo push/pop/empty, fill interior with frame intact, BMP bit-exact
  save/load round-trip, garbage-file rejection, the real
  `/paint-test.png` fixture by size and pixel value, refusal to write a
  `.PNG` name, a 16x16 save/load that keeps its size, a top-down BMP, and a
  same-colour fill reported as a no-op. Expects `PAINT SMOKE PASS`.
- `tools/shoot.sh --scenario paint` drives tool switching by keyboard in the
  real window and asserts the status bar on screen (`(untitled)`, `Pencil`,
  `undo 0`, `Brush 9px`).