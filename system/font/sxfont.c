/* libsxfont: scalable TrueType fonts over FreeType
 * (savanxp/font.h, docs/desktop/FONT_LIBRARY.md).
 *
 * Memory only and call-free: FreeType is built without file streams
 * (FT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT) and gets its memory from the
 * program's malloc. What is decided here rather than in FreeType -- the
 * render mode from gasp, the metrics Windows reports, the synthetic bold,
 * the choice of face -- is the document's, section by section. */

#include "savanxp/font.h"
#include "sxgfx/pixels.h"

#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GASP_H
#include FT_OUTLINE_H
#include FT_SFNT_NAMES_H
#include FT_SIZES_H
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

/* --- Objects ------------------------------------------------------------- */

struct sx_font_engine {
    FT_Library library;
    struct sx_font_face *faces;
};

struct sx_font_face {
    struct sx_font_engine *engine;
    struct sx_font_face *next;
    FT_Face ft;
    struct sx_font_face_info info;
    int symbol;
    struct sx_font_strike *strikes;
};

/* The glyph cache: an open-addressed table of glyphs and an arena of their
 * coverage. When either fills, both are emptied, so what a strike keeps is
 * bounded and the same calls always give the same cache. */
#define CACHE_SLOTS 1024u
#define CACHE_ARENA (256u * 1024u)
#define CACHE_NO_BITMAP 0xffffffffu

struct cache_entry {
    unsigned glyph_plus_one;     /* 0: empty slot */
    int advance, left, top;
    unsigned width, height;
    unsigned offset;             /* into the arena, or CACHE_NO_BITMAP */
};

struct sx_font_strike {
    struct sx_font_face *face;
    struct sx_font_strike *next;
    FT_Size size;
    struct sx_font_strike_options options;
    struct sx_font_strike_metrics metrics;
    FT_Int32 load_flags;
    FT_Render_Mode render_mode;
    struct cache_entry *slots;
    unsigned used_slots;
    uint8_t *arena;
    unsigned arena_used;
    uint8_t *scratch;            /* a glyph too large for the arena */
    size_t scratch_size;
};

struct catalog_entry {
    struct sx_font_face_info info;
    uint32_t tag;
    unsigned face_index;
};

struct sx_font_catalog {
    struct sx_font_engine *engine;
    struct catalog_entry *entries;
    unsigned count, capacity;
};

/* --- Errors -------------------------------------------------------------- */

const char *sx_font_error_string(int result) {
    switch (result) {
    case SX_FONT_OK: return "success";
    case SX_FONT_ERROR_ARGUMENT: return "invalid argument";
    case SX_FONT_ERROR_MEMORY: return "out of memory";
    case SX_FONT_ERROR_FORMAT: return "not a TrueType font, or a damaged one";
    case SX_FONT_ERROR_UNSUPPORTED: return "unsupported font format";
    case SX_FONT_ERROR_SIZE: return "pixel size out of range";
    case SX_FONT_ERROR_GLYPH: return "glyph out of range or unreadable";
    case SX_FONT_ERROR_NOT_FOUND: return "no face in the catalog";
    default: return "unknown font error";
    }
}

static int from_freetype(FT_Error error) {
    switch (error) {
    case FT_Err_Ok: return SX_FONT_OK;
    case FT_Err_Out_Of_Memory: return SX_FONT_ERROR_MEMORY;
    case FT_Err_Invalid_Argument: return SX_FONT_ERROR_ARGUMENT;
    case FT_Err_Invalid_Glyph_Index: return SX_FONT_ERROR_GLYPH;
    case FT_Err_Unknown_File_Format: return SX_FONT_ERROR_FORMAT;
    default: return SX_FONT_ERROR_FORMAT;
    }
}

/* --- Engine -------------------------------------------------------------- */

int sx_font_engine_create(struct sx_font_engine **out) {
    if (out == NULL) return SX_FONT_ERROR_ARGUMENT;
    *out = NULL;
    struct sx_font_engine *engine = calloc(1, sizeof *engine);
    if (engine == NULL) return SX_FONT_ERROR_MEMORY;
    FT_Error error = FT_Init_FreeType(&engine->library);
    if (error != FT_Err_Ok) {
        free(engine);
        return error == FT_Err_Out_Of_Memory ? SX_FONT_ERROR_MEMORY : SX_FONT_ERROR_FORMAT;
    }
    *out = engine;
    return SX_FONT_OK;
}

void sx_font_engine_destroy(struct sx_font_engine *engine) {
    if (engine == NULL) return;
    while (engine->faces != NULL) sx_font_face_close(engine->faces);
    FT_Done_FreeType(engine->library);
    free(engine);
}

/* --- Faces --------------------------------------------------------------- */

static unsigned scale_round(long value, unsigned ppem, unsigned units_per_em) {
    /* value * ppem / units_per_em, rounded half up, for value >= 0. */
    if (value <= 0 || units_per_em == 0) return 0;
    unsigned long long scaled = (unsigned long long)value * ppem * 2u + units_per_em;
    return (unsigned)(scaled / (2ull * units_per_em));
}

