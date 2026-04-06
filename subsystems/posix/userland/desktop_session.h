#pragma once

#include "libc.h"

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
    struct
    {
        const char *path;
        long pid;
        int section_fd;
        int input_write_fd;
        int mouse_write_fd;
        int present_read_fd;
        int present_nonblocking;
        void *mapped_view;
        struct savanxp_gpu_client_surface_header *header;
        uint32_t *pixels;
    } client;
};
