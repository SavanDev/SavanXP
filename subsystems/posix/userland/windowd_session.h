#pragma once

#include "libc.h"
#include "windowd_appinfo.h"
#include "windowd_compositor_client.h"

#define WINDOWD_MAX_OVERLAY_CLIENTS 12

enum windowd_client_kind
{
    WINDOWD_CLIENT_SHELL = 0,
    WINDOWD_CLIENT_APP = 1,
};

#define WINDOWD_CLIENT_PATH_CAPACITY 192

struct windowd_client
{
    /* Owned copy: launch requests arrive in stack buffers that die with the
     * request, so pointing at the caller's string would dangle. */
    char path[WINDOWD_CLIENT_PATH_CAPACITY];
    /* Titulo, icono y accent resueltos desde el .sxe del binario, una sola vez
     * al crear la ventana (docs/SXE_FORMAT.md, fase 4). */
    struct windowd_presentation presentation;
    long pid;
    int section_fd;
    int input_write_fd;
    int mouse_write_fd;
    int submit_event_fd;
    int retire_event_fd;
    int shutdown_event_fd;
    int launch_read_fd;
    int cursor_hint_read_fd;
    int size_hint_read_fd;
    int last_cursor_hint_shape;
    /* Size hint (savanxp/wm_protocol.h): se atiende UNA sola vez y solo
     * mientras la ventana siga en la geometria con la que se lanzo. Guardamos
     * el indice de cascada del launch para poder recolocarla despues de
     * cambiarle el tamano: centrarla con el tamano viejo la dejaria corrida. */
    int size_hint_applied;
    int cascade_index;
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

struct windowd_session
{
    struct savanxp_gfx_context gfx;
    struct windowd_compositor_connection compositor;
    int input_fd;
    int mouse_fd;
    int hw_cursor_enabled;
    int current_cursor_shape;
    int previous_cursor_shape;
    /* Redimensionado por bordes en curso. Se guarda el marco al agarrar y el
     * punto de agarre: el marco nuevo se calcula siempre desde ese origen, no
     * de forma incremental, asi que el error no se acumula al arrastrar. */
    int resize_slot;
    uint32_t resize_edges;
    struct sx_rect resize_origin_frame;
    int resize_grab_x;
    int resize_grab_y;
    /* Task List (Ctrl+Esc): estado de UI del WM, no del chrome del shell. */
    int tasklist_open;
    int tasklist_selected;
    int tasklist_last_click_index;
    unsigned long tasklist_last_click_ms;
    int active_client_kind;
    int active_overlay_slot;
    int fullscreen_slot;
    int overlay_count;
    int overlay_order[WINDOWD_MAX_OVERLAY_CLIENTS];
    /* Cliente de fondo (shellui): superficie full-screen al fondo del z-order
     * que dibuja el wallpaper (A2, ver docs/WM_SUBSYSTEM.md). Pasivo: no recibe
     * foco ni input en A2.2. Distinto del shell_client (terminal on-demand). */
    struct windowd_client background_client;
    struct windowd_client shell_client;
    struct windowd_client overlay_clients[WINDOWD_MAX_OVERLAY_CLIENTS];
};
