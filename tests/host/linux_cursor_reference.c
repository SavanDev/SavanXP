/* Compose the desktop's real cursor/shadow over an RGB Linux golden.
 * The protocol comparison must include every pupil pixel, even under the
 * cursor. This host-only adapter uses the compositor's portable routines. */
#include <stdio.h>
#include <stdlib.h>
#include "../../system/compositord/cursor_cur.h"
#include "../../system/compositord/shadow.h"

int main(int argc, char** argv) {
    if (argc != 6) return 2;
    const unsigned width = strtoul(argv[2], 0, 10), height = strtoul(argv[3], 0, 10);
    const int px = atoi(argv[4]), py = atoi(argv[5]);
    if (!width || !height || width > 4096 || height > 4096) return 2;
    FILE* file = fopen(argv[1], "rb"); if (!file) return 2;
    unsigned char* bytes = malloc(CUR_MAX_FILE_SIZE); if (!bytes) return 2;
    size_t size = fread(bytes, 1, CUR_MAX_FILE_SIZE, file); fclose(file);
    struct cursor_image asset;
    if (cursor_cur_decode(bytes, size, &asset)) return 2;
    free(bytes);
    struct sx_bitmap bitmap;
    if (!sx_bitmap_create(&bitmap, width, height, SX_PIXEL_FORMAT_BGRA8888)) return 2;
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        unsigned char rgb[3]; if (fread(rgb, 1, 3, stdin) != 3) return 2;
        bitmap.pixels[y * (bitmap.info.pitch / 4) + x] = 0xff000000u | (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
    }
    struct cursor_shadow shadow; cursor_shadow_build(&asset, &shadow);
    struct savanxp_fb_info info = {0}; struct sx_bitmap overlay;
    info.width = shadow.width; info.height = shadow.height; info.pitch = shadow.width * 4;
    info.bpp = 32; info.buffer_size = info.pitch * info.height;
    sx_bitmap_wrap(&overlay, shadow.pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    struct sx_painter painter; sx_painter_init(&painter, &bitmap);
    const int left = px - asset.hotspot_x, top = py - asset.hotspot_y;
    sx_painter_blit_bitmap(&painter, &overlay, left - CURSOR_SHADOW_MARGIN, top - CURSOR_SHADOW_MARGIN);
    for (int y = 0; y < asset.height; ++y) for (int x = 0; x < asset.width; ++x) {
        const int dx = left + x, dy = top + y;
        if (dx < 0 || dy < 0 || dx >= (int)width || dy >= (int)height) continue;
        uint32_t* pixel = &bitmap.pixels[dy * (bitmap.info.pitch / 4) + dx];
        *pixel = cursor_image_pixel(&asset, y * asset.width + x, *pixel);
    }
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const uint32_t pixel = bitmap.pixels[y * (bitmap.info.pitch / 4) + x];
        unsigned char rgb[3] = {pixel >> 16, pixel >> 8, pixel};
        if (fwrite(rgb, 1, 3, stdout) != 3) return 2;
    }
    sx_bitmap_destroy(&bitmap); return 0;
}
