#pragma once

#include "libc.h"
#include "savanxp/wm_shell_protocol.h"
#include "windowd_appinfo.h"
#include "windowd_compositor_client.h"

#define WINDOWD_MAX_OVERLAY_CLIENTS 12

/* What has the keyboard: an application window, or nothing (the desktop). */
enum windowd_client_kind
{
    WINDOWD_CLIENT_NONE = 0,
    WINDOWD_CLIENT_APP = 1,
};

/* Where a window sits in the stack and how input treats it
 * (docs/desktop/DESKTOP_ARCHITECTURE.md#window-roles). Every window of the
 * desktop process has one; application windows are NORMAL. */
enum windowd_window_role
{
    WINDOWD_ROLE_NORMAL = 0,
    WINDOWD_ROLE_BACKGROUND = 1,
    WINDOWD_ROLE_PANEL = 2,
    WINDOWD_ROLE_POPUP = 3,
};

/* The desktop process's windows: its main window (the background, id 0) and
 * one per id it may ask for. Indexed by window_id. */
#define WINDOWD_DESKTOP_WINDOW_COUNT ((int)(1u + SAVANXP_WM_MAX_OWNED_WINDOWS))

/* Global hotkeys the shell registered (savanxp/wm_shell_protocol.h). */
#define WINDOWD_MAX_HOTKEYS 8

struct windowd_hotkey
{
    uint32_t key;
    uint32_t modifiers;
};

#define WINDOWD_CLIENT_PATH_CAPACITY 192

struct windowd_client
{
    /* Owned copy: launch requests arrive in stack buffers that die with the
     * request, so pointing at the caller's string would dangle. */
    char path[WINDOWD_CLIENT_PATH_CAPACITY];
    /* Titulo, icono y accent resueltos desde el .sxe del binario, una sola vez
     * al crear la ventana (docs/userland/SXE_FORMAT.md, fase 4). */
    struct windowd_presentation presentation;
    long pid;
    /* Los UNICOS descriptores que windowd guarda por cliente (savanxp/
     * wm_protocol.h v4). Todo lo demas vive en la seccion, que se cierra
     * despues del fork: a 64 descriptores por proceso, cada fd que se sume aca
     * le resta ventanas simultaneas a la sesion (docs/desktop/WM_SUBSYSTEM.md). */
    int events_write_fd;
    int wake_event_fd;
    /* Entrada nativa (sin pipe de eventos): anillo en seccion propia, creada
     * en el launch si el binario es nativo y cerrada tras el fork como la
     * superficie. El mapeo propio la mantiene viva. */
    int native_input;
    struct savanxp_wm_input_ring *input_ring;
    /* Shell channels (savanxp/wm_shell_protocol.h): only the desktop's main
     * window has them. -1 in any other client. */
    int shell_request_read_fd;
    struct savanxp_wm_window_list *window_list;
    int last_cursor_hint_shape;
    /* Estado del WM sobre los pedidos del header. Se guarda aca y no se relee
     * del header porque el cliente puede escribir ahi lo que quiera. */
    uint32_t launch_tail;
    uint32_t size_hint_consumed_sequence;
    uint32_t window_request_consumed_sequence;
    /* Ventana con dueno (docs/desktop/OWNED_WINDOWS.md): slot del overlay dueno, o -1 en
     * una ventana principal. Una con dueno es del MISMO proceso -- comparte pid
     * y, con duplicados propios, el pipe de eventos y el wake -- pero es una
     * ventana aparte: su superficie, su marco, su lugar en el z-order. No se
     * lanza, no se mata y no va a la lista de tareas. window_id es el id que le
     * dio el cliente, y viaja en cada evento que se le rutea. */
    int owner_slot;
    uint32_t window_id;
    /* enum windowd_window_role, and the role's savanxp_wm_window_request
     * role_flags. dismiss_requested: the WM already asked this popup to
     * close; it is not asked again while the process gets to it. */
    uint32_t role;
    uint32_t role_flags;
    int dismiss_requested;
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
    /* Evento de submit compartido por TODOS los clientes (SAVANXP_WM_FD_SUBMIT_
     * EVENT): uno por sesion en vez de uno por ventana. */
    int submit_event_fd;
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
    /* Alt+Tab switcher: open while Alt is held after a Tab; switcher_selected
     * is a task index (windowd_task_client). WM UI, like the window frames. */
    int switcher_open;
    int switcher_selected;
    int active_client_kind;
    int active_overlay_slot;
    int fullscreen_slot;
    int overlay_count;
    int overlay_order[WINDOWD_MAX_OVERLAY_CLIENTS];
    /* The desktop process (docs/desktop/DESKTOP_ARCHITECTURE.md): background,
     * taskbar and popups are its windows, told apart by role. windowd does not
     * know what any of them shows. */
    struct windowd_client desktop_windows[WINDOWD_DESKTOP_WINDOW_COUNT];
    unsigned long desktop_started_ms;
    int desktop_quick_exits;
    struct windowd_hotkey hotkeys[WINDOWD_MAX_HOTKEYS];
    int hotkey_count;
    /* Key whose KEY_DOWN went to the desktop as a hotkey: its KEY_UP follows
     * it there instead of landing in the focused app. 0 = none. */
    uint32_t hotkey_held_key;
    struct windowd_client overlay_clients[WINDOWD_MAX_OVERLAY_CLIENTS];
};
