/* libsxfont's self-test (sxfont_selftest.h). The expected numbers come from
 * the fonts' own tables, read with fontTools and scaled by hand, except the
 * glyph checksums: those were recorded from this library and checked against
 * the build host's FreeType with the v35 interpreter
 * (docs/desktop/FONT_LIBRARY.md, "Verification"). */

#include "sxfont_selftest.h"

#include <stdio.h>
#include <string.h>

#include "sxfont/font.h"
#include "sxgfx/pixels.h"

struct context {
    sxfont_selftest_load load;
    sxfont_selftest_report report;
    void *user;
    int failures;
};

static void fail(struct context *c, const char *line) {
    c->failures++;
    c->report(c->user, line);
}

static void expect_int(struct context *c, const char *what, long actual, long expected) {
    if (actual == expected) return;
    char line[200];
    snprintf(line, sizeof line, "%s: %ld, expected %ld", what, actual, expected);
    fail(c, line);
}

static void expect_text(struct context *c, const char *what, const char *actual, const char *expected) {
    if (strcmp(actual, expected) == 0) return;
    char line[200];
    snprintf(line, sizeof line, "%s: \"%s\", expected \"%s\"", what, actual, expected);
    fail(c, line);
}

static const char *const kFiles[12] = {
    "LiberationSans-Regular.ttf",  "LiberationSans-Bold.ttf",
    "LiberationSans-Italic.ttf",   "LiberationSans-BoldItalic.ttf",
    "LiberationSerif-Regular.ttf", "LiberationSerif-Bold.ttf",
    "LiberationSerif-Italic.ttf",  "LiberationSerif-BoldItalic.ttf",
    "LiberationMono-Regular.ttf",  "LiberationMono-Bold.ttf",
    "LiberationMono-Italic.ttf",   "LiberationMono-BoldItalic.ttf",
};
enum { SANS = 0, SANS_BOLD = 1, SANS_BOLD_ITALIC = 3, SERIF = 4, MONO = 8 };

struct file {
    const void *data;
    size_t size;
};

static struct sx_font_face *open_face(struct context *c, struct sx_font_engine *engine, const struct file *file,
                                      const char *name) {
    struct sx_font_face *face = NULL;
    int result = sx_font_face_open(engine, file->data, file->size, 0, &face);
    if (result != SX_FONT_OK) {
        char line[160];
        snprintf(line, sizeof line, "%s opens: %s", name, sx_font_error_string(result));
        fail(c, line);
    }
    return face;
}

static struct sx_font_strike *open_strike(struct context *c, struct sx_font_face *face, unsigned ppem,
                                          unsigned flags) {
    struct sx_font_strike_options options = {ppem, flags};
    struct sx_font_strike *strike = NULL;
    int result = sx_font_strike_open(face, &options, &strike);
    if (result != SX_FONT_OK) {
        char line[160];
        snprintf(line, sizeof line, "a strike at %u ppem (flags 0x%x) opens: %s", ppem, flags,
                 sx_font_error_string(result));
        fail(c, line);
    }
    return strike;
}

/* --- Faces --------------------------------------------------------------- */

