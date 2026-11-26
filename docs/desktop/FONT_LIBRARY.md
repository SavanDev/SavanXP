# libsxfont: scalable fonts at run time

`libsxfont.so.0.4`, declared in `sxfont/font.h` (`include/sxfont/font.h`;
the POSIX SDK's `savanxp/font.h` includes it), draws TrueType fonts at
any size. It is the system library the
[WordPad roadmap](../roadmaps/open/WIN32_WORDPAD_ROADMAP.md#d1-real-fonts-rasterized-at-run-time-by-a-system-library-2026-11-21)
asked for before WP6: a word processor needs text from 8 to 72 points, in
bold and italic, in several faces, with the widths a control lays lines out
with. Until it, every glyph on SavanXP came from a table baked at build
time ([UI_FONTS.md](UI_FONTS.md)), and those tables stay what the desktop
draws its own interface with.

It is built on [FreeType](https://freetype.org) 2.15.0, vendored in
`vendor/freetype/`, and ships with the Liberation faces in `/system/fonts`.
Portions of this software are copyright © 2026 The FreeType Project
(https://freetype.org). All rights reserved.

## What it is

- **One library for every subsystem.** SUN and POSIX programs link it like
  `libsximg`, and the SDK ships it (`runtime/libraries.txt`). Win32's
  `gdi32` reaches it through `winhost` (below). A capability of one
  subsystem's text is everyone's ([feature parity](../../AGENTS.md#feature-parity-between-subsystems)).
- **Memory only.** It opens no file and makes no system call: the caller
  reads a font file and hands over its bytes, as `libsximg` takes an
  image's. FreeType is built without file streams
  (`FT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT`) and gets its memory from the
  program's `malloc`. `cmake/FreeType.cmake` checks what the archive asks
  the program for: memory and string functions, `qsort` and `strtol`.
  `setjmp` and `longjmp`, which the `cmap` validator needs, are a copy of
  the runtime's `setjmp.S` inside the library, since in-tree programs do not
  link that group.
- **FreeType is hidden.** It is linked in whole with `--exclude-libs,ALL`,
  so the library exports only `sx_font_*`. A program that brings its own
  FreeType (a port) does not meet this one.
- **TrueType only.** Faces in `.ttf` and `.ttc` files, with or without
  instructions, with embedded bitmaps. CFF OpenType (`OTTO`), Type 1, WOFF
  and bitmap fonts (`.fon`) answer `SX_FONT_ERROR_UNSUPPORTED`: nothing on
  the WordPad path uses them, and each is a FreeType module to add when a
  consumer does.

## The API

The objects nest: an **engine** (FreeType's library object) holds
**faces** (one face of one file), and a face holds **strikes** (the face at
one pixel size, with its glyph cache). Closing an outer object closes what
it holds. A **catalog** is apart: the names and metrics of every face a
program can choose from, and the choice. All of one engine is used by one
thread at a time; a program with several threads gives each its own
engine, or takes turns (as `winhost`'s threads already do).

| Call | What it does |
| --- | --- |
| `sx_font_engine_create`, `_destroy` | The engine |
| `sx_font_face_open`, `_close` | Face `index` of a file in memory, which must stay alive and unchanged while the face is open |
| `sx_font_face_info` | Family and style (name IDs 1 and 2, as Windows reads them), weight, italic, bold, fixed pitch, hinted, symbol, and the tables' metrics in font units |
| `sx_font_glyph_index` | A codepoint's glyph; a symbol font also answers U+0020–U+00FF from its U+F000 page, as Windows does |
| `sx_font_ppem_for_cell_height` | The em size for a positive `lfHeight`, a cell height |
| `sx_font_strike_open`, `_close`, `_metrics` | A size in pixels per em, 1 to 1000, with synthetic bold or italic, or a forced render mode |
| `sx_font_glyph_advance` | A glyph's advance in pixels, without drawing it |
| `sx_font_glyph_render` | A glyph's coverage, one byte per pixel, with its advance and its offsets from the origin |
| `sx_font_catalog_create`, `_add`, `_count`, `_entry`, `_match` | The faces to choose from, and the one a request gets |
| `sx_font_gfx_face` | A face for the platform's text primitives, drawn with a strike, to install in a slot of `libgfx2d` ([UI_FONTS.md](UI_FONTS.md#faces-at-run-time)) |
| `sx_font_error_string` | A static English description of a result |

Every call that can fail returns `SX_FONT_OK` or a negative
`SX_FONT_ERROR_*` and zeroes its output on error. The coverage a render
returns belongs to the strike and stays valid until that strike's next
render or its close.

A strike's sizes are in pixels; points are the caller's to convert
(`ppem = points * dpi / 72`, 96 DPI on SavanXP). Positioning, kerning and
line breaking are the caller's too: GDI does not kern unless asked, and
WordPad's control lays out lines from the advances.

## How it draws

### Hinting: the v35 interpreter

FreeType is built without `TT_CONFIG_OPTION_SUBPIXEL_HINTING`, which leaves
only its version 35 TrueType interpreter: the one upstream's own
configuration describes as "rendering behavior like on Windows without
ClearType, i.e., Windows XP without ClearType enabled and Win9x". The
fonts' instructions run as the font's designer wrote them for that
rasterizer, and the advances come from `hdmx` where a face has one, as
GDI's do. There is no auto-hinter: a face without instructions is drawn
unhinted.

### Smoothing: what gasp says

Each strike is hinted at every size and chooses 1-bit or grey from the
face's `gasp` table at its size, as Windows XP's standard font smoothing
does:

- **Hinting** runs whatever `GASP_GRIDFIT` says: XP's rasterizer runs the
  instructions even where gasp leaves it clear (Liberation's up to 8 ppem),
  and the advances `GetCharWidthW` reports there are the hinted ones
  (`text.exe`, [WIN32.md](../userland/WIN32.md#user32-and-gdi32), "Text").
- **`GASP_DOGRAY` set:** grey, coverage 0 to 255. Not set: 1-bit, every
  byte `0x00` or `0xff`.
- **No gasp table:** the ranges Liberation Sans ships: grey up to 8 ppem,
  1-bit from 9 to 17, grey from 18. That is SavanXP's choice for such a
  face, not a measurement of Windows.

So text at reading sizes is as crisp as the desktop's: Liberation Sans is
1-bit from 9 to 17 ppem, which covers 7 to 12 points at 96 DPI, and grey
only where a stair step would show, in headings. That keeps the reason
behind [UI_FONTS.md](UI_FONTS.md)'s rule (crisp text in a Win9x frame)
without applying its letter, which is about the tables the chrome is drawn
with. `SX_FONT_FORCE_MONO` (1-bit and hinted at every size) and
`SX_FONT_FORCE_GRAY` override the table for a caller that needs either.
There is no subpixel (ClearType) rendering:
`FT_CONFIG_OPTION_SUBPIXEL_RENDERING` stays off.

### Metrics as Windows reports them

A strike's metrics are what `GetTextMetrics` would say for that size, each
scaled from font units and rounded half up:

- **Ascent and descent** come from the face's `VDMX` record for the size
  when it has one (Arial at 8 ppem is 8 and 2, where the scaled metrics
  give 7 and 2), else from OS/2's `usWinAscent` and `usWinDescent`, not
  from `hhea`: GDI's cell is those two, and programs size lines from it.
  The height is their sum; the internal leading is the height less the em.
  `sx_font_ppem_for_cell_height` reads `VDMX` the same way, and
  `sx_font_face_info` says whether the face has it (`vdmx`), its caret
  slope and whether the file is signed (`SX_FONT_SIGNED`).
- **External leading** is the part of `hhea`'s line gap that the win
  metrics did not already take: `lineGap - ((winAscent + winDescent) -
  (ascender - descender))`, at least 0.
- **Average and largest width** come from `xAvgCharWidth` and
  `advanceWidthMax`; underline from `post`, strikeout from OS/2, each at
  least a pixel thick.
- **A fixed-pitch face** (`post`'s `isFixedPitch`) has one advance,
  `advanceWidthMax` scaled, whatever the hinting does to each glyph:
  Courier New at 16 ppem is 10 pixels for every glyph on Windows XP.

`gdi32` adds what is GDI's own on top (the 16.16 scale, the positive
heights, the outline metrics), measured in WP6 with `text.exe`: with
Liberation every metric equals Windows XP RTM's at every size it asks for;
with the user's own Arial, Times New Roman and Courier New, every metric
does and a few glyphs' advances differ by a pixel
([WIN32.md](../userland/WIN32.md#user32-and-gdi32), "Text").

### Synthetic bold and italic

A face without a bold is made bold as GDI makes one: each pixel takes the
larger of itself and its left neighbour, so the glyph is one column wider,
and its advance and the strike's widths grow by one. It is the rule
`gdi32` already applies to its bitmap strike. Italic is FreeType's own
oblique shear, about 12 degrees, applied to the hinted outline; embedded
bitmaps are skipped for such a strike, since a bitmap cannot be slanted.
Liberation ships all four styles, so the system's own faces never need
either.

### The glyph cache

Each strike keeps the glyphs it drew: a table of 1024 slots and an arena of
coverage that starts at 16 KiB and doubles as glyphs need it, up to 256 KiB.
Starting small matters because every program with an interface holds two
strikes ([UI_FONTS.md](UI_FONTS.md#interface-faces-from-files)). When the
table is three-quarters full, or the arena is at its largest and cannot take
the next glyph, both are emptied at once and filling starts over. It is the simplest bound that keeps a strike's memory fixed and makes
the same calls always give the same cache. A glyph larger than a quarter of
the arena is drawn into a scratch buffer of its own and not kept. Advances
asked for without drawing are cached in the same slots.

## Choosing a face

The catalog is built by the program from files it reads, one
`sx_font_catalog_add` each with a tag of its own (an index into its list of
paths). Only names and metrics are kept; the matched file is read again and
opened with `sx_font_face_open`. Matching, in order:

1. **The family asked for**, compared as Windows compares names: ASCII
   letters without case.
2. **A metric-compatible substitute**, both ways: Arial and Helvetica with
   Liberation Sans, Times New Roman and Times with Liberation Serif,
   Courier New and Courier with Liberation Mono. A document that names
   Arial lays out as it did on Windows XP; with the user's own Arial
   installed, Arial is the exact match and is used.
3. **The default face for the pitch asked for**: Liberation Mono for
   `SX_FONT_FIXED_PITCH`, Liberation Sans otherwise (then Courier New or
   Arial, then any face of that pitch, then the first face).

Within the family the face whose slant matches wins, then the nearest
weight. Bold is synthesized when the request is 600 or heavier and the face
is lighter than 600, italic when the request is italic and the face is not;
the match says which (`synth_flags`), and the caller passes them to
`sx_font_strike_open`. The match also says how it was found
(`SX_FONT_MATCH_EXACT`, `_SUBSTITUTE`, `_FALLBACK`), which is what
`EnumFontFamiliesEx` and the font dialog need to tell a real face from a
stand-in.

The UI faces are not in the catalog: MS Sans Serif, MS Shell Dlg and the
System font stay the baked strike in `gdi32` and in the toolkit
([UI_FONTS.md](UI_FONTS.md)).

## Where the font files live

| Directory | What | Source |
| --- | --- | --- |
| `/system/fonts/` | Liberation Sans, Serif and Mono, Regular, Bold, Italic and Bold Italic: twelve files | `assets/fonts/liberation/`, staged by `savanxp_diskfs` |
| `/system/win/drive_c/windows/fonts/` | The user's own Windows XP fonts, copied like the other files of the WordPad profile | The user; never in the repository |

`./build.sh build` brings `/system/fonts` up to date and leaves anything
else on the disk where it is, as for external programs in `/bin`. Liberation
2.1.5 is SIL OFL-1.1 and shipped unmodified
([THIRD_PARTY_PROVENANCE.md](../third-party/THIRD_PARTY_PROVENANCE.md#liberation-fonts-scalable-document-faces)).

## Win32: through winhost (WP6)

`gdi32` is 32-bit guest code and `libsxfont` is an x86-64 library, so
`gdi32` does not link it. `winhost` links `sxfont.c` and FreeType
statically and serves it to the guest (`loader/fonts.c`, services 58 to
62, shared with the harness like the others): it keeps a catalog of the
faces in `/system/fonts` and `C:\windows\fonts`, opens a strike for a
`LOGFONTW`, and copies advances and a glyph's coverage into guest memory;
`gdi32` caches each font's advances so that measuring a line is not a
service call per character. The harness links `freetype_host`, the same
sources built for Linux, so a fixture gives the same pixels in both. One
FreeType, built once per host architecture, rather than a second build for
`i686-pc-windows-msvc` inside `gdi32`. The coverage stays linear here;
`gdi32` blends it in linear light as XP does.

## The configuration

`system/font/freetype/ftoption.h` is upstream's file with its changed lines
marked `SavanXP: on` or `SavanXP: off`; `ftmodule.h` registers the four
modules compiled (`truetype`, `sfnt`, `smooth`, `raster`).

| Option | Set | Why |
| --- | --- | --- |
| `FT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT` | on | Memory only: no `fopen` in the library |
| `TT_CONFIG_OPTION_SUBPIXEL_HINTING` | off | Only the v35 interpreter, Windows XP's |
| `FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES` | off | No `getenv`: a program cannot change how text looks behind the library's back |
| `FT_CONFIG_OPTION_USE_ZLIB`, `_USE_LZW` | off | Compressed fonts are not read; WOFF is unsupported |
| `FT_CONFIG_OPTION_POSTSCRIPT_NAMES`, `_ADOBE_GLYPH_LIST`, `TT_CONFIG_OPTION_POSTSCRIPT_NAMES` | off | Glyph names are not needed, and `psnames` is not built |
| `TT_CONFIG_OPTION_GX_VAR_SUPPORT`, `_VARC` | off | No variable fonts |
| `TT_CONFIG_OPTION_COLOR_LAYERS`, `FT_CONFIG_OPTION_SVG` | off | No colour fonts |
| `TT_CONFIG_OPTION_BDF`, `FT_CONFIG_OPTION_MAC_FONTS`, `_GUESSING_EMBEDDED_RFORK`, `_INCREMENTAL` | off | Formats and loaders nothing uses |
| `AF_CONFIG_OPTION_CJK`, `_INDIC` | off | The auto-hinter is not built |
| `TT_CONFIG_OPTION_EMBEDDED_BITMAPS`, `_SFNT_NAMES`, the `cmap` formats, `TT_CONFIG_OPTION_BYTECODE_INTERPRETER` | on, as upstream | Hand-drawn strikes, family names, every cmap, the hinting |

Updating FreeType means comparing upstream's new `ftoption.h` with this one
option by option ([`vendor/freetype/README.md`](../../vendor/freetype/README.md)).

## Verification

`system/font/sxfont_selftest.c` is one self-test that runs twice:
`sxfont-test`, a host test over `assets/fonts/liberation`, and `fonttest`,
a SUN program that maps `libsxfont.so.0.4` and reads the same files from
`/system/fonts` (the `font-test` scenario). It checks:

- every face's names, weight, flags and win metrics, and the strike
  metrics at 13 and 100 ppem, against the fonts' own tables, read with
  fontTools and scaled by hand;
- the render mode and hinting gasp gives at the edges of each face's
  ranges, and the two overrides;
- the glyphs of a sample line at five sizes and faces, as checksums of
  their advances, offsets and coverage. They were recorded from this
  library and are the ones the build host's FreeType 2.15.0 gives with
  `interpreter-version` 35, computed by a separate program over the
  system's `libfreetype`;
- that 1-bit strikes hold only `0x00` and `0xff`, a space has no pixels,
  synthetic bold is the glyph drawn twice a pixel apart, a fixed-pitch face
  has one advance, and the cache gives back what was drawn after it was
  emptied and for a glyph larger than it;
- the catalog's matches, substitutes, fallbacks and synthesis, and the
  errors: a file of text, a CFF font, a truncated file, sizes out of range,
  a glyph past the face.
- that Liberation Sans installed in `libgfx2d`'s UI slot measures the
  strike's advances and draws, through `gfx_blit_text`, exactly the canvas
  composed glyph by glyph from the strike, that Liberation Mono gives the
  monospace slot an 8-pixel cell, and that the baked faces come back. On
  SavanXP this runs in `fonttest` against the mapped `libgfx2d.so.0.4`.

## Not done

- **Kerning and shaping.** `GetKerningPairs` and complex scripts are later
  needs; Uniscribe never loaded for WordPad's Latin text.
- **Subpixel (ClearType) rendering.** The SUN toolkit's text blends
  coverage linearly; only `gdi32` corrects for gamma.
- **Formats beyond TrueType** (CFF, Type 1, WOFF, `.fon`), and variable and
  colour fonts.
- **A catalog that scans directories.** The library opens no file, so each
  program (and in WP6, `winhost`) lists `/system/fonts` and the user's
  fonts itself.
