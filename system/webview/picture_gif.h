#pragma once

/* GIF pictures for Web Viewer (docs/desktop/WEBVIEWER.md#images), through
 * NetSurf's libnsgif: the first frame, as 0xAARRGGBB with its transparent
 * pixels clear. No system calls: the network worker runs it. */

#include <stddef.h>
#include <stdint.h>

/* Whether `data` starts like a GIF. */
int webview_gif_is(const unsigned char *data, size_t size);

/* Decodes the first frame into malloc'd pixels, `*width` by `*height`, no
 * more than `max_side` a side. 0, or -1. */
int webview_gif_decode(const unsigned char *data, size_t size, unsigned max_side, uint32_t **pixels, int *width,
    int *height);
