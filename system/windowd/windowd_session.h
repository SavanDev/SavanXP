#pragma once

#include "libc.h"
#include "wm/wm_shell_protocol.h"
#include "windowd_presentation.h"
#include "windowd_compositor_client.h"
#include "windowd_consent.h"

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
    /* The program, for labels and logs: the path windowd spawned, or the
     * process name for a client that connected on its own. */
    char path[WINDOWD_CLIENT_PATH_CAPACITY];
    /* Title, icon, accent and style: the file name until the window publishes
     * its own (windowd_presentation.h). */
    struct windowd_presentation presentation;
    /* > 0 once the window exists: every pass over the slots keys on it. */
    long pid;
    /* A slot held for a process windowd spawned (the desktop, the selftests'
     * apps) until it connects; pid stays 0 meanwhile, so nothing treats the
     * slot as a window. 0 = not reserved. */
    long reserved_pid;
    /* The ONLY descriptors windowd keeps per client (savanxp/wm_protocol.h).
     * Everything else lives in the section, closed once sent: at 64
     * descriptors per process, every fd added here takes simultaneous windows
     * away from the session (docs/desktop/WM_SUBSYSTEM.md). */
    int events_write_fd;
    int wake_event_fd;
    /* Input on a ring in its own section instead of the event pipe, when the
     * hello asked for it (SAVANXP_WM_HELLO_INPUT_RING). Closed once sent, like
     * the surface; windowd's own mapping keeps it alive. */
    int native_input;
    struct savanxp_wm_input_ring *input_ring;
    /* Shell channels (savanxp/wm_shell_protocol.h): only the desktop's main
     * window has them -- the connection's request pipe and the window list.
     * -1 in any other client. */
    int shell_request_read_fd;
    struct savanxp_wm_window_list *window_list;
    int last_cursor_hint_shape;
    /* The WM's own state about the header requests. Kept here and never read
     * back from the header, where the client may write whatever it likes. */
    uint32_t size_hint_consumed_sequence;
    uint32_t window_request_consumed_sequence;
    uint32_t presentation_consumed_sequence;
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
    /* A main window asked to close (its X, Alt+F4): when, and whether it has
     * outlived WINDOWD_HUNG_CLOSE_MS still running -- "not responding". windowd
     * never ends the process; the window goes when the process does
     * (docs/desktop/DESKTOP_ARCHITECTURE.md#closing-a-window). 0 = not asked. */
    unsigned long close_requested_ms;
    int not_responding;
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
    /* The surface's id in compositord's scene (0 = not attached), and whether
     * the client has published its first frame, which is what puts the window
     * in the scene (windowd_client_in_scene). Frames themselves are
     * compositord's to consume. */
    uint32_t surface_id;
    int started;
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

/* A connection accepted on the WM's port whose hello has not arrived yet
 * (savanxp/wm_protocol.h). read_fd < 0 = free. */
#define WINDOWD_MAX_PENDING_CONNECTIONS 4

struct windowd_pending_connection
{
    long pid;
    int read_fd;
    int write_fd;
    unsigned long deadline_ms;
};

struct windowd_session
{
    struct savanxp_gfx_context gfx;
    struct windowd_compositor_connection compositor;
    int input_fd;
    int mouse_fd;
    /* Submit event shared by ALL clients (sent with every welcome): one per
     * session instead of one per window. It is compositord's (the frame event
     * of compositor_protocol.h): windowd only hands it out. */
    int submit_event_fd;
    /* windowd's request event, shared the same way, for clients that asked
     * for it in their hello (SAVANXP_WM_HELLO_REQUEST_EVENT). */
    int request_event_fd;
    /* The port clients connect to (SAVANXP_WM_PORT_NAME), and the connections
     * still owing their hello. */
    int port_fd;
    struct windowd_pending_connection pending[WINDOWD_MAX_PENDING_CONNECTIONS];
    /* How long a window asked to close may keep running before it is shown as
     * not responding. A field so the selftest need not wait it out. */
    unsigned long hung_close_ms;
    /* The cursor moved or changed shape since the last commit: compositord
     * draws it, so a commit has to tell it even with nothing else damaged. */
    int cursor_moved;
    /* Pointer capture (savanxp_wm_client_requests.pointer_capture): 1 while
     * the active window holds it, which hides the cursor. The last absolute
     * position turns a tablet's positions into motion. */
    int pointer_captured;
    int capture_has_last;
    int capture_last_x;
    int capture_last_y;
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
    /* The elevation prompt (windowd_consent.h): above everything, and while
     * it is up it has all the input. */
    struct windowd_consent consent;
    int active_client_kind;
    int active_overlay_slot;
    int fullscreen_slot;
    int overlay_count;
    int overlay_order[WINDOWD_MAX_OVERLAY_CLIENTS];
    /* The desktop process (docs/desktop/DESKTOP_ARCHITECTURE.md): background,
     * taskbar and popups are its windows, told apart by role. windowd does not
     * know what any of them shows. */
    struct windowd_client desktop_windows[WINDOWD_DESKTOP_WINDOW_COUNT];
    struct windowd_hotkey hotkeys[WINDOWD_MAX_HOTKEYS];
    int hotkey_count;
    /* Key whose KEY_DOWN went to the desktop as a hotkey: its KEY_UP follows
     * it there instead of landing in the focused app. 0 = none. */
    uint32_t hotkey_held_key;
    struct windowd_client overlay_clients[WINDOWD_MAX_OVERLAY_CLIENTS];
};
