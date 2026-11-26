/* The interface faces from /system/fonts (sxfont/ui.h). */

#include "sxfont/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxfont/font.h"
#include "sxgfx/pixels.h"

/* Weak: a static program links this unit without libsxfont, and then these
 * are null and the baked tables stay. */
#pragma weak sx_font_engine_create
#pragma weak sx_font_engine_destroy
#pragma weak sx_font_face_open
#pragma weak sx_font_strike_open
#pragma weak sx_font_gfx_face

/* Each item's slot, default file and size. The defaults are the faces the
 * tables were baked from, at the sizes they were baked at, so they draw the
 * same pixels (ui-face-files-test). The monospace slot keeps UniFont's
 * table. */
static const struct {
    int slot;
    const char *key;
    const char *path;
    unsigned size;
    unsigned min_size, max_size;
} kItems[SXFONT_UI_ITEMS] = {
    {GFX_FACE_UI, "body", "/system/fonts/MSSansSerif.ttf", 13, SXFONT_UI_BODY_MIN_SIZE, SXFONT_UI_BODY_MAX_SIZE},
    {GFX_FACE_TITLE, "caption", "/system/fonts/RetroSans.ttf", 17, SXFONT_UI_CAPTION_MIN_SIZE,
     SXFONT_UI_CAPTION_MAX_SIZE},
};

/* ---- the configuration ------------------------------------------------------ */

void sxfont_ui_default_config(struct sxfont_ui_config *config) {
    if (config == NULL) return;
    memset(config, 0, sizeof *config);
    for (int i = 0; i < SXFONT_UI_ITEMS; i++) {
        snprintf(config->items[i].path, sizeof config->items[i].path, "%s", kItems[i].path);
        config->items[i].size = kItems[i].size;
    }
}

unsigned sxfont_ui_min_size(int item) {
    return item >= 0 && item < SXFONT_UI_ITEMS ? kItems[item].min_size : 0;
}

unsigned sxfont_ui_max_size(int item) {
    return item >= 0 && item < SXFONT_UI_ITEMS ? kItems[item].max_size : 0;
}

/* A decimal number of at most three digits that is the whole value. */
static int parse_size(const char *value, size_t length, unsigned *out) {
    unsigned number = 0;
    if (length == 0 || length > 3) return -1;
    for (size_t i = 0; i < length; i++) {
        if (value[i] < '0' || value[i] > '9') return -1;
        number = number * 10u + (unsigned)(value[i] - '0');
    }
    *out = number;
    return 0;
}

static int key_is(const char *key, size_t length, const char *prefix, const char *suffix) {
    size_t p = strlen(prefix), s = strlen(suffix);
    return length == p + 1 + s && memcmp(key, prefix, p) == 0 && key[p] == '_' && memcmp(key + p + 1, suffix, s) == 0;
}

static void parse_line(struct sxfont_ui_config *config, const char *line, size_t length) {
    const char *equals = memchr(line, '=', length);
    if (equals == NULL) return;
    size_t key_length = (size_t)(equals - line);
    const char *value = equals + 1;
    size_t value_length = length - key_length - 1;
    unsigned number;
    if (key_length == 9 && memcmp(line, "smoothing", 9) == 0) {
        if (value_length == 1 && (value[0] == '0' || value[0] == '1')) config->smoothing = value[0] == '1';
        return;
    }
    for (int i = 0; i < SXFONT_UI_ITEMS; i++) {
        if (key_is(line, key_length, kItems[i].key, "font")) {
            /* Absolute, and whole: a cut path would name another file. */
            if (value_length > 1 && value[0] == '/' && value_length < sizeof config->items[i].path) {
                memcpy(config->items[i].path, value, value_length);
                config->items[i].path[value_length] = '\0';
            }
        } else if (key_is(line, key_length, kItems[i].key, "size")) {
            if (parse_size(value, value_length, &number) == 0 && number >= kItems[i].min_size &&
                number <= kItems[i].max_size)
                config->items[i].size = number;
        }
    }
}

void sxfont_ui_parse_config(struct sxfont_ui_config *config, const char *text) {
    if (config == NULL) return;
    sxfont_ui_default_config(config);
    while (text != NULL && *text != '\0') {
        size_t length = strcspn(text, "\n");
        size_t trimmed = length;
        while (trimmed > 0 && (text[trimmed - 1] == '\r' || text[trimmed - 1] == ' ')) trimmed--;
        parse_line(config, text, trimmed);
        text += length;
        if (*text == '\n') text++;
    }
}

size_t sxfont_ui_format_config(const struct sxfont_ui_config *config, char *out, size_t capacity) {
    if (config == NULL || out == NULL || capacity == 0) return 0;
    int length = snprintf(out, capacity, "%s_font=%s\n%s_size=%u\n%s_font=%s\n%s_size=%u\nsmoothing=%d\n",
                          kItems[0].key, config->items[0].path, kItems[0].key, config->items[0].size, kItems[1].key,
                          config->items[1].path, kItems[1].key, config->items[1].size, config->smoothing ? 1 : 0);
    if (length < 0 || (size_t)length >= capacity) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)length;
}

int sxfont_ui_config_equal(const struct sxfont_ui_config *a, const struct sxfont_ui_config *b) {
    if (a == NULL || b == NULL) return a == b;
    if ((a->smoothing != 0) != (b->smoothing != 0)) return 0;
    for (int i = 0; i < SXFONT_UI_ITEMS; i++)
        if (a->items[i].size != b->items[i].size || strcmp(a->items[i].path, b->items[i].path) != 0) return 0;
    return 1;
}

