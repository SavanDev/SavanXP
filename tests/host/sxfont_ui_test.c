/* The interface faces' configuration and loader (include/sxfont/ui.h,
 * docs/desktop/UI_FONTS.md, "Choosing the faces"): appearance.cfg's format,
 * the faces it opens, the fallback to the default face when a file is
 * missing, the 1-bit rule and smoothing, and the reload that replaces the
 * installed faces.
 *
 * Paths under /system/fonts are read from the source tree, where the build
 * stages them from. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxfont/ui.h"
#include "sxgfx/pixels.h"

static int g_failures;
static char g_config_text[1024];

static void check(int ok, const char *what) {
    if (!ok) {
        printf("  FAIL %s\n", what);
        g_failures++;
    }
}

static void *read_host_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void *data = length > 0 ? malloc((size_t)length) : NULL;
    if (data != NULL && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

/* /system/fonts/X from where the build takes X; "/config" is g_config_text. */
static void *read_file(const char *path, size_t *size) {
    static const char prefix[] = "/system/fonts/";
    char host[512];
    if (strcmp(path, "/config") == 0) {
        size_t length = strlen(g_config_text);
        char *copy = malloc(length + 1);
        if (copy == NULL) return NULL;
        memcpy(copy, g_config_text, length + 1);
        *size = length;
        return copy;
    }
    if (strncmp(path, prefix, sizeof prefix - 1) != 0) return NULL;
    const char *name = path + sizeof prefix - 1;
    snprintf(host, sizeof host, "%s/%s", strncmp(name, "Liberation", 10) == 0 ? SAVANXP_LIBERATION_DIR
                                                                               : SAVANXP_UI_FONT_DIR, name);
    return read_host_file(host, size);
}

static void test_format(void) {
    struct sxfont_ui_config defaults, config, again;
    char text[SXFONT_UI_CONFIG_BYTES];

    sxfont_ui_default_config(&defaults);
    check(strcmp(defaults.items[SXFONT_UI_ITEM_BODY].path, "/system/fonts/MSSansSerif.ttf") == 0 &&
              defaults.items[SXFONT_UI_ITEM_BODY].size == 13 &&
              strcmp(defaults.items[SXFONT_UI_ITEM_CAPTION].path, "/system/fonts/RetroSans.ttf") == 0 &&
              defaults.items[SXFONT_UI_ITEM_CAPTION].size == 17 && defaults.smoothing == 0,
          "the defaults are the baked faces at their sizes, unsmoothed");
    sxfont_ui_parse_config(&config, "");
    check(sxfont_ui_config_equal(&config, &defaults), "an empty file is the defaults");

    sxfont_ui_parse_config(&config,
                           "body_size=40\ncaption_size=abc\nbody_font=relative.ttf\nsmoothing=2\nunknown=1\n"
                           "caption_font=\nbody_size=9\ncaption_size=21\n");
    check(sxfont_ui_config_equal(&config, &defaults), "bad values and unknown keys leave the defaults");

    sxfont_ui_parse_config(&config, "caption_size=12\r\nbody_font=/system/fonts/LiberationSans-Regular.ttf\r\n"
                                    "smoothing=1\r\nbody_size=16");
    check(config.items[SXFONT_UI_ITEM_CAPTION].size == 12 &&
              strcmp(config.items[SXFONT_UI_ITEM_BODY].path, "/system/fonts/LiberationSans-Regular.ttf") == 0 &&
              config.items[SXFONT_UI_ITEM_BODY].size == 16 && config.smoothing == 1,
          "good values are taken, with CRLF lines and no final newline");

    size_t length = sxfont_ui_format_config(&config, text, sizeof text);
    sxfont_ui_parse_config(&again, text);
    check(length > 0 && length == strlen(text) && sxfont_ui_config_equal(&config, &again),
          "the file it writes reads back as the same configuration");
    check(sxfont_ui_format_config(&config, text, 8) == 0 && text[0] == '\0', "a buffer too small gives nothing");

    check(sxfont_ui_min_size(SXFONT_UI_ITEM_BODY) == SXFONT_UI_BODY_MIN_SIZE &&
              sxfont_ui_max_size(SXFONT_UI_ITEM_CAPTION) == SXFONT_UI_CAPTION_MAX_SIZE &&
              sxfont_ui_max_size(SXFONT_UI_ITEMS) == 0,
          "each item's size range");
}

/* Whether the text primitives' glyph for 'a' in `slot` has grey pixels. */
static int has_grey(int slot) {
    static uint32_t pixels[64 * 64];
    struct savanxp_fb_info info = {0};
    info.width = 64;
    info.height = 64;
    info.pitch = 64 * 4;
    info.bpp = 32;
    for (size_t i = 0; i < 64 * 64; i++) pixels[i] = 0x00ffffffu;
    if (slot == GFX_FACE_TITLE)
        gfx_blit_text_title(pixels, &info, 4, 4, "a", 0);
    else
        gfx_blit_text(pixels, &info, 4, 4, "a", 0);
    for (size_t i = 0; i < 64 * 64; i++) {
        uint32_t value = pixels[i] & 0xffu;
        if (value != 0 && value != 0xffu) return 1;
    }
    return 0;
}