static void check_faces(struct context *c, struct sx_font_engine *engine, const struct file *files) {
    static const struct {
        const char *family, *style;
        unsigned weight, flags;
        int win_ascent, win_descent;
    } kExpected[12] = {
        {"Liberation Sans", "Regular", 400, 0, 1854, 434},
        {"Liberation Sans", "Bold", 700, SX_FONT_BOLD, 1854, 434},
        {"Liberation Sans", "Italic", 400, SX_FONT_ITALIC, 1854, 434},
        {"Liberation Sans", "Bold Italic", 700, SX_FONT_BOLD | SX_FONT_ITALIC, 1854, 434},
        {"Liberation Serif", "Regular", 400, 0, 1825, 443},
        {"Liberation Serif", "Bold", 700, SX_FONT_BOLD, 1825, 443},
        {"Liberation Serif", "Italic", 400, SX_FONT_ITALIC, 1825, 443},
        {"Liberation Serif", "Bold Italic", 700, SX_FONT_BOLD | SX_FONT_ITALIC, 1825, 443},
        {"Liberation Mono", "Regular", 400, SX_FONT_FIXED_PITCH, 1705, 615},
        {"Liberation Mono", "Bold", 700, SX_FONT_BOLD | SX_FONT_FIXED_PITCH, 1705, 615},
        {"Liberation Mono", "Italic", 400, SX_FONT_ITALIC | SX_FONT_FIXED_PITCH, 1705, 615},
        {"Liberation Mono", "Bold Italic", 700, SX_FONT_BOLD | SX_FONT_ITALIC | SX_FONT_FIXED_PITCH, 1705, 615},
    };
    for (unsigned i = 0; i < 12; i++) {
        struct sx_font_face *face = open_face(c, engine, &files[i], kFiles[i]);
        if (face == NULL) continue;
        struct sx_font_face_info info;
        sx_font_face_info(face, &info);
        char what[96];
        snprintf(what, sizeof what, "%s family", kFiles[i]);
        expect_text(c, what, info.family, kExpected[i].family);
        snprintf(what, sizeof what, "%s style", kFiles[i]);
        expect_text(c, what, info.style, kExpected[i].style);
        snprintf(what, sizeof what, "%s weight", kFiles[i]);
        expect_int(c, what, info.weight, kExpected[i].weight);
        snprintf(what, sizeof what, "%s flags", kFiles[i]);
        expect_int(c, what, info.flags, kExpected[i].flags | SX_FONT_HINTED);
        snprintf(what, sizeof what, "%s usWinAscent", kFiles[i]);
        expect_int(c, what, info.win_ascent, kExpected[i].win_ascent);
        snprintf(what, sizeof what, "%s usWinDescent", kFiles[i]);
        expect_int(c, what, info.win_descent, kExpected[i].win_descent);
        snprintf(what, sizeof what, "%s units per em", kFiles[i]);
        expect_int(c, what, info.units_per_em, 2048);
        snprintf(what, sizeof what, "%s faces in the file", kFiles[i]);
        expect_int(c, what, info.faces_in_file, 1);
        sx_font_face_close(face);
    }

    struct sx_font_face *sans = open_face(c, engine, &files[SANS], kFiles[SANS]);
    if (sans == NULL) return;
    struct sx_font_face_info info;
    sx_font_face_info(sans, &info);
    expect_int(c, "Liberation Sans glyphs", info.glyph_count, 2620);
    expect_int(c, "Liberation Sans xAvgCharWidth", info.avg_char_width, 1187);
    expect_int(c, "Liberation Sans code pages (Latin 1 among them)", (long)(info.code_page_range[0] & 1u), 1);
    expect_int(c, "glyph of 'A'", sx_font_glyph_index(sans, 'A'), 36);
    expect_int(c, "glyph of ' '", sx_font_glyph_index(sans, ' '), 3);
    expect_int(c, "glyph of U+00F1 exists", sx_font_glyph_index(sans, 0x00f1) != 0, 1);
    expect_int(c, "glyph of U+10FFFD", sx_font_glyph_index(sans, 0x10fffd), 0);

    /* A positive LOGFONT height is the cell: 15 pixels is 13 ppem here
     * (12 + 3), 14 is 12 ppem (11 + 3), 16 is 14 ppem (13 + 3). */
    unsigned ppem = 0;
    sx_font_ppem_for_cell_height(sans, 15, &ppem);
    expect_int(c, "ppem for a 15-pixel cell", ppem, 13);
    sx_font_ppem_for_cell_height(sans, 14, &ppem);
    expect_int(c, "ppem for a 14-pixel cell", ppem, 12);
    sx_font_ppem_for_cell_height(sans, 16, &ppem);
    expect_int(c, "ppem for a 16-pixel cell", ppem, 14);
    sx_font_face_close(sans);
}

/* --- Errors -------------------------------------------------------------- */

