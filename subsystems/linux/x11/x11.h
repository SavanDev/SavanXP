#pragma once

/* Bounded X11 core engine. No sockets, system calls or window-manager ABI;
 * the host TCP harness and the SUN server supply the same callbacks. */
#include <stddef.h>
#include <stdint.h>
#include "sxgfx/gfx2d.h"

#define SX11_MAX_REQUEST 65536u
#define SX11_MAX_WINDOWS 16
#define SX11_MAX_GCS 32
#define SX11_MAX_ATOMS 32
#define SX11_ROOT 0x100u

struct sx11_window {
    uint32_t id, parent, event_mask, background;
    int16_t x, y;
    uint16_t width, height;
    int mapped, delete_protocol, close_sent;
    char title[128];
    struct sx_bitmap bitmap;
    void* host;
};
struct sx11_gc { uint32_t id, foreground, background; };
struct sx11_atom { char name[128]; uint32_t id; };
struct sx11_ops {
    /* send must preserve all bytes in order or fail the connection. */
    int (*send)(void*, const void*, size_t);
    int (*map)(void*, struct sx11_window*);
    void (*destroy)(void*, struct sx11_window*);
    void (*present)(void*, struct sx11_window*);
    int (*pointer)(void*, struct sx11_window*, int*, int*, int*, int*, unsigned*);
};
struct sx11_client {
    struct sx11_ops ops;
    void* host;
    uint16_t screen_width, screen_height, sequence;
    int setup, failed;
    size_t used;
    unsigned char input[SX11_MAX_REQUEST];
    struct sx11_window windows[SX11_MAX_WINDOWS];
    struct sx11_gc gcs[SX11_MAX_GCS];
    struct sx11_atom atoms[SX11_MAX_ATOMS];
};
void sx11_init(struct sx11_client*, const struct sx11_ops*, void*, uint16_t, uint16_t);
/* Arbitrary stream fragments; 0 on success, -1 after a fatal framing/I/O error. */
int sx11_feed(struct sx11_client*, const void*, size_t);
/* A clean stream end must have neither a partial setup nor partial request. */
int sx11_finish(struct sx11_client*);
void sx11_destroy(struct sx11_client*);
int sx11_resize(struct sx11_client*, uint32_t, uint16_t, uint16_t);
int sx11_close_window(struct sx11_client*, uint32_t);
