#include "cursor_catalog.h"
#include "sxn/sxn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct cursor_image images[SAVANXP_CURSOR_SHAPE_COUNT];
static int background;
static const char *names[SAVANXP_CURSOR_SHAPE_COUNT] = {
    "ocr_normal.cur", "ocr_wait.cur", "ocr_ibeam.cur", "ocr_sizeall.cur",
    "ocr_sizewe.cur", "ocr_sizens.cur", "ocr_no.cur", "ocr_hand.cur",
    "ocr_sizenwse.cur", "ocr_sizenesw.cur"
};

/* Only used when even the on-disk arrow cannot be read. */
static void emergency_arrow(struct cursor_image *image) {
    int x, y;
    memset(image, 0, sizeof(*image));
    image->width = 12; image->height = 18;
    for (y = 0; y < 18; ++y) for (x = 0; x <= y / 2 && x < 12; ++x)
        image->pixels[y * 12 + x] = x == 0 || x == y / 2 || y == 17 ? 0xff000000u : 0xffffffffu;
}

void cursor_catalog_load(void) {
    unsigned shape;
    unsigned char *bytes = malloc(CUR_MAX_FILE_SIZE + 1);
    background = 0;
    for (shape = 0; shape < SAVANXP_CURSOR_SHAPE_COUNT; ++shape) {
        char path[96], log[192];
        const char *error = "out of memory";
        size_t size = 0;
        long handle = -1, result = 0;
        snprintf(path, sizeof(path), "/system/cursors/%s", names[shape]);
        if (bytes) {
            handle = sxn_open_file(path, SXN_FILE_READ);
            if (handle < 0) error = "cannot open file";
            else {
                while (size < CUR_MAX_FILE_SIZE + 1) {
                    result = sxn_read(handle, bytes + size, CUR_MAX_FILE_SIZE + 1 - size);
                    if (result <= 0) break;
                    size += (size_t)result;
                }
                (void)sxn_close(handle);
                error = result < 0 ? "cannot read file" : cursor_cur_decode(bytes, size, &images[shape]);
            }
        }
        if (error) {
            if (shape == SAVANXP_CURSOR_ARROW) emergency_arrow(&images[shape]);
            else images[shape] = images[SAVANXP_CURSOR_ARROW];
            snprintf(log, sizeof(log), "compositord: cursor %s: %s (status=%ld); using arrow", path, error, handle < 0 ? handle : result);
        } else snprintf(log, sizeof(log), "compositord: cursor %s loaded %dx%d hotspot=%d,%d background=%d",
            names[shape], images[shape].width, images[shape].height,
            images[shape].hotspot_x, images[shape].hotspot_y, images[shape].needs_background);
        (void)sxn_log(log);
        background |= images[shape].needs_background;
    }
    free(bytes);
}

const struct cursor_image *cursor_catalog_get(unsigned shape) {
    return &images[shape < SAVANXP_CURSOR_SHAPE_COUNT ? shape : SAVANXP_CURSOR_ARROW];
}
int cursor_catalog_needs_background(void) { return background; }