static int scale_signed(long value, unsigned ppem, unsigned units_per_em) {
    return value < 0 ? -(int)scale_round(-value, ppem, units_per_em)
                     : (int)scale_round(value, ppem, units_per_em);
}

/* UTF-16BE to UTF-8, cut at a character boundary to fit `size`. */
static void utf16be_to_utf8(const FT_Byte *in, FT_UInt length, char *out, size_t size) {
    size_t used = 0;
    for (FT_UInt i = 0; i + 1 < length;) {
        uint32_t c = ((uint32_t)in[i] << 8) | in[i + 1];
        i += 2;
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < length) {
            uint32_t low = ((uint32_t)in[i] << 8) | in[i + 1];
            if (low >= 0xdc00 && low < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                i += 2;
            }
        }
        if (c == 0) break;
        char bytes[4];
        size_t n;
        if (c < 0x80) { bytes[0] = (char)c; n = 1; }
        else if (c < 0x800) { bytes[0] = (char)(0xc0 | (c >> 6)); bytes[1] = (char)(0x80 | (c & 0x3f)); n = 2; }
        else if (c < 0x10000) {
            bytes[0] = (char)(0xe0 | (c >> 12)); bytes[1] = (char)(0x80 | ((c >> 6) & 0x3f));
            bytes[2] = (char)(0x80 | (c & 0x3f)); n = 3;
        } else {
            bytes[0] = (char)(0xf0 | (c >> 18)); bytes[1] = (char)(0x80 | ((c >> 12) & 0x3f));
            bytes[2] = (char)(0x80 | ((c >> 6) & 0x3f)); bytes[3] = (char)(0x80 | (c & 0x3f)); n = 4;
        }
        if (used + n + 1 > size) break;
        memcpy(out + used, bytes, n);
        used += n;
    }
    out[used] = '\0';
}

/* Name `id` as Windows reads it: platform 3, US English first, then any
 * Windows language; FreeType's own name when the table has neither. */
static void read_name(FT_Face ft, FT_UShort id, const char *fallback, char *out, size_t size) {
    FT_UInt count = FT_Get_Sfnt_Name_Count(ft);
    int best = -1, best_rank = 0;
    for (FT_UInt i = 0; i < count; i++) {
        FT_SfntName name;
        if (FT_Get_Sfnt_Name(ft, i, &name) != FT_Err_Ok) continue;
        if (name.platform_id != TT_PLATFORM_MICROSOFT || name.name_id != id) continue;
        if (name.encoding_id != TT_MS_ID_UNICODE_CS && name.encoding_id != TT_MS_ID_SYMBOL_CS) continue;
        int rank = name.language_id == TT_MS_LANGID_ENGLISH_UNITED_STATES ? 2 : 1;
        if (rank > best_rank) { best = (int)i; best_rank = rank; }
    }
    if (best >= 0) {
        FT_SfntName name;
        if (FT_Get_Sfnt_Name(ft, (FT_UInt)best, &name) == FT_Err_Ok) {
            utf16be_to_utf8(name.string, name.string_len, out, size);
            if (out[0] != '\0') return;
        }
    }
    size_t n = fallback != NULL ? strlen(fallback) : 0;
    if (n >= size) n = size - 1;
    if (n > 0) memcpy(out, fallback, n);
    out[n] = '\0';
}

static int has_table(FT_Face ft, FT_ULong tag) {
    FT_ULong length = 0;
    return FT_Load_Sfnt_Table(ft, tag, 0, NULL, &length) == FT_Err_Ok && length > 0;
}

static void fill_info(struct sx_font_face *face) {
    FT_Face ft = face->ft;
    struct sx_font_face_info *info = &face->info;
    memset(info, 0, sizeof *info);
    read_name(ft, TT_NAME_ID_FONT_FAMILY, ft->family_name, info->family, sizeof info->family);
    read_name(ft, TT_NAME_ID_FONT_SUBFAMILY, ft->style_name, info->style, sizeof info->style);
    info->faces_in_file = (unsigned)ft->num_faces;
    info->glyph_count = (unsigned)ft->num_glyphs;
    info->units_per_em = ft->units_per_EM;

    const TT_OS2 *os2 = FT_Get_Sfnt_Table(ft, FT_SFNT_OS2);
    const TT_HoriHeader *hhea = FT_Get_Sfnt_Table(ft, FT_SFNT_HHEA);
    const TT_Postscript *post = FT_Get_Sfnt_Table(ft, FT_SFNT_POST);
    if (hhea != NULL) {
        info->hhea_ascent = hhea->Ascender;
        info->hhea_descent = hhea->Descender;
        info->hhea_line_gap = hhea->Line_Gap;
        info->max_advance = hhea->advance_Width_Max;
    }
    if (os2 != NULL && os2->version != 0xffff) {
        info->weight = os2->usWeightClass;
        info->win_ascent = os2->usWinAscent;
        info->win_descent = os2->usWinDescent;
        info->avg_char_width = os2->xAvgCharWidth;
        info->strikeout_position = os2->yStrikeoutPosition;
        info->strikeout_size = os2->yStrikeoutSize;
        if (os2->version >= 1) {
            info->code_page_range[0] = (uint32_t)os2->ulCodePageRange1;
            info->code_page_range[1] = (uint32_t)os2->ulCodePageRange2;
        }
        memcpy(info->panose, os2->panose, sizeof info->panose);
        if (os2->fsSelection & 0x0001u) info->flags |= SX_FONT_ITALIC;
        if (os2->fsSelection & 0x0020u) info->flags |= SX_FONT_BOLD;
    } else {
        info->weight = (ft->style_flags & FT_STYLE_FLAG_BOLD) ? 700u : 400u;
        info->win_ascent = info->hhea_ascent;
        info->win_descent = -info->hhea_descent;
        info->avg_char_width = info->max_advance / 2;
        info->strikeout_position = info->win_ascent / 3;
        info->strikeout_size = (int)info->units_per_em / 20;
    }
    if (info->weight == 0) info->weight = 400;
    if (ft->style_flags & FT_STYLE_FLAG_ITALIC) info->flags |= SX_FONT_ITALIC;
    if (ft->style_flags & FT_STYLE_FLAG_BOLD) info->flags |= SX_FONT_BOLD;
    if (FT_IS_FIXED_WIDTH(ft)) info->flags |= SX_FONT_FIXED_PITCH;
    if (has_table(ft, TTAG_fpgm) || has_table(ft, TTAG_prep)) info->flags |= SX_FONT_HINTED;
    if (face->symbol) info->flags |= SX_FONT_SYMBOL;
    if (post != NULL) {
        info->underline_position = post->underlinePosition;
        info->underline_thickness = post->underlineThickness;
    }
}

