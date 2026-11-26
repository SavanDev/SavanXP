/* The text primitives of pixels.c, for real (docs/desktop/UI_FONTS.md,
 * "Faces at run time"). gfx2d-test stubs them out to see which path the
 * painter takes; this one draws.
 *
 * The baked faces' checksums were recorded from pixels.c before faces could
 * be installed, so a face slot that changes one pixel of the baked text, its
 * widths or its heights fails here. Then a face of the test's own is
 * installed in each slot and must be what the primitives measure and draw,
 * and removing it must bring the baked face back. */

#include <stdio.h>
#include <string.h>

#include "sxgfx/pixels.h"

static int g_failures;

static void expect(long actual, long expected, const char* what) {
    if (actual != expected) {
        printf("  FAIL %s: %ld, expected %ld\n", what, actual, expected);
        g_failures++;
    }
}

#define W 160
#define H 24
static uint32_t g_pixels[W * H];
static struct savanxp_fb_info g_info;

static void canvas(uint32_t background) {
    memset(&g_info, 0, sizeof g_info);
    g_info.width = W;
    g_info.height = H;
    g_info.pitch = W * 4u;
    g_info.bpp = 32u;
    g_info.buffer_size = g_info.pitch * H;
    for (int i = 0; i < W * H; ++i) g_pixels[i] = background;
}

static unsigned long checksum(void) {
    unsigned long hash = 2166136261ul;
    const unsigned char* bytes = (const unsigned char*)g_pixels;
    for (size_t i = 0; i < sizeof g_pixels; ++i) {
        hash ^= bytes[i];
        hash = (hash * 16777619ul) & 0xfffffffful;
    }
    return hash;
}

/* Text with accents, an invalid byte, a codepoint no table has, and box
 * drawing for the monospace face. */
static const char* const kSamples[] = {
    "SavanXP: WordPad, ni\xc3\xb1o 0123",
    "\xc3\x81rbol \xc3\xa9\xc3\xa8 \xc2\xbf? \xff \xe2\x80\x94 \xe4\xb8\xad",
    "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90 Wg|_",
};

struct path {
    const char* name;
    int (*width)(const char*);
    int (*height)(void);
    void (*blit)(uint32_t*, const struct savanxp_fb_info*, int, int, const char*, uint32_t);
    void (*blit_clip)(uint32_t*, const struct savanxp_fb_info*, int, int, const char*, uint32_t, int, int, int, int);
};

static int mono_height(void) { return gfx_cell_height(); }

static const struct path kPaths[3] = {
    {"ui", gfx_text_width, gfx_text_height, gfx_blit_text, gfx_blit_text_clip},
    {"mono", gfx_text_width_mono, mono_height, gfx_blit_text_mono, gfx_blit_text_mono_clip},
    {"title", gfx_text_width_title, gfx_text_height_title, gfx_blit_text_title, gfx_blit_text_title_clip},
};

/* One number per path: every sample's width, the height, and the canvas
 * after drawing it plain on white and clipped on grey at an odd offset. */
static unsigned long path_checksum(const struct path* p) {
    unsigned long hash = 2166136261ul;
    for (size_t i = 0; i < sizeof kSamples / sizeof kSamples[0]; ++i) {
        int numbers[2] = {p->width(kSamples[i]), p->height()};
        for (size_t b = 0; b < sizeof numbers; ++b) {
            hash ^= ((const unsigned char*)numbers)[b];
            hash = (hash * 16777619ul) & 0xfffffffful;
        }
        canvas(0x00ffffffu);
        p->blit(g_pixels, &g_info, 3, 2, kSamples[i], 0x00000080u);
        hash ^= checksum();
        hash = (hash * 16777619ul) & 0xfffffffful;
        canvas(0x00c0c0c0u);
        p->blit_clip(g_pixels, &g_info, -5, 4, kSamples[i], 0x00102030u, 7, 5, 101, 15);
        hash ^= checksum();
        hash = (hash * 16777619ul) & 0xfffffffful;
    }
    return hash;
}

static void check_baked(const char* when) {
    /* Recorded from pixels.c as it was before face slots. */
    static const unsigned long kBaked[3] = {1717711241ul, 927950123ul, 3376070509ul};
    char what[96];
    for (int i = 0; i < 3; ++i) {
        snprintf(what, sizeof what, "%s: the baked %s face", when, kPaths[i].name);
        expect((long)path_checksum(&kPaths[i]), (long)kBaked[i], what);
    }
    expect(gfx_text_height(), 13, "the baked UI line");
    expect(gfx_text_height_title(), 17, "the baked caption line");
    expect(gfx_cell_width(), 8, "the baked cell width");
    expect(gfx_cell_height(), 16, "the baked cell height");
}

/* A face of the test's own: every codepoint is a solid block `codepoint % 4
 * + 2` pixels wide and 5 rows high, one pixel right of the pen, sitting on
 * the baseline, advancing one pixel more than its width. 'x' is blank. */
static unsigned char g_block[16 * 5];
static int g_glyph_calls;
static int g_advance_calls;

static void block_glyph(void* context, unsigned int codepoint, struct gfx_glyph* out) {
    int* tag = context;
    (*tag)++;
    g_glyph_calls++;
    out->width = codepoint == 'x' ? 0 : (int)(codepoint % 4u) + 2;
    out->rows = codepoint == 'x' ? 0 : 5;
    out->left = 1;
    out->top = 5;
    out->advance = (int)(codepoint % 4u) + 3;
    out->pitch = 16;
    out->coverage = codepoint == 'x' ? 0 : g_block;
}

static int block_advance(void* context, unsigned int codepoint) {
    (void)context;
    g_advance_calls++;
    return (int)(codepoint % 4u) + 3;
}

