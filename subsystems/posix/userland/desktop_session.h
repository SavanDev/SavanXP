#pragma once

#include "libc.h"
#include "desktop_compositor_client.h"

#define DESKTOP_MAX_OVERLAY_CLIENTS 12

enum desktop_client_kind
{
    DESKTOP_CLIENT_SHELL = 0,
    DESKTOP_CLIENT_APP = 1,
};

struct desktop_client
{
    const char *path;
    long pid;
    int section_fd;
    int input_write_fd;
    int mouse_write_fd;
    int submit_event_fd;
    int retire_event_fd;
    int shutdown_event_fd;
    int launch_read_fd;
    void *mapped_view;
    struct savanxp_gpu_client_surface_header *header;
    struct savanxp_gpu_dirty_rect_batch *command_batches;
    uint32_t *pixels;
    struct savanxp_fb_info surface_info;
    uint64_t consumed_submit_sequence;
    uint64_t pending_retire_sequence;
    int window_x;
    int window_y;
    int window_width;
    int window_height;
    int restore_window_x;
    int restore_window_y;
    int restore_window_width;
    int restore_window_height;
    int frame_visible;
    int minimized;
    int maximized;
    int active;
    /* Fullscreen is composited by software. fullscreen_capable is set at launch
     * for apps whose surface is sized for a low fullscreen mode; fullscreen
     * tracks the active state and fs_restore_* snapshots windowed geometry. */
    int fullscreen_capable;
    int fullscreen;
    int fs_restore_window_x;
    int fs_restore_window_y;
    int fs_restore_surface_width;
    int fs_restore_surface_height;
    int fs_restore_frame_visible;
    int fs_restore_maximized;
};

struct desktop_session
{
    struct savanxp_gfx_context gfx;
    struct desktop_compositor_connection compositor;
    int input_fd;
    int mouse_fd;
    int hw_cursor_enabled;
    int active_client_kind;
    int active_overlay_slot;
    int fullscreen_slot;
    int overlay_count;
    int overlay_order[DESKTOP_MAX_OVERLAY_CLIENTS];
    struct desktop_client shell_client;
    struct desktop_client overlay_clients[DESKTOP_MAX_OVERLAY_CLIENTS];
};