/* What the first four bytes say the file is. */
static int classify(const unsigned char *data, size_t size) {
    if (size < 12) return SX_FONT_ERROR_FORMAT;
    if (memcmp(data, "\x00\x01\x00\x00", 4) == 0 || memcmp(data, "true", 4) == 0 ||
        memcmp(data, "ttcf", 4) == 0)
        return SX_FONT_OK;
    if (memcmp(data, "OTTO", 4) == 0 || memcmp(data, "%!", 2) == 0 || memcmp(data, "MZ", 2) == 0 ||
        memcmp(data, "wOFF", 4) == 0 || memcmp(data, "wOF2", 4) == 0 || memcmp(data, "typ1", 4) == 0)
        return SX_FONT_ERROR_UNSUPPORTED;
    return SX_FONT_ERROR_FORMAT;
}

int sx_font_face_open(struct sx_font_engine *engine, const void *data, size_t size, unsigned index,
                      struct sx_font_face **out) {
    if (out == NULL) return SX_FONT_ERROR_ARGUMENT;
    *out = NULL;
    if (engine == NULL || data == NULL || size == 0 || size > 0x7fffffffu || index > 0xffffu)
        return SX_FONT_ERROR_ARGUMENT;
    int kind = classify(data, size);
    if (kind != SX_FONT_OK) return kind;

    struct sx_font_face *face = calloc(1, sizeof *face);
    if (face == NULL) return SX_FONT_ERROR_MEMORY;
    FT_Error error = FT_New_Memory_Face(engine->library, data, (FT_Long)size, (FT_Long)index, &face->ft);
    if (error != FT_Err_Ok) {
        free(face);
        return from_freetype(error);
    }
    if (!FT_IS_SCALABLE(face->ft) || !FT_IS_SFNT(face->ft)) {
        FT_Done_Face(face->ft);
        free(face);
        return SX_FONT_ERROR_UNSUPPORTED;
    }
    /* Unicode when the font maps it; a symbol font only has (3,0). */
    if (FT_Select_Charmap(face->ft, FT_ENCODING_UNICODE) != FT_Err_Ok) {
        if (FT_Select_Charmap(face->ft, FT_ENCODING_MS_SYMBOL) == FT_Err_Ok) face->symbol = 1;
    }
    face->engine = engine;
    fill_info(face);
    face->next = engine->faces;
    engine->faces = face;
    *out = face;
    return SX_FONT_OK;
}

void sx_font_face_close(struct sx_font_face *face) {
    if (face == NULL) return;
    while (face->strikes != NULL) sx_font_strike_close(face->strikes);
    struct sx_font_face **link = &face->engine->faces;
    while (*link != NULL && *link != face) link = &(*link)->next;
    if (*link == face) *link = face->next;
    FT_Done_Face(face->ft);
    free(face);
}

int sx_font_face_info(const struct sx_font_face *face, struct sx_font_face_info *out) {
    if (out == NULL) return SX_FONT_ERROR_ARGUMENT;
    if (face == NULL) {
        memset(out, 0, sizeof *out);
        return SX_FONT_ERROR_ARGUMENT;
    }
    *out = face->info;
    return SX_FONT_OK;
}

unsigned sx_font_glyph_index(const struct sx_font_face *face, uint32_t codepoint) {
    if (face == NULL || face->ft->charmap == NULL) return 0;
    FT_UInt glyph = FT_Get_Char_Index(face->ft, codepoint);
    if (glyph == 0 && face->symbol && codepoint >= 0x20 && codepoint <= 0xff)
        glyph = FT_Get_Char_Index(face->ft, 0xf000u | codepoint);
    return glyph;
}