static void test_faces(void) {
    struct sxfont_ui_config config;
    struct sxfont_ui_faces *faces;
    int default_width;

    sxfont_ui_default_config(&config);
    faces = sxfont_ui_faces_open(&config, read_file);
    check(faces != NULL && sxfont_ui_faces_count(faces) == 2, "the default faces open from their files");
    sxfont_ui_faces_install(faces);
    check(gfx_face_installed(GFX_FACE_UI) && gfx_face_installed(GFX_FACE_TITLE) &&
              !gfx_face_installed(GFX_FACE_MONO),
          "installing them fills the body and caption slots only");
    check(gfx_text_height() == 13 && gfx_text_height_title() == 17, "the default lines are the tables'");
    default_width = gfx_text_width("Window Text");

    {
        struct sxfont_ui_config other = config;
        struct sxfont_ui_faces *liberation;
        snprintf(other.items[SXFONT_UI_ITEM_BODY].path, sizeof other.items[0].path, "%s",
                 "/system/fonts/LiberationSans-Regular.ttf");
        other.items[SXFONT_UI_ITEM_BODY].size = 15;
        liberation = sxfont_ui_faces_open(&other, read_file);
        sxfont_ui_faces_install(liberation);
        check(sxfont_ui_faces_count(liberation) == 2 && gfx_text_height() != 13 &&
                  gfx_text_width("Window Text") != default_width,
              "another body face measures as itself");
        sxfont_ui_faces_install(faces);
        check(gfx_text_height() == 13 && gfx_text_width("Window Text") == default_width,
              "installing the first faces again restores them");
        sxfont_ui_faces_close(liberation);
    }

    {
        struct sxfont_ui_config missing = config;
        struct sxfont_ui_faces *fallback;
        snprintf(missing.items[SXFONT_UI_ITEM_BODY].path, sizeof missing.items[0].path, "%s",
                 "/system/fonts/Missing.ttf");
        missing.items[SXFONT_UI_ITEM_BODY].size = 11;
        fallback = sxfont_ui_faces_open(&missing, read_file);
        sxfont_ui_faces_install(fallback);
        check(sxfont_ui_faces_count(fallback) == 2 && gfx_text_height() == 13 &&
                  gfx_text_width("Window Text") == default_width,
              "a missing file falls back to the default face at its size");
        sxfont_ui_faces_close(fallback);
    }

    {
        /* Liberation Sans' gasp asks for grey at 20 ppem: 1-bit without
         * smoothing, grey with it. MS Sans Serif's asks for 1-bit at every
         * size, so it stays so either way. */
        struct sxfont_ui_config outline = config;
        struct sxfont_ui_faces *mono, *smooth;
        snprintf(outline.items[SXFONT_UI_ITEM_CAPTION].path, sizeof outline.items[0].path, "%s",
                 "/system/fonts/LiberationSans-Regular.ttf");
        outline.items[SXFONT_UI_ITEM_CAPTION].size = 20;
        mono = sxfont_ui_faces_open(&outline, read_file);
        sxfont_ui_faces_install(mono);
        check(gfx_face_installed(GFX_FACE_TITLE) && !has_grey(GFX_FACE_TITLE) && !has_grey(GFX_FACE_UI),
              "without smoothing every interface face is 1-bit, an outline face whose gasp asks for grey included");
        outline.smoothing = 1;
        smooth = sxfont_ui_faces_open(&outline, read_file);
        sxfont_ui_faces_install(smooth);
        check(has_grey(GFX_FACE_TITLE), "with smoothing, a face whose gasp asks for grey draws grey");
        check(!has_grey(GFX_FACE_UI), "with smoothing, a face whose gasp asks for 1-bit stays 1-bit");
        sxfont_ui_faces_install(faces);
        sxfont_ui_faces_close(mono);
        sxfont_ui_faces_close(smooth);
    }

    sxfont_ui_faces_install(NULL);
    check(!gfx_face_installed(GFX_FACE_UI) && !gfx_face_installed(GFX_FACE_TITLE), "NULL puts the tables back");
    sxfont_ui_faces_close(faces);
}

static void test_reload(void) {
    check(sxfont_ui_current() == NULL, "nothing is current before the first load");
    snprintf(g_config_text, sizeof g_config_text, "body_font=/system/fonts/LiberationSerif-Regular.ttf\nbody_size=12\n");
    check(sxfont_ui_load(read_file, "/config") == 2 && sxfont_ui_current() != NULL && gfx_text_height() != 13,
          "the first load reads the configuration");
    snprintf(g_config_text, sizeof g_config_text, "%s", "");
    check(sxfont_ui_load(read_file, "/config") == 2 && gfx_text_height() != 13,
          "a second load answers as the first and changes nothing");
    check(sxfont_ui_reload(read_file, "/config") == 2 && gfx_text_height() == 13 && gfx_text_height_title() == 17,
          "a reload reads the configuration again and replaces the faces");
    check(sxfont_ui_reload(read_file, NULL) == 2 && gfx_text_height() == 13, "no configuration is the defaults");
}

int main(void) {
    test_format();
    test_faces();
    test_reload();
    if (g_failures != 0) {
        printf("sxfont-ui-test: %d checks failed\n", g_failures);
        return 1;
    }
    printf("sxfont-ui-test: PASS\n");
    return 0;
}