static int count_ink(uint32_t ink) {
    int total = 0;
    for (int i = 0; i < W * H; ++i) total += g_pixels[i] == ink;
    return total;
}

static void check_installed(void) {
    memset(g_block, 0xff, sizeof g_block);
    int tag = 0;
    struct gfx_face face = {9, 12, 0, block_glyph, block_advance, &tag};
    /* "AB": 'A' (65 % 4 = 1) is 3 wide, advances 4; 'B' is 4 wide, advances 5. */
    expect(gfx_face_install(GFX_FACE_UI, &face), 0, "a face goes into the UI slot");
    expect(gfx_face_installed(GFX_FACE_UI), 1, "the UI slot says it holds a face");
    expect(gfx_face_installed(GFX_FACE_TITLE), 0, "the caption slot still holds its baked face");
    expect(gfx_text_height(), 12, "the installed UI line");
    g_advance_calls = 0;
    expect(gfx_text_width("AB"), 9, "the installed UI width");
    expect(g_advance_calls, 2, "measuring asks for advances only");
    canvas(0);
    tag = 0;
    gfx_blit_text(g_pixels, &g_info, 10, 2, "AB", 0x00ffffffu);
    expect(tag, 2, "drawing passes the face its context");
    expect(count_ink(0x00ffffffu), (3 + 4) * 5, "the installed glyphs' pixels");
    /* 'A' at pen 10 + left 1, rows from baseline 2 + 9 - 5 = 6 to 10. */
    expect(g_pixels[6 * W + 11] == 0x00ffffffu, 1, "'A' starts left + 1, top - 5 from the baseline");
    expect(g_pixels[5 * W + 11] == 0, 1, "nothing above 'A'");
    expect(g_pixels[6 * W + 15] == 0x00ffffffu, 1, "'B' starts at 'A''s advance");
    canvas(0);
    gfx_blit_text_clip(g_pixels, &g_info, 10, 2, "AB", 0x00ffffffu, 0, 0, 13, 8);
    expect(count_ink(0x00ffffffu), 2 * 2, "the installed face is clipped");
    canvas(0);
    gfx_blit_text(g_pixels, &g_info, 0, 0, "xx", 0x00ffffffu);
    expect(count_ink(0x00ffffffu), 0, "a glyph without coverage draws nothing");

    /* Without advance(), measuring draws; the caption keeps its ink rule. */
    struct gfx_face title = {9, 14, 0, block_glyph, 0, &tag};
    expect(gfx_face_install(GFX_FACE_TITLE, &title), 0, "a face goes into the caption slot");
    g_glyph_calls = 0;
    /* 'C' (3) is 5 wide and advances 6: ink ends at left 1 + 5 = 6. */
    expect(gfx_text_width_title("C"), 6, "the caption measures to its ink or its advance");
    /* 'D' (0) is 2 wide and advances 3: ink and advance both end at 3. */
    expect(gfx_text_width_title("D"), 3, "a glyph narrower than its advance");
    expect(g_glyph_calls, 2, "without advance(), measuring asks for glyphs");
    expect(gfx_text_height_title(), 14, "the installed caption line");

    /* The monospace slot advances by the cell whatever the glyph says. */
    struct gfx_face mono = {9, 11, 7, block_glyph, 0, &tag};
    struct gfx_face no_cell = {9, 11, 0, block_glyph, 0, &tag};
    expect(gfx_face_install(GFX_FACE_MONO, &no_cell), -1, "a monospace face needs a cell");
    expect(gfx_face_install(GFX_FACE_MONO, &mono), 0, "a face goes into the monospace slot");
    expect(gfx_cell_width(), 7, "the installed cell width");
    expect(gfx_cell_height(), 11, "the installed cell height");
    expect(gfx_text_width_mono("A\xc3\xa9" "B"), 21, "three cells");
    canvas(0);
    gfx_blit_text_mono(g_pixels, &g_info, 0, 0, "AA", 0x00ffffffu);
    expect(g_pixels[4 * W + 1] == 0x00ffffffu && g_pixels[4 * W + 8] == 0x00ffffffu, 1,
           "the second cell starts 7 pixels on");
    canvas(0);
    gfx_blit_text_mono_clip(g_pixels, &g_info, 0, 0, "AA", 0x00ffffffu, 0, 0, 8, 24);
    /* The second cell's ink starts at 8, past the clip. */
    expect(count_ink(0x00ffffffu), 3 * 5, "the monospace face is clipped");

    /* Refused: a slot that does not exist, a face that cannot draw. */
    struct gfx_face broken = {9, 12, 0, 0, 0, 0};
    struct gfx_face flat = {9, 0, 0, block_glyph, 0, 0};
    expect(gfx_face_install(-1, &face), -1, "slot -1");
    expect(gfx_face_install(GFX_FACE_SLOTS, &face), -1, "a slot past the last");
    expect(gfx_face_install(GFX_FACE_UI, &broken), -1, "a face without glyph()");
    expect(gfx_face_install(GFX_FACE_UI, &flat), -1, "a face with no line height");
    expect(gfx_face_installed(GFX_FACE_UI), 1, "a refused face leaves the installed one");
    expect(gfx_face_installed(GFX_FACE_SLOTS), 0, "a slot past the last holds nothing");

    for (int slot = 0; slot < GFX_FACE_SLOTS; ++slot) expect(gfx_face_install(slot, 0), 0, "NULL brings the baked face back");
    expect(gfx_face_installed(GFX_FACE_UI), 0, "the UI slot is baked again");
}

int main(void) {
    check_baked("before");
    check_installed();
    check_baked("after");
    if (g_failures != 0) {
        printf("pixels-text-test: %d checks failed\n", g_failures);
        return 1;
    }
    printf("pixels-text-test: PASS\n");
    return 0;
}