static void check_errors(struct context *c, struct sx_font_engine *engine, const struct file *files) {
    static const unsigned char garbage[64] = "this is not a font at all, only some words";
    static const unsigned char cff[64] = "OTTO\0\x0a\0\x80";
    struct sx_font_face *face = (struct sx_font_face *)1;
    expect_int(c, "a file of text", sx_font_face_open(engine, garbage, sizeof garbage, 0, &face),
               SX_FONT_ERROR_FORMAT);
    expect_int(c, "a failed open leaves no face", face == NULL, 1);
    expect_int(c, "a CFF OpenType file", sx_font_face_open(engine, cff, sizeof cff, 0, &face),
               SX_FONT_ERROR_UNSUPPORTED);
    expect_int(c, "a null file", sx_font_face_open(engine, NULL, 100, 0, &face), SX_FONT_ERROR_ARGUMENT);
    expect_int(c, "a TrueType file cut short", sx_font_face_open(engine, files[SANS].data, 4096, 0, &face) < 0, 1);
    expect_int(c, "face 1 of a single face", sx_font_face_open(engine, files[SANS].data, files[SANS].size, 1, &face),
               SX_FONT_ERROR_ARGUMENT);

    face = open_face(c, engine, &files[SANS], kFiles[SANS]);
    if (face == NULL) return;
    struct sx_font_strike_options options = {0, 0};
    struct sx_font_strike *strike = NULL;
    expect_int(c, "0 ppem", sx_font_strike_open(face, &options, &strike), SX_FONT_ERROR_SIZE);
    options.ppem = SX_FONT_MAX_PPEM + 1;
    expect_int(c, "1001 ppem", sx_font_strike_open(face, &options, &strike), SX_FONT_ERROR_SIZE);
    options.ppem = 13;
    options.flags = SX_FONT_FORCE_MONO | SX_FONT_FORCE_GRAY;
    expect_int(c, "1-bit and grey at once", sx_font_strike_open(face, &options, &strike), SX_FONT_ERROR_ARGUMENT);
    options.flags = 0x80000000u;
    expect_int(c, "an unknown strike flag", sx_font_strike_open(face, &options, &strike), SX_FONT_ERROR_ARGUMENT);
    strike = open_strike(c, face, 13, 0);
    if (strike != NULL) {
        struct sx_font_glyph glyph;
        int advance = 0;
        expect_int(c, "a glyph past the face", sx_font_glyph_render(strike, 2620, &glyph), SX_FONT_ERROR_GLYPH);
        expect_int(c, "the advance of a glyph past the face", sx_font_glyph_advance(strike, 99999, &advance),
                   SX_FONT_ERROR_GLYPH);
    }
    expect_text(c, "an unknown result's text", sx_font_error_string(-1000), "unknown font error");
    sx_font_face_close(face); /* closes the strike too */
}

/* --- Strikes ------------------------------------------------------------- */

static void check_metrics(struct context *c, struct sx_font_engine *engine, const struct file *files) {
    struct sx_font_face *sans = open_face(c, engine, &files[SANS], kFiles[SANS]);
    if (sans == NULL) return;
    struct sx_font_strike *strike = open_strike(c, sans, 13, 0);
    if (strike != NULL) {
        /* 1854, 434, 1187, 2740, post -67/150, OS/2 530/102 at 13/2048. */
        struct sx_font_strike_metrics m;
        sx_font_strike_metrics(strike, &m);
        expect_int(c, "13 ppem ascent", m.ascent, 12);
        expect_int(c, "13 ppem descent", m.descent, 3);
        expect_int(c, "13 ppem height", m.height, 15);
        expect_int(c, "13 ppem internal leading", m.internal_leading, 2);
        expect_int(c, "13 ppem external leading", m.external_leading, 0);
        expect_int(c, "13 ppem average width", m.avg_char_width, 8);
        expect_int(c, "13 ppem largest advance", m.max_advance, 17);
        expect_int(c, "13 ppem underline position", m.underline_position, 0);
        expect_int(c, "13 ppem underline thickness", m.underline_thickness, 1);
        expect_int(c, "13 ppem strikeout position", m.strikeout_position, 3);
        expect_int(c, "13 ppem strikeout size", m.strikeout_size, 1);
    }
    strike = open_strike(c, sans, 100, SX_FONT_SYNTH_BOLD);
    if (strike != NULL) {
        /* 1854 * 100 / 2048 = 90.5, 434 -> 21.2, line gap 67 -> 3.3. */
        struct sx_font_strike_metrics m;
        sx_font_strike_metrics(strike, &m);
        expect_int(c, "100 ppem ascent", m.ascent, 91);
        expect_int(c, "100 ppem descent", m.descent, 21);
        expect_int(c, "100 ppem external leading", m.external_leading, 3);
        expect_int(c, "100 ppem emboldened average width", m.avg_char_width, 58 + 1);
        expect_int(c, "100 ppem underline position", m.underline_position, 3);
        expect_int(c, "100 ppem underline thickness", m.underline_thickness, 7);
    }
    sx_font_face_close(sans);
}

/* What gasp says, per size: Liberation Sans 1-bit from 9 to 17 ppem, grey
 * around it; Liberation Serif's own ranges start at 10. Hinted at every
 * size, as Windows XP draws them even where gasp leaves GRIDFIT clear. */