static int cell_height_at(const struct sx_font_face_info *info, unsigned ppem) {
    return (int)(scale_round(info->win_ascent, ppem, info->units_per_em) +
                 scale_round(info->win_descent, ppem, info->units_per_em));
}

int sx_font_ppem_for_cell_height(const struct sx_font_face *face, unsigned height, unsigned *ppem) {
    if (ppem == NULL) return SX_FONT_ERROR_ARGUMENT;
    *ppem = 0;
    if (face == NULL || height == 0) return SX_FONT_ERROR_ARGUMENT;
    const struct sx_font_face_info *info = &face->info;
    long cell = (long)info->win_ascent + info->win_descent;
    if (cell <= 0) return SX_FONT_ERROR_FORMAT;
    unsigned guess = scale_round((long)height, info->units_per_em, (unsigned)cell);
    unsigned best = 0;
    int best_distance = 0;
    for (unsigned candidate = guess > 1 ? guess - 1 : 1; candidate <= guess + 1; candidate++) {
        if (candidate < 1 || candidate > SX_FONT_MAX_PPEM) continue;
        int distance = cell_height_at(info, candidate) - (int)height;
        if (distance < 0) distance = -distance;
        if (best == 0 || distance < best_distance) {
            best = candidate;
            best_distance = distance;
        }
    }
    if (best == 0) return SX_FONT_ERROR_SIZE;
    *ppem = best;
    return SX_FONT_OK;
}

/* --- Strikes ------------------------------------------------------------- */

/* Without a gasp table, the ranges Liberation Sans ships: grey and not
 * grid-fitted up to 8 ppem, 1-bit and hinted up to 17, grey and hinted
 * above. */
static void choose_rendering(struct sx_font_strike *strike) {
    unsigned ppem = strike->options.ppem;
    unsigned flags = strike->options.flags;
    FT_Int gasp = FT_Get_Gasp(strike->face->ft, ppem);
    int gridfit, gray;
    if (gasp == FT_GASP_NO_TABLE) {
        gridfit = ppem > 8;
        gray = ppem <= 8 || ppem >= 18;
    } else {
        gridfit = (gasp & FT_GASP_DO_GRIDFIT) != 0;
        gray = (gasp & FT_GASP_DO_GRAY) != 0;
    }
    if (flags & SX_FONT_FORCE_MONO) {
        gray = 0;
        gridfit = 1;
    } else if (flags & SX_FONT_FORCE_GRAY) {
        gray = 1;
    }
    strike->metrics.render = gray ? SX_FONT_RENDER_GRAY : SX_FONT_RENDER_MONO;
    strike->metrics.hinted = (unsigned)gridfit;
    if (!gridfit) strike->load_flags = FT_LOAD_NO_HINTING;
    else strike->load_flags = gray ? FT_LOAD_TARGET_NORMAL : FT_LOAD_TARGET_MONO;
    if (flags & SX_FONT_SYNTH_ITALIC) strike->load_flags |= FT_LOAD_NO_BITMAP;
    strike->render_mode = gray ? FT_RENDER_MODE_NORMAL : FT_RENDER_MODE_MONO;
}

static void compute_metrics(struct sx_font_strike *strike) {
    const struct sx_font_face_info *info = &strike->face->info;
    struct sx_font_strike_metrics *m = &strike->metrics;
    unsigned ppem = strike->options.ppem, em = info->units_per_em;
    int bold = (strike->options.flags & SX_FONT_SYNTH_BOLD) != 0;
    m->ppem = ppem;
    m->ascent = (int)scale_round(info->win_ascent, ppem, em);
    m->descent = (int)scale_round(info->win_descent, ppem, em);
    m->height = m->ascent + m->descent;
    m->internal_leading = m->height - (int)ppem;
    if (m->internal_leading < 0) m->internal_leading = 0;
    /* The line gap Windows adds is what hhea's leaves after the win
     * metrics have taken their part of it. */
    long gap = (long)info->hhea_line_gap -
               (((long)info->win_ascent + info->win_descent) - ((long)info->hhea_ascent - info->hhea_descent));
    m->external_leading = gap > 0 ? (int)scale_round(gap, ppem, em) : 0;
    m->avg_char_width = (int)scale_round(info->avg_char_width, ppem, em) + bold;
    m->max_advance = (int)scale_round(info->max_advance, ppem, em) + bold;
    m->underline_position = -scale_signed(info->underline_position, ppem, em);
    m->underline_thickness = (int)scale_round(info->underline_thickness, ppem, em);
    if (m->underline_thickness < 1) m->underline_thickness = 1;
    m->strikeout_position = scale_signed(info->strikeout_position, ppem, em);
    m->strikeout_size = (int)scale_round(info->strikeout_size, ppem, em);
    if (m->strikeout_size < 1) m->strikeout_size = 1;
}

