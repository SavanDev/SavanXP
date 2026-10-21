# UI fonts

How the desktop's text is drawn, and the rule that keeps it looking like a
solid Win9x/2000-era system rather than a modern toolkit in a grey costume.

## The faces

| Painter font | Face | Size | Source | Table |
|---|---|---|---|---|
| `SX_FONT_UI` | MS Sans Serif (Wine's) | 13px bitmap strike | `assets/desktop/fonts/ms_sans_serif.sfd` | `gfx_font_noto.inc` |
| `SX_FONT_UI_TITLE` | RetroSans | 17px | `assets/desktop/fonts/RetroSans.ttf` | `gfx_font_title.inc` |
| `SX_FONT_MONO` | GNU UniFont | 8x16 | `assets/desktop/fonts/unifont.hex` | `console_font_unifont.inc` |

The body tables keep their historical `SX_NOTO_*` / `kNoto*` names: the body
face used to be Noto Sans, and renaming the symbols would touch every consumer
(`gfx_impl.inc`, `tools/glyphs.py`, the host tests) for no change in behaviour.

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

`gfx2d-test` enforces this: `case_ui_fonts_are_one_bit_and_fit_their_line`
fails on any grey coverage byte, and on any glyph that pokes out of the line
its ascent and descent declare.

## Changing a face

1. Put the source in `assets/desktop/fonts/` with its license, and add it to
   `docs/third-party/THIRD_PARTY_PROVENANCE.md`.
2. Find the size at which it is 1-bit (for a TTF, render a sample at every
   size and keep the one with no grey pixels).
3. Bake it with the command in `assets/README.md`, keeping the table's prefix.
4. Run `./build.sh test` (the 1-bit check), `./build.sh smoke sxgui-smoke`
   (the on-screen glyph check, which reads the tables back out of the built
   `libsxgfx`) and look at `tools/shoot.sh` captures: layouts follow the line
   height, so a different height reflows menus, lists and tabs.

## Coverage gaps

A bitmap strike is drawn for one code page, so some of the punctuation the
tables cover (`NOTO_RANGES` in `genfont.py`) is not in Wine's 13px strike.
Hyphen, non-breaking hyphen, figure dash and one dot leader are drawn with
their ASCII twins (`SFD_ALIASES`); the rest (U+2016, U+2017, U+201B, U+201F,
U+2023, U+2025, U+2027) advance like a space, the same contract as any
codepoint outside the table. `genfont.py` lists them every time it bakes.
