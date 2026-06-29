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
    struct savanxp_fb_info display_info;
    struct savanxp_gpu_info gpu_info;
    uint64_t pending_present_sequence;
    uint32_t next_serial;
    int connected;
};

void desktop_compositor_connection_init(struct desktop_compositor_connection *connection);
int desktop_compositor_open(struct desktop_compositor_connection *connection);
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