int sx_font_strike_open(struct sx_font_face *face, const struct sx_font_strike_options *options,
                        struct sx_font_strike **out) {
    if (out == NULL) return SX_FONT_ERROR_ARGUMENT;
    *out = NULL;
    if (face == NULL || options == NULL) return SX_FONT_ERROR_ARGUMENT;
    const unsigned known = SX_FONT_SYNTH_BOLD | SX_FONT_SYNTH_ITALIC | SX_FONT_FORCE_MONO | SX_FONT_FORCE_GRAY;
    if ((options->flags & ~known) != 0 ||
        (options->flags & (SX_FONT_FORCE_MONO | SX_FONT_FORCE_GRAY)) == (SX_FONT_FORCE_MONO | SX_FONT_FORCE_GRAY))
        return SX_FONT_ERROR_ARGUMENT;
    if (options->ppem < 1 || options->ppem > SX_FONT_MAX_PPEM) return SX_FONT_ERROR_SIZE;

    struct sx_font_strike *strike = calloc(1, sizeof *strike);
    if (strike == NULL) return SX_FONT_ERROR_MEMORY;
    strike->slots = calloc(CACHE_SLOTS, sizeof *strike->slots);
    strike->arena = malloc(CACHE_ARENA);
    if (strike->slots == NULL || strike->arena == NULL) {
        free(strike->slots);
        free(strike->arena);
        free(strike);
        return SX_FONT_ERROR_MEMORY;
    }
    FT_Error error = FT_New_Size(face->ft, &strike->size);
    if (error == FT_Err_Ok) error = FT_Activate_Size(strike->size);
    if (error == FT_Err_Ok) error = FT_Set_Pixel_Sizes(face->ft, 0, options->ppem);
    if (error != FT_Err_Ok) {
        if (strike->size != NULL) FT_Done_Size(strike->size);
        free(strike->slots);
        free(strike->arena);
        free(strike);
        return error == FT_Err_Out_Of_Memory ? SX_FONT_ERROR_MEMORY : SX_FONT_ERROR_SIZE;
    }
    strike->face = face;
    strike->options = *options;
    choose_rendering(strike);
    compute_metrics(strike);
    strike->next = face->strikes;
    face->strikes = strike;
    *out = strike;
    return SX_FONT_OK;
}

void sx_font_strike_close(struct sx_font_strike *strike) {
    if (strike == NULL) return;
    struct sx_font_strike **link = &strike->face->strikes;
    while (*link != NULL && *link != strike) link = &(*link)->next;
    if (*link == strike) *link = strike->next;
    FT_Done_Size(strike->size);
    free(strike->slots);
    free(strike->arena);
    free(strike->scratch);
    free(strike);
}

int sx_font_strike_metrics(const struct sx_font_strike *strike, struct sx_font_strike_metrics *out) {
    if (out == NULL) return SX_FONT_ERROR_ARGUMENT;
    if (strike == NULL) {
        memset(out, 0, sizeof *out);
        return SX_FONT_ERROR_ARGUMENT;
    }
    *out = strike->metrics;
    return SX_FONT_OK;
}

/* --- Glyphs -------------------------------------------------------------- */

static void cache_clear(struct sx_font_strike *strike) {
    memset(strike->slots, 0, CACHE_SLOTS * sizeof *strike->slots);
    strike->used_slots = 0;
    strike->arena_used = 0;
}

static struct cache_entry *cache_find(struct sx_font_strike *strike, unsigned glyph) {
    unsigned slot = (unsigned)(((uint32_t)glyph * 2654435761u) >> 22) & (CACHE_SLOTS - 1u);
    for (unsigned probe = 0; probe < CACHE_SLOTS; probe++) {
        struct cache_entry *entry = &strike->slots[(slot + probe) & (CACHE_SLOTS - 1u)];
        if (entry->glyph_plus_one == glyph + 1u || entry->glyph_plus_one == 0) return entry;
    }
    return NULL;
}

/* A slot for `glyph`, emptying the cache first when it is three-quarters
 * full or the arena cannot take `bytes` more. */
static struct cache_entry *cache_claim(struct sx_font_strike *strike, unsigned glyph, size_t bytes) {
    struct cache_entry *entry = cache_find(strike, glyph);
    int is_new = entry == NULL || entry->glyph_plus_one == 0;
    if ((is_new && strike->used_slots + 1u > CACHE_SLOTS * 3u / 4u) || strike->arena_used + bytes > CACHE_ARENA) {
        cache_clear(strike);
        entry = cache_find(strike, glyph);
        is_new = 1;
    }
    if (is_new) {
        memset(entry, 0, sizeof *entry);
        entry->glyph_plus_one = glyph + 1u;
        entry->offset = CACHE_NO_BITMAP;
        strike->used_slots++;
    }
    return entry;
}

static int load_glyph(struct sx_font_strike *strike, unsigned glyph) {
    FT_Face ft = strike->face->ft;
    if (glyph >= (unsigned)ft->num_glyphs) return SX_FONT_ERROR_GLYPH;
    if (FT_Activate_Size(strike->size) != FT_Err_Ok) return SX_FONT_ERROR_SIZE;
    FT_Error error = FT_Load_Glyph(ft, glyph, strike->load_flags);
    return error == FT_Err_Ok ? SX_FONT_OK : SX_FONT_ERROR_GLYPH;
}

