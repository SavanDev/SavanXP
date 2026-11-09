#include "cursor_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct cursor_image images[SAVANXP_CURSOR_SHAPE_COUNT];
static char selected[SXN_CURSOR_ID_SIZE] = "classic";
static long save_status;
static int background;
static const char *roles[SAVANXP_CURSOR_SHAPE_COUNT] = {
    "arrow", "wait", "text", "move", "resize_h", "resize_v", "unavailable", "link",
    "resize_nwse", "resize_nesw"
};

static int valid_id(const char *id) {
    unsigned i;
    if (!id || !id[0]) return 0;
    for (i = 0; i < SXN_CURSOR_ID_SIZE; ++i) {
        char c = id[i];
        if (!c) return 1;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 0;
}
static long read_file(const char *path, void *bytes, size_t capacity) {
    long handle = sxn_open_file(path, SXN_FILE_READ), result;
    size_t done = 0;
    if (handle < 0) return handle;
    while (done < capacity) {
        result = sxn_read(handle, (unsigned char *)bytes + done, capacity - done);
        if (result <= 0) { (void)sxn_close(handle); return result < 0 ? result : (long)done; }
        done += (size_t)result;
    }
    (void)sxn_close(handle);
    return -SAVANXP_E2BIG;
}
unsigned cursor_catalog_discover(struct cursor_theme themes[CURSOR_MAX_THEMES]) {
    char text[4096], *line;
    long size = read_file("/system/cursors/themes.list", text, sizeof(text)-1);
    unsigned count = 0;
    if (size < 0) return 0;
    text[size] = 0;
    line = text;
    while (*line && count < CURSOR_MAX_THEMES) {
        char *end = strchr(line, '\n'), *name, *license;
        unsigned i;
        if (end) *end = 0;
        name = strchr(line, '|');
        license = name ? strchr(name+1, '|') : 0;
        if (name && license) {
            *name++ = 0; *license++ = 0;
            for (i = 0; i < count && strcmp(themes[i].id, line); ++i) {}
            if (i == count && valid_id(line) && name[0] && strlen(name) < sizeof(themes[0].name) &&
                license[0] && strlen(license) < sizeof(themes[0].license)) {
                strcpy(themes[count].id, line); strcpy(themes[count].name, name); strcpy(themes[count].license, license);
                ++count;
            }
        }
        if (!end) break;
        line = end + 1;
    }
    return count;
}

/* Only used when even the on-disk classic arrow cannot be read. */
static void emergency_arrow(struct cursor_image *image) {
    int x, y;
    memset(image, 0, sizeof(*image));
    image->width = 12; image->height = 18;
    for (y = 0; y < 18; ++y) for (x = 0; x <= y / 2 && x < 12; ++x)
        image->pixels[y * 12 + x] = x == 0 || x == y / 2 || y == 17 ? 0xff000000u : 0xffffffffu;
}
long cursor_catalog_read(const char *theme, struct cursor_image *output, int strict) {
    unsigned shape;
    unsigned char *bytes;
    if (!valid_id(theme) || !output) return -SAVANXP_EINVAL;
    bytes = malloc(CUR_MAX_FILE_SIZE + 1);
    if (!bytes) return -SAVANXP_ENOMEM;
    for (shape = 0; shape < SAVANXP_CURSOR_SHAPE_COUNT; ++shape) {
        char path[128], log[192];
        long size;
        const char *error;
        snprintf(path, sizeof(path), "/system/cursors/%s/%s.cur", theme, roles[shape]);
        size = read_file(path, bytes, CUR_MAX_FILE_SIZE + 1);
        error = size < 0 ? "cannot read file" : cursor_cur_decode(bytes, (size_t)size, &output[shape]);
        if (error) {
            snprintf(log, sizeof(log), "compositord: cursor %s: %s (status=%ld)", path, error, size);
            (void)sxn_log(log);
            if (strict) { free(bytes); return size < 0 ? size : -SAVANXP_EINVAL; }
            if (shape == SAVANXP_CURSOR_ARROW) emergency_arrow(&output[shape]);
            else output[shape] = output[SAVANXP_CURSOR_ARROW];
        }
    }
    free(bytes);
    return 0;
}
static void update_background(void) {
    unsigned shape;
    background = 0;
    for (shape = 0; shape < SAVANXP_CURSOR_SHAPE_COUNT; ++shape) background |= images[shape].needs_background;
}
long cursor_catalog_select(const char *theme) {
    struct cursor_theme themes[CURSOR_MAX_THEMES];
    struct cursor_image *candidate;
    unsigned count = cursor_catalog_discover(themes), i;
    long result;
    if (!valid_id(theme)) return -SAVANXP_EINVAL;
    for (i = 0; i < count && strcmp(theme, themes[i].id); ++i) {}
    if (i == count) return -SAVANXP_ENOENT;
    candidate = malloc(sizeof(images));
    if (!candidate) return -SAVANXP_ENOMEM;
    result = cursor_catalog_read(theme, candidate, 1);
    if (!result) {
        memcpy(images, candidate, sizeof(images));
        strcpy(selected, theme);
        update_background();
    }
    free(candidate);
    return result;
}
void cursor_catalog_load(void) {
    char saved[SXN_CURSOR_ID_SIZE+1];
    long count;
    if (cursor_catalog_read("classic", images, 0) < 0) {
        unsigned shape;
        emergency_arrow(&images[0]);
        for (shape = 1; shape < SAVANXP_CURSOR_SHAPE_COUNT; ++shape) images[shape] = images[0];
    }
    strcpy(selected, "classic"); update_background();
    count = read_file(SXN_CURSOR_SETTINGS_PATH, saved, sizeof(saved));
    if (count > 0) {
        saved[count] = 0;
        if (saved[count-1] == '\n') saved[count-1] = 0;
        if (cursor_catalog_select(saved) < 0)
            (void)sxn_log("compositord: saved cursor pack unavailable; using SavanXP Classic");
    }
    save_status = count < 0 && count != -SAVANXP_ENOENT ? count : 0;
    (void)sxn_log("compositord: cursor catalog ready");
}
long cursor_catalog_save(void) {
    char text[SXN_CURSOR_ID_SIZE+1];
    int size = snprintf(text, sizeof(text), "%s\n", selected);
    save_status = sxn_write_file(SXN_CURSOR_SETTINGS_PATH, text, (unsigned long)size);
    return save_status;
}
const char *cursor_catalog_theme(void) { return selected; }
long cursor_catalog_save_status(void) { return save_status; }
const struct cursor_image *cursor_catalog_get(unsigned shape) {
    return &images[shape < SAVANXP_CURSOR_SHAPE_COUNT ? shape : SAVANXP_CURSOR_ARROW];
}
int cursor_catalog_needs_background(void) { return background; }
