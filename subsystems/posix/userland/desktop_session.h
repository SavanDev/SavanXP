#pragma once

#include "libc.h"

#define DESKTOP_MAX_OVERLAY_CLIENTS 4

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
    int frame_visible;
    int active;
};

struct desktop_session
{
    struct savanxp_gfx_context gfx;
    struct savanxp_gpu_info gpu_info;
    int gpu_fd;
    int input_fd;
    int mouse_fd;
    int display_section_fd;
    uint32_t display_surface_id;
    uint64_t pending_present_sequence;
    void *display_view;
    uint32_t *display_pixels;
    int hw_cursor_enabled;
    int active_client_kind;
    int active_overlay_slot;
    int overlay_count;
    int overlay_order[DESKTOP_MAX_OVERLAY_CLIENTS];
    struct desktop_client shell_client;
    struct desktop_client overlay_clients[DESKTOP_MAX_OVERLAY_CLIENTS];
};