static int slot_advance(const struct sx_font_strike *strike) {
    FT_Pos advance = strike->face->ft->glyph->advance.x;
    int pixels = (int)((advance + 32) >> 6);
    return pixels + ((strike->options.flags & SX_FONT_SYNTH_BOLD) != 0);
}

int sx_font_glyph_advance(struct sx_font_strike *strike, unsigned glyph, int *advance) {
    if (advance == NULL) return SX_FONT_ERROR_ARGUMENT;
    *advance = 0;
    if (strike == NULL) return SX_FONT_ERROR_ARGUMENT;
    struct cache_entry *entry = cache_find(strike, glyph);
    if (entry != NULL && entry->glyph_plus_one == glyph + 1u) {
        *advance = entry->advance;
        return SX_FONT_OK;
    }
    int result = load_glyph(strike, glyph);
    if (result != SX_FONT_OK) return result;
    int pixels = slot_advance(strike);
    entry = cache_claim(strike, glyph, 0);
    entry->advance = pixels;
    *advance = pixels;
    return SX_FONT_OK;
}

/* The rendered bitmap as one coverage byte per pixel, emboldened when the
 * strike says so: each pixel takes the larger of itself and its left
 * neighbour, one column wider -- Windows' bold for a face without one. */
static void copy_coverage(const FT_Bitmap *bitmap, int bold, uint8_t *out) {
    unsigned width = bitmap->width, height = bitmap->rows;
    unsigned out_width = width + (bold ? 1u : 0u);
    for (unsigned y = 0; y < height; y++) {
        const unsigned char *row = bitmap->buffer + (long)y * bitmap->pitch;
        uint8_t *dest = out + (size_t)y * out_width;
        uint8_t previous = 0;
        for (unsigned x = 0; x < out_width; x++) {
            uint8_t value = 0;
            if (x < width) {
                if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
                    value = (row[x >> 3] & (0x80u >> (x & 7u))) ? 0xffu : 0x00u;
                } else if (bitmap->num_grays == 256 || bitmap->num_grays < 2) {
                    value = row[x];
                } else {
                    value = (uint8_t)((row[x] * 255u + (bitmap->num_grays - 1u) / 2u) / (bitmap->num_grays - 1u));
                }
            }
            dest[x] = bold && previous > value ? previous : value;
            previous = value;
        }
    }
}

int sx_font_glyph_render(struct sx_font_strike *strike, unsigned glyph, struct sx_font_glyph *out) {
    if (out == NULL) return SX_FONT_ERROR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (strike == NULL) return SX_FONT_ERROR_ARGUMENT;

    struct cache_entry *entry = cache_find(strike, glyph);
    if (entry != NULL && entry->glyph_plus_one == glyph + 1u && entry->offset != CACHE_NO_BITMAP) {
        out->advance = entry->advance;
        out->left = entry->left;
        out->top = entry->top;
        out->width = out->pitch = entry->width;
        out->height = entry->height;
        out->coverage = entry->width != 0 && entry->height != 0 ? strike->arena + entry->offset : NULL;
        return SX_FONT_OK;
    }

    int result = load_glyph(strike, glyph);
    if (result != SX_FONT_OK) return result;
    FT_GlyphSlot slot = strike->face->ft->glyph;
    int advance = slot_advance(strike);
    if (slot->format == FT_GLYPH_FORMAT_OUTLINE && slot->outline.n_contours == 0) {
        /* A glyph with no outline (a space) has no pixels. FreeType's
         * monochrome renderer would still give it a 1x1 bitmap. */
        entry = cache_claim(strike, glyph, 0);
        entry->advance = advance;
        entry->offset = 0;
        out->advance = advance;
        return SX_FONT_OK;
    }
    if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (strike->options.flags & SX_FONT_SYNTH_ITALIC) {
            /* FreeType's own oblique shear, about 12 degrees. */
            FT_Matrix shear = {0x10000L, 0x0366AL, 0, 0x10000L};
            FT_Outline_Transform(&slot->outline, &shear);
        }
        if (FT_Render_Glyph(slot, strike->render_mode) != FT_Err_Ok) return SX_FONT_ERROR_GLYPH;
    }
    if (slot->format != FT_GLYPH_FORMAT_BITMAP) return SX_FONT_ERROR_GLYPH;
    const FT_Bitmap *bitmap = &slot->bitmap;
    if (bitmap->pixel_mode != FT_PIXEL_MODE_MONO && bitmap->pixel_mode != FT_PIXEL_MODE_GRAY)
        return SX_FONT_ERROR_GLYPH;
    if (bitmap->pitch < 0) return SX_FONT_ERROR_GLYPH;

    int bold = (strike->options.flags & SX_FONT_SYNTH_BOLD) != 0;
    unsigned width = bitmap->width != 0 && bitmap->rows != 0 ? bitmap->width + (bold ? 1u : 0u) : 0u;
    unsigned height = width != 0 ? bitmap->rows : 0u;
    size_t bytes = (size_t)width * height;
    uint8_t *coverage = NULL;

    if (bytes > CACHE_ARENA / 4u) {
        /* Too large to keep: drawn into the strike's scratch buffer, and only
         * its advance is cached. */
        if (bytes > strike->scratch_size) {
            uint8_t *grown = realloc(strike->scratch, bytes);
            if (grown == NULL) return SX_FONT_ERROR_MEMORY;
            strike->scratch = grown;
            strike->scratch_size = bytes;
        }
        coverage = strike->scratch;
        entry = cache_claim(strike, glyph, 0);
        entry->advance = advance;
    } else {
        entry = cache_claim(strike, glyph, bytes);
        entry->advance = advance;
        entry->left = slot->bitmap_left;
        entry->top = slot->bitmap_top;
        entry->width = width;
        entry->height = height;
        entry->offset = strike->arena_used;
        coverage = strike->arena + strike->arena_used;
        strike->arena_used += (unsigned)bytes;
    }
    if (bytes != 0) copy_coverage(bitmap, bold, coverage);

    out->advance = advance;
    out->left = slot->bitmap_left;
    out->top = slot->bitmap_top;
    out->width = out->pitch = width;
    out->height = height;
    out->coverage = bytes != 0 ? coverage : NULL;
    return SX_FONT_OK;
}

