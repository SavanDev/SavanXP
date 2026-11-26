# UI fonts

How the desktop's text is drawn, and the rule that keeps it looking like a
solid Win9x/2000-era system rather than a modern toolkit in a grey costume.

## The faces

| Painter font | Face | Size | Source | File in `/system/fonts` | Table (the fallback) |
|---|---|---|---|---|---|
| `SX_FONT_UI` | MS Sans Serif (Wine's) | 13px bitmap strike | `assets/desktop/fonts/ms_sans_serif.sfd` | `MSSansSerif.ttf` | `gfx_font_noto.inc` |
| `SX_FONT_UI_TITLE` | RetroSans | 17px | `assets/desktop/fonts/RetroSans.ttf` | `RetroSans.ttf` | `gfx_font_title.inc` |
| `SX_FONT_MONO` | GNU UniFont | 8x16 | `assets/desktop/fonts/unifont.hex` | none | `console_font_unifont.inc` |

Programs that map `libsxfont` draw the body and captions from the files;
everything else draws from the tables, which hold the same pixels
([Interface faces from files](#interface-faces-from-files)).

The body tables keep their historical `SX_NOTO_*` / `kNoto*` names: the body
face used to be Noto Sans, and renaming the symbols would touch every consumer
(`system/gfx/pixels.c`, `tools/glyphs.py`, the host tests) for no change in behaviour.

The 13px strike is MS Sans Serif 8pt at 96 DPI, the face Windows 95, 98 and
2000 used for menus, dialogs and controls: ascent 11, descent 2, line height 13.
Captions deliberately do not follow Windows (bold MS Sans Serif): RetroSans,
together with the per-application caption accent, is part of what makes the
desktop SavanXP's own.

## The rule: every UI face is 1-bit

Every coverage byte in the body and caption tables is `0x00` or `0xff`. No
antialiasing anywhere in the chrome.

Why: Win9x chrome is drawn in hard pixels: two-pixel bevels in four exact
greys, dithered checkers, 1-bit icon masks. Antialiased text sitting in that
frame reads as soft and "unfinished", and it was the largest single
difference between SavanXP and the systems it takes after. Crisp text is what
makes the bevels look deliberate.

How to keep it:

- **Prefer a hand-drawn bitmap strike** (`genfont.py sfd`). A designer drew it
  at that exact pixel size; nothing is rasterised.
- **A pixel-font TTF only at its native size.** Pixel fonts shipped as TTF are
  outlines on a pixel grid; FreeType renders them 1-bit only when the pixel
  size matches that grid. RetroSans' grid is 17px. Baking it at 18px left 38%
  of the caption ink grey, which is the blur that was visible in every title
  bar until it was rebaked.
- **An outline font (Noto Sans and the like) never goes into a UI table.** It
  is fine where smoothness is the point, like the boot wordmark
  (`tools/gen_boot_logo.py`).

The rule is about these tables, which the chrome is drawn with. Document
text in scalable fonts comes from `libsxfont`, which follows each face's
gasp table: 1-bit and hinted at reading sizes, grey only in large headings
([FONT_LIBRARY.md](FONT_LIBRARY.md#smoothing-what-gasp-says)).

`gfx2d-test` enforces this: `case_ui_fonts_are_one_bit_and_fit_their_line`
fails on any grey coverage byte, and on any glyph that pokes out of the line
its ascent and descent declare.

## Faces at run time

The three faces are slots (`GFX_FACE_UI`, `GFX_FACE_MONO`,
`GFX_FACE_TITLE` in `sxgfx/pixels.h`, the same values as the painter's
`sx_font_id`). Each slot draws with its baked table until a program installs
another face with `gfx_face_install`, and installing `NULL` brings the table
back. A face is a small table of its own: ascent, line height, the
monospace cell, and a `glyph` call (with an optional `advance` call for
measuring) that hands over coverage. `libsxfont` builds one from a strike
(`sx_font_gfx_face`, [FONT_LIBRARY.md](FONT_LIBRARY.md)), so a PIE program
that links it can draw its interface in a TrueType face.

- **Nothing changes unless a program installs a face.** The baked faces go
  through the same slot code, and `pixels-text-test` holds their output to
  checksums recorded from `pixels.c` before slots existed: every pixel,
  width and height of the three faces, plain and clipped.
- **`libgfx2d` does not depend on `libsxfont`.** A face is plain data and
  function pointers, so static programs, which carry `pixels.c` in their
  runtime, keep the tables and pull no FreeType; only a program that links
  `libsxfont` can install one of its faces.
- **The baked monospace face keeps its bit-mask path**, which an 80x24
  terminal repaint needs; an installed monospace face advances by its cell
  whatever each glyph says, as the grid requires.
- **The slots are the process's.** A program installs its faces before it
  draws, from the thread that draws, and removes a face before closing what
  it points at (a `libsxfont` strike).
- **The 1-bit rule still holds for what the chrome is drawn with**: a
  `libsxfont` face meant for the interface is opened with
  `SX_FONT_FORCE_MONO`.

The kernel's console and boot screen, and `gdi32` until the WordPad
roadmap's WP6, keep drawing from the tables directly.

## Interface faces from files

The body and caption faces are files in `/system/fonts`, and the programs
that draw the interface read them when they start:

- **`MSSansSerif.ttf`** is Wine's 13px strike written as TrueType by
  `genfont.py sfdttf`: each glyph's outline is its pixels, on a grid of 128
  units per pixel (an em of 1664), so at 13 ppem the rasterizer gives the
  strike back exactly. Codepoints the strike lacks draw a blank `.notdef` as
  wide as the space, as the table's fallback does. Its gasp table asks for
  1-bit, grid-fitted text at every size. It is generated once and committed,
  like the tables, so the build needs no fontTools.
- **`RetroSans.ttf`** is the original file the caption table was baked from.
  It also covers the characters the table left out; one it lacks draws its
  own `.notdef`, where the table advanced like a space.
- **The monospace face stays baked.** UniFont is a `.hex` bitmap font with
  tens of thousands of glyphs, and the terminal's grid needs its 8x16 cell.

**Who loads them.** `sxfont_ui_load` (`include/sxfont/ui.h`, unit
`system/font/sxfont_ui.c` in the runtime's `gui` group) opens each file at
the table's size with `SX_FONT_FORCE_MONO` and installs it in its slot; the
first call does the work and later ones answer the same. Each subsystem
reads the files its own way: SUN's frame calls it from `sxngui_app_init`
(`sxngui_load_interface_faces`), POSIX's from `sxgui_app_init`
(`sxgui_load_interface_faces`), and the two programs that draw without a
frame call it themselves: `windowd` for captions and the consent prompt
(it logs `windowd: N interface faces from /system/fonts`), and the desktop
for the taskbar and the Start menu. `windowd` became a PIE for this; it
maps `libgfx2d` and `libsxfont`.

**Who does not.** The unit refers to `libsxfont` weakly, so a program that
does not map it, which includes every static program, keeps the tables and
links no FreeType. A missing or damaged file leaves its slot on the table
too. That is the fallback the tables are kept for, together with the
kernel and, until WP6, `gdi32`. Static programs that draw text today:
`console` (monospace only), `nightwood` and `sysinfo`.

**Why the checks still read the tables.** `ui-face-files-test` renders both
files through `libsxfont` as the runtimes open them and compares every
codepoint the tables cover: the advance, and each lit pixel relative to the
pen and the baseline. Both files match their tables exactly, and so do the
ascent and the line (the strike's height plus its external leading:
13 = 11 + 2 for the body, 17 = 12 + 4 + 1 for captions). While that holds,
`sxgui-smoke` and `tools/glyphs.py`, which read the tables out of the
binaries, are reading what the files draw. `fonttest` checks on SavanXP that
the frame loads both files, leaves the monospace slot baked, and measures
as the tables do.

**What it costs.** Each program with an interface holds a FreeType engine,
the two files (about 135 KB) and two strikes, whose glyph caches start at
16 KB and grow as glyphs are drawn ([FONT_LIBRARY.md](FONT_LIBRARY.md#the-glyph-cache)).

## Changing a face

1. Put the source in `assets/desktop/fonts/` with its license, and add it to
   `docs/third-party/THIRD_PARTY_PROVENANCE.md`.
2. Find the size at which it is 1-bit (for a TTF, render a sample at every
   size and keep the one with no grey pixels).
3. Bake it with the command in `assets/README.md`, keeping the table's prefix,
   and make the file the runtimes load: the TTF itself, or `genfont.py
   sfdttf` for a bitmap strike. Update `kInterfaceFaces` in
   `system/font/sxfont_ui.c` and the staging in `CMakeLists.txt` if the file
   or the size changes.
4. Run `./build.sh test` (the 1-bit check, and `ui-face-files-test`, which
   holds the file to the table), `./build.sh smoke sxgui-smoke` (the
   on-screen glyph check, which reads the tables back out of the built
   `libsxgfx`) and look at `tools/shoot.sh` captures: layouts follow the line
   height, so a different height reflows menus, lists and tabs.

## Coverage gaps

A bitmap strike is drawn for one code page, so some of the punctuation the
tables cover (`NOTO_RANGES` in `genfont.py`) is not in Wine's 13px strike.
Hyphen, non-breaking hyphen, figure dash and one dot leader are drawn with
their ASCII twins (`SFD_ALIASES`); the rest (U+2016, U+2017, U+201B, U+201F,
U+2023, U+2025, U+2027) advance like a space, the same contract as any
codepoint outside the table. `genfont.py` lists them every time it bakes.