static void check_render_modes(struct context *c, struct sx_font_engine *engine, const struct file *files) {
    static const struct {
        unsigned file, ppem, flags, render, hinted;
    } kCases[] = {
        {SANS, 8, 0, SX_FONT_RENDER_GRAY, 1},
        {SANS, 9, 0, SX_FONT_RENDER_MONO, 1},
        {SANS, 13, 0, SX_FONT_RENDER_MONO, 1},
        {SANS, 17, 0, SX_FONT_RENDER_MONO, 1},
        {SANS, 18, 0, SX_FONT_RENDER_GRAY, 1},
        {SANS, 96, 0, SX_FONT_RENDER_GRAY, 1},
        {SANS_BOLD, 10, 0, SX_FONT_RENDER_GRAY, 1},
        {SERIF, 10, 0, SX_FONT_RENDER_GRAY, 1},
        {SERIF, 11, 0, SX_FONT_RENDER_MONO, 1},
        {SANS, 30, SX_FONT_FORCE_MONO, SX_FONT_RENDER_MONO, 1},
        {SANS, 6, SX_FONT_FORCE_MONO, SX_FONT_RENDER_MONO, 1},
        {SANS, 13, SX_FONT_FORCE_GRAY, SX_FONT_RENDER_GRAY, 1},
    };
    for (size_t i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
        struct sx_font_face *face = open_face(c, engine, &files[kCases[i].file], kFiles[kCases[i].file]);
        if (face == NULL) continue;
        struct sx_font_strike *strike = open_strike(c, face, kCases[i].ppem, kCases[i].flags);
        if (strike != NULL) {
            struct sx_font_strike_metrics m;
            sx_font_strike_metrics(strike, &m);
            char what[120];
            snprintf(what, sizeof what, "%s at %u ppem (0x%x): render", kFiles[kCases[i].file], kCases[i].ppem,
                     kCases[i].flags);
            expect_int(c, what, m.render, kCases[i].render);
            snprintf(what, sizeof what, "%s at %u ppem (0x%x): hinted", kFiles[kCases[i].file], kCases[i].ppem,
                     kCases[i].flags);
            expect_int(c, what, m.hinted, kCases[i].hinted);
        }
        sx_font_face_close(face);
    }
}

/* --- Glyphs -------------------------------------------------------------- */

static unsigned long fnv1a(unsigned long hash, const void *bytes, size_t size) {
    const unsigned char *p = bytes;
    for (size_t i = 0; i < size; i++) {
        hash ^= p[i];
        hash = (hash * 16777619ul) & 0xfffffffful;
    }
    return hash;
}

/* A checksum of a run of text: each glyph's advance, offsets, size and
 * coverage. */
static unsigned long text_checksum(struct context *c, struct sx_font_face *face, struct sx_font_strike *strike,
                                   const char *text, int *only_two_levels, int *total_advance) {
    unsigned long hash = 2166136261ul;
    *only_two_levels = 1;
    *total_advance = 0;
    for (const char *p = text; *p != '\0'; p++) {
        unsigned index = sx_font_glyph_index(face, (unsigned char)*p);
        struct sx_font_glyph glyph;
        if (sx_font_glyph_render(strike, index, &glyph) != SX_FONT_OK) {
            fail(c, "a glyph of the sample text renders");
            continue;
        }
        int header[5] = {glyph.advance, glyph.left, glyph.top, (int)glyph.width, (int)glyph.height};
        hash = fnv1a(hash, header, sizeof header);
        for (unsigned y = 0; y < glyph.height; y++) {
            const uint8_t *row = glyph.coverage + (size_t)y * glyph.pitch;
            hash = fnv1a(hash, row, glyph.width);
            for (unsigned x = 0; x < glyph.width; x++)
                if (row[x] != 0x00 && row[x] != 0xff) *only_two_levels = 0;
        }
        *total_advance += glyph.advance;
    }
    return hash;
}

static const char kSample[] = "SavanXP: WordPad, ni\xf1o 0123";
/* The same line in UTF-8, as the text primitives take it; kSample's bytes
 * are its codepoints (Latin-1). */
static const char kSampleUtf8[] = "SavanXP: WordPad, ni\xc3\xb1o 0123";