/* --- The platform's text primitives -------------------------------------- */

static void gfx_face_glyph(void *context, unsigned int codepoint, struct gfx_glyph *out) {
    struct sx_font_strike *strike = context;
    struct sx_font_glyph glyph;
    memset(out, 0, sizeof *out);
    if (sx_font_glyph_render(strike, sx_font_glyph_index(strike->face, codepoint), &glyph) != SX_FONT_OK) return;
    out->width = (int)glyph.width;
    out->rows = (int)glyph.height;
    out->left = glyph.left;
    out->top = glyph.top;
    out->advance = glyph.advance;
    out->pitch = (int)glyph.pitch;
    out->coverage = glyph.coverage;
}

static int gfx_face_advance(void *context, unsigned int codepoint) {
    struct sx_font_strike *strike = context;
    int advance = 0;
    (void)sx_font_glyph_advance(strike, sx_font_glyph_index(strike->face, codepoint), &advance);
    return advance;
}

int sx_font_gfx_face(struct sx_font_strike *strike, struct gfx_face *out) {
    if (out == NULL) return SX_FONT_ERROR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (strike == NULL) return SX_FONT_ERROR_ARGUMENT;
    int cell = gfx_face_advance(strike, 'M');
    out->ascent = strike->metrics.ascent;
    out->line_height = strike->metrics.height;
    out->cell_width = cell > 0 ? cell : strike->metrics.max_advance;
    out->glyph = gfx_face_glyph;
    out->advance = gfx_face_advance;
    out->context = strike;
    return SX_FONT_OK;
}

/* --- Catalog ------------------------------------------------------------- */

int sx_font_catalog_create(struct sx_font_engine *engine, struct sx_font_catalog **out) {
    if (out == NULL) return SX_FONT_ERROR_ARGUMENT;
    *out = NULL;
    if (engine == NULL) return SX_FONT_ERROR_ARGUMENT;
    struct sx_font_catalog *catalog = calloc(1, sizeof *catalog);
    if (catalog == NULL) return SX_FONT_ERROR_MEMORY;
    catalog->engine = engine;
    *out = catalog;
    return SX_FONT_OK;
}

void sx_font_catalog_destroy(struct sx_font_catalog *catalog) {
    if (catalog == NULL) return;
    free(catalog->entries);
    free(catalog);
}

static int catalog_push(struct sx_font_catalog *catalog, const struct sx_font_face_info *info, uint32_t tag,
                        unsigned face_index) {
    if (catalog->count == catalog->capacity) {
        unsigned capacity = catalog->capacity != 0 ? catalog->capacity * 2u : 16u;
        struct catalog_entry *grown = realloc(catalog->entries, capacity * sizeof *grown);
        if (grown == NULL) return SX_FONT_ERROR_MEMORY;
        catalog->entries = grown;
        catalog->capacity = capacity;
    }
    struct catalog_entry *entry = &catalog->entries[catalog->count++];
    entry->info = *info;
    entry->tag = tag;
    entry->face_index = face_index;
    return SX_FONT_OK;
}

int sx_font_catalog_add(struct sx_font_catalog *catalog, const void *data, size_t size, uint32_t tag) {
    if (catalog == NULL) return SX_FONT_ERROR_ARGUMENT;
    struct sx_font_face *face = NULL;
    int result = sx_font_face_open(catalog->engine, data, size, 0, &face);
    if (result != SX_FONT_OK) return result;
    unsigned faces = face->info.faces_in_file != 0 ? face->info.faces_in_file : 1u;
    unsigned before = catalog->count;
    result = catalog_push(catalog, &face->info, tag, 0);
    sx_font_face_close(face);
    for (unsigned index = 1; result == SX_FONT_OK && index < faces; index++) {
        if (sx_font_face_open(catalog->engine, data, size, index, &face) != SX_FONT_OK) continue;
        result = catalog_push(catalog, &face->info, tag, index);
        sx_font_face_close(face);
    }
    if (result != SX_FONT_OK) {
        catalog->count = before;
        return result;
    }
    return (int)(catalog->count - before);
}

unsigned sx_font_catalog_count(const struct sx_font_catalog *catalog) {
    return catalog != NULL ? catalog->count : 0u;
}

