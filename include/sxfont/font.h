#pragma once

#include <stddef.h>
#include <stdint.h>

/* libsxfont: scalable TrueType fonts, rasterized at run time
 * (docs/desktop/FONT_LIBRARY.md). Shared by SUN, POSIX and, through
 * winhost, Win32's gdi32.
 *
 * Memory only: the library opens no file and makes no system call. The
 * caller reads a font file and hands over its bytes, which must stay
 * unchanged and alive as long as any face opened from them. Everything an
 * engine owns (faces, strikes, catalogs) is used by one thread at a time.
 *
 * Every call that can fail returns SX_FONT_OK (zero) or a negative
 * SX_FONT_ERROR_*. Output structures are zeroed on error. */

#define SX_FONT_OK 0
#define SX_FONT_ERROR_ARGUMENT -1    /* null pointer, size or index out of range */
#define SX_FONT_ERROR_MEMORY -2      /* an allocation failed */
#define SX_FONT_ERROR_FORMAT -3      /* not a TrueType file, or a damaged one */
#define SX_FONT_ERROR_UNSUPPORTED -4 /* a font the library does not draw: CFF, Type 1, a bitmap font */
#define SX_FONT_ERROR_SIZE -5        /* a pixel size outside 1..SX_FONT_MAX_PPEM */
#define SX_FONT_ERROR_GLYPH -6       /* a glyph index past the face, or a glyph that does not load */
#define SX_FONT_ERROR_NOT_FOUND -7   /* a catalog with no face to offer */

#define SX_FONT_MAX_PPEM 1000u
#define SX_FONT_NAME_MAX 64u

/* Face flags (struct sx_font_face_info.flags, struct sx_font_request.flags). */
#define SX_FONT_ITALIC 0x0001u       /* the face is italic or oblique */
#define SX_FONT_BOLD 0x0002u         /* OS/2 says bold (fsSelection) */
#define SX_FONT_FIXED_PITCH 0x0004u  /* every glyph has one advance */
#define SX_FONT_HINTED 0x0008u       /* the face carries TrueType instructions */
#define SX_FONT_SYMBOL 0x0010u       /* a symbol font: its cmap is (3,0) */
#define SX_FONT_SIGNED 0x0020u       /* the file carries a digital signature (DSIG) */

/* Strike flags (struct sx_font_strike_options.flags). */
#define SX_FONT_SYNTH_BOLD 0x0100u   /* embolden: one pixel wider, one more of advance */
#define SX_FONT_SYNTH_ITALIC 0x0200u /* slant the outlines */
#define SX_FONT_FORCE_MONO 0x0400u   /* 1-bit at every size, whatever gasp says */
#define SX_FONT_FORCE_GRAY 0x0800u   /* grey at every size, whatever gasp says */

/* How a strike draws (struct sx_font_strike_metrics.render). */
#define SX_FONT_RENDER_MONO 1u       /* every coverage byte is 0x00 or 0xff */
#define SX_FONT_RENDER_GRAY 2u       /* coverage 0..255 */

struct sx_font_engine;
struct sx_font_face;
struct sx_font_strike;
struct sx_font_catalog;

/* A face as its file describes it, in font units unless named otherwise. */
struct sx_font_face_info {
    char family[SX_FONT_NAME_MAX];   /* UTF-8, name ID 1 as Windows reads it */
    char style[SX_FONT_NAME_MAX];    /* name ID 2: "Regular", "Bold Italic" */
    unsigned faces_in_file;          /* more than one in a .ttc */
    unsigned weight;                 /* OS/2 usWeightClass, 100..900 */
    unsigned flags;                  /* SX_FONT_ITALIC, _BOLD, _FIXED_PITCH, _HINTED, _SYMBOL */
    unsigned glyph_count;
    unsigned units_per_em;
    int win_ascent, win_descent;     /* OS/2 usWinAscent, usWinDescent, both positive */
    int hhea_ascent, hhea_descent, hhea_line_gap; /* descent negative, as in the table */
    int avg_char_width;              /* OS/2 xAvgCharWidth */
    int max_advance;                 /* hhea advanceWidthMax */
    int underline_position, underline_thickness; /* post table */
    int strikeout_position, strikeout_size;      /* OS/2 */
    uint32_t code_page_range[2];     /* OS/2 ulCodePageRange1 and 2 */
    uint8_t panose[10];
    /* What GetOutlineTextMetrics and font enumeration report. */
    int x_min, y_min, x_max, y_max;  /* head's bounding box */
    int typo_ascender, typo_descender, typo_line_gap; /* OS/2, descender negative */
    int cap_height, x_height;        /* OS/2 version 2 and later, else 0 */
    unsigned fs_selection, fs_type;  /* OS/2 */
    int32_t italic_angle;            /* post, degrees in 16.16 */
    unsigned first_char, last_char;  /* OS/2 usFirstCharIndex, usLastCharIndex */
    char full_name[SX_FONT_NAME_MAX]; /* UTF-8, name ID 4: "Liberation Sans Bold" */
    uint32_t unicode_range[4];       /* OS/2 ulUnicodeRange1 to 4 */
    unsigned lowest_ppem;            /* head lowestRecPPEM */
    int caret_slope_rise, caret_slope_run; /* hhea */
    unsigned vdmx;                   /* nonzero: VDMX sets the extents by size */
};

struct sx_font_strike_options {
    unsigned ppem;                   /* pixels per em, 1..SX_FONT_MAX_PPEM */
    unsigned flags;                  /* SX_FONT_SYNTH_*, SX_FONT_FORCE_* */
};