static void check_glyphs(struct context *c, struct sx_font_engine *engine, const struct file *files) {
    static const struct {
        unsigned file, ppem;
        unsigned long checksum;
        int advance;
        int two_levels;
    } kCases[] = {
        /* 1-bit and hinted where gasp says so; Liberation Sans Bold Italic's
         * gasp turns grey above 10 ppem. The host's FreeType 2.15.0 with
         * interpreter-version 35 gives the same five checksums. */
        {SANS, 13, 3709550523ul, 177, 1},
        {SANS, 24, 3926584139ul, 332, 0},
        {SERIF, 13, 206087742ul, 163, 1},
        {MONO, 13, 2395557570ul, 216, 1},
        {SANS_BOLD_ITALIC, 16, 2156093618ul, 230, 0},
    };
    for (size_t i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
        struct sx_font_face *face = open_face(c, engine, &files[kCases[i].file], kFiles[kCases[i].file]);
        if (face == NULL) continue;
        struct sx_font_strike *strike = open_strike(c, face, kCases[i].ppem, 0);
        if (strike != NULL) {
            int two_levels = 0, advance = 0;
            unsigned long checksum = text_checksum(c, face, strike, kSample, &two_levels, &advance);
            char what[120];
            snprintf(what, sizeof what, "%s at %u ppem: checksum", kFiles[kCases[i].file], kCases[i].ppem);
            expect_int(c, what, (long)checksum, (long)kCases[i].checksum);
            snprintf(what, sizeof what, "%s at %u ppem: advance of the sample", kFiles[kCases[i].file],
                     kCases[i].ppem);
            expect_int(c, what, advance, kCases[i].advance);
            snprintf(what, sizeof what, "%s at %u ppem: only 0x00 and 0xff", kFiles[kCases[i].file],
                     kCases[i].ppem);
            expect_int(c, what, two_levels, kCases[i].two_levels);
            /* The cached copy is the drawn one. */
            unsigned long again = text_checksum(c, face, strike, kSample, &two_levels, &advance);
            snprintf(what, sizeof what, "%s at %u ppem: from the cache", kFiles[kCases[i].file], kCases[i].ppem);
            expect_int(c, what, (long)again, (long)checksum);
        }
        sx_font_face_close(face);
    }

    /* Liberation Mono: one advance for every printable ASCII character. */
    struct sx_font_face *mono = open_face(c, engine, &files[MONO], kFiles[MONO]);
    if (mono != NULL) {
        struct sx_font_strike *strike = open_strike(c, mono, 13, 0);
        int first = -1, same = 1;
        for (unsigned ch = 0x20; strike != NULL && ch < 0x7f; ch++) {
            int advance = 0;
            sx_font_glyph_advance(strike, sx_font_glyph_index(mono, ch), &advance);
            if (first < 0) first = advance;
            if (advance != first) same = 0;
        }
        expect_int(c, "Liberation Mono has one advance at 13 ppem", same, 1);
        expect_int(c, "Liberation Mono's advance at 13 ppem", first, 8);
        sx_font_face_close(mono);
    }
}

static void check_space_and_bold(struct context *c, struct sx_font_engine *engine, const struct file *files) {
    struct sx_font_face *sans = open_face(c, engine, &files[SANS], kFiles[SANS]);
    if (sans == NULL) return;
    struct sx_font_strike *plain = open_strike(c, sans, 13, 0);
    struct sx_font_strike *bold = open_strike(c, sans, 13, SX_FONT_SYNTH_BOLD);
    if (plain == NULL || bold == NULL) {
        sx_font_face_close(sans);
        return;
    }
    struct sx_font_glyph glyph;
    sx_font_glyph_render(plain, sx_font_glyph_index(sans, ' '), &glyph);
    expect_int(c, "a space has no pixels", glyph.coverage == NULL && glyph.width == 0, 1);
    expect_int(c, "a space advances", glyph.advance > 0, 1);
    int advance = 0;
    sx_font_glyph_advance(plain, sx_font_glyph_index(sans, ' '), &advance);
    expect_int(c, "a space's advance without drawing it", advance, glyph.advance);

    unsigned h = sx_font_glyph_index(sans, 'H');
    struct sx_font_glyph normal, heavy;
    sx_font_glyph_render(plain, h, &normal);
    uint8_t copy[64 * 64];
    int fits = normal.width * normal.height <= sizeof copy && normal.coverage != NULL;
    expect_int(c, "'H' at 13 ppem has pixels", fits, 1);
    if (!fits) {
        sx_font_face_close(sans);
        return;
    }
    for (unsigned y = 0; y < normal.height; y++)
        memcpy(copy + y * normal.width, normal.coverage + y * normal.pitch, normal.width);
    sx_font_glyph_render(bold, h, &heavy);
    expect_int(c, "emboldened 'H' is one column wider", heavy.width, normal.width + 1);
    expect_int(c, "emboldened 'H' advances one more", heavy.advance, normal.advance + 1);
    expect_int(c, "emboldened 'H' keeps its rows", heavy.height, normal.height);
    int smeared = 1;
    for (unsigned y = 0; y < normal.height && heavy.coverage != NULL; y++) {
        for (unsigned x = 0; x < heavy.width; x++) {
            uint8_t here = x < normal.width ? copy[y * normal.width + x] : 0;
            uint8_t left = x > 0 ? copy[y * normal.width + x - 1] : 0;
            if (heavy.coverage[y * heavy.pitch + x] != (here > left ? here : left)) smeared = 0;
        }
    }
    expect_int(c, "emboldened 'H' is 'H' drawn twice a pixel apart", smeared, 1);
    sx_font_face_close(sans);
}

