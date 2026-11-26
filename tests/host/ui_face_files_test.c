/* The interface faces from files draw what the baked tables draw
 * (docs/desktop/UI_FONTS.md, "Interface faces from files").
 *
 * MSSansSerif.ttf (genfont.py sfdttf, from Wine's .sfd) at 13 ppem and
 * RetroSans.ttf at 17, through libsxfont as the runtimes open them
 * (SX_FONT_FORCE_MONO), against gfx_font_noto.inc and gfx_font_title.inc:
 * the ascent and line, and for every codepoint the tables cover, the advance
 * and the lit pixels relative to the pen and the baseline. While this holds,
 * a check that reads the tables (tools/glyphs.py) reads what is on screen. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savanxp/font.h"
#include "sxgfx/pixels.h"

#include "gfx_font_noto.inc"
#include "gfx_font_title.inc"

static int g_failures;

static void fail(const char* line) {
    printf("  FAIL %s\n", line);
    g_failures++;
}

static void* read_file(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void* data = length > 0 ? malloc((size_t)length) : NULL;
    if (data != NULL && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

/* A glyph as a set of lit pixels on a canvas around the origin. */
#define SPAN 64
struct picture {
    unsigned char lit[SPAN * SPAN];
    int advance;
};

static void draw(struct picture* p, int left, int top, int width, int rows, int pitch, const unsigned char* coverage) {
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < width; x++) {
            int px = left + x + SPAN / 4, py = SPAN / 2 - top + y;
            if (px < 0 || py < 0 || px >= SPAN || py >= SPAN) continue;
            unsigned char value = coverage[y * pitch + x];
            if (value != 0) p->lit[py * SPAN + px] = value;
        }
    }
}

static void check_face(const char* file, unsigned ppem, int ascent, int line, const struct gfx_face* baked,
                       const unsigned int* codepoints, unsigned count) {
    size_t size = 0;
    void* data = read_file(file, &size);
    char what[200];
    if (data == NULL) {
        snprintf(what, sizeof what, "%s reads", file);
        fail(what);
        return;
    }
    struct sx_font_engine* engine = NULL;
    struct sx_font_face* face = NULL;
    struct sx_font_strike* strike = NULL;
    struct sx_font_strike_options options = {ppem, SX_FONT_FORCE_MONO};
    struct gfx_face from_file;
    if (sx_font_engine_create(&engine) != SX_FONT_OK || sx_font_face_open(engine, data, size, 0, &face) != SX_FONT_OK ||
        sx_font_strike_open(face, &options, &strike) != SX_FONT_OK || sx_font_gfx_face(strike, &from_file) != SX_FONT_OK) {
        snprintf(what, sizeof what, "%s opens at %u ppem", file, ppem);
        fail(what);
        sx_font_engine_destroy(engine);
        free(data);
        return;
    }
    if (from_file.ascent != ascent || from_file.line_height != line) {
        snprintf(what, sizeof what, "%s: ascent %d and line %d, the table's are %d and %d", file, from_file.ascent,
                 from_file.line_height, ascent, line);
        fail(what);
    }
    unsigned differ = 0;
    for (unsigned i = 0; i < count; i++) {
        static struct picture a, b;
        struct gfx_glyph g;
        memset(&a, 0, sizeof a);
        memset(&b, 0, sizeof b);
        baked->glyph(baked->context, codepoints[i], &g);
        a.advance = g.advance;
        if (g.coverage != NULL) draw(&a, g.left, g.top, g.width, g.rows, g.pitch, g.coverage);
        from_file.glyph(from_file.context, codepoints[i], &g);
        b.advance = g.advance;
        if (g.coverage != NULL) draw(&b, g.left, g.top, g.width, g.rows, g.pitch, g.coverage);
        if (memcmp(&a, &b, sizeof a) != 0) {
            if (differ++ < 8) {
                snprintf(what, sizeof what, "%s: U+%04X differs from the table (advance %d, file %d)", file,
                         codepoints[i], a.advance, b.advance);
                fail(what);
            }
        }
    }
    printf("  %s at %u ppem: %u codepoints compared, %u differ\n", file, ppem, count, differ);
    sx_font_engine_destroy(engine);
    free(data);
}

/* The baked tables as faces, the way pixels.c reads them. */
static void noto_glyph(void* context, unsigned int cp, struct gfx_glyph* out) {
    const struct sx_noto_glyph* g = sx_noto_glyph(cp);
    (void)context;
    *out = (struct gfx_glyph){g->width, g->rows, g->left, g->top, g->advance, g->width, kNotoCoverage + g->offset};
}

static void title_glyph(void* context, unsigned int cp, struct gfx_glyph* out) {
    const struct sx_title_glyph* g = sx_title_glyph(cp);
    (void)context;
    *out = (struct gfx_glyph){g->width, g->rows, g->left, g->top, g->advance, g->width, kTitleCoverage + g->offset};
}

int main(void) {
    static unsigned int noto[1024], title[1024];
    unsigned noto_count = 0, title_count = 0;
    for (int i = 0; i < kNotoGlyphRangeCount; i++)
        for (unsigned cp = kNotoGlyphRanges[i].first; cp <= kNotoGlyphRanges[i].last; cp++) noto[noto_count++] = cp;
    for (int i = 0; i < kTitleGlyphRangeCount; i++)
        for (unsigned cp = kTitleGlyphRanges[i].first; cp <= kTitleGlyphRanges[i].last; cp++) title[title_count++] = cp;
    /* Outside the tables: the body face's file is blank and space-wide there,
     * as the table's fallback. */
    noto[noto_count++] = 0x4e2d;
    noto[noto_count++] = 0xfffd;

    struct gfx_face baked_ui = {SX_NOTO_ASCENT, SX_NOTO_LINE_HEIGHT, 0, noto_glyph, NULL, NULL};
    struct gfx_face baked_title = {SX_TITLE_ASCENT, SX_TITLE_LINE_HEIGHT, 0, title_glyph, NULL, NULL};
    check_face(SAVANXP_UI_FONT_DIR "/MSSansSerif.ttf", 13, SX_NOTO_ASCENT, SX_NOTO_LINE_HEIGHT, &baked_ui, noto,
               noto_count);
    check_face(SAVANXP_UI_FONT_DIR "/RetroSans.ttf", 17, SX_TITLE_ASCENT, SX_TITLE_LINE_HEIGHT, &baked_title, title,
               title_count);
    if (g_failures != 0) {
        printf("ui-face-files-test: %d checks failed\n", g_failures);
        return 1;
    }
    printf("ui-face-files-test: PASS\n");
    return 0;
}
