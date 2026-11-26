# FreeType, vendored

FreeType 2.15.0, the font engine under `libsxfont`
([`docs/desktop/FONT_LIBRARY.md`](../../docs/desktop/FONT_LIBRARY.md)). The
release, the archive's checksum and its signature are in
`FREETYPE_RELEASE.txt`. SavanXP takes FreeType under the FreeType License,
`docs/FTL.TXT`; `LICENSE.TXT` is the release's statement of the two licenses
it is offered under.

Every file here is byte-for-byte the release's. Do not edit them: a change in
behaviour goes in the configuration (`system/font/freetype/`) or in
`libsxfont` itself.

Only what `libsxfont` builds, and the public headers, are kept:

| Kept | What it is |
| --- | --- |
| `include/` | The public and internal headers, without `include/dlg/` |
| `src/base/` | The base layer |
| `src/sfnt/` | The SFNT tables (`cmap`, `hmtx`, `OS/2`, `gasp`, embedded bitmaps) |
| `src/truetype/` | The TrueType glyph loader and the bytecode interpreter |
| `src/smooth/` | The anti-aliasing rasterizer |
| `src/raster/` | The monochrome rasterizer |

Left out of the release on purpose:

| Left out | Why |
| --- | --- |
| `include/dlg/` | The Boost-licensed logging library, used only with `FT_DEBUG_LOGGING` |
| `src/` other modules | Not built: CFF, Type 1, the auto-hinter, BDF, PCF, the compressors, SVG, SDF and the rest |
| `docs/GPLv2.TXT` | The license SavanXP does not take |
| `builds/`, `devel/`, `tests/`, `objs/`, `subprojects/`, the other build systems | `cmake/FreeType.cmake` builds the library |

Two files that are compiled are not under the FreeType License:
`src/base/fthash.c`, included by `ftbase.c`, carries an X11-style notice
(`assets/licenses/freetype-fthash.txt`), and `src/base/md5.c` is public
domain and compiled only with `FT_DEBUG_LEVEL_TRACE`, which SavanXP never
defines.

To update: download the new archive and its `.sig`, check the signature,
copy the same directories over, compare upstream's
`include/freetype/config/ftoption.h` with `system/font/freetype/ftoption.h`
option by option, and update `FREETYPE_RELEASE.txt` and
`docs/third-party/THIRD_PARTY_PROVENANCE.md`.