/* Every glyph of the face at a large size, more than the cache holds, then
 * the first one again: what comes back after the cache was emptied is what
 * was drawn first. A glyph larger than the arena is drawn apart. */
static void check_cache(struct context *c, struct sx_font_engine *engine, const struct file *files) {
    struct sx_font_face *sans = open_face(c, engine, &files[SANS], kFiles[SANS]);
    if (sans == NULL) return;
    struct sx_font_strike *strike = open_strike(c, sans, 120, 0);
    if (strike != NULL) {
        int two = 0, advance = 0;
        unsigned long first = text_checksum(c, sans, strike, "Wg", &two, &advance);
        struct sx_font_glyph glyph;
        unsigned rendered = 0;
        for (unsigned index = 0; index < 2620; index++)
            if (sx_font_glyph_render(strike, index, &glyph) == SX_FONT_OK) rendered++;
        expect_int(c, "every glyph of Liberation Sans renders at 120 ppem", rendered, 2620);
        unsigned long again = text_checksum(c, sans, strike, "Wg", &two, &advance);
        expect_int(c, "a glyph drawn again after the cache emptied", (long)again, (long)first);
    }
    strike = open_strike(c, sans, 700, 0);
    if (strike != NULL) {
        int two = 0, advance = 0;
        unsigned long first = text_checksum(c, sans, strike, "W", &two, &advance);
        unsigned long again = text_checksum(c, sans, strike, "W", &two, &advance);
        expect_int(c, "a glyph larger than the cache, twice", (long)again, (long)first);
        struct sx_font_glyph glyph;
        sx_font_glyph_render(strike, sx_font_glyph_index(sans, 'W'), &glyph);
        expect_int(c, "'W' at 700 ppem is larger than a quarter of the cache",
                   (long)glyph.width * glyph.height > 64L * 1024L, 1);
    }
    sx_font_face_close(sans);
}

/* --- Catalog ------------------------------------------------------------- */