/* A strike's metrics in whole pixels, rounded as Windows rounds them. */
struct sx_font_strike_metrics {
    unsigned ppem;
    unsigned render;                 /* SX_FONT_RENDER_MONO or _GRAY */
    unsigned hinted;                 /* 1 when the glyphs are grid-fitted */
    int ascent, descent;             /* from usWinAscent and usWinDescent */
    int height;                      /* ascent + descent */
    int internal_leading;            /* height - ppem */
    int external_leading;            /* the line gap Windows adds, at least 0 */
    int avg_char_width, max_advance; /* plus one with SX_FONT_SYNTH_BOLD */
    int underline_position;          /* below the baseline, positive downwards */
    int underline_thickness;         /* at least 1 */
    int strikeout_position;          /* above the baseline */
    int strikeout_size;              /* at least 1 */
};

/* One drawn glyph. `coverage` is width*height bytes, row by row from the
 * top, `pitch` bytes apart; it belongs to the strike and stays valid until
 * the next sx_font_glyph_render or sx_font_strike_close on that strike. */
struct sx_font_glyph {
    int advance;                     /* pixels to the next glyph's origin */
    int left;                        /* from the origin to the bitmap's left edge */
    int top;                         /* from the baseline up to the bitmap's top row */
    unsigned width, height, pitch;
    const uint8_t *coverage;         /* NULL when the glyph has no pixels (a space) */
};

/* What a program asks the catalog for. */
struct sx_font_request {
    const char *family;              /* UTF-8; NULL or "" for the default face */
    unsigned weight;                 /* 0 means 400 */
    unsigned flags;                  /* SX_FONT_ITALIC, SX_FONT_FIXED_PITCH */
};

#define SX_FONT_MATCH_EXACT 1u       /* the family asked for */
#define SX_FONT_MATCH_SUBSTITUTE 2u  /* a metric-compatible face (Arial -> Liberation Sans) */
#define SX_FONT_MATCH_FALLBACK 3u    /* the default face for the pitch asked for */

/* What the catalog offers: which file (the caller's tag), which face in it,
 * and what to synthesize with sx_font_strike_open. */
struct sx_font_match {
    uint32_t tag;
    unsigned face_index;
    unsigned synth_flags;            /* SX_FONT_SYNTH_BOLD, SX_FONT_SYNTH_ITALIC */
    unsigned quality;                /* SX_FONT_MATCH_* */
    char family[SX_FONT_NAME_MAX];   /* the family offered */
};

#ifdef __cplusplus
extern "C" {
#endif

/* The engine: FreeType's library object and the faces opened in it. */
int sx_font_engine_create(struct sx_font_engine **out);
void sx_font_engine_destroy(struct sx_font_engine *engine);

/* Opens face `index` (0 unless the file is a collection) of a TrueType file
 * held in memory. Closing the engine closes its faces too. */
int sx_font_face_open(struct sx_font_engine *engine, const void *data, size_t size, unsigned index,
                      struct sx_font_face **out);
void sx_font_face_close(struct sx_font_face *face);
int sx_font_face_info(const struct sx_font_face *face, struct sx_font_face_info *out);

/* The glyph a Unicode codepoint maps to, or 0 (.notdef) when the face has
 * none. A symbol font also answers U+0020..U+00FF from its U+F000 page, as
 * Windows does. */
unsigned sx_font_glyph_index(const struct sx_font_face *face, uint32_t codepoint);

/* The em size of a font asked for by its cell height (a positive LOGFONT
 * lfHeight): the ppem whose ascent plus descent is closest to `height`. */
int sx_font_ppem_for_cell_height(const struct sx_font_face *face, unsigned height, unsigned *ppem);

/* A face at one size, with its glyph cache. Closing the face closes its
 * strikes. */
int sx_font_strike_open(struct sx_font_face *face, const struct sx_font_strike_options *options,
                        struct sx_font_strike **out);
void sx_font_strike_close(struct sx_font_strike *strike);
int sx_font_strike_metrics(const struct sx_font_strike *strike, struct sx_font_strike_metrics *out);

/* The advance of one glyph, in pixels, without drawing it. */
int sx_font_glyph_advance(struct sx_font_strike *strike, unsigned glyph, int *advance);
int sx_font_glyph_render(struct sx_font_strike *strike, unsigned glyph, struct sx_font_glyph *out);

/* The faces a program can choose from. Each file is read when it is added,
 * and only its names and metrics are kept: the caller may free the bytes,
 * and opens the matched file again with sx_font_face_open. */
int sx_font_catalog_create(struct sx_font_engine *engine, struct sx_font_catalog **out);
void sx_font_catalog_destroy(struct sx_font_catalog *catalog);
/* Adds every face of a file; returns how many (at least 1) or an error. */
int sx_font_catalog_add(struct sx_font_catalog *catalog, const void *data, size_t size, uint32_t tag);
unsigned sx_font_catalog_count(const struct sx_font_catalog *catalog);
/* Face `index` of the catalog, in the order they were added. */
int sx_font_catalog_entry(const struct sx_font_catalog *catalog, unsigned index, struct sx_font_face_info *info,
                          uint32_t *tag, unsigned *face_index);
int sx_font_catalog_match(const struct sx_font_catalog *catalog, const struct sx_font_request *request,
                          struct sx_font_match *out);

/* A face for the platform's text primitives (sxgfx/pixels.h,
 * gfx_face_install) that draws with this strike: its ascent, its height plus
 * its external leading as the line (the line spacing the face asks for), the
 * advance of 'M' as the monospace cell. The face points at the
 * strike, so it is removed from its slot (gfx_face_install(slot, NULL))
 * before the strike closes. */
struct gfx_face;
int sx_font_gfx_face(struct sx_font_strike *strike, struct gfx_face *out);

/* A static English description of a result; never NULL. */
const char *sx_font_error_string(int result);

#ifdef __cplusplus
}
#endif