void sxfont_ui_read_config(struct sxfont_ui_config *config, sxfont_ui_read_file read_file, const char *path) {
    size_t size = 0;
    char *data = path != NULL && read_file != NULL ? read_file(path, &size) : NULL;
    if (data == NULL || size >= SXFONT_UI_CONFIG_BYTES * 4) {
        free(data);
        sxfont_ui_default_config(config);
        return;
    }
    char text[SXFONT_UI_CONFIG_BYTES * 4];
    memcpy(text, data, size);
    text[size] = '\0';
    free(data);
    sxfont_ui_parse_config(config, text);
}

/* ---- the faces -------------------------------------------------------------- */

/* Every file read for an item, the failed tries included: a face that opened
 * points at its bytes until the engine closes it. */
#define SXFONT_UI_BUFFERS (SXFONT_UI_ITEMS * 2)

struct sxfont_ui_faces {
    struct sx_font_engine *engine;
    void *buffers[SXFONT_UI_BUFFERS];
    int buffer_count;
    int has[SXFONT_UI_ITEMS];
    struct gfx_face gfx[SXFONT_UI_ITEMS];
};

static int libsxfont_mapped(void) {
    return sx_font_engine_create != NULL && sx_font_engine_destroy != NULL && sx_font_face_open != NULL &&
           sx_font_strike_open != NULL && sx_font_gfx_face != NULL;
}

/* One item's face at `size`, from `path`. Its face and strike live in the
 * engine. */
static int open_item(struct sxfont_ui_faces *faces, int item, const char *path, unsigned size, unsigned flags,
                     sxfont_ui_read_file read_file) {
    size_t length = 0;
    struct sx_font_face *face = NULL;
    struct sx_font_strike *strike = NULL;
    struct sx_font_strike_options options = {size, flags};
    if (faces->buffer_count == SXFONT_UI_BUFFERS) return -1;
    void *data = read_file(path, &length);
    if (data == NULL) return -1;
    faces->buffers[faces->buffer_count++] = data;
    if (sx_font_face_open(faces->engine, data, length, 0, &face) != SX_FONT_OK ||
        sx_font_strike_open(face, &options, &strike) != SX_FONT_OK ||
        sx_font_gfx_face(strike, &faces->gfx[item]) != SX_FONT_OK)
        return -1;
    faces->has[item] = 1;
    return 0;
}

struct sxfont_ui_faces *sxfont_ui_faces_open(const struct sxfont_ui_config *config, sxfont_ui_read_file read_file) {
    if (config == NULL || read_file == NULL || !libsxfont_mapped()) return NULL;
    struct sxfont_ui_faces *faces = calloc(1, sizeof *faces);
    if (faces == NULL) return NULL;
    if (sx_font_engine_create(&faces->engine) != SX_FONT_OK) {
        free(faces);
        return NULL;
    }
    /* Without smoothing, 1-bit at every size: the chrome's text is never
     * grey (UI_FONTS.md). With it, what each face's gasp table says. */
    unsigned flags = config->smoothing ? 0u : SX_FONT_FORCE_MONO;
    for (int i = 0; i < SXFONT_UI_ITEMS; i++) {
        const struct sxfont_ui_item *item = &config->items[i];
        if (open_item(faces, i, item->path, item->size, flags, read_file) == 0) continue;
        /* A file that went missing or does not open: the default face, so
         * the interface keeps the look it was made for. */
        if (strcmp(item->path, kItems[i].path) != 0 || item->size != kItems[i].size)
            (void)open_item(faces, i, kItems[i].path, kItems[i].size, flags, read_file);
    }
    return faces;
}

int sxfont_ui_faces_count(const struct sxfont_ui_faces *faces) {
    int count = 0;
    for (int i = 0; faces != NULL && i < SXFONT_UI_ITEMS; i++) count += faces->has[i];
    return count;
}

void sxfont_ui_faces_install(const struct sxfont_ui_faces *faces) {
    for (int i = 0; i < SXFONT_UI_ITEMS; i++) {
        if (faces == NULL || !faces->has[i] || gfx_face_install(kItems[i].slot, &faces->gfx[i]) != 0)
            (void)gfx_face_install(kItems[i].slot, NULL);
    }
}

void sxfont_ui_faces_close(struct sxfont_ui_faces *faces) {
    if (faces == NULL) return;
    sx_font_engine_destroy(faces->engine);
    for (int i = 0; i < faces->buffer_count; i++) free(faces->buffers[i]);
    free(faces);
}

/* ---- the process's faces ---------------------------------------------------- */

static struct sxfont_ui_faces *g_current;
static int g_loaded = -1;

int sxfont_ui_reload(sxfont_ui_read_file read_file, const char *config_path) {
    struct sxfont_ui_config config;
    sxfont_ui_read_config(&config, read_file, config_path);
    struct sxfont_ui_faces *faces = sxfont_ui_faces_open(&config, read_file);
    if (faces == NULL && g_current != NULL) {
        /* Out of memory: what is on screen stays. */
        return g_loaded;
    }
    sxfont_ui_faces_install(faces);
    /* The slots point at the new faces now, so the old ones can go. */
    sxfont_ui_faces_close(g_current);
    g_current = faces;
    g_loaded = sxfont_ui_faces_count(faces);
    return g_loaded;
}

int sxfont_ui_load(sxfont_ui_read_file read_file, const char *config_path) {
    if (g_loaded >= 0) return g_loaded;
    return sxfont_ui_reload(read_file, config_path);
}

const struct sxfont_ui_faces *sxfont_ui_current(void) {
    return g_current;
}