static void check_catalog(struct context *c, struct sx_font_engine *engine, const struct file *files) {
    struct sx_font_catalog *catalog = NULL;
    if (sx_font_catalog_create(engine, &catalog) != SX_FONT_OK) {
        fail(c, "a catalog is created");
        return;
    }
    struct sx_font_request request = {"Arial", 400, 0};
    struct sx_font_match match;
    expect_int(c, "an empty catalog", sx_font_catalog_match(catalog, &request, &match), SX_FONT_ERROR_NOT_FOUND);
    for (unsigned i = 0; i < 12; i++) {
        char what[96];
        snprintf(what, sizeof what, "%s joins the catalog", kFiles[i]);
        expect_int(c, what, sx_font_catalog_add(catalog, files[i].data, files[i].size, 100u + i), 1);
    }
    expect_int(c, "faces in the catalog", sx_font_catalog_count(catalog), 12);
    static const unsigned char garbage[32] = "not a font";
    expect_int(c, "a file of text does not join", sx_font_catalog_add(catalog, garbage, sizeof garbage, 1),
               SX_FONT_ERROR_FORMAT);

    static const struct {
        const char *family;
        unsigned weight, flags;
        uint32_t tag;
        unsigned quality, synth;
    } kCases[] = {
        {"Arial", 400, 0, 100, SX_FONT_MATCH_SUBSTITUTE, 0},
        {"arial", 700, SX_FONT_ITALIC, 103, SX_FONT_MATCH_SUBSTITUTE, 0},
        {"Times New Roman", 700, 0, 105, SX_FONT_MATCH_SUBSTITUTE, 0},
        {"Courier New", 400, SX_FONT_ITALIC, 110, SX_FONT_MATCH_SUBSTITUTE, 0},
        {"Liberation Serif", 0, 0, 104, SX_FONT_MATCH_EXACT, 0},
        {"LIBERATION MONO", 900, 0, 109, SX_FONT_MATCH_EXACT, 0},
        {"Liberation Sans", 300, 0, 100, SX_FONT_MATCH_EXACT, 0},
        {"Comic Sans MS", 400, 0, 100, SX_FONT_MATCH_FALLBACK, 0},
        {"Lucida Console", 400, SX_FONT_FIXED_PITCH, 108, SX_FONT_MATCH_FALLBACK, 0},
        {NULL, 400, 0, 100, SX_FONT_MATCH_FALLBACK, 0},
        {"", 700, SX_FONT_FIXED_PITCH | SX_FONT_ITALIC, 111, SX_FONT_MATCH_FALLBACK, 0},
    };
    for (size_t i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
        struct sx_font_request r = {kCases[i].family, kCases[i].weight, kCases[i].flags};
        char what[120];
        snprintf(what, sizeof what, "\"%s\" %u 0x%x: matches", kCases[i].family ? kCases[i].family : "(null)",
                 kCases[i].weight, kCases[i].flags);
        expect_int(c, what, sx_font_catalog_match(catalog, &r, &match), SX_FONT_OK);
        snprintf(what, sizeof what, "\"%s\" %u 0x%x: file", kCases[i].family ? kCases[i].family : "(null)",
                 kCases[i].weight, kCases[i].flags);
        expect_int(c, what, (long)match.tag, (long)kCases[i].tag);
        snprintf(what, sizeof what, "\"%s\" %u 0x%x: quality", kCases[i].family ? kCases[i].family : "(null)",
                 kCases[i].weight, kCases[i].flags);
        expect_int(c, what, match.quality, kCases[i].quality);
        snprintf(what, sizeof what, "\"%s\" %u 0x%x: synthesis", kCases[i].family ? kCases[i].family : "(null)",
                 kCases[i].weight, kCases[i].flags);
        expect_int(c, what, match.synth_flags, kCases[i].synth);
    }
    struct sx_font_face_info info;
    uint32_t tag = 0;
    unsigned face_index = 9;
    expect_int(c, "catalog entry 4", sx_font_catalog_entry(catalog, 4, &info, &tag, &face_index), SX_FONT_OK);
    expect_text(c, "catalog entry 4's family", info.family, "Liberation Serif");
    expect_int(c, "catalog entry 4's tag", (long)tag, 104);
    expect_int(c, "catalog entry 4's face", face_index, 0);
    expect_int(c, "catalog entry 12", sx_font_catalog_entry(catalog, 12, &info, &tag, &face_index),
               SX_FONT_ERROR_ARGUMENT);
    sx_font_catalog_destroy(catalog);

    /* With only Liberation Sans Regular, bold and italic are made. */
    if (sx_font_catalog_create(engine, &catalog) != SX_FONT_OK) return;
    sx_font_catalog_add(catalog, files[SANS].data, files[SANS].size, 7);
    struct sx_font_request heavy = {"Arial", 700, SX_FONT_ITALIC};
    sx_font_catalog_match(catalog, &heavy, &match);
    expect_int(c, "bold italic Arial from Regular: synthesis", match.synth_flags,
               SX_FONT_SYNTH_BOLD | SX_FONT_SYNTH_ITALIC);
    expect_text(c, "bold italic Arial from Regular: family", match.family, "Liberation Sans");
    struct sx_font_request courier = {"Courier New", 400, SX_FONT_FIXED_PITCH};
    sx_font_catalog_match(catalog, &courier, &match);
    expect_int(c, "Courier New with no fixed-pitch face", match.quality, SX_FONT_MATCH_FALLBACK);
    expect_int(c, "Courier New with no fixed-pitch face: file", (long)match.tag, 7);
    sx_font_catalog_destroy(catalog);
}

/* --- Through the platform's text primitives ------------------------------ */

#define CANVAS_W 240
#define CANVAS_H 24

/* Liberation Sans in the UI slot draws what the strike draws: the canvas
 * gfx_blit_text paints equals one composed here glyph by glyph, pen by
 * advance, baseline at the strike's ascent. Then the baked face is back. */