int sx_font_catalog_entry(const struct sx_font_catalog *catalog, unsigned index, struct sx_font_face_info *info,
                          uint32_t *tag, unsigned *face_index) {
    if (info != NULL) memset(info, 0, sizeof *info);
    if (tag != NULL) *tag = 0;
    if (face_index != NULL) *face_index = 0;
    if (catalog == NULL || index >= catalog->count) return SX_FONT_ERROR_ARGUMENT;
    const struct catalog_entry *entry = &catalog->entries[index];
    if (info != NULL) *info = entry->info;
    if (tag != NULL) *tag = entry->tag;
    if (face_index != NULL) *face_index = entry->face_index;
    return SX_FONT_OK;
}

/* Family names compare as Windows compares them: ASCII letters without
 * case, everything else byte for byte. */
static int same_family(const char *a, const char *b) {
    for (;; a++, b++) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
    }
}

static int family_present(const struct sx_font_catalog *catalog, const char *family) {
    for (unsigned i = 0; i < catalog->count; i++)
        if (same_family(catalog->entries[i].info.family, family)) return 1;
    return 0;
}

/* Metric-compatible pairs, both ways: a document that names either face
 * lays out the same with the other. */
static const char *const kSubstitutes[][2] = {
    {"Arial", "Liberation Sans"},
    {"Helvetica", "Liberation Sans"},
    {"Times New Roman", "Liberation Serif"},
    {"Times", "Liberation Serif"},
    {"Courier New", "Liberation Mono"},
    {"Courier", "Liberation Mono"},
};

static const char *substitute_for(const struct sx_font_catalog *catalog, const char *family) {
    for (size_t i = 0; i < sizeof kSubstitutes / sizeof kSubstitutes[0]; i++) {
        if (same_family(family, kSubstitutes[i][0]) && family_present(catalog, kSubstitutes[i][1]))
            return kSubstitutes[i][1];
    }
    for (size_t i = 0; i < sizeof kSubstitutes / sizeof kSubstitutes[0]; i++) {
        if (same_family(family, kSubstitutes[i][1]) && family_present(catalog, kSubstitutes[i][0]))
            return kSubstitutes[i][0];
    }
    return NULL;
}

static const char *fallback_for(const struct sx_font_catalog *catalog, unsigned flags) {
    static const char *const proportional[] = {"Liberation Sans", "Arial"};
    static const char *const fixed[] = {"Liberation Mono", "Courier New"};
    const char *const *list = (flags & SX_FONT_FIXED_PITCH) ? fixed : proportional;
    for (unsigned i = 0; i < 2; i++)
        if (family_present(catalog, list[i])) return list[i];
    for (unsigned i = 0; i < catalog->count; i++) {
        if (((catalog->entries[i].info.flags & SX_FONT_FIXED_PITCH) != 0) == ((flags & SX_FONT_FIXED_PITCH) != 0))
            return catalog->entries[i].info.family;
    }
    return catalog->entries[0].info.family;
}

int sx_font_catalog_match(const struct sx_font_catalog *catalog, const struct sx_font_request *request,
                          struct sx_font_match *out) {
    if (out == NULL) return SX_FONT_ERROR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (catalog == NULL || request == NULL) return SX_FONT_ERROR_ARGUMENT;
    if (catalog->count == 0) return SX_FONT_ERROR_NOT_FOUND;

    const char *family = NULL;
    unsigned quality = SX_FONT_MATCH_FALLBACK;
    if (request->family != NULL && request->family[0] != '\0') {
        if (family_present(catalog, request->family)) {
            family = request->family;
            quality = SX_FONT_MATCH_EXACT;
        } else if ((family = substitute_for(catalog, request->family)) != NULL) {
            quality = SX_FONT_MATCH_SUBSTITUTE;
        }
    }
    if (family == NULL) family = fallback_for(catalog, request->flags);

    unsigned weight = request->weight != 0 ? request->weight : 400u;
    int want_italic = (request->flags & SX_FONT_ITALIC) != 0;
    const struct catalog_entry *best = NULL;
    unsigned best_score = 0;
    for (unsigned i = 0; i < catalog->count; i++) {
        const struct catalog_entry *entry = &catalog->entries[i];
        if (!same_family(entry->info.family, family)) continue;
        int italic = (entry->info.flags & SX_FONT_ITALIC) != 0;
        unsigned score = (italic != want_italic ? 10000u : 0u) +
                         (entry->info.weight > weight ? entry->info.weight - weight : weight - entry->info.weight);
        if (best == NULL || score < best_score) {
            best = entry;
            best_score = score;
        }
    }
    if (best == NULL) return SX_FONT_ERROR_NOT_FOUND;

    out->tag = best->tag;
    out->face_index = best->face_index;
    out->quality = quality;
    if (weight >= 600u && best->info.weight < 600u) out->synth_flags |= SX_FONT_SYNTH_BOLD;
    if (want_italic && (best->info.flags & SX_FONT_ITALIC) == 0) out->synth_flags |= SX_FONT_SYNTH_ITALIC;
    memcpy(out->family, best->info.family, sizeof out->family);
    return SX_FONT_OK;
}
