#pragma once

#include "libc.h"
#include "savanxp/compositor_protocol.h"

struct windowd_compositor_connection
{
    long pid;
    int request_fd;
    int reply_fd;
    /* The private port of a compositord windowd started itself, in its test
     * modes (compositor_protocol.h); pid is that child, -1 in the session. */
    char port_name[32];
    void *display_view;
    uint32_t *framebuffer;
    struct savanxp_fb_info requested_info;
    struct savanxp_fb_info display_info;
    struct savanxp_gpu_info gpu_info;
    uint64_t pending_present_sequence;
    uint32_t next_serial;
    int connected;
    int cursor_enabled;
    int cursor_x;
    int cursor_y;
    int cursor_visible;
    int cursor_shape;
    /* Desglose que estampo compositord en las replies de present. Se acumula
       sobre todas las que se leyeron -- un present con mas de 32 rects se
       parte en varias RPC -- hasta que windowd_stats_frame_end lo consume.
       No se resetea al empezar un present, asi funciona igual si la reply se
       lee despues (ver "Asynchronous present" en docs/desktop/GRAPHICS_PERF.md). */
    struct savanxp_compositor_service_timing last_present_timing;
};

void windowd_compositor_connection_init(struct windowd_compositor_connection *connection);
/* Connects to the session's compositord, or to one it starts itself when
   `own_daemon` (the test modes, which run without a session). */
int windowd_compositor_open(struct windowd_compositor_connection *connection, int own_daemon);
/* Returns non-zero while the daemon link is live. There is no reconnection:
   without compositord the session is over (sessiond restarts it). */
int windowd_compositor_connected(const struct windowd_compositor_connection *connection);
void windowd_compositor_close(struct windowd_compositor_connection *connection);
int windowd_compositor_present(
    struct windowd_compositor_connection *connection,
    const struct sx_rect *rects,
    size_t rect_count);
int windowd_compositor_sync_present(
    struct windowd_compositor_connection *connection,
    int wait_for_target,
    int *ready);
int windowd_compositor_get_timeline(
    struct windowd_compositor_connection *connection,
    struct savanxp_gpu_present_timeline *timeline);
/* Programs a video mode; compositord re-imports the display section with it.
   The mapping does not move: the section is sized for the largest mode. On 0,
   connection->display_info holds the geometry that resulted, which may not be
   the one asked for. */
int windowd_compositor_set_mode(
    struct windowd_compositor_connection *connection,
    uint32_t width,
    uint32_t height);
int windowd_compositor_enable_cursor(
    struct windowd_compositor_connection *connection,
    int cursor_x,
    int cursor_y);
int windowd_compositor_move_cursor(
    struct windowd_compositor_connection *connection,
    int cursor_x,
    int cursor_y,
    int visible);
int windowd_compositor_set_cursor_shape(
    struct windowd_compositor_connection *connection,
    int shape);