static void check_text_primitives(struct context *c, struct sx_font_engine *engine, const struct file *files) {
    static uint32_t drawn[CANVAS_W * CANVAS_H], composed[CANVAS_W * CANVAS_H];
    int baked_height = gfx_text_height();
    int baked_width = gfx_text_width(kSampleUtf8);
    struct sx_font_face *sans = open_face(c, engine, &files[SANS], kFiles[SANS]);
    struct sx_font_face *mono = open_face(c, engine, &files[MONO], kFiles[MONO]);
    if (sans == NULL || mono == NULL) {
        sx_font_face_close(sans);
        sx_font_face_close(mono);
        return;
    }
    struct sx_font_strike *strike = open_strike(c, sans, 13, SX_FONT_FORCE_MONO);
    struct sx_font_strike *cells = open_strike(c, mono, 13, SX_FONT_FORCE_MONO);
    struct gfx_face face, cell_face;
    if (strike == NULL || cells == NULL || sx_font_gfx_face(strike, &face) != SX_FONT_OK ||
        sx_font_gfx_face(cells, &cell_face) != SX_FONT_OK) {
        fail(c, "a strike becomes a face for the text primitives");
        sx_font_face_close(sans);
        sx_font_face_close(mono);
        return;
    }
    expect_int(c, "the strike's face: ascent", face.ascent, 12);
    expect_int(c, "the strike's face: line", face.line_height, 15);
    expect_int(c, "the UI slot takes the strike's face", gfx_face_install(GFX_FACE_UI, &face), 0);
    expect_int(c, "the UI slot's line is the strike's", gfx_text_height(), 15);
    expect_int(c, "the UI slot measures the strike's advances", gfx_text_width(kSampleUtf8), 177);

    struct savanxp_fb_info info;
    memset(&info, 0, sizeof info);
    info.width = CANVAS_W;
    info.height = CANVAS_H;
    info.pitch = CANVAS_W * 4u;
    info.bpp = 32u;
    info.buffer_size = info.pitch * CANVAS_H;
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) drawn[i] = composed[i] = 0x00ffffffu;
    gfx_blit_text(drawn, &info, 4, 3, kSampleUtf8, 0x00000000u);
    int pen = 4, baseline = 3 + 12;
    for (const char *p = kSample; *p != '\0'; p++) {
        struct sx_font_glyph glyph;
        sx_font_glyph_render(strike, sx_font_glyph_index(sans, (unsigned char)*p), &glyph);
        for (unsigned y = 0; y < glyph.height; y++) {
            for (unsigned x = 0; x < glyph.width; x++) {
                int px = pen + glyph.left + (int)x, py = baseline - glyph.top + (int)y;
                if (px >= 0 && py >= 0 && px < CANVAS_W && py < CANVAS_H && glyph.coverage[y * glyph.pitch + x] != 0)
                    composed[py * CANVAS_W + px] = 0x00000000u;
            }
        }
        pen += glyph.advance;
    }
    expect_int(c, "gfx_blit_text draws the strike's glyphs", memcmp(drawn, composed, sizeof drawn) == 0, 1);

    expect_int(c, "the monospace slot takes Liberation Mono", gfx_face_install(GFX_FACE_MONO, &cell_face), 0);
    expect_int(c, "Liberation Mono's cell at 13 ppem", gfx_cell_width(), 8);
    expect_int(c, "Liberation Mono's cell height at 13 ppem", gfx_cell_height(), 15);

    gfx_face_install(GFX_FACE_UI, NULL);
    gfx_face_install(GFX_FACE_MONO, NULL);
    expect_int(c, "the baked UI face is back: line", gfx_text_height(), baked_height);
    expect_int(c, "the baked UI face is back: width", gfx_text_width(kSampleUtf8), baked_width);
    expect_int(c, "the baked monospace face is back", gfx_cell_width(), 8);
    sx_font_face_close(sans);
    sx_font_face_close(mono);
}

int sxfont_selftest(sxfont_selftest_load load, sxfont_selftest_report report, void *user) {
    struct context c = {load, report, user, 0};
    struct file files[12];
    for (unsigned i = 0; i < 12; i++) {
        files[i].data = load(user, kFiles[i], &files[i].size);
        if (files[i].data == NULL) {
            char line[120];
            snprintf(line, sizeof line, "%s is there to read", kFiles[i]);
            fail(&c, line);
            return c.failures;
        }
    }
    struct sx_font_engine *engine = NULL;
    if (sx_font_engine_create(&engine) != SX_FONT_OK) {
        fail(&c, "the engine starts");
        return c.failures;
    }
    check_faces(&c, engine, files);
    check_errors(&c, engine, files);
    check_metrics(&c, engine, files);
    check_render_modes(&c, engine, files);
    check_glyphs(&c, engine, files);
    check_space_and_bold(&c, engine, files);
    check_cache(&c, engine, files);
    check_catalog(&c, engine, files);
    check_text_primitives(&c, engine, files);
    sx_font_engine_destroy(engine);
    return c.failures;
}
