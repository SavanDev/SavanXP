#pragma once

#include "libc.h"
#include "savanxp/compositor_protocol.h"

struct desktop_compositor_connection
{
    long pid;
    int request_fd;
    int reply_fd;
    int display_section_fd;
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
};

void desktop_compositor_connection_init(struct desktop_compositor_connection *connection);
int desktop_compositor_open(struct desktop_compositor_connection *connection);
/* Returns non-zero while the daemon link is live. After a present/sync error the
   caller should consult this to decide between recovery and a hard failure. */
int desktop_compositor_connected(const struct desktop_compositor_connection *connection);
/* Respawn compositord after it died mid-session, reusing the existing display
   section so the shell's backbuffer pointer stays valid, then repaint the whole
   surface and restore the hardware cursor. Returns 0 on success. */
int desktop_compositor_reconnect(struct desktop_compositor_connection *connection);
void desktop_compositor_close(struct desktop_compositor_connection *connection);
int desktop_compositor_present(
    struct desktop_compositor_connection *connection,
    const struct sx_rect *rects,
    size_t rect_count);
int desktop_compositor_sync_present(
    struct desktop_compositor_connection *connection,
    int wait_for_target,
    int *ready);
int desktop_compositor_get_timeline(
    struct desktop_compositor_connection *connection,
    struct savanxp_gpu_present_timeline *timeline);
int desktop_compositor_enable_cursor(
    struct desktop_compositor_connection *connection,
    int cursor_x,
    int cursor_y);
int desktop_compositor_move_cursor(
    struct desktop_compositor_connection *connection,
    int cursor_x,
    int cursor_y,
    int visible);
