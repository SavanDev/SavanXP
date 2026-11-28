/* GIF pictures through libnsgif (picture_gif.h), written from its header,
 * include/nsgif.h: the library asks the program for a bitmap to decode
 * into, and this one is a plain buffer of 32-bit pixels whose bytes are
 * B, G, R, A -- 0xAARRGGBB on this little-endian system, which is what
 * Web Viewer's painter blends. */

#include "picture_gif.h"

#include <nsgif.h>

#include <stdlib.h>
#include <string.h>

struct gif_bitmap
{
    uint32_t *pixels;
};

static nsgif_bitmap_t *gif_bitmap_create(int width, int height)
{
    struct gif_bitmap *bitmap;

    if (width <= 0 || height <= 0)
    {
        return NULL;
    }
    bitmap = calloc(1, sizeof(*bitmap));
    if (bitmap == NULL)
    {
        return NULL;
    }
    bitmap->pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
    if (bitmap->pixels == NULL)
    {
        free(bitmap);
        return NULL;
    }
    return bitmap;
}

static void gif_bitmap_destroy(nsgif_bitmap_t *bitmap)
{
    struct gif_bitmap *own = bitmap;

    if (own != NULL)
    {
        free(own->pixels);
        free(own);
    }
}

static uint8_t *gif_bitmap_buffer(nsgif_bitmap_t *bitmap)
{
    return (uint8_t *)((struct gif_bitmap *)bitmap)->pixels;
}

int webview_gif_is(const unsigned char *data, size_t size)
{
    return size >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0);
}

int webview_gif_decode(const unsigned char *data, size_t size, unsigned max_side, uint32_t **pixels, int *width,
    int *height)
{
    static const nsgif_bitmap_cb_vt k_bitmap = {
        .create = gif_bitmap_create,
        .destroy = gif_bitmap_destroy,
        .get_buffer = gif_bitmap_buffer,
    };
    nsgif_t *gif = NULL;
    nsgif_bitmap_t *frame = NULL;
    const nsgif_info_t *info;
    int result = -1;

    *pixels = NULL;
    if (nsgif_create(&k_bitmap, NSGIF_BITMAP_FMT_B8G8R8A8, &gif) != NSGIF_OK)
    {
        return -1;
    }
    /* A scan error past the first frame still leaves that frame. */
    (void)nsgif_data_scan(gif, size, data);
    nsgif_data_complete(gif);
    info = nsgif_get_info(gif);
    if (info != NULL && info->frame_count > 0 && info->width > 0 && info->height > 0 &&
        info->width <= max_side && info->height <= max_side &&
        nsgif_frame_decode(gif, 0, &frame) == NSGIF_OK && frame != NULL)
    {
        struct gif_bitmap *own = frame;

        /* Take the pixels: the library frees its bitmap with the GIF. */
        *pixels = own->pixels;
        own->pixels = NULL;
        *width = (int)info->width;
        *height = (int)info->height;
        result = 0;
    }
    nsgif_destroy(gif);
    return result;
}
