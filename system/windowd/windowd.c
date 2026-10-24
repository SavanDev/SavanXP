/* First: renames the POSIX calls to their native versions (windowd_sys.h). */
#include "windowd_sys.h"

#include "libc.h"
#include "sxn/sxn.h"
#include "windowd_session.h"
#include "windowd_presentation.h"
#include "windowd_layout.h"
#include "windowd_render.h"

#define WINDOWD_MAX_MOUSE_EVENTS_PER_FRAME 16
#define WINDOWD_SURFACE_PAGE_SIZE 4096u
/* A window asked to close that is still running after this long is shown as
 * not responding (Windows waits about as long before it says so). */
#define WINDOWD_HUNG_CLOSE_MS 5000ul
/* Low render size used for composited fullscreen apps: the client renders here
 * and the shell scales it to the display when F11 fullscreen is active. */
#define WINDOWD_FULLSCREEN_MODE_WIDTH 640
#define WINDOWD_FULLSCREEN_MODE_HEIGHT 400

/* The session (the desktop, Program Manager, restarting them) belongs to
 * sessiond; windowd starts a desktop only for its own selftest. */
static const char *k_desktop_path = "/bin/desktop";

static int launch_overlay_client(struct windowd_session *session, const char *path, const char *argument);
/* Defined with the rest of the Alt+Tab switcher, further down; the selftest
 * uses them first. */
static void switcher_close(struct windowd_session *session, struct windowd_dirty_rect *dirty);
static void switcher_switch_to(struct windowd_session *session, struct windowd_dirty_rect *dirty, int task_index);
static int wm_handle_key(
    struct windowd_session *session,
    const struct savanxp_input_event *key_event,
    struct windowd_dirty_rect *dirty);
static int windowd_active_task_index(const struct windowd_session *session);
static void resize_overlay_client_surface(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    int slot,
    int surface_width,
    int surface_height);

static void close_fd_if_needed(int *fd)
{
    if (fd != 0 && *fd >= 0)
    {
        savanxp_close(*fd);
        *fd = -1;
    }
}

/* The session and its compositord link, for the paths that attach and detach
 * client surfaces without a session at hand (connect_client,
 * destroy_client_instance). One session per windowd. */
static struct windowd_session *g_session = 0;

/* Size of a process's descriptor table (process::kMaxFileHandles). */
#define WINDOWD_PROCESS_FD_LIMIT 64

static void reset_client(struct windowd_client *client)
{
    if (client == 0)
    {
        return;
    }

    memset(client, 0, sizeof(*client));
    client->events_write_fd = -1;
    client->wake_event_fd = -1;
    client->shell_request_read_fd = -1;
    /* -1 y no 0: con el memset toda ventana seria "con dueno el slot 0". */
    client->owner_slot = -1;
}

static int overlay_slot_valid(int slot)
{
    return slot >= 0 && slot < WINDOWD_MAX_OVERLAY_CLIENTS;
}

static struct windowd_client *overlay_client_at(struct windowd_session *session, int slot)
{
    if (session == 0 || !overlay_slot_valid(slot))
    {
        return 0;
    }
    return &session->overlay_clients[slot];
}

static const struct windowd_client *overlay_client_at_const(const struct windowd_session *session, int slot)
{
    if (session == 0 || !overlay_slot_valid(slot))
    {
        return 0;
    }
    return &session->overlay_clients[slot];
}

static int overlay_slot_for_client_ptr(const struct windowd_session *session, const struct windowd_client *client)
{
    int slot;

    if (session == 0 || client == 0)
    {
        return -1;
    }

    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        if (client == &session->overlay_clients[slot])
        {
            return slot;
        }
    }
    return -1;
}

static int find_free_overlay_slot(const struct windowd_session *session)
{
    int slot;

    if (session == 0)
    {
        return -1;
    }

    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        /* A slot held for a process windowd spawned is taken too. */
        if (session->overlay_clients[slot].pid <= 0 && session->overlay_clients[slot].reserved_pid <= 0)
        {
            return slot;
        }
    }
    return -1;
}

static void remove_overlay_from_order(struct windowd_session *session, int slot)
{
    int index;

    if (session == 0 || !overlay_slot_valid(slot))
    {
        return;
    }

    for (index = 0; index < session->overlay_count; ++index)
    {
        if (session->overlay_order[index] == slot)
        {
            for (; index + 1 < session->overlay_count; ++index)
            {
                session->overlay_order[index] = session->overlay_order[index + 1];
            }
            session->overlay_order[session->overlay_count - 1] = -1;
            session->overlay_count -= 1;
            break;
        }
    }
}

static void append_overlay_to_order(struct windowd_session *session, int slot)
{
    if (session == 0 || !overlay_slot_valid(slot))
    {
        return;
    }

    remove_overlay_from_order(session, slot);
    if (session->overlay_count >= WINDOWD_MAX_OVERLAY_CLIENTS)
    {
        return;
    }
    session->overlay_order[session->overlay_count++] = slot;
}

static int overlay_client_visible(const struct windowd_client *client)
{
    return client != 0 && client->pid > 0 && !client->minimized;
}

static int top_visible_overlay_slot(const struct windowd_session *session)
{
    int order_index;

    if (session == 0)
    {
        return -1;
    }

    for (order_index = session->overlay_count - 1; order_index >= 0; --order_index)
    {
        int slot = session->overlay_order[order_index];
        if (slot >= 0 && slot < WINDOWD_MAX_OVERLAY_CLIENTS && overlay_client_visible(&session->overlay_clients[slot]))
        {
            return slot;
        }
    }
    return -1;
}

static void refresh_active_state(struct windowd_session *session)
{
    int slot;
    int visible_overlay_slot = -1;

    if (session == 0)
    {
        return;
    }

    if (!overlay_slot_valid(session->active_overlay_slot) ||
        !overlay_client_visible(&session->overlay_clients[session->active_overlay_slot]))
    {
        session->active_overlay_slot = -1;
    }

    visible_overlay_slot = top_visible_overlay_slot(session);

    /* With no window active, the top visible one takes the focus; only an
     * empty (or all minimized) desktop has none. */
    if (session->active_client_kind != WINDOWD_CLIENT_APP || session->active_overlay_slot < 0)
    {
        session->active_client_kind = visible_overlay_slot >= 0 ? WINDOWD_CLIENT_APP : WINDOWD_CLIENT_NONE;
        session->active_overlay_slot = visible_overlay_slot;
    }

    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        session->overlay_clients[slot].active =
            overlay_client_visible(&session->overlay_clients[slot]) &&
            session->active_client_kind == WINDOWD_CLIENT_APP &&
            slot == session->active_overlay_slot;
    }
}

/* Every client the session has, desktop windows first, as one index space, for
 * the passes that treat them all alike (frames, retire, reaping). */
#define WINDOWD_SESSION_CLIENT_COUNT (WINDOWD_DESKTOP_WINDOW_COUNT + WINDOWD_MAX_OVERLAY_CLIENTS)

static struct windowd_client *session_client_at(struct windowd_session *session, int index)
{
    if (session == 0 || index < 0 || index >= WINDOWD_SESSION_CLIENT_COUNT)
    {
        return 0;
    }
    if (index < WINDOWD_DESKTOP_WINDOW_COUNT)
    {
        return &session->desktop_windows[index];
    }
    return &session->overlay_clients[index - WINDOWD_DESKTOP_WINDOW_COUNT];
}

static int is_desktop_window(const struct windowd_session *session, const struct windowd_client *client)
{
    return session != 0 && client != 0 &&
        client >= &session->desktop_windows[0] &&
        client < &session->desktop_windows[WINDOWD_DESKTOP_WINDOW_COUNT];
}

/* Height of the strips the desktop's panels reserve on each edge: what a
 * maximized window stays out of. */
static void desktop_reserved_strips(const struct windowd_session *session, int *top, int *bottom)
{
    int index;

    *top = 0;
    *bottom = 0;
    for (index = 1; index < WINDOWD_DESKTOP_WINDOW_COUNT; ++index)
    {
        const struct windowd_client *client = &session->desktop_windows[index];

        if (client->pid <= 0 || client->role != WINDOWD_ROLE_PANEL)
        {
            continue;
        }
        if ((client->role_flags & SAVANXP_WM_PANEL_EDGE_TOP) != 0)
        {
            *top += client->window_height;
        }
        else
        {
            *bottom += client->window_height;
        }
    }
}

/* --- Ventanas con dueno (docs/desktop/OWNED_WINDOWS.md) ------------------------------
 *
 * Un dialogo es una ventana del mismo proceso que su dueno, en un slot de
 * overlay propio. El z-order, la activacion y el minimizado tratan a dueno y
 * dialogos como una FAMILIA: suben juntos, el dialogo siempre arriba, y mientras
 * haya uno el dueno no recibe input. */

static int overlay_client_owned(const struct windowd_client *client)
{
    return client != 0 && client->owner_slot >= 0;
}

/* Slot de la ventana principal de la familia: el del dueno para una ventana con
 * dueno, el mismo slot para las demas. */
static int overlay_root_slot(const struct windowd_session *session, int slot)
{
    if (session == 0 || !overlay_slot_valid(slot) || !overlay_slot_valid(session->overlay_clients[slot].owner_slot))
    {
        return slot;
    }
    return session->overlay_clients[slot].owner_slot;
}

/* Slot de la ventana `window_id` de `owner_slot`, o de cualquiera de sus
 * ventanas con dueno si window_id es 0. -1 si no hay. */
static int owned_window_slot(const struct windowd_session *session, int owner_slot, uint32_t window_id)
{
    int slot;

    if (session == 0 || !overlay_slot_valid(owner_slot))
    {
        return -1;
    }
    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        const struct windowd_client *client = &session->overlay_clients[slot];

        if (client->pid > 0 && client->owner_slot == owner_slot &&
            (window_id == 0 || client->window_id == window_id))
        {
            return slot;
        }
    }
    return -1;
}

/* Deshabilitado = tiene una ventana con dueno abierta. No recibe input: un click
 * sobre el solo levanta la familia, y el dialogo queda activo. Es el
 * EnableWindow(owner, FALSE) de DialogBox, decidido por el WM. */
/*
 * The display windows are laid out on: the native mode, whatever the scanout
 * runs at right now. While an application is fullscreen the scanout drops to
 * a low mode (apply_display_mode), but every window keeps native coordinates
 * -- exit_overlay_fullscreen restores them untouched -- so a window opened,
 * moved or resized meanwhile has to be measured against the native mode too.
 * Measured against the low mode, it kept the low mode's geometry after the
 * restore: a desktop started during a fullscreen game got a 640-pixel
 * taskbar. Drawing, damage and the cursor still use session->gfx.info, the
 * mode actually on screen.
 */
static const struct savanxp_fb_info *layout_display(const struct windowd_session *session)
{
    const struct savanxp_fb_info *native = &session->compositor.requested_info;

    return native->width != 0 && native->height != 0 ? native : &session->gfx.info;
}

static int overlay_client_disabled(const struct windowd_session *session, const struct windowd_client *client)
{
    int slot = overlay_slot_for_client_ptr(session, client);

    return overlay_slot_valid(slot) && !overlay_client_owned(client) && owned_window_slot(session, slot, 0) >= 0;
}

static void raise_overlay(struct windowd_session *session, int slot)
{
    int root;
    int child;

    if (session == 0 || !overlay_slot_valid(slot) || session->overlay_clients[slot].pid <= 0)
    {
        refresh_active_state(session);
        return;
    }

    /* Se levante la ventana que se levante, la familia termina en el mismo
     * orden: el dueno y encima sus ventanas con dueno. Si no, un click sobre el
     * dueno taparia a su propio dialogo, que es el unico que puede cerrarse. */
    root = overlay_root_slot(session, slot);
    session->overlay_clients[root].minimized = 0;
    append_overlay_to_order(session, root);
    session->active_overlay_slot = root;
    for (child = 0; child < WINDOWD_MAX_OVERLAY_CLIENTS; ++child)
    {
        struct windowd_client *owned = &session->overlay_clients[child];

        if (owned->pid <= 0 || owned->owner_slot != root)
        {
            continue;
        }
        owned->minimized = 0;
        append_overlay_to_order(session, child);
        session->active_overlay_slot = child;
    }
    /* Con mas de un dialogo, el que se pidio queda arriba y activo. */
    if (slot != root)
    {
        append_overlay_to_order(session, slot);
        session->active_overlay_slot = slot;
    }
    session->active_client_kind = WINDOWD_CLIENT_APP;
    refresh_active_state(session);
}

static struct windowd_client *active_client(struct windowd_session *session)
{
    if (session == 0)
    {
        return 0;
    }
    if (session->active_client_kind == WINDOWD_CLIENT_APP && overlay_slot_valid(session->active_overlay_slot))
    {
        return &session->overlay_clients[session->active_overlay_slot];
    }
    return 0;
}

static int drag_overlay_slot_active(const struct windowd_session *session, int slot)
{
    return session != 0 && overlay_slot_valid(slot) && overlay_client_visible(&session->overlay_clients[slot]);
}

/* Marca como sucio el marco de la ventana y de las que tiene con dueno. */
static void dirty_overlay_family(struct windowd_session *session, struct windowd_dirty_rect *dirty, int root)
{
    int slot;

    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        const struct windowd_client *client = &session->overlay_clients[slot];
        struct sx_rect frame_rect;

        if (client->pid <= 0 || (slot != root && client->owner_slot != root))
        {
            continue;
        }
        frame_rect = windowd_client_frame_rect(client);
        windowd_dirty_rect_add(dirty, &session->gfx.info, frame_rect.x, frame_rect.y, frame_rect.width, frame_rect.height);
    }
}

/* Minimizar actua sobre la familia entera: un dialogo no se queda flotando
 * sobre el escritorio con su dueno escondido. */
static void minimize_overlay_client(struct windowd_session *session, struct windowd_dirty_rect *dirty, int slot)
{
    struct windowd_client *client = 0;
    int root = overlay_root_slot(session, slot);
    int child;

    client = overlay_client_at(session, root);
    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0 || client->minimized)
    {
        return;
    }

    dirty_overlay_family(session, dirty, root);
    client->minimized = 1;
    for (child = 0; child < WINDOWD_MAX_OVERLAY_CLIENTS; ++child)
    {
        if (session->overlay_clients[child].pid > 0 && session->overlay_clients[child].owner_slot == root)
        {
            session->overlay_clients[child].minimized = 1;
        }
    }
    refresh_active_state(session);
}

static void restore_overlay_client(struct windowd_session *session, struct windowd_dirty_rect *dirty, int slot)
{
    struct windowd_client *client = overlay_client_at(session, slot);

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0)
    {
        return;
    }

    /* raise_overlay desminimiza la familia; el danio se toma despues, con todos
     * los marcos ya en su lugar. */
    raise_overlay(session, slot);
    dirty_overlay_family(session, dirty, overlay_root_slot(session, slot));
}

static void toggle_overlay_client_maximized(struct windowd_session *session, struct windowd_dirty_rect *dirty, int slot)
{
    struct windowd_client *client = overlay_client_at(session, slot);
    int area_x = 0;
    int area_y = 0;
    int area_width = 0;
    int area_height = 0;
    int target_surface_width = 0;
    int target_surface_height = 0;
    struct sx_rect previous_frame;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0)
    {
        return;
    }

    /* El marco viejo se toma ANTES de tocar la geometria: resize_overlay_
     * client_surface calcula su "marco previo" con window_x/y/width/height ya
     * pisados, o sea el marco nuevo. Al restaurar eso dejaba sin dañar toda el
     * area maximizada, y el fondo quedaba con restos de la ventana hasta que
     * otra cosa pasara por encima. */
    previous_frame = windowd_client_frame_rect(client);
    if (!client->maximized)
    {
        client->restore_window_x = client->window_x;
        client->restore_window_y = client->window_y;
        client->restore_window_width = client->window_width;
        client->restore_window_height = client->window_height;
        /* The only place the WM leaves out what the desktop's panels
         * reserve. */
        {
            int reserved_top = 0;
            int reserved_bottom = 0;

            desktop_reserved_strips(session, &reserved_top, &reserved_bottom);
            windowd_maximize_area_bounds(
                layout_display(session),
                reserved_top,
                reserved_bottom,
                &area_x, &area_y, &area_width, &area_height);
        }
        client->window_x = area_x;
        client->window_y = area_y;
        client->window_width = area_width;
        client->window_height = area_height;
        target_surface_width = area_width - (WINDOWD_WINDOW_BORDER * 2);
        target_surface_height = area_height - WINDOWD_WINDOW_TITLEBAR_HEIGHT - WINDOWD_WINDOW_BORDER;
        client->maximized = 1;
    }
    else
    {
        if (client->restore_window_width > 0 && client->restore_window_height > 0)
        {
            client->window_x = client->restore_window_x;
            client->window_y = client->restore_window_y;
            client->window_width = client->restore_window_width;
            client->window_height = client->restore_window_height;
            target_surface_width = client->window_width - (WINDOWD_WINDOW_BORDER * 2);
            target_surface_height = client->window_height - WINDOWD_WINDOW_TITLEBAR_HEIGHT - WINDOWD_WINDOW_BORDER;
        }
        client->maximized = 0;
    }

    resize_overlay_client_surface(session, dirty, slot, target_surface_width, target_surface_height);
    windowd_dirty_rect_add(dirty, &session->gfx.info, previous_frame.x, previous_frame.y, previous_frame.width, previous_frame.height);
    windowd_dirty_rect_add_client(dirty, client);
    raise_overlay(session, slot);
}

static uint32_t client_surface_capacity_width(const struct windowd_client *client)
{
    return client != 0 ? client->surface_info.pitch / (uint32_t)sizeof(uint32_t) : 0;
}

static uint32_t client_surface_capacity_height(const struct windowd_client *client)
{
    if (client == 0 || client->surface_info.pitch == 0)
    {
        return 0;
    }
    return client->surface_info.buffer_size / client->surface_info.pitch;
}

static void resize_overlay_client_surface(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    int slot,
    int surface_width,
    int surface_height)
{
    struct windowd_client *client = overlay_client_at(session, slot);
    struct sx_rect previous_frame;
    struct sx_rect current_frame;
    uint32_t max_width = 0;
    uint32_t max_height = 0;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0)
    {
        return;
    }

    max_width = client_surface_capacity_width(client);
    max_height = client_surface_capacity_height(client);
    if (max_width == 0 || max_height == 0)
    {
        return;
    }

    surface_width = windowd_clamp_int(surface_width, 1, (int)max_width);
    surface_height = windowd_clamp_int(surface_height, 1, (int)max_height);
    if ((int)client->surface_info.width == surface_width && (int)client->surface_info.height == surface_height)
    {
        return;
    }

    previous_frame = windowd_client_frame_rect(client);
    /* Limpiar ANTES de publicar el tamano nuevo en el header. El header es el
     * unico aviso que tiene el cliente: apenas lo ve cambiado repinta y copia
     * su frame a la superficie compartida, y si el memset corre despues le
     * borra parte de ese frame -- el cliente ya no repinta hasta el proximo
     * input, asi que la ventana queda medio negra. Con este orden, cuando el
     * cliente se entera la superficie ya esta limpia. */
    memset(client->pixels, 0, client->surface_info.buffer_size);
    client->surface_info.width = (uint32_t)surface_width;
    client->surface_info.height = (uint32_t)surface_height;
    if (client->header != 0)
    {
        client->header->info.width = client->surface_info.width;
        client->header->info.height = client->surface_info.height;
    }
    if (client->frame_visible)
    {
        client->window_width = surface_width + (WINDOWD_WINDOW_BORDER * 2);
        client->window_height = surface_height + WINDOWD_WINDOW_TITLEBAR_HEIGHT + WINDOWD_WINDOW_BORDER;
        if (!client->maximized)
        {
            client->restore_window_width = client->window_width;
            client->restore_window_height = client->window_height;
        }
        windowd_clamp_overlay_frame_position(
            layout_display(session),
            client->window_width,
            client->window_height,
            &client->window_x,
            &client->window_y);
    }
    current_frame = windowd_client_frame_rect(client);
    windowd_dirty_rect_add(dirty, &session->gfx.info, previous_frame.x, previous_frame.y, previous_frame.width, previous_frame.height);
    windowd_dirty_rect_add(dirty, &session->gfx.info, current_frame.x, current_frame.y, current_frame.width, current_frame.height);
}

/* Aplica un marco nuevo (posicion Y tamano) a una ventana. resize_overlay_
 * client_surface ajusta el tamano pero NO mueve el origen, y al arrastrar los
 * bordes izquierdo o superior el borde opuesto tiene que quedar anclado, o sea
 * que la ventana se mueve mientras cambia de tamano. Se daña el marco viejo
 * aparte porque la funcion de resize calcula su "marco previo" con el origen ya
 * actualizado, y sin esto quedaria basura donde estaba la ventana. */
static void apply_overlay_client_frame(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    int slot,
    struct sx_rect frame)
{
    struct windowd_client *client = overlay_client_at(session, slot);
    struct sx_rect previous_frame;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0 || !client->frame_visible)
    {
        return;
    }

    previous_frame = windowd_client_frame_rect(client);
    client->window_x = frame.x;
    client->window_y = frame.y;
    client->restore_window_x = frame.x;
    client->restore_window_y = frame.y;
    resize_overlay_client_surface(
        session,
        dirty,
        slot,
        frame.width - (WINDOWD_WINDOW_BORDER * 2),
        frame.height - WINDOWD_WINDOW_TITLEBAR_HEIGHT - WINDOWD_WINDOW_BORDER);
    windowd_dirty_rect_add(dirty, &session->gfx.info, previous_frame.x, previous_frame.y, previous_frame.width, previous_frame.height);
}

/* Marco resultante de arrastrar los bordes 'edges' desde el marco de origen.
 * El borde opuesto al que se arrastra queda anclado, y el minimo se aplica
 * recortando contra ese ancla (no moviendola), que es lo que hace que la
 * ventana "tope" en vez de empezar a desplazarse. */
static struct sx_rect resize_frame_for_drag(
    struct sx_rect origin,
    uint32_t edges,
    int delta_x,
    int delta_y)
{
    struct sx_rect frame = origin;

    if ((edges & WINDOWD_RESIZE_EDGE_LEFT) != 0)
    {
        int right = sx_rect_right(origin);
        frame.x = origin.x + delta_x;
        if (frame.x > right - WINDOWD_WINDOW_MIN_WIDTH)
        {
            frame.x = right - WINDOWD_WINDOW_MIN_WIDTH;
        }
        frame.width = right - frame.x;
    }
    else if ((edges & WINDOWD_RESIZE_EDGE_RIGHT) != 0)
    {
        frame.width = origin.width + delta_x;
        if (frame.width < WINDOWD_WINDOW_MIN_WIDTH)
        {
            frame.width = WINDOWD_WINDOW_MIN_WIDTH;
        }
    }

    if ((edges & WINDOWD_RESIZE_EDGE_TOP) != 0)
    {
        int bottom = sx_rect_bottom(origin);
        frame.y = origin.y + delta_y;
        if (frame.y > bottom - WINDOWD_WINDOW_MIN_HEIGHT)
        {
            frame.y = bottom - WINDOWD_WINDOW_MIN_HEIGHT;
        }
        frame.height = bottom - frame.y;
    }
    else if ((edges & WINDOWD_RESIZE_EDGE_BOTTOM) != 0)
    {
        frame.height = origin.height + delta_y;
        if (frame.height < WINDOWD_WINDOW_MIN_HEIGHT)
        {
            frame.height = WINDOWD_WINDOW_MIN_HEIGHT;
        }
    }

    return frame;
}

/* Reprograma el modo de video del scanout y adopta la geometria nueva. El
 * puntero al backbuffer no se mueve -- la seccion de display esta dimensionada
 * para el modo mas grande -- pero si cambia el stride, y con el toda la
 * composicion: windowd_render envuelve el backbuffer con gfx.info en cada
 * frame, asi que actualizar gfx.info alcanza para que el shell entero componga
 * en la resolucion nueva. Devuelve 0 si el modo quedo aplicado. */
static int apply_display_mode(struct windowd_session *session, uint32_t width, uint32_t height)
{
    if (session == 0 || width == 0 || height == 0)
    {
        return -1;
    }
    if (session->gfx.info.width == width && session->gfx.info.height == height)
    {
        return 0;
    }
    if (windowd_compositor_set_mode(&session->compositor, width, height) != 0)
    {
        return -1;
    }

    /* compositord composes the whole scene again in the new mode. */
    session->gfx.info = session->compositor.display_info;
    return 0;
}

/* Enter fullscreen as a composited shell policy. The daemon keeps owning the GPU
 * scanout while the shell hides chrome and scales the client surface to fill the
 * display. This path works on both VirtIO and the flat framebuffer backend,
 * avoiding direct client-scanout imports until the kernel grows handle passing
 * or another connectable surface-export mechanism. */
static int enter_overlay_fullscreen(struct windowd_session *session, struct windowd_dirty_rect *dirty, int slot)
{
    struct windowd_client *client = overlay_client_at(session, slot);

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0)
    {
        return -1;
    }
    if (!client->fullscreen_capable || client->fullscreen || client->minimized || session->fullscreen_slot >= 0)
    {
        return -1;
    }

    client->fs_restore_window_x = client->window_x;
    client->fs_restore_window_y = client->window_y;
    client->fs_restore_frame_visible = client->frame_visible;
    client->fs_restore_maximized = client->maximized;

    /* Modo de video bajo: si el adaptador sabe cambiarlo, el scanout pasa a la
     * resolucion de la superficie del cliente y sus pixeles van 1:1 al
     * framebuffer, sin la pasada de escalado por software. Va antes de fijar la
     * geometria de abajo, que se calcula desde gfx.info. Si el cambio no se
     * puede, se sigue componiendo escalado: es degradacion, no error. */
    (void)apply_display_mode(session, client->surface_info.width, client->surface_info.height);

    client->fullscreen = 1;
    client->frame_visible = 0;
    client->maximized = 0;
    client->window_x = 0;
    client->window_y = 0;
    client->window_width = (int)session->gfx.info.width;
    client->window_height = (int)session->gfx.info.height;
    session->fullscreen_slot = slot;
    raise_overlay(session, slot);
    windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
    return 0;
}

/* Leave composited fullscreen and restore the windowed chrome/geometry. */
static void exit_overlay_fullscreen(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    int slot = session != 0 ? session->fullscreen_slot : -1;
    struct windowd_client *client = overlay_client_at(session, slot);

    if (session == 0 || dirty == 0 || client == 0)
    {
        if (session != 0)
        {
            session->fullscreen_slot = -1;
        }
        return;
    }

    session->fullscreen_slot = -1;
    client->fullscreen = 0;

    /* Volver al modo nativo antes de restaurar y repintar: las posiciones
     * guardadas de las ventanas estan en coordenadas nativas, y el dirty rect
     * del final tiene que cubrir la pantalla entera, no la chica. */
    (void)apply_display_mode(
        session,
        session->compositor.requested_info.width,
        session->compositor.requested_info.height);

    client->frame_visible = client->fs_restore_frame_visible;
    client->maximized = client->fs_restore_maximized;
    client->window_x = client->fs_restore_window_x;
    client->window_y = client->fs_restore_window_y;
    client->window_width = (int)client->surface_info.width + (client->frame_visible ? (WINDOWD_WINDOW_BORDER * 2) : 0);
    client->window_height = (int)client->surface_info.height + (client->frame_visible ? (WINDOWD_WINDOW_TITLEBAR_HEIGHT + WINDOWD_WINDOW_BORDER) : 0);
    windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
}

static void move_overlay_client_window(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    int slot,
    int window_x,
    int window_y)
{
    struct windowd_client *client = overlay_client_at(session, slot);
    struct sx_rect previous_frame;
    struct sx_rect current_frame;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0)
    {
        return;
    }

    previous_frame = windowd_client_frame_rect(client);
    windowd_clamp_overlay_frame_position(
        layout_display(session),
        previous_frame.width,
        previous_frame.height,
        &window_x,
        &window_y);
    if (client->window_x == window_x && client->window_y == window_y)
    {
        return;
    }

    client->window_x = window_x;
    client->window_y = window_y;
    current_frame = windowd_client_frame_rect(client);
    windowd_dirty_rect_add(
        dirty,
        &session->gfx.info,
        previous_frame.x,
        previous_frame.y,
        previous_frame.width,
        previous_frame.height);
    windowd_dirty_rect_add(
        dirty,
        &session->gfx.info,
        current_frame.x,
        current_frame.y,
        current_frame.width,
        current_frame.height);
}

static const struct windowd_client *top_overlay_client_at_point(const struct windowd_session *session, int x, int y)
{
    int order_index;

    if (session == 0)
    {
        return 0;
    }

    for (order_index = session->overlay_count - 1; order_index >= 0; --order_index)
    {
        int slot = session->overlay_order[order_index];
        const struct windowd_client *client = overlay_client_at_const(session, slot);
        if (overlay_client_visible(client) && windowd_point_in_frame(client, x, y))
        {
            return client;
        }
    }
    return 0;
}

/* Topmost desktop window of `role` under the point, or 0. Higher ids stack
 * above lower ones, as build_layers draws them. */
static const struct windowd_client *top_desktop_window_at_point(
    const struct windowd_session *session, uint32_t role, int x, int y)
{
    int index;

    for (index = WINDOWD_DESKTOP_WINDOW_COUNT - 1; index >= 0; --index)
    {
        const struct windowd_client *client = &session->desktop_windows[index];

        if (client->pid > 0 && client->role == role && windowd_point_in_client(client, x, y))
        {
            return client;
        }
    }
    return 0;
}

/* Hit test in stacking order: popups, panels, application windows, background.
 * A fullscreen app covers the desktop's panels and popups, so they are neither
 * drawn nor hit while it is up. */
static const struct windowd_client *top_client_at_point(const struct windowd_session *session, int x, int y)
{
    const struct windowd_client *client;

    if (session == 0)
    {
        return 0;
    }
    if (session->fullscreen_slot < 0)
    {
        client = top_desktop_window_at_point(session, WINDOWD_ROLE_POPUP, x, y);
        if (client == 0)
        {
            client = top_desktop_window_at_point(session, WINDOWD_ROLE_PANEL, x, y);
        }
        if (client != 0)
        {
            return client;
        }
    }
    client = top_overlay_client_at_point(session, x, y);
    if (client != 0)
    {
        return client;
    }
    return top_desktop_window_at_point(session, WINDOWD_ROLE_BACKGROUND, x, y);
}

/* An overlay client connected but that hasn't submitted its first frame yet is
 * still starting up -- surfaced to the user as the WAIT cursor. */
static int any_overlay_client_starting(const struct windowd_session *session)
{
    int slot;

    if (session == 0)
    {
        return 0;
    }
    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        const struct windowd_client *client = &session->overlay_clients[slot];
        if (client->pid > 0 && !client->started)
        {
            return 1;
        }
    }
    return 0;
}

/* Priority: dragging a window > an app starting up > a clickable desktop/menu
 * item > a hint reported by the hovered app's own widgets > the plain arrow. */
static int resolve_cursor_shape(
    const struct windowd_session *session,
    const struct windowd_client *current_hover_client,
    int cursor_x,
    int cursor_y,
    int drag_active)
{
    (void)cursor_x;
    (void)cursor_y;
    if (drag_active)
    {
        return SAVANXP_CURSOR_MOVE;
    }
    if (any_overlay_client_starting(session))
    {
        return SAVANXP_CURSOR_WAIT;
    }
    /* Sobre un borde redimensionable, el cursor lo anticipa. No hay glifos
     * diagonales, asi que las esquinas caen en el eje horizontal. */
    {
        uint32_t edges = windowd_resize_edge_from_point(current_hover_client, cursor_x, cursor_y);
        if (edges != WINDOWD_RESIZE_EDGE_NONE)
        {
            return ((edges & (WINDOWD_RESIZE_EDGE_LEFT | WINDOWD_RESIZE_EDGE_RIGHT)) != 0)
                ? SAVANXP_CURSOR_RESIZE_H
                : SAVANXP_CURSOR_RESIZE_V;
        }
    }
    /* Un dueno deshabilitado no reacciona al puntero, asi que tampoco muestra
     * el cursor que pidieron sus widgets. */
    if (current_hover_client != 0 &&
        !overlay_client_disabled(session, current_hover_client) &&
        windowd_point_in_client(current_hover_client, cursor_x, cursor_y))
    {
        return current_hover_client->last_cursor_hint_shape;
    }
    return SAVANXP_CURSOR_ARROW;
}

static int windowd_stage_failed(const char *stage, long result)
{
    if (result < 0)
    {
        eprintf("desktop: %s failed (%s)\n", stage, result_error_string(result));
    }
    else
    {
        eprintf("desktop: %s failed\n", stage);
    }
    return -1;
}

/* Devuelve 1 si el evento se entrego. Con el pipe lleno el write no bloquea
 * (extremo no-bloqueante) y el evento se DESCARTA: preferimos perder input de un
 * cliente que no drena antes que congelar la sesion. Es seguro para el puntero,
 * que lleva posicion absoluta -- el proximo evento corrige --, y aceptable para
 * el teclado, que se recupera solo en cuanto el cliente vuelve a leer. Siempre
 * se escribe un registro entero (ver savanxp_wm_event). */
/* Entrada para un cliente nativo: el mismo registro va al anillo en vez de
 * al pipe (el cliente no puede leerlo), con la misma disciplina SPSC que la
 * cola de launches -- registro primero, head despues -- y el mismo descarte
 * en lleno. El wake lo senala aca mismo porque, a diferencia del submit del
 * cliente, la entrada no pasa por ningun otro aviso. */
static int route_event_ring(const struct windowd_client *client, const struct savanxp_wm_event *event)
{
    struct savanxp_wm_input_ring *ring = 0;
    uint64_t head = 0;

    if (client == 0 || event == 0)
    {
        return 0;
    }
    ring = client->input_ring;
    if (ring == 0 || ring->magic != SAVANXP_WM_INPUT_RING_MAGIC ||
        ring->version != SAVANXP_WM_INPUT_RING_VERSION_1)
    {
        return 0;
    }
    head = ring->head;
    if (head - ring->tail >= SAVANXP_WM_INPUT_RING_CAPACITY)
    {
        return 0;
    }
    ring->records[head % SAVANXP_WM_INPUT_RING_CAPACITY] = *event;
    ring->records[head % SAVANXP_WM_INPUT_RING_CAPACITY].window_id = client->window_id;
    __asm__ volatile("" ::: "memory");
    ring->head = head + 1u;
    if (client->wake_event_fd >= 0)
    {
        (void)event_set(client->wake_event_fd);
    }
    return 1;
}

static int route_event(const struct windowd_client *client, const struct savanxp_wm_event *event)
{
    struct savanxp_wm_event tagged;

    if (client == 0 || event == 0)
    {
        return 0;
    }
    if (client->native_input)
    {
        return route_event_ring(client, event);
    }
    if (client->events_write_fd < 0)
    {
        return 0;
    }
    /* El pipe es del proceso: el id dice a cual de sus ventanas va. */
    tagged = *event;
    tagged.window_id = client->window_id;
    return savanxp_write(client->events_write_fd, &tagged, sizeof(tagged)) == (long)sizeof(tagged);
}

static int route_key(const struct windowd_client *client, const struct savanxp_input_event *key_event)
{
    struct savanxp_wm_event event;

    if (key_event == 0)
    {
        return 0;
    }
    memset(&event, 0, sizeof(event));
    event.kind = SAVANXP_WM_EVENT_KEY;
    event.payload.key = *key_event;
    return route_event(client, &event);
}

/* Deliver the pointer to a client in its own surface-local coordinates, so the
 * app hit-tests in local space and stays aligned with the system cursor the
 * compositor draws. */
static int route_pointer(
    const struct windowd_client *client, int cursor_x, int cursor_y, int wheel, uint32_t buttons)
{
    struct sx_rect surface_rect;
    struct savanxp_wm_event event;

    if (client == 0 || client->events_write_fd < 0)
    {
        return 0;
    }
    surface_rect = windowd_client_surface_rect(client);
    memset(&event, 0, sizeof(event));
    event.kind = SAVANXP_WM_EVENT_POINTER;
    event.payload.pointer.x = cursor_x - surface_rect.x;
    event.payload.pointer.y = cursor_y - surface_rect.y;
    event.payload.pointer.wheel = wheel;
    event.payload.pointer.buttons = buttons;
    return route_event(client, &event);
}

static size_t coalesce_mouse_events(
    const struct savanxp_mouse_event *events,
    size_t event_count,
    struct savanxp_mouse_event *coalesced,
    size_t coalesced_capacity,
    uint32_t initial_buttons)
{
    uint32_t current_buttons = initial_buttons;
    size_t index = 0;
    size_t coalesced_count = 0;
    int last_was_button_transition = 0;

    if (events == 0 || coalesced == 0 || coalesced_capacity == 0)
    {
        return 0;
    }

    for (index = 0; index < event_count; ++index)
    {
        const struct savanxp_mouse_event *event = &events[index];
        int button_transition = event->buttons != current_buttons;

        if (button_transition ||
            coalesced_count == 0 ||
            coalesced[coalesced_count - 1u].buttons != event->buttons ||
            last_was_button_transition)
        {
            if (coalesced_count >= coalesced_capacity)
            {
                break;
            }
            coalesced[coalesced_count++] = *event;
        }
        else
        {
            coalesced[coalesced_count - 1u].delta_x += event->delta_x;
            coalesced[coalesced_count - 1u].delta_y += event->delta_y;
            /* La rueda se SUMA como los deltas y no se pisa: fundir dos eventos
             * quedandose con el wheel del primero descarta ticks, y un tick
             * perdido es scroll que no ocurre nunca. */
            coalesced[coalesced_count - 1u].wheel += event->wheel;
            /* La posicion se PISA con la del evento que se acaba de fundir,
             * porque es la ultima que vio el host y es la que manda. Si el que
             * entra no trae posicion, el fundido tampoco la lleva: conservar la
             * vieja aplicaria una posicion que ya no es de nadie. Los deltas
             * de los dos siguen sumando igual, y con el flag puesto el
             * consumidor asigna la posicion en vez de sumar los deltas. */
            if ((event->flags & SAVANXP_MOUSE_FLAG_ABSOLUTE) != 0u)
            {
                coalesced[coalesced_count - 1u].absolute_x = event->absolute_x;
                coalesced[coalesced_count - 1u].absolute_y = event->absolute_y;
                coalesced[coalesced_count - 1u].flags |= SAVANXP_MOUSE_FLAG_ABSOLUTE;
            }
            else
            {
                coalesced[coalesced_count - 1u].flags &= ~(uint32_t)SAVANXP_MOUSE_FLAG_ABSOLUTE;
            }
        }

        current_buttons = event->buttons;
        last_was_button_transition = button_transition;
    }

    return coalesced_count;
}

/* Alive and not yet ended: a process that exited is "ended" until its parent
 * reaps it, and gone after. */
static int windowd_process_alive(long pid)
{
    struct sxn_process_query query;

    return pid > 0 && sxn_process_query(pid, &query) == 0 && query.state == SXN_PROCESS_RUNNING;
}

/* Zombies pendientes de `waitpid`, para no bloquear el bucle de eventos.
 *
 * `destroy_client_instance` cerraba la ventana con un `waitpid` bloqueante en
 * el mismo `for(;;)`: matar un programa que aun esta en su carga (ldso antes
 * de `main`, disco + secciones bajo el big lock) dejaba a windowd sin rutear
 * input, sin componer y sin presentar hasta que el hijo moria. Ahora la
 * ventana se desmonta enseguida y el pid queda aqui; el barrido de cada vuelta
 * solo llama a `waitpid` cuando `windowd_process_alive` ya dijo que no sigue
 * vivo (zombie o ido), que son los dos casos que el kernel resuelve sin
 * bloquear. */
#define WINDOWD_MAX_PENDING_REAPS 32
static long g_pending_reaps[WINDOWD_MAX_PENDING_REAPS];
static int g_pending_reap_count = 0;

static void queue_pending_reap(long pid)
{
    int index;

    if (pid <= 0)
    {
        return;
    }
    for (index = 0; index < g_pending_reap_count; ++index)
    {
        if (g_pending_reaps[index] == pid)
        {
            return;
        }
    }
    if (g_pending_reap_count >= WINDOWD_MAX_PENDING_REAPS)
    {
        return;
    }
    g_pending_reaps[g_pending_reap_count++] = pid;
}

static void sweep_pending_reaps(void)
{
    int status = 0;
    int index = 0;

    while (index < g_pending_reap_count)
    {
        long pid = g_pending_reaps[index];
        if (windowd_process_alive(pid))
        {
            ++index;
            continue;
        }
        (void)savanxp_waitpid((int)pid, &status);
        g_pending_reaps[index] = g_pending_reaps[--g_pending_reap_count];
    }
}

/* The first frame of a window is what puts it in the scene
 * (windowd_client_in_scene): windowd sees it in its own mapping of the header
 * and latches it, so a client cannot take it back. Its frame is repainted with
 * it. */
static void service_client_start(struct windowd_dirty_rect *dirty, struct windowd_client *client)
{
    if (client == 0 || client->pid <= 0 || client->started || client->header == 0)
    {
        return;
    }
    if (*(volatile uint64_t *)&client->header->submit_sequence != 0)
    {
        client->started = 1;
        windowd_dirty_rect_add_client(dirty, client);
    }
}

static void service_client_starts(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    int index;

    for (index = 0; index < WINDOWD_SESSION_CLIENT_COUNT; ++index)
    {
        service_client_start(dirty, session_client_at(session, index));
    }
}

/* A surface's id in compositord's scene: its slot in the session plus 2
 * (1 is windowd's own surface). */
static uint32_t session_surface_id(const struct windowd_session *session, const struct windowd_client *client)
{
    if (session == 0 || client == 0)
    {
        return 0;
    }
    if (client >= &session->desktop_windows[0] && client < &session->desktop_windows[WINDOWD_DESKTOP_WINDOW_COUNT])
    {
        return (uint32_t)(client - &session->desktop_windows[0]) + 2u;
    }
    if (client >= &session->overlay_clients[0] && client < &session->overlay_clients[WINDOWD_MAX_OVERLAY_CLIENTS])
    {
        return (uint32_t)(client - &session->overlay_clients[0]) + 2u + (uint32_t)WINDOWD_DESKTOP_WINDOW_COUNT;
    }
    return 0;
}

/* Hands a client surface to compositord (ATTACH): the section and the wake
 * event it signals composed and retired on. windowd keeps both. */
static int attach_client_surface(struct windowd_client *client, int section_fd, int wake_event_fd)
{
    struct savanxp_compositor_surface surface;
    uint32_t id = session_surface_id(g_session, client);

    if (id == 0 || client->mapped_view == 0 || client->header == 0)
    {
        return -1;
    }
    memset(&surface, 0, sizeof(surface));
    surface.surface_id = id;
    surface.flags = SAVANXP_COMPOSITOR_SURFACE_CLIENT;
    surface.section_size = (uint64_t)client->header->pixels_offset + client->surface_info.buffer_size;
    surface.pixels_offset = (uint32_t)((unsigned char *)client->pixels - (unsigned char *)client->mapped_view);
    surface.pitch = client->surface_info.pitch;
    surface.capacity_width = client_surface_capacity_width(client);
    surface.capacity_height = client_surface_capacity_height(client);
    if (windowd_compositor_attach(&g_session->compositor, &surface, section_fd, wake_event_fd) < 0)
    {
        return -1;
    }
    client->surface_id = id;
    return 0;
}

static void detach_client_surface(struct windowd_client *client)
{
    if (client != 0 && client->surface_id != 0 && g_session != 0)
    {
        (void)windowd_compositor_detach(&g_session->compositor, client->surface_id);
        client->surface_id = 0;
    }
}

static void destroy_overlay_client(struct windowd_session *session, int slot, int terminate_client);
static void destroy_desktop(struct windowd_session *session, int terminate_client);

/* compositord found a client's batches malformed: a broken client. For the
 * desktop that is the whole process, which the session manager restarts. */
static void drop_broken_clients(struct windowd_session *session, struct windowd_dirty_rect *dirty, uint32_t broken)
{
    int index;

    for (index = 0; broken != 0 && index < WINDOWD_SESSION_CLIENT_COUNT; ++index)
    {
        struct windowd_client *client = session_client_at(session, index);
        uint32_t id = session_surface_id(session, client);

        if (client == 0 || client->surface_id == 0 || (broken & (1u << id)) == 0)
        {
            continue;
        }
        eprintf("desktop: malformed frames from %s\n", client->path[0] != '\0' ? client->path : "?");
        if (index < WINDOWD_DESKTOP_WINDOW_COUNT)
        {
            destroy_desktop(session, 1);
        }
        else
        {
            destroy_overlay_client(session, index - WINDOWD_DESKTOP_WINDOW_COUNT, 1);
        }
        windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
    }
}

/*
 * Commits the scene (docs/desktop/DESKTOP_ARCHITECTURE.md#the-scene):
 * repaints windowd's surface over `dirty`, writes the layers, that damage and
 * the cursor into the scene section, and has compositord compose. `wait`: the
 * reply comes once the frame is on the screen (the selftests sample it).
 */
static int windowd_commit(
    struct windowd_session *session,
    int cursor_x,
    int cursor_y,
    struct windowd_dirty_rect *dirty,
    int wait)
{
    struct savanxp_compositor_scene *scene = session->compositor.scene;
    struct savanxp_compositor_reply reply;
    size_t count = windowd_dirty_rect_count(dirty);
    size_t index;
    int result;

    if (scene == 0)
    {
        return -1;
    }
    windowd_paint_wm_surface(session, dirty);

    scene->magic = SAVANXP_COMPOSITOR_SCENE_MAGIC;
    scene->flags = 0;
    scene->layer_count = windowd_build_scene(session, scene->layers, SAVANXP_COMPOSITOR_MAX_LAYERS);
    scene->damage_count = 0;
    if (count > SAVANXP_COMPOSITOR_MAX_DAMAGE)
    {
        scene->flags |= SAVANXP_COMPOSITOR_SCENE_FULL_DAMAGE;
    }
    else
    {
        for (index = 0; index < count; ++index)
        {
            struct sx_rect rect;

            if (windowd_dirty_rect_at(dirty, index, &rect))
            {
                scene->damage[scene->damage_count].x = rect.x;
                scene->damage[scene->damage_count].y = rect.y;
                scene->damage[scene->damage_count].width = rect.width;
                scene->damage[scene->damage_count].height = rect.height;
                scene->damage_count += 1u;
            }
        }
    }
    scene->cursor_x = cursor_x;
    scene->cursor_y = cursor_y;
    scene->cursor_shape = (uint32_t)session->current_cursor_shape;
    scene->cursor_visible = 1u;

    result = windowd_compositor_commit(&session->compositor, wait ? SAVANXP_COMPOSITOR_COMMIT_WAIT : 0u, &reply);
    windowd_dirty_rect_reset(dirty);
    session->cursor_moved = 0;
    if (result < 0)
    {
        return windowd_stage_failed("compositord commit", result);
    }
    drop_broken_clients(session, dirty, reply.broken_surfaces);
    return 0;
}

/* `background`: the desktop's main window, full screen and frameless.
 * Otherwise an application window. */
static void fill_client_surface_info(
    const struct windowd_session *session,
    int background,
    struct savanxp_fb_info *client_info)
{
    if (session == 0 || client_info == 0)
    {
        return;
    }

    if (!background)
    {
        windowd_fill_overlay_surface_info(layout_display(session), client_info);
    }
    else
    {
        windowd_fill_shell_surface_info(layout_display(session), client_info);
    }
}

static void position_client_window(
    const struct windowd_session *session,
    struct windowd_client *client,
    int background,
    int cascade_index)
{
    if (session == 0 || client == 0)
    {
        return;
    }

    if (!background)
    {
        windowd_place_overlay_window(
            layout_display(session),
            &client->surface_info,
            cascade_index,
            &client->window_x,
            &client->window_y,
            &client->window_width,
            &client->window_height);
        client->restore_window_x = client->window_x;
        client->restore_window_y = client->window_y;
        client->restore_window_width = client->window_width;
        client->restore_window_height = client->window_height;
        client->frame_visible = 1;
        client->minimized = 0;
        client->maximized = 0;
    }
    else
    {
        client->window_x = 0;
        client->window_y = 0;
        client->window_width = (int)client->surface_info.width;
        client->window_height = (int)client->surface_info.height;
        client->restore_window_x = client->window_x;
        client->restore_window_y = client->window_y;
        client->restore_window_width = client->window_width;
        client->restore_window_height = client->window_height;
        client->frame_visible = 0;
        client->minimized = 0;
        client->maximized = 0;
    }
}

static void destroy_client_instance(struct windowd_client *client, int terminate_client)
{
    int status = 0;

    if (client == 0)
    {
        return;
    }

    /* Out of compositord first: it stops reading the section before it goes. */
    detach_client_surface(client);

    /* Shutdown request: the flag first, then the wake. In that order, a client
     * that resets the event and looks at the header again does not miss it. */
    if (client->header != 0)
    {
        client->header->flags |= SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN;
    }
    if (client->wake_event_fd >= 0)
    {
        (void)event_set(client->wake_event_fd);
    }
    /* A slot still waiting for the process windowd spawned: that process is
     * windowd's child, so it is killed (when asked) and reaped like a window's. */
    if (client->reserved_pid > 0)
    {
        if (terminate_client)
        {
            (void)savanxp_kill((int)client->reserved_pid, SAVANXP_SIGKILL);
        }
        if (windowd_process_alive(client->reserved_pid))
        {
            queue_pending_reap(client->reserved_pid);
        }
        else
        {
            (void)savanxp_waitpid((int)client->reserved_pid, &status);
        }
    }
    /* Only a process's main window (id 0) owns the process. Any other window
     * of it -- a dialog, or a panel or popup of the desktop -- shares the pid
     * of a process that is still running: it is not waited for.
     *
     * A connected process is never killed: closing its event pipe below is a
     * hang-up its runtime answers by ending the process
     * (docs/desktop/DESKTOP_ARCHITECTURE.md#closing-a-window). One that never
     * reads it is not responding, and ending it is the shell's offer. */
    if (client->pid > 0 && client->window_id == 0)
    {
        if (windowd_process_alive(client->pid))
        {
            queue_pending_reap(client->pid);
        }
        else
        {
            (void)savanxp_waitpid((int)client->pid, &status);
        }
    }
    close_fd_if_needed(&client->events_write_fd);
    close_fd_if_needed(&client->wake_event_fd);
    close_fd_if_needed(&client->shell_request_read_fd);
    if (client->mapped_view != 0 && !result_is_error((long)client->mapped_view))
    {
        (void)unmap_view(client->mapped_view);
    }
    if (client->input_ring != 0)
    {
        (void)unmap_view(client->input_ring);
    }
    if (client->window_list != 0)
    {
        (void)unmap_view(client->window_list);
    }
    reset_client(client);
}

/*
 * Creates the section of `client`'s surface at the size of surface_info, maps
 * it in the WM and writes its header. Returns the section's fd, which the
 * caller sends to the client through the event pipe's handle queue and then
 * closes; the WM's mapping keeps it alive. -1 on failure, leaving nothing open
 * or mapped.
 */
static int create_client_surface(struct windowd_client *client)
{
    struct savanxp_gpu_client_surface_header *header;
    unsigned long command_bytes = 0;
    unsigned long pixels_offset = 0;
    unsigned long section_size = 0;
    int section_fd = -1;

    command_bytes = (unsigned long)(SAVANXP_GPU_CLIENT_BATCH_CAPACITY * sizeof(struct savanxp_gpu_dirty_rect_batch));
    /* Page-align the pixel region. Older fullscreen-exclusive scanout used this
     * directly; keeping the alignment keeps the client ABI simple. */
    pixels_offset = ((unsigned long)sizeof(*header) + command_bytes + (WINDOWD_SURFACE_PAGE_SIZE - 1u)) & ~(unsigned long)(WINDOWD_SURFACE_PAGE_SIZE - 1u);
    section_size = pixels_offset + client->surface_info.buffer_size;
    section_fd = (int)section_create(section_size, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (section_fd < 0)
    {
        return -1;
    }
    client->mapped_view = map_view(section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)client->mapped_view))
    {
        client->mapped_view = 0;
        close_fd_if_needed(&section_fd);
        return -1;
    }

    header = (struct savanxp_gpu_client_surface_header *)client->mapped_view;
    /* All of it, requests included: garbage there would be requests nobody
     * made. */
    memset(header, 0, sizeof(*header));
    header->magic = SAVANXP_GPU_CLIENT_SURFACE_MAGIC;
    header->command_offset = (uint32_t)sizeof(*header);
    header->pixels_offset = (uint32_t)pixels_offset;
    header->info = client->surface_info;
    header->version = SAVANXP_GPU_CLIENT_SURFACE_VERSION_5;
    header->pixel_format = SAVANXP_GPU_SURFACE_FORMAT_BGRX8888;
    header->batch_capacity = SAVANXP_GPU_CLIENT_BATCH_CAPACITY;
    header->rect_capacity = SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS;
    client->header = header;
    client->command_batches = (struct savanxp_gpu_dirty_rect_batch *)((unsigned char *)client->mapped_view + header->command_offset);
    client->pixels = (uint32_t *)((unsigned char *)client->mapped_view + header->pixels_offset);
    memset(client->command_batches, 0, command_bytes);
    memset(client->pixels, 0, client->surface_info.buffer_size);
    return section_fd;
}

static void copy_client_path(struct windowd_client *client, const char *path)
{
    size_t length = path != 0 ? strlen(path) : 0u;

    if (length >= sizeof(client->path))
    {
        length = sizeof(client->path) - 1u;
    }
    if (length != 0u)
    {
        memcpy(client->path, path, length);
    }
    client->path[length] = '\0';
}

/* The label of a client that connected on its own: the process name the
 * kernel keeps (the file name of its image). */
static void client_path_from_process(struct windowd_client *client, long pid)
{
    struct sxn_process_query query;

    client->path[0] = '\0';
    if (sxn_process_query(pid, &query) == 0)
    {
        query.name[sizeof(query.name) - 1u] = '\0';
        copy_client_path(client, query.name);
    }
}

/*
 * Starts a program the session itself needs (the desktop, Program Manager,
 * the selftests' apps). It gets only stdio and opens its window by connecting
 * to the port, like a program any other client starts (protocol v5,
 * savanxp/wm_protocol.h): windowd does not wire anything into it.
 */
static long windowd_spawn(const char *path, const char *argument)
{
    const char *argv[3] = {path, 0, 0};
    int argc = 1;

    if (argument != 0 && argument[0] != '\0')
    {
        argv[1] = argument;
        argc = 2;
    }
    return spawn(path, argv, argc);
}

static void close_pending_connection(struct windowd_pending_connection *connection)
{
    close_fd_if_needed(&connection->read_fd);
    close_fd_if_needed(&connection->write_fd);
    connection->pid = 0;
    connection->deadline_ms = 0;
}

/* Answers a connection it will not serve with a welcome that carries only the
 * error, so the client fails at once instead of at its timeout. */
static void refuse_connection(struct windowd_pending_connection *connection, int status)
{
    struct savanxp_wm_event record;

    memset(&record, 0, sizeof(record));
    record.kind = SAVANXP_WM_EVENT_WELCOME;
    record.payload.welcome.status = status;
    if (connection->write_fd >= 0)
    {
        (void)savanxp_fcntl(connection->write_fd, SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK);
        (void)savanxp_write(connection->write_fd, &record, sizeof(record));
    }
    close_pending_connection(connection);
}

/*
 * Builds the channels of an admitted connection and hands them over: the
 * handles go into the event pipe's queue -- the surface section, the wake
 * event, the session's submit event (compositord's), then the input ring or
 * the window list, then windowd's request event if the hello asked for it --
 * and the welcome record after them. windowd keeps two descriptors: the
 * event pipe's write end and the wake event; the shell also keeps its request
 * pipe. `client` already has its surface_info and role. 0, or -1 leaving the
 * client reset.
 */
static int connect_client(
    struct windowd_client *client,
    struct windowd_pending_connection *connection,
    int submit_event_fd,
    int shell_role,
    int input_ring,
    int request_event_fd)
{
    struct savanxp_wm_event record;
    int section_fd = -1;
    int window_list_section_fd = -1;
    int input_ring_fd = -1;
    int wake_event = -1;
    uint32_t handle_count = 0;

    section_fd = create_client_surface(client);
    if (section_fd < 0)
    {
        return -1;
    }

    if (input_ring)
    {
        input_ring_fd = (int)section_create(
            sizeof(struct savanxp_wm_input_ring),
            SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
        if (input_ring_fd < 0)
        {
            goto fail;
        }
        client->input_ring = (struct savanxp_wm_input_ring *)map_view(
            input_ring_fd,
            SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
        if (result_is_error((long)client->input_ring))
        {
            client->input_ring = 0;
            goto fail;
        }
        memset(client->input_ring, 0, sizeof(*client->input_ring));
        client->input_ring->magic = SAVANXP_WM_INPUT_RING_MAGIC;
        client->input_ring->version = SAVANXP_WM_INPUT_RING_VERSION_1;
        client->native_input = 1;
    }

    wake_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    if (wake_event < 0)
    {
        goto fail;
    }
    /* The WRITE end goes non-blocking: the WM cannot be held up by a client
     * that does not drain its events. A freshly connected app is raised at
     * once but takes a while to start reading, and every mouse move meanwhile
     * goes to its pipe; with blocking writes the full pipe froze the session. */
    if (savanxp_fcntl(connection->write_fd, SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0)
    {
        goto fail;
    }

    if (shell_role)
    {
        /* The list goes in a SECTION, not a pipe: it weighs more than ten KiB
         * and a pipe can take it half way if the client falls behind, which
         * would desynchronize the stream for good. */
        window_list_section_fd = (int)section_create(
            sizeof(struct savanxp_wm_window_list),
            SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
        if (window_list_section_fd < 0)
        {
            goto fail;
        }
        client->window_list = (struct savanxp_wm_window_list *)map_view(
            window_list_section_fd,
            SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
        if (result_is_error((long)client->window_list))
        {
            client->window_list = 0;
            goto fail;
        }
        memset(client->window_list, 0, sizeof(*client->window_list));
        client->window_list->version = SAVANXP_WM_SHELL_PROTOCOL_VERSION;
    }

    if (attach_client_surface(client, section_fd, wake_event) < 0)
    {
        goto fail;
    }

    /* Five at most, in a queue of eight. The client takes them all before it
     * asks for an owned window, whose section uses that queue. */
    if (pipe_send_handle(connection->write_fd, section_fd) < 0 ||
        pipe_send_handle(connection->write_fd, wake_event) < 0 ||
        pipe_send_handle(connection->write_fd, submit_event_fd) < 0)
    {
        goto fail;
    }
    handle_count = 3u;
    if (input_ring_fd >= 0 || window_list_section_fd >= 0)
    {
        if (pipe_send_handle(connection->write_fd, input_ring_fd >= 0 ? input_ring_fd : window_list_section_fd) < 0)
        {
            goto fail;
        }
        handle_count = 4u;
    }
    if (request_event_fd >= 0)
    {
        if (pipe_send_handle(connection->write_fd, request_event_fd) < 0)
        {
            goto fail;
        }
        handle_count += 1u;
    }

    memset(&record, 0, sizeof(record));
    record.kind = SAVANXP_WM_EVENT_WELCOME;
    record.payload.welcome.status = 0;
    record.payload.welcome.handle_count = handle_count;
    record.payload.welcome.flags = (shell_role ? SAVANXP_WM_WELCOME_SHELL : 0u) |
        (request_event_fd >= 0 ? SAVANXP_WM_WELCOME_REQUEST_EVENT : 0u);
    /* The pipe is empty: a whole record always fits. */
    if (savanxp_write(connection->write_fd, &record, sizeof(record)) != (long)sizeof(record))
    {
        goto fail;
    }

    close_fd_if_needed(&section_fd);
    close_fd_if_needed(&window_list_section_fd);
    close_fd_if_needed(&input_ring_fd);
    client->events_write_fd = connection->write_fd;
    connection->write_fd = -1;
    client->wake_event_fd = wake_event;
    if (shell_role)
    {
        /* Already non-blocking: the shell's requests are read like every
         * other input of the WM. */
        client->shell_request_read_fd = connection->read_fd;
        connection->read_fd = -1;
    }
    client->size_hint_consumed_sequence = 0;
    return 0;

fail:
    close_fd_if_needed(&window_list_section_fd);
    close_fd_if_needed(&input_ring_fd);
    close_fd_if_needed(&wake_event);
    close_fd_if_needed(&section_fd);
    /* pid is still 0: this only unmaps and resets. */
    destroy_client_instance(client, 0);
    return -1;
}

/* Destroys every window of the desktop process, its role windows before its
 * main window (which owns the process), and forgets its hotkeys. */
static void destroy_desktop(struct windowd_session *session, int terminate_client)
{
    int index;

    if (session == 0)
    {
        return;
    }
    for (index = WINDOWD_DESKTOP_WINDOW_COUNT - 1; index >= 1; --index)
    {
        destroy_client_instance(&session->desktop_windows[index], 0);
    }
    destroy_client_instance(&session->desktop_windows[0], terminate_client);
    session->hotkey_count = 0;
    session->hotkey_held_key = 0;
}

/* Starts a desktop for the selftest; in a session, sessiond does. When it
 * connects asking for the shell role, its main window becomes the background
 * (full screen, frameless, bottom of the stack, never focused); everything
 * else it shows it asks for as role windows. windowd being its parent is what
 * passes the shell rule (started_by_session). The slot is held so its death
 * before connecting is still noticed. */
static int launch_desktop_client(struct windowd_session *session)
{
    struct windowd_client *client = 0;
    long pid = 0;

    if (session == 0)
    {
        return -1;
    }

    destroy_desktop(session, 1);
    client = &session->desktop_windows[0];
    reset_client(client);
    pid = windowd_spawn(k_desktop_path, 0);
    if (pid < 0)
    {
        eprintf("windowd: cannot start %s (%s)\n", k_desktop_path, result_error_string(pid));
        return -1;
    }
    client->reserved_pid = pid;
    copy_client_path(client, k_desktop_path);
    return 0;
}

/* Publica el estado de las ventanas en la seccion compartida.
 *
 * Seqlock: la secuencia sube a IMPAR antes de tocar las entradas y a PAR al
 * terminar, asi el cliente sabe si leyo un estado a medio escribir. Hace falta
 * de verdad -- son dos procesos y al WM lo pueden desalojar en el medio. */
/* Comparacion palabra a palabra. Las dos structs se escriben enteras (memset y
 * luego todos los campos), asi que el padding tambien coincide y no hay falsos
 * distintos. Va a mano porque este libc no trae memcmp.
 *
 * De a uint64_t y no de a byte porque desde que la entrada transporta el icono
 * la lista pasa de un KiB a mas de diez, y esto corre en CADA vuelta del bucle
 * del WM. La struct se alinea a 8 (tiene un uint64_t) y su tamano es multiplo
 * de 8, asi que el recorrido es exacto; una la escribio el WM en la seccion y
 * la otra es estatica, las dos con la alineacion que pide el tipo.
 */
_Static_assert(
    sizeof(struct savanxp_wm_window_list) % sizeof(uint64_t) == 0,
    "la lista de ventanas se compara de a uint64_t: su tamano debe ser multiplo de 8");

static int window_lists_equal(
    const struct savanxp_wm_window_list *left,
    const struct savanxp_wm_window_list *right)
{
    const uint64_t *a = (const uint64_t *)left;
    const uint64_t *b = (const uint64_t *)right;
    size_t index;

    for (index = 0; index < sizeof(*left) / sizeof(uint64_t); ++index)
    {
        if (a[index] != b[index])
        {
            return 0;
        }
    }
    return 1;
}

/* El icono viaja copiado tal cual: los dos lados tienen que medir lo mismo o la
 * copia se sale del destino. */
_Static_assert(
    WINDOWD_PRESENTATION_ICON_EXTENT == SAVANXP_WM_WINDOW_ICON_EXTENT,
    "el icono de la presentacion y el de la entrada del shell deben coincidir");

static void publish_window_list(struct windowd_session *session)
{
    /* Estatico y no del stack: pasan los diez KiB -- cada entrada lleva su
     * icono -- y este WM ya tiene el habito de no dejar buffers grandes en el
     * stack. */
    static struct savanxp_wm_window_list staging;
    struct savanxp_wm_window_list *list;
    int index;
    int written = 0;

    if (session == 0 || session->desktop_windows[0].window_list == 0)
    {
        return;
    }
    list = session->desktop_windows[0].window_list;

    memset(&staging, 0, sizeof(staging));

    /*
     * Orden ESTABLE por slot, no por z-order.
     *
     * windowd_task_client enumera en z-order, que es lo que quieren el Task
     * List y Alt+Tab -- la ventana anterior primero. Pero una barra de tareas
     * necesita lo contrario: si los botones se reordenan al cambiar de ventana,
     * el click siguiente cae sobre otra cosa. Win95 los deja fijos y aca
     * tambien: el slot de un overlay no cambia mientras la ventana viva.
     */
    for (index = 0; index < WINDOWD_MAX_OVERLAY_CLIENTS && written < (int)SAVANXP_WM_MAX_WINDOWS; ++index)
    {
        const struct windowd_client *client = &session->overlay_clients[index];
        struct savanxp_wm_window_entry *entry;
        const char *title;
        size_t length;

        /* Un dialogo no es una tarea: su boton es el de su dueno, y el id (el
         * pid) seria el mismo. */
        if (client->pid <= 0 || client->owner_slot >= 0)
        {
            continue;
        }
        entry = &staging.windows[written];
        /* El id es el pid: estable mientras la ventana viva, y el WM lo puede
         * resolver de vuelta sin llevar un contador propio. NO se usa el indice
         * del arreglo porque se reordena con el z-order, y un click que llegara
         * con un indice viejo activaria otra ventana. */
        entry->window_id = (uint32_t)client->pid;
        entry->flags = SAVANXP_WM_WINDOW_FLAG_NONE;
        if (session->active_client_kind == WINDOWD_CLIENT_APP &&
            index == overlay_root_slot(session, session->active_overlay_slot))
        {
            entry->flags |= SAVANXP_WM_WINDOW_FLAG_ACTIVE;
        }
        if (client->minimized)
        {
            entry->flags |= SAVANXP_WM_WINDOW_FLAG_MINIMIZED;
        }
        if (client->not_responding)
        {
            entry->flags |= SAVANXP_WM_WINDOW_FLAG_NOT_RESPONDING;
        }
        /* Icono: van los pixeles propios del .sxe cuando el binario los trajo,
         * y el id del set horneado como ultimo recurso. La barra no puede
         * resolverlos sola -- el fallback generico para todo el mundo era
         * justamente lo que se veia antes de que la entrada los transporte. */
        entry->icon_id = client->presentation.fallback_icon_id;
        entry->icon_extent = 0;
        if (client->presentation.icon_extent == SAVANXP_WM_WINDOW_ICON_EXTENT)
        {
            entry->icon_extent = SAVANXP_WM_WINDOW_ICON_EXTENT;
            memcpy(
                entry->icon_pixels,
                client->presentation.icon_pixels,
                sizeof(entry->icon_pixels));
        }

        title = windowd_presentation_label(&client->presentation, client->path);
        length = title != 0 ? strlen(title) : 0;
        if (length >= sizeof(entry->title))
        {
            length = sizeof(entry->title) - 1;
        }
        memcpy(entry->title, title, length);
        entry->title[length] = '\0';
        ++written;
    }

    staging.count = (uint32_t)written;
    staging.version = SAVANXP_WM_SHELL_PROTOCOL_VERSION;

    /*
     * Se publica solo si algo cambio de verdad. Comparar es mas barato y mucho
     * mas confiable que marcar un flag en cada mutador: alcanza con olvidarse
     * UNO -- abrir, cerrar, activar, minimizar, restaurar, renombrar -- para que
     * la barra quede mostrando un estado viejo, y ese bug no se ve hasta que
     * alguien mira la pantalla. La secuencia se excluye de la comparacion
     * porque es justamente lo que cambia al publicar.
     */
    staging.sequence = list->sequence;
    if (window_lists_equal(&staging, list))
    {
        return;
    }

    /* Seqlock: impar mientras se escribe, par cuando quedo estable. */
    list->sequence += 1;
    memcpy(list->windows, staging.windows, sizeof(list->windows));
    list->count = staging.count;
    list->version = staging.version;
    list->sequence += 1;
}

/* Resolves a window id from the window list (a pid) to the slot of that
 * process's main window, or -1. */
static int resolve_window_id(const struct windowd_session *session, uint32_t window_id)
{
    int slot;

    if (session == 0 || window_id == 0)
    {
        return -1;
    }
    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        if (session->overlay_clients[slot].pid > 0 &&
            session->overlay_clients[slot].owner_slot < 0 &&
            (uint32_t)session->overlay_clients[slot].pid == window_id)
        {
            return slot;
        }
    }
    return -1;
}

/* The combinations windowd keeps for itself. A hotkey on one of them would
 * take Alt+Tab or Alt+F4 away from every application. */
static int hotkey_reserved(uint32_t key, uint32_t modifiers)
{
    if (key == SAVANXP_KEY_F11)
    {
        return 1;
    }
    if ((key == SAVANXP_KEY_TAB || key == SAVANXP_KEY_F4) && (modifiers & SAVANXP_KEY_MOD_ALT) != 0)
    {
        return 1;
    }
    return 0;
}

#define WINDOWD_HOTKEY_MODIFIERS (SAVANXP_KEY_MOD_SHIFT | SAVANXP_KEY_MOD_CTRL | SAVANXP_KEY_MOD_ALT)

static void register_hotkey(struct windowd_session *session, uint32_t key, uint32_t modifiers)
{
    int index;

    modifiers &= WINDOWD_HOTKEY_MODIFIERS;
    if (key == SAVANXP_KEY_NONE || hotkey_reserved(key, modifiers))
    {
        eprintf("windowd: hotkey %u/%u refused\n", (unsigned)key, (unsigned)modifiers);
        return;
    }
    for (index = 0; index < session->hotkey_count; ++index)
    {
        if (session->hotkeys[index].key == key && session->hotkeys[index].modifiers == modifiers)
        {
            return;
        }
    }
    if (session->hotkey_count >= WINDOWD_MAX_HOTKEYS)
    {
        eprintf("windowd: hotkey %u/%u refused, table full\n", (unsigned)key, (unsigned)modifiers);
        return;
    }
    session->hotkeys[session->hotkey_count].key = key;
    session->hotkeys[session->hotkey_count].modifiers = modifiers;
    session->hotkey_count += 1;
}

/* Delivers a registered hotkey to the desktop's main window instead of the
 * focused application. The KEY_UP of a key whose KEY_DOWN went there follows
 * it, so the application never sees half a key. Returns 1 if consumed. */
static int deliver_hotkey(
    struct windowd_session *session,
    const struct savanxp_input_event *key_event,
    struct windowd_dirty_rect *dirty)
{
    const struct windowd_client *desktop = &session->desktop_windows[0];
    int index;

    if (key_event->type == SAVANXP_INPUT_EVENT_KEY_UP &&
        session->hotkey_held_key != 0 && key_event->key == session->hotkey_held_key)
    {
        session->hotkey_held_key = 0;
        (void)route_key(desktop, key_event);
        return 1;
    }
    if (key_event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || desktop->pid <= 0)
    {
        return 0;
    }
    /* AltGr is Ctrl+Alt on many layouts, and AltGr plus a letter is a
     * character someone is typing, never a hotkey. */
    if ((key_event->modifiers & SAVANXP_KEY_MOD_ALT_GR) != 0)
    {
        return 0;
    }
    for (index = 0; index < session->hotkey_count; ++index)
    {
        if (session->hotkeys[index].key == key_event->key &&
            session->hotkeys[index].modifiers == (key_event->modifiers & WINDOWD_HOTKEY_MODIFIERS))
        {
            /* A hotkey hands control to the shell, whose windows are not drawn
             * over a fullscreen app (and whose coordinates are the native
             * mode's): leave fullscreen first, or the Start menu would open
             * where nobody can see it. */
            if (session->fullscreen_slot >= 0)
            {
                exit_overlay_fullscreen(session, dirty);
            }
            session->hotkey_held_key = key_event->key;
            (void)route_key(desktop, key_event);
            return 1;
        }
    }
    return 0;
}

static void service_shell_requests(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    struct savanxp_wm_shell_request request;
    int request_fd;

    if (session == 0 || session->desktop_windows[0].shell_request_read_fd < 0)
    {
        return;
    }
    request_fd = session->desktop_windows[0].shell_request_read_fd;

    while (savanxp_read(request_fd, &request, sizeof(request)) == (long)sizeof(request))
    {
        int slot;
        int previous_active_slot = session->active_overlay_slot;

        if (request.action == SAVANXP_WM_SHELL_REGISTER_HOTKEY)
        {
            register_hotkey(session, request.key, request.modifiers);
            continue;
        }
        slot = resolve_window_id(session, request.window_id);
        if (!overlay_slot_valid(slot))
        {
            continue;
        }
        if (request.action == SAVANXP_WM_SHELL_ACTIVATE)
        {
            if (session->overlay_clients[slot].minimized)
            {
                restore_overlay_client(session, dirty, slot);
            }
            raise_overlay(session, slot);
            windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
        }
        else if (request.action == SAVANXP_WM_SHELL_MINIMIZE && !session->overlay_clients[slot].minimized)
        {
            minimize_overlay_client(session, dirty, slot);
        }
        /* Activation moved to another window, whose caption changes colour.
         * The pointer path repaints that at the end of its event; a request
         * from the taskbar has no such end, so it is done here. */
        if (session->active_overlay_slot != previous_active_slot)
        {
            if (overlay_slot_valid(previous_active_slot))
            {
                windowd_dirty_rect_add_client(dirty, overlay_client_at_const(session, previous_active_slot));
            }
            if (overlay_slot_valid(session->active_overlay_slot))
            {
                windowd_dirty_rect_add_client(dirty, overlay_client_at_const(session, session->active_overlay_slot));
            }
        }
    }
}

static void destroy_overlay_client(struct windowd_session *session, int slot, int terminate_client)
{
    struct windowd_client *client = overlay_client_at(session, slot);
    int child;

    if (client == 0)
    {
        return;
    }

    /* Las ventanas con dueno se van antes que su dueno: son del mismo proceso y
     * no tienen sentido sin el. Nunca se matan (no son un proceso). */
    if (client->pid > 0 && client->owner_slot < 0)
    {
        for (child = 0; child < WINDOWD_MAX_OVERLAY_CLIENTS; ++child)
        {
            if (child != slot && session->overlay_clients[child].pid > 0 &&
                session->overlay_clients[child].owner_slot == slot)
            {
                destroy_client_instance(&session->overlay_clients[child], 0);
                remove_overlay_from_order(session, child);
            }
        }
    }

    if (session->fullscreen_slot == slot)
    {
        /* The fullscreen client is going away: clear shell policy state so the
         * next composed frame restores normal desktop chrome. */
        session->fullscreen_slot = -1;
        /* Y devolver el modo de video, que es del scanout y no del cliente: si
         * no, matar una app a pantalla completa (End Task, o que se caiga sola)
         * dejaria el escritorio en la resolucion baja para siempre. */
        (void)apply_display_mode(
            session,
            session->compositor.requested_info.width,
            session->compositor.requested_info.height);
    }

    destroy_client_instance(client, terminate_client);
    remove_overlay_from_order(session, slot);
    refresh_active_state(session);
}

/*
 * Cerrar una ventana, por la X de su marco o por Alt+F4: los dos caminos son el
 * mismo gesto y tienen que hacer lo mismo.
 *
 * - Una ventana principal se destruye con su proceso, como siempre hizo la X.
 * - Un dialogo (ventana con dueno) no se destruye: se le pide al proceso que lo
 *   cierre, igual que ESC. El proceso decide que significa cancelar y despues
 *   manda el CLOSE (docs/desktop/OWNED_WINDOWS.md).
 *
 * Devuelve 1 si habia una ventana que cerrar.
 */
static int close_overlay_window(struct windowd_session *session, struct windowd_dirty_rect *dirty, int slot)
{
    struct windowd_client *client = overlay_client_at(session, slot);

    if (client == 0 || client->pid <= 0)
    {
        return 0;
    }
    if (overlay_client_owned(client))
    {
        if (client->header != 0)
        {
            client->header->flags |= SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN;
        }
        if (client->wake_event_fd >= 0)
        {
            (void)event_set(client->wake_event_fd);
        }
        return 1;
    }

    /* A main window is asked, like a dialog: the window goes when the process
     * ends (reap_dead_clients), and a process that does not end is shown as
     * not responding (service_close_requests). A fullscreen one first gives
     * the video mode back, so nothing waits behind a low resolution. */
    if (client->fullscreen)
    {
        exit_overlay_fullscreen(session, dirty);
    }
    if (client->header != 0)
    {
        client->header->flags |= SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN;
    }
    if (client->wake_event_fd >= 0)
    {
        (void)event_set(client->wake_event_fd);
    }
    if (client->close_requested_ms == 0 || client->not_responding)
    {
        /* Asking again after "not responding" starts the wait over: the shell
         * offers to end it again if it still does not close. */
        if (client->not_responding)
        {
            client->not_responding = 0;
            windowd_dirty_rect_add_client(dirty, client);
        }
        /* 0 means "not asked"; never a time in the future, which the
         * unsigned difference in service_close_requests would read as a wait
         * already over. */
        client->close_requested_ms = uptime_ms();
        if (client->close_requested_ms == 0)
        {
            client->close_requested_ms = 1;
        }
    }
    return 1;
}

/* Windows asked to close that keep running become "not responding": their
 * caption says so and the window list tells the shell, which offers to end
 * the process. */
static void service_close_requests(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    unsigned long now = uptime_ms();
    int slot;

    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        struct windowd_client *client = &session->overlay_clients[slot];

        if (client->pid <= 0 || client->owner_slot >= 0 || client->close_requested_ms == 0 ||
            client->not_responding || now - client->close_requested_ms < session->hung_close_ms)
        {
            continue;
        }
        client->not_responding = 1;
        windowd_dirty_rect_add_client(dirty, client);
        eprintf("windowd: %s (pid %ld) is not responding\n",
            client->path[0] != '\0' ? client->path : "?", client->pid);
    }
}

/* Starts a program the session needs and holds a window slot for it until it
 * connects (admit_connection). Returns the slot, or -1 with nothing held.
 * Programs anybody else starts need no slot held: they take a free one when
 * they connect. */
static int launch_overlay_client(struct windowd_session *session, const char *path, const char *argument)
{
    struct windowd_client *client = 0;
    int slot = -1;
    long pid = 0;

    if (session == 0 || path == 0)
    {
        return -1;
    }

    slot = find_free_overlay_slot(session);
    if (!overlay_slot_valid(slot))
    {
        eprintf("windowd: no free window slots for %s\n", path);
        return -1;
    }

    client = &session->overlay_clients[slot];
    reset_client(client);
    pid = windowd_spawn(path, argument);
    if (pid < 0)
    {
        eprintf("windowd: cannot start %s (%s)\n", path, result_error_string(pid));
        return -1;
    }
    client->reserved_pid = pid;
    copy_client_path(client, path);
    return slot;
}

/* The parent of `pid` in the process table, or 0. */
static long process_parent(long pid)
{
    struct sxn_process_query query;

    return sxn_process_query(pid, &query) == 0 ? (long)query.parent_pid : 0;
}

/*
 * Who may be the shell (docs/desktop/DESKTOP_ARCHITECTURE.md#who-is-the-shell-
 * decided-by-parentage): a process the session manager started -- its parent
 * is windowd's own parent -- or one windowd started itself (its selftests, and
 * the desktop until sessiond exists). The pid comes from the port, which the
 * client cannot forge. A program the desktop launches has the desktop as its
 * parent, and an orphan is reparented to init, so neither passes.
 */
static int started_by_session(long pid)
{
    long self = savanxp_getpid();
    long parent = process_parent(pid);

    return parent > 0 && (parent == self || parent == process_parent(self));
}

/* 1 if `pid` already has a main window: a process gets one connection. */
static int process_has_window(const struct windowd_session *session, long pid)
{
    int slot;

    if (session->desktop_windows[0].pid == pid)
    {
        return 1;
    }
    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        if (session->overlay_clients[slot].pid == pid && session->overlay_clients[slot].owner_slot < 0)
        {
            return 1;
        }
    }
    return 0;
}

/*
 * The hello arrived: the connection becomes a window. The desktop's process
 * gets the background and the shell role; a process windowd started gets the
 * slot held for it; anybody else, a free slot. The surface is sized now, from
 * the display as it is and from the launch flags the program declares about
 * itself (a low-resolution fullscreen program).
 */
static void admit_connection(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    struct windowd_pending_connection *connection,
    const struct savanxp_wm_hello *hello)
{
    struct windowd_client *client = 0;
    int shell_role = 0;
    int slot = -1;

    if (hello->magic != SAVANXP_WM_HELLO_MAGIC || hello->version != SAVANXP_WM_PROTOCOL_VERSION)
    {
        eprintf("windowd: pid %ld speaks another WM protocol (%u)\n", connection->pid, (unsigned)hello->version);
        refuse_connection(connection, -SAVANXP_EINVAL);
        return;
    }
    if (connection->pid <= 0 || process_has_window(session, connection->pid))
    {
        refuse_connection(connection, -SAVANXP_EBUSY);
        return;
    }

    if ((hello->flags & SAVANXP_WM_HELLO_SHELL) != 0u)
    {
        long reserved = session->desktop_windows[0].reserved_pid;

        /* One shell at a time, and only one the session started. A desktop
         * windowd spawned and is still waiting for holds the place too. */
        if (!started_by_session(connection->pid) || session->desktop_windows[0].pid > 0 ||
            (reserved > 0 && reserved != connection->pid))
        {
            eprintf("windowd: pid %ld may not be the shell\n", connection->pid);
            refuse_connection(connection, -SAVANXP_EACCES);
            return;
        }
        client = &session->desktop_windows[0];
        client->reserved_pid = 0;
        fill_client_surface_info(session, 1, &client->surface_info);
        position_client_window(session, client, 1, 0);
        client->role = WINDOWD_ROLE_BACKGROUND;
        shell_role = 1;
    }
    else
    {
        for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
        {
            if (session->overlay_clients[slot].reserved_pid == connection->pid)
            {
                break;
            }
        }
        if (slot == WINDOWD_MAX_OVERLAY_CLIENTS)
        {
            slot = find_free_overlay_slot(session);
            if (!overlay_slot_valid(slot))
            {
                eprintf("windowd: no free window slots for pid %ld\n", connection->pid);
                refuse_connection(connection, -SAVANXP_EBUSY);
                return;
            }
            reset_client(&session->overlay_clients[slot]);
            client_path_from_process(&session->overlay_clients[slot], connection->pid);
        }
        client = &session->overlay_clients[slot];
        client->reserved_pid = 0;
        fill_client_surface_info(session, 0, &client->surface_info);
        if ((hello->launch_flags & SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN) != 0u)
        {
            /* Allocate the surface at the low fullscreen render size. The same
             * buffer is used windowed and fullscreen; fullscreen scales it in
             * the shell composition pass. */
            client->fullscreen_capable = 1;
            client->surface_info.width = WINDOWD_FULLSCREEN_MODE_WIDTH;
            client->surface_info.height = WINDOWD_FULLSCREEN_MODE_HEIGHT;
            client->surface_info.pitch = WINDOWD_FULLSCREEN_MODE_WIDTH * (uint32_t)sizeof(uint32_t);
            client->surface_info.buffer_size = client->surface_info.pitch * WINDOWD_FULLSCREEN_MODE_HEIGHT;
        }
        client->cascade_index = session->overlay_count;
        position_client_window(session, client, 0, client->cascade_index);
    }

    if (connect_client(client, connection, session->submit_event_fd, shell_role,
            (hello->flags & SAVANXP_WM_HELLO_INPUT_RING) != 0u,
            (hello->flags & SAVANXP_WM_HELLO_REQUEST_EVENT) != 0u ? session->request_event_fd : -1) < 0)
    {
        eprintf("windowd: cannot set up the window of pid %ld\n", connection->pid);
        refuse_connection(connection, -SAVANXP_ENOMEM);
        if (shell_role)
        {
            /* Keep watching it: when it exits, the desktop is restarted. */
            session->desktop_windows[0].reserved_pid = connection->pid;
        }
        return;
    }

    /* The file name until the window publishes who it is: windowd does not
     * open the binary for that (service_client_presentation). */
    windowd_presentation_default(&client->presentation, client->path);
    client->pid = connection->pid;
    close_pending_connection(connection);
    if (!shell_role)
    {
        append_overlay_to_order(session, slot);
        raise_overlay(session, slot);
    }
    windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
}

/*
 * Connections (protocol v5, savanxp/wm_protocol.h): take what the port holds
 * while there is room to park it, then read the hellos that arrived. Nothing
 * here blocks -- the port and the pipes are only read when ready -- and a
 * client that does not say hello in time is dropped.
 */
#define WINDOWD_HELLO_TIMEOUT_MS 2000ul

static void service_connections(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    unsigned long now = uptime_ms();
    int index;

    if (session == 0 || session->port_fd < 0)
    {
        return;
    }

    for (;;)
    {
        struct sxn_port_connection accepted;
        struct windowd_pending_connection *connection = 0;

        for (index = 0; index < WINDOWD_MAX_PENDING_CONNECTIONS; ++index)
        {
            if (session->pending[index].read_fd < 0)
            {
                connection = &session->pending[index];
                break;
            }
        }
        /* Full: the rest wait in the port's own backlog. */
        if (connection == 0)
        {
            break;
        }
        memset(&accepted, 0, sizeof(accepted));
        if (sxn_port_accept(session->port_fd, &accepted) < 0)
        {
            break;
        }
        connection->pid = (long)accepted.client_pid;
        connection->read_fd = accepted.read_handle;
        connection->write_fd = accepted.write_handle;
        connection->deadline_ms = now + WINDOWD_HELLO_TIMEOUT_MS;
        (void)savanxp_fcntl(connection->read_fd, SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK);
    }

    for (index = 0; index < WINDOWD_MAX_PENDING_CONNECTIONS; ++index)
    {
        struct windowd_pending_connection *connection = &session->pending[index];
        struct savanxp_wm_hello hello;
        struct savanxp_pollfd pollfd;
        long got = 0;

        if (connection->read_fd < 0)
        {
            continue;
        }
        pollfd.fd = connection->read_fd;
        pollfd.events = SAVANXP_POLLIN | SAVANXP_POLLHUP;
        pollfd.revents = 0;
        if (savanxp_poll(&pollfd, 1, 0) <= 0 || (pollfd.revents & SAVANXP_POLLIN) == 0)
        {
            /* Hung up before saying anything, or out of time. */
            if ((pollfd.revents & SAVANXP_POLLHUP) != 0 || now >= connection->deadline_ms)
            {
                close_pending_connection(connection);
            }
            continue;
        }
        got = savanxp_read(connection->read_fd, &hello, sizeof(hello));
        if (got != (long)sizeof(hello))
        {
            /* A hello is written whole: anything else is not this protocol. */
            refuse_connection(connection, -SAVANXP_EINVAL);
            continue;
        }
        admit_connection(session, dirty, connection, &hello);
    }
}

/* `own_daemon`: start a compositord of its own (the test modes); otherwise
 * connect to the session's. */
static int open_compositor_session(struct windowd_session *session, int own_daemon)
{
    int result = 0;
    int slot;

    memset(session, 0, sizeof(*session));
    windowd_compositor_connection_init(&session->compositor);
    session->input_fd = -1;
    session->mouse_fd = -1;
    session->submit_event_fd = -1;
    session->request_event_fd = -1;
    session->port_fd = -1;
    for (slot = 0; slot < WINDOWD_MAX_PENDING_CONNECTIONS; ++slot)
    {
        session->pending[slot].read_fd = -1;
        session->pending[slot].write_fd = -1;
    }
    session->active_client_kind = WINDOWD_CLIENT_NONE;
    session->active_overlay_slot = -1;
    session->fullscreen_slot = -1;
    session->overlay_count = 0;
    session->hung_close_ms = WINDOWD_HUNG_CLOSE_MS;
    /* -1, not 0: memset would leave slot 0 "being resized". */
    session->resize_slot = -1;
    session->resize_edges = WINDOWD_RESIZE_EDGE_NONE;
    for (slot = 0; slot < WINDOWD_DESKTOP_WINDOW_COUNT; ++slot)
    {
        reset_client(&session->desktop_windows[slot]);
    }
    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        reset_client(&session->overlay_clients[slot]);
        session->overlay_order[slot] = -1;
    }

    result = windowd_compositor_open(&session->compositor, own_daemon);
    if (result < 0)
    {
        return windowd_stage_failed("open compositord", result);
    }

    session->gfx.info = session->compositor.display_info;
    session->input_fd = (int)savanxp_open_mode("/dev/input0", SAVANXP_OPEN_READ);
    if (session->input_fd < 0)
    {
        return windowd_stage_failed("open /dev/input0", session->input_fd);
    }

    session->mouse_fd = (int)savanxp_open_mode("/dev/mouse0", SAVANXP_OPEN_READ);
    if (session->mouse_fd < 0)
    {
        eprintf("desktop: /dev/mouse0 unavailable (%s), continuing keyboard-only\n", result_error_string(session->mouse_fd));
        session->mouse_fd = -1;
    }

    /* The submit event every client gets with its welcome is compositord's:
     * a frame wakes compositord, not windowd. windowd only hands it out. */
    session->submit_event_fd = session->compositor.frame_event_fd;
    session->compositor.frame_event_fd = -1;
    /* windowd's own, for clients that set it after a request in their header
     * (SAVANXP_WM_HELLO_REQUEST_EVENT); the loop resets it before looking at
     * all of them. */
    session->request_event_fd = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    if (session->request_event_fd < 0)
    {
        return windowd_stage_failed("create request event", session->request_event_fd);
    }

    /* Where programs connect to get a window. It must exist before the first
     * launch; a second windowd finds the name taken. */
    session->port_fd = (int)sxn_port_create(SAVANXP_WM_PORT_NAME);
    if (session->port_fd < 0)
    {
        return windowd_stage_failed("create the WM port", session->port_fd);
    }

    windowd_set_wm_surface((uint32_t *)session->compositor.wm_view, session->compositor.wm_pitch);
    g_session = session;
    refresh_active_state(session);
    return 0;
}

static void close_compositor_session(struct windowd_session *session)
{
    int slot;

    if (session == 0)
    {
        return;
    }

    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        destroy_overlay_client(session, slot, 1);
    }
    destroy_desktop(session, 1);
    for (slot = 0; slot < WINDOWD_MAX_PENDING_CONNECTIONS; ++slot)
    {
        close_pending_connection(&session->pending[slot]);
    }
    /* Connections still in the port's backlog go with it. */
    close_fd_if_needed(&session->port_fd);
    close_fd_if_needed(&session->input_fd);
    close_fd_if_needed(&session->mouse_fd);
    close_fd_if_needed(&session->submit_event_fd);
    close_fd_if_needed(&session->request_event_fd);
    windowd_compositor_close(&session->compositor);
    windowd_set_wm_surface(0, 0);
    g_session = 0;
}


static int reap_dead_clients(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    int slot;

    if (session == 0 || dirty == 0)
    {
        return 0;
    }

    if ((session->desktop_windows[0].pid > 0 && !windowd_process_alive(session->desktop_windows[0].pid)) ||
        (session->desktop_windows[0].reserved_pid > 0 && !windowd_process_alive(session->desktop_windows[0].reserved_pid)))
    {
        /* A solid background until the session manager's new desktop
         * connects. */
        destroy_desktop(session, 0);
        windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
    }

    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        struct windowd_client *client = &session->overlay_clients[slot];
        if (client->pid > 0 && client->owner_slot < 0 && !windowd_process_alive(client->pid))
        {
            destroy_overlay_client(session, slot, 0);
            windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
        }
        else if (client->reserved_pid > 0 && !windowd_process_alive(client->reserved_pid))
        {
            /* It died before connecting: there is no window to take down. */
            destroy_client_instance(client, 0);
        }
    }

    return 0;
}

/* Adopts the presentation the window published in its header (title, icon,
 * accent, style), under the same seqlock reading as the size hint: an odd
 * sequence is half written, and one that changed while copying is read again
 * next pass, never retried here. */
static void service_client_presentation(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    struct windowd_client *client)
{
    static struct savanxp_wm_presentation copy;
    uint32_t sequence = 0;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0 || client->header == 0)
    {
        return;
    }
    sequence = client->header->requests.presentation.sequence;
    if ((sequence & 1u) != 0 || sequence == client->presentation_consumed_sequence)
    {
        return;
    }
    memcpy(&copy, &client->header->requests.presentation, sizeof(copy));
    if (client->header->requests.presentation.sequence != sequence)
    {
        return;
    }
    client->presentation_consumed_sequence = sequence;
    if (windowd_presentation_adopt(&client->presentation, &copy))
    {
        /* The caption shows title, icon and accent. */
        windowd_dirty_rect_add_client(dirty, client);
    }
}

/* Picks up the cursor shape an app requests for its own widgets (e.g. the
 * pointer entering a textfield) from the surface header. Just caches the latest
 * valid value on the client; resolve_cursor_shape() decides whether it is
 * actually shown, and only while this exact client is the one under the
 * pointer. No repaint here -- the shape-resolution pass on the next mouse
 * event picks it up. */
static void service_client_cursor_hints(struct windowd_client *client)
{
    uint32_t shape = 0;

    if (client == 0 || client->pid <= 0 || client->header == 0)
    {
        return;
    }

    shape = client->header->requests.cursor_shape;
    if (shape < (uint32_t)SAVANXP_CURSOR_SHAPE_COUNT)
    {
        client->last_cursor_hint_shape = (int)shape;
    }
}

/* Recoloca la ventana con el mismo criterio del launch (centrada + cascada)
 * despues de cambiarle el tamano: resize_overlay_client_surface ancla el
 * origen, asi que una ventana que encoge queda descentrada. */
static void reposition_overlay_client_window(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    struct windowd_client *client)
{
    struct sx_rect previous;
    struct sx_rect current;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    if (session == 0 || dirty == 0 || client == 0 || !client->frame_visible)
    {
        return;
    }

    windowd_place_overlay_window(layout_display(session), &client->surface_info, client->cascade_index, &x, &y, &width, &height);
    if (x == client->window_x && y == client->window_y)
    {
        return;
    }

    previous = windowd_client_frame_rect(client);
    client->window_x = x;
    client->window_y = y;
    current = windowd_client_frame_rect(client);
    windowd_dirty_rect_add(dirty, &session->gfx.info, previous.x, previous.y, previous.width, previous.height);
    windowd_dirty_rect_add(dirty, &session->gfx.info, current.x, current.y, current.width, current.height);
}

/* Drena los size hints: el tamano que el programa dice necesitar para su
 * contenido (savanxp/wm_protocol.h). El WM lo obedece UNA vez, apenas arranca
 * la app, y solo si la ventana sigue en la geometria del launch -- despues
 * manda el usuario, y una app que insistiera no podria pelearle el resize.
 * resize_overlay_client_surface ya recorta a la capacidad de la superficie.
 *
 * LA EXCEPCION es la ventana de tamano fijo: ahi el usuario no tiene ninguna
 * geometria que defender -- no puede redimensionarla ni maximizarla --, asi que
 * la razon de la regla no aplica y el hint se atiende siempre. Es lo que hace
 * que el buscaminas cambie de nivel y la ventana lo siga en vez de quedarse
 * grande con el tablero chico adentro.
 *
 * slot < 0 = consumir y descartar: el header es el mismo para todos, asi que
 * el shell y el cliente de fondo tambien pueden pedir un tamano, pero son
 * full-screen por definicion. */
/*
 * La regla de arriba, aislada para poder asertarla: la ventana tiene que estar
 * en estado de aceptar un tamano -- con marco, ni maximizada ni a pantalla
 * completa -- y ademas ser el PRIMER hint, salvo que sea de tamano fijo, donde
 * no hay geometria de usuario que defender y valen todos.
 */
static int client_accepts_size_hint(const struct windowd_client *client)
{
    if (client == 0 || !client->frame_visible || client->maximized || client->fullscreen)
    {
        return 0;
    }
    return !client->size_hint_applied || windowd_client_fixed_size(client);
}

static void service_client_size_hints(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    struct windowd_client *client,
    int slot)
{
    uint32_t sequence = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    if (client == 0 || client->pid <= 0 || client->header == 0)
    {
        return;
    }

    /* Seqlock del lado lector: una secuencia impar es un pedido a medio
     * escribir, y si cambio mientras se copiaba el par hay que volver a leer en
     * la proxima vuelta. Nunca se reintenta aca: un cliente que se quedara en
     * impar no puede trabar el loop del WM. */
    sequence = client->header->requests.size_hint_sequence;
    if ((sequence & 1u) != 0 || sequence == client->size_hint_consumed_sequence)
    {
        return;
    }
    width = client->header->requests.size_hint_width;
    height = client->header->requests.size_hint_height;
    if (client->header->requests.size_hint_sequence != sequence)
    {
        return;
    }
    client->size_hint_consumed_sequence = sequence;

    if (width == 0 || height == 0)
    {
        eprintf("desktop: invalid size hint from %s\n", client->path[0] != '\0' ? client->path : "?");
        return;
    }
    if (session == 0 || dirty == 0 || !overlay_slot_valid(slot) ||
        !client_accepts_size_hint(client))
    {
        return;
    }
    {
        /* Recolocar en la cascada solo en el PRIMER hint: ahi la ventana
         * sigue donde la puso el launch y centrarla con el tamano nuevo es
         * lo correcto. En los siguientes ya la movio el usuario, y volver
         * a la posicion de cascada seria teletransportarsela bajo el
         * cursor. resize_overlay_client_surface deja el origen quieto y
         * reencuadra sola si el tamano nuevo no entra. */
        int first_hint = !client->size_hint_applied;

        client->size_hint_applied = 1;
        resize_overlay_client_surface(session, dirty, slot, (int)width, (int)height);
        if (first_hint)
        {
            reposition_overlay_client_window(session, dirty, client);
        }
    }
}

/*
 * Crea la ventana con dueno que pide el cliente del slot `owner_slot`
 * (docs/desktop/OWNED_WINDOWS.md). Todo lo que cuesta sale de lo que ya existe: un slot
 * de overlay libre, una seccion para la superficie -- que viaja al cliente por
 * la cola de handles de SU pipe de eventos -- y duplicados del pipe y del wake
 * del dueno. Ningun pipe nuevo, ningun proceso nuevo.
 *
 * Devuelve 0 con el handle ya encolado, o -errno sin dejar nada.
 */
static int open_owned_window(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    int owner_slot,
    const struct savanxp_wm_window_request *request)
{
    struct windowd_client *owner = overlay_client_at(session, owner_slot);
    struct windowd_client *client = 0;
    struct sx_rect owner_frame;
    int slot = -1;
    int section_fd = -1;
    long result = 0;
    size_t length = 0;

    if (owner == 0 || owner->pid <= 0 || owner->events_write_fd < 0 || owner->wake_event_fd < 0)
    {
        return -SAVANXP_EINVAL;
    }
    if (request->window_id == 0 || request->window_id > SAVANXP_WM_MAX_OWNED_WINDOWS ||
        owned_window_slot(session, owner_slot, request->window_id) >= 0)
    {
        return -SAVANXP_EINVAL;
    }
    /* Un dialogo que no entra en la pantalla con su marco no se puede usar;
     * mejor que el cliente lo dibuje adentro de su ventana como antes. */
    if (request->width == 0 || request->height == 0 ||
        request->width + (uint32_t)(WINDOWD_WINDOW_BORDER * 2) > layout_display(session)->width ||
        request->height + (uint32_t)(WINDOWD_WINDOW_TITLEBAR_HEIGHT + WINDOWD_WINDOW_BORDER) > layout_display(session)->height)
    {
        return -SAVANXP_EINVAL;
    }
    slot = find_free_overlay_slot(session);
    if (!overlay_slot_valid(slot))
    {
        return -SAVANXP_ENOSPC;
    }

    /* A pantalla completa solo se compone la app: su dialogo quedaria activo e
     * invisible, y la app, bloqueada. */
    if (session->fullscreen_slot == owner_slot)
    {
        exit_overlay_fullscreen(session, dirty);
    }

    client = &session->overlay_clients[slot];
    reset_client(client);
    client->surface_info.width = request->width;
    client->surface_info.height = request->height;
    client->surface_info.pitch = request->width * (uint32_t)sizeof(uint32_t);
    client->surface_info.bpp = 32u;
    client->surface_info.buffer_size = client->surface_info.pitch * request->height;

    section_fd = create_client_surface(client);
    if (section_fd < 0)
    {
        reset_client(client);
        return -SAVANXP_ENOMEM;
    }
    client->events_write_fd = (int)savanxp_dup(owner->events_write_fd);
    client->wake_event_fd = (int)savanxp_dup(owner->wake_event_fd);
    result = (client->events_write_fd < 0 || client->wake_event_fd < 0)
        ? -SAVANXP_EBADF
        : pipe_send_handle(owner->events_write_fd, section_fd);
    if (result >= 0 && attach_client_surface(client, section_fd, client->wake_event_fd) < 0)
    {
        result = -SAVANXP_EIO;
    }
    close_fd_if_needed(&section_fd);
    if (result < 0)
    {
        destroy_client_instance(client, 0);
        return (int)result;
    }

    client->pid = owner->pid;
    client->owner_slot = owner_slot;
    client->window_id = request->window_id;
    memcpy(client->path, owner->path, sizeof(client->path));
    /* Presentacion del dueno (accent, icono) con el titulo del dialogo, y de
     * tamano fijo: el tamano lo decide el layout del dialogo. */
    client->presentation = owner->presentation;
    length = strlen(request->title);
    if (length >= sizeof(client->presentation.label))
    {
        length = sizeof(client->presentation.label) - 1u;
    }
    memcpy(client->presentation.label, request->title, length);
    client->presentation.label[length] = '\0';
    client->presentation.window_flags |= SAVANXP_WM_WINDOW_STYLE_FIXED_SIZE;

    /* Centrado sobre el dueno y adentro de la pantalla. */
    client->window_width = (int)request->width + (WINDOWD_WINDOW_BORDER * 2);
    client->window_height = (int)request->height + WINDOWD_WINDOW_TITLEBAR_HEIGHT + WINDOWD_WINDOW_BORDER;
    owner_frame = windowd_client_frame_rect(owner);
    client->window_x = owner_frame.x + (owner_frame.width - client->window_width) / 2;
    client->window_y = owner_frame.y + (owner_frame.height - client->window_height) / 2;
    windowd_clamp_overlay_frame_position(
        layout_display(session), client->window_width, client->window_height, &client->window_x, &client->window_y);
    client->restore_window_x = client->window_x;
    client->restore_window_y = client->window_y;
    client->restore_window_width = client->window_width;
    client->restore_window_height = client->window_height;
    client->frame_visible = 1;
    client->cascade_index = owner->cascade_index;
    /* Nunca pide tamano: el que tiene es el que pidio. */
    client->size_hint_applied = 1;

    raise_overlay(session, slot);
    windowd_dirty_rect_add_client(dirty, owner);
    windowd_dirty_rect_add_client(dirty, client);
    return 0;
}

static void close_owned_window(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    int owner_slot,
    uint32_t window_id)
{
    int slot = owned_window_slot(session, owner_slot, window_id);
    int was_active = 0;
    struct sx_rect frame;

    if (window_id == 0 || !overlay_slot_valid(slot))
    {
        return;
    }
    was_active = session->active_client_kind == WINDOWD_CLIENT_APP && session->active_overlay_slot == slot;
    frame = windowd_client_frame_rect(&session->overlay_clients[slot]);
    destroy_overlay_client(session, slot, 0);
    windowd_dirty_rect_add(dirty, &session->gfx.info, frame.x, frame.y, frame.width, frame.height);
    /* El foco vuelve al dueno, salvo que el usuario ya estuviera en otra app. */
    if (was_active)
    {
        raise_overlay(session, owner_slot);
    }
    windowd_dirty_rect_add_client(dirty, overlay_client_at_const(session, owner_slot));
}

/*
 * Creates a role window of the desktop (docs/desktop/DESKTOP_ARCHITECTURE.md#
 * window-roles): a panel on a screen edge or a popup where the desktop says.
 * Same transport as an owned window -- a section queued on the process's event
 * pipe, duplicates of its pipe and wake event -- and no family rules: a role
 * window is never focused, never raised over the application windows by a
 * click, and stacks by role. The desktop's main window is the background.
 *
 * Returns 0 with the handle queued, or -errno leaving nothing behind.
 */
static int open_desktop_window(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    const struct savanxp_wm_window_request *request)
{
    struct windowd_client *owner = &session->desktop_windows[0];
    struct windowd_client *client = 0;
    const struct savanxp_fb_info *display = layout_display(session);
    int section_fd = -1;
    long result = 0;
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;

    if (owner->pid <= 0 || owner->events_write_fd < 0 || owner->wake_event_fd < 0 ||
        request->window_id == 0 || request->window_id >= (uint32_t)WINDOWD_DESKTOP_WINDOW_COUNT ||
        session->desktop_windows[request->window_id].pid > 0)
    {
        return -SAVANXP_EINVAL;
    }

    if (request->role == SAVANXP_WM_WINDOW_ROLE_PANEL)
    {
        /* Full width on its edge; the thickness is the client's, and it may
         * not take more than half of the screen. */
        if (request->height == 0 || request->height > display->height / 2u)
        {
            return -SAVANXP_EINVAL;
        }
        width = (int)display->width;
        height = (int)request->height;
        x = 0;
        y = (request->role_flags & SAVANXP_WM_PANEL_EDGE_TOP) != 0 ? 0 : (int)display->height - height;
    }
    else if (request->role == SAVANXP_WM_WINDOW_ROLE_POPUP)
    {
        if (request->width == 0 || request->height == 0 ||
            request->width > display->width || request->height > display->height)
        {
            return -SAVANXP_EINVAL;
        }
        width = (int)request->width;
        height = (int)request->height;
        x = windowd_clamp_int(request->x, 0, (int)display->width - width);
        y = windowd_clamp_int(request->y, 0, (int)display->height - height);
    }
    else
    {
        /* The desktop has no dialogs yet; a NORMAL window would need an owner
         * among the application windows. */
        return -SAVANXP_EINVAL;
    }

    client = &session->desktop_windows[request->window_id];
    reset_client(client);
    client->surface_info.width = (uint32_t)width;
    client->surface_info.height = (uint32_t)height;
    client->surface_info.pitch = (uint32_t)width * (uint32_t)sizeof(uint32_t);
    client->surface_info.bpp = 32u;
    client->surface_info.buffer_size = client->surface_info.pitch * (uint32_t)height;

    section_fd = create_client_surface(client);
    if (section_fd < 0)
    {
        reset_client(client);
        return -SAVANXP_ENOMEM;
    }
    client->events_write_fd = (int)savanxp_dup(owner->events_write_fd);
    client->wake_event_fd = (int)savanxp_dup(owner->wake_event_fd);
    result = (client->events_write_fd < 0 || client->wake_event_fd < 0)
        ? -SAVANXP_EBADF
        : pipe_send_handle(owner->events_write_fd, section_fd);
    if (result >= 0 && attach_client_surface(client, section_fd, client->wake_event_fd) < 0)
    {
        result = -SAVANXP_EIO;
    }
    close_fd_if_needed(&section_fd);
    if (result < 0)
    {
        destroy_client_instance(client, 0);
        return (int)result;
    }

    client->pid = owner->pid;
    client->window_id = request->window_id;
    client->role = request->role == SAVANXP_WM_WINDOW_ROLE_PANEL ? WINDOWD_ROLE_PANEL : WINDOWD_ROLE_POPUP;
    client->role_flags = request->role_flags;
    memcpy(client->path, owner->path, sizeof(client->path));
    client->presentation = owner->presentation;
    client->window_x = x;
    client->window_y = y;
    client->window_width = width;
    client->window_height = height;
    client->restore_window_x = x;
    client->restore_window_y = y;
    client->restore_window_width = width;
    client->restore_window_height = height;
    client->frame_visible = 0;
    client->size_hint_applied = 1;
    windowd_dirty_rect_add_client(dirty, client);
    return 0;
}

static void close_desktop_window(struct windowd_session *session, struct windowd_dirty_rect *dirty, uint32_t window_id)
{
    struct windowd_client *client = 0;

    if (window_id == 0 || window_id >= (uint32_t)WINDOWD_DESKTOP_WINDOW_COUNT)
    {
        return;
    }
    client = &session->desktop_windows[window_id];
    if (client->pid <= 0)
    {
        return;
    }
    windowd_dirty_rect_add_client(dirty, client);
    destroy_client_instance(client, 0);
}

/* A press at x,y asks every dismissable popup to close when it lands on
 * nothing, or on a window of another process. A press on another window of
 * the popup's own process is that process's to interpret: the toggle button
 * that opened the popup, for instance. The press itself goes on as usual. */
static void dismiss_popups_for_press(struct windowd_session *session, int x, int y)
{
    const struct windowd_client *hit = top_client_at_point(session, x, y);
    int index;

    for (index = 1; index < WINDOWD_DESKTOP_WINDOW_COUNT; ++index)
    {
        struct windowd_client *popup = &session->desktop_windows[index];

        if (popup->pid <= 0 || popup->role != WINDOWD_ROLE_POPUP || popup->dismiss_requested ||
            (popup->role_flags & SAVANXP_WM_POPUP_DISMISS_ON_OUTSIDE) == 0)
        {
            continue;
        }
        if (hit != 0 && hit->pid == popup->pid)
        {
            continue;
        }
        /* The flag first, then the wake: a client that resets the event and
         * looks at the header again cannot miss it. */
        if (popup->header != 0)
        {
            popup->header->flags |= SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN;
        }
        if (popup->wake_event_fd >= 0)
        {
            (void)event_set(popup->wake_event_fd);
        }
        popup->dismiss_requested = 1;
    }
}

/* Drains the window request in the header (savanxp_wm_client_requests.window).
 * Like everything in the header: copied, validated, and only then used. The
 * desktop's main window may open role windows; the main window of an
 * application may open owned (NORMAL) windows. Any other request is answered
 * with an error, which the runtime takes as "keep drawing the dialog inside". */
static void service_client_window_requests(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    struct windowd_client *client,
    int slot)
{
    struct savanxp_wm_window_request request;
    uint32_t sequence = 0;
    int status = 0;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0 || client->header == 0)
    {
        return;
    }

    sequence = client->header->requests.window_sequence;
    if ((sequence & 1u) != 0 || sequence == client->window_request_consumed_sequence)
    {
        return;
    }
    memcpy(&request, &client->header->requests.window, sizeof(request));
    if (client->header->requests.window_sequence != sequence)
    {
        return;
    }
    client->window_request_consumed_sequence = sequence;
    request.title[SAVANXP_WM_WINDOW_TITLE_CAPACITY - 1u] = '\0';

    if (client == &session->desktop_windows[0])
    {
        if (request.action == SAVANXP_WM_WINDOW_ACTION_OPEN)
        {
            status = open_desktop_window(session, dirty, &request);
            if (status < 0)
            {
                eprintf("windowd: desktop window %u (role %u) refused (%s)\n",
                    (unsigned)request.window_id, (unsigned)request.role, result_error_string(status));
            }
        }
        else if (request.action == SAVANXP_WM_WINDOW_ACTION_CLOSE)
        {
            close_desktop_window(session, dirty, request.window_id);
        }
        else
        {
            status = -SAVANXP_EINVAL;
        }
    }
    /* Roles other than NORMAL belong to the shell. */
    else if (!overlay_slot_valid(slot) || client->owner_slot >= 0 ||
        request.role != SAVANXP_WM_WINDOW_ROLE_NORMAL)
    {
        status = -SAVANXP_EINVAL;
    }
    else if (request.action == SAVANXP_WM_WINDOW_ACTION_OPEN)
    {
        status = open_owned_window(session, dirty, slot, &request);
        if (status < 0)
        {
            eprintf("desktop: owned window %u for %s refused (%s)\n",
                (unsigned)request.window_id,
                client->path[0] != '\0' ? client->path : "?",
                result_error_string(status));
        }
    }
    else if (request.action == SAVANXP_WM_WINDOW_ACTION_CLOSE)
    {
        close_owned_window(session, dirty, slot, request.window_id);
    }
    else
    {
        status = -SAVANXP_EINVAL;
    }

    /* Respuesta despues del handle: el cliente lo va a buscar apenas vea la
     * secuencia, asi que tiene que estar encolado antes. */
    client->header->requests.window_reply_status = status;
    client->header->requests.window_reply_sequence = sequence;
    if (client->wake_event_fd >= 0)
    {
        (void)event_set(client->wake_event_fd);
    }
}

/* One pass over the desktop process: frames and cursor hints of
 * every window it has, then what only its main window carries -- window
 * requests, shell requests -- and the window list it reads. */
static void service_desktop(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    int index;

    for (index = 0; index < WINDOWD_DESKTOP_WINDOW_COUNT; ++index)
    {
        struct windowd_client *client = &session->desktop_windows[index];

        if (client->pid <= 0)
        {
            continue;
        }
        service_client_start(dirty, client);
        service_client_cursor_hints(client);
        /* A role window's size is the desktop's to ask for when it opens
         * it, not a hint: consumed and dropped. */
        service_client_size_hints(session, dirty, client, -1);
    }
    service_client_window_requests(session, dirty, &session->desktop_windows[0], -1);
    service_shell_requests(session, dirty);
    publish_window_list(session);
}

/*
 * Headless compositor self-test driven by init under /SMOKE.
 *
 * It reuses the exact compose/present building blocks of the main loop, but
 * runs a bounded, scripted sequence instead of polling real input: it launches
 * a self-animating client (gfxdemo), exercises the window-management paths
 * (maximize/restore/move/minimize/restore) and validates that client present
 * batches flow all the way to GPU retire on the v3 timeline. Prints the token
 * the build runner watches for and returns non-zero on any failure.
 */
/* Instrumented reproduction of the "cursor leaves rectangular residue over
 * static compositor-drawn content" report, now against compositord's software
 * cursor: commits move the cursor ONTO a probe point, then OFF it with nothing
 * else damaged, and we read the composed display at the old position back. If
 * the compose restores it, display == clean baseline; any residue means
 * compositord did not repaint the old cursor footprint. */
static int windowd_cursor_repro(void)
{
    struct windowd_session session;
    struct windowd_dirty_rect dirty = {0};
    const struct savanxp_fb_info *info;
    uint32_t *fb;
    uint32_t stride;
    struct sx_rect dlg;
    int qx;
    int qy;
    int rx;
    int ry;
    int fx;
    int fy;
    int fw;
    int fh;
    int row;
    int col;
    int diff_after = 0;
    int diff_oncursor = 0;
    /* Cursor footprint is small (24x24-ish); cap the capture generously. */
    static uint32_t baseline_block[64 * 64];
    static uint32_t oncursor_block[64 * 64];

    if (open_compositor_session(&session, 1) < 0)
    {
        puts_fd(2, "CURSOR REPRO FAIL compositor startup\n");
        close_compositor_session(&session);
        return 1;
    }

    info = &session.gfx.info;
    fb = session.compositor.framebuffer;
    stride = info->pitch / 4u;

    /* Probe point Q: centro de la pantalla (antes era el centro del dialogo de
     * energia, que se retiro con el chrome; lo unico que importa es sondear un
     * punto interior estable). R: lo bastante lejos como para que su rastro de
     * cursor no se solape con la huella de Q. */
    dlg = sx_rect_make(
        ((int)info->width - 300) / 2,
        ((int)info->height - 132) / 2,
        300,
        132);
    qx = dlg.x + (dlg.width / 2);
    qy = dlg.y + (dlg.height / 2);

    /* Footprint of the cursor when its hotspot is at Q. */
    windowd_cursor_bounds(qx, qy, SAVANXP_CURSOR_ARROW, &fx, &fy, &fw, &fh);
    if (fw > 64)
    {
        fw = 64;
    }
    if (fh > 64)
    {
        fh = 64;
    }
    rx = qx + fw + 12;
    ry = qy;

    /* Frame A: dialog visible, cursor parked far away (top-left corner, off the
     * dialog). Full repaint so the backbuffer holds the clean image. */
    windowd_dirty_rect_add_fullscreen(&dirty, info);
    (void)windowd_commit(&session, 4, 4, &dirty, 1);
    for (row = 0; row < fh; ++row)
    {
        for (col = 0; col < fw; ++col)
        {
            baseline_block[(row * fw) + col] = fb[((size_t)(fy + row) * stride) + (size_t)(fx + col)];
        }
    }

    /* Frame B: move cursor ONTO Q, nothing else damaged. */
    (void)windowd_commit(&session, qx, qy, &dirty, 1);
    for (row = 0; row < fh; ++row)
    {
        for (col = 0; col < fw; ++col)
        {
            uint32_t px = fb[((size_t)(fy + row) * stride) + (size_t)(fx + col)];
            oncursor_block[(row * fw) + col] = px;
            if (px != baseline_block[(row * fw) + col])
            {
                ++diff_oncursor;
            }
        }
    }

    /* Frame C: move cursor OFF Q to R. The display at Q must return to the
     * clean baseline. */
    (void)windowd_commit(&session, rx, ry, &dirty, 1);
    {
        int min_col = fw;
        int min_row = fh;
        int max_col = -1;
        int max_row = -1;
        int sample_col = -1;
        int sample_row = -1;
        uint32_t sample_res = 0;
        uint32_t sample_base = 0;
        uint32_t sample_cursor = 0;
        for (row = 0; row < fh; ++row)
        {
            for (col = 0; col < fw; ++col)
            {
                uint32_t px = fb[((size_t)(fy + row) * stride) + (size_t)(fx + col)];
                if (px != baseline_block[(row * fw) + col])
                {
                    ++diff_after;
                    if (col < min_col) min_col = col;
                    if (col > max_col) max_col = col;
                    if (row < min_row) min_row = row;
                    if (row > max_row) max_row = row;
                    if (sample_col < 0)
                    {
                        sample_col = col;
                        sample_row = row;
                        sample_res = px;
                        sample_base = baseline_block[(row * fw) + col];
                        sample_cursor = oncursor_block[(row * fw) + col];
                    }
                }
            }
        }
        if (diff_after != 0)
        {
            printf("CURSOR REPRO residue bbox col[%d..%d] row[%d..%d] at(%d,%d)\n",
                min_col, max_col, min_row, max_row, sample_col, sample_row);
            printf("CURSOR REPRO sample base=%u oncursor=%u after=%u\n",
                sample_base, sample_cursor, sample_res);
        }
    }

    printf("CURSOR REPRO dlg=(%d,%d %dx%d) Q=(%d,%d) foot=(%d,%d %dx%d)\n",
        dlg.x, dlg.y, dlg.width, dlg.height, qx, qy, fx, fy, fw, fh);
    printf("CURSOR REPRO diff_oncursor=%d diff_after=%d\n", diff_oncursor, diff_after);
    close_compositor_session(&session);

    if (diff_oncursor == 0)
    {
        printf("CURSOR REPRO FAIL inconclusive: cursor never drew opaque pixels over Q\n");
        return 1;
    }
    if (diff_after != 0)
    {
        printf("CURSOR REPRO FAIL residue: %d px of old cursor footprint not repainted\n", diff_after);
        return 1;
    }
    printf("CURSOR REPRO PASS backbuffer restored, no residue\n");
    return 0;
}

/* La coalescencia es el unico punto del camino de la rueda donde perder ticks
 * no se nota: el cursor sigue andando, el scroll simplemente se queda corto y
 * parece "el mouse anda pesado". Por eso se asserta el total, que es la unica
 * propiedad que importa -- cuantos eventos salgan es decision del coalescer. */
static int windowd_wheel_coalesce_selftest(void)
{
    struct savanxp_mouse_event raw[6];
    struct savanxp_mouse_event coalesced[6];
    size_t count;
    size_t index;
    int total = 0;

    memset(raw, 0, sizeof(raw));
    memset(coalesced, 0, sizeof(coalesced));

    /* Una tanda de movimiento con rueda y sin cambio de botones: el caso que
     * el coalescer funde en un solo evento. */
    for (index = 0; index < 4u; ++index)
    {
        raw[index].delta_x = 1;
        raw[index].wheel = 1;
    }
    /* Y dos mas con el boton apretado, que fuerzan eventos aparte. */
    raw[4].wheel = -3;
    raw[4].buttons = SAVANXP_MOUSE_BUTTON_LEFT;
    raw[5].wheel = -1;
    raw[5].buttons = SAVANXP_MOUSE_BUTTON_LEFT;

    count = coalesce_mouse_events(raw, 6u, coalesced, 6u, 0);
    if (count == 0u || count > 6u)
    {
        return 1;
    }
    for (index = 0; index < count; ++index)
    {
        total += coalesced[index].wheel;
    }
    /* 4 * (+1) + (-3) + (-1) = 0 solo si no se perdio ni se duplico ninguno. */
    if (total != 0)
    {
        return 1;
    }

    /* Cuatro eventos de rueda pura, sin movimiento ni botones: no hay nada que
     * los distinga entre si, asi que es el peor caso para un coalescer que
     * pisara el campo en vez de sumarlo. */
    memset(raw, 0, sizeof(raw));
    memset(coalesced, 0, sizeof(coalesced));
    for (index = 0; index < 4u; ++index)
    {
        raw[index].wheel = 1;
    }
    count = coalesce_mouse_events(raw, 4u, coalesced, 6u, 0);
    total = 0;
    for (index = 0; index < count; ++index)
    {
        total += coalesced[index].wheel;
    }
    return total == 4 ? 0 : 1;
}

/* La posicion del puntero tiene que sobrevivir a la fusion, y de una sola
 * manera: la del ultimo evento, que es la que vio el host. Un coalescer que
 * sumara las posiciones inventaria un punto que nadie estuvo ahi, y uno que
 * conservara la primera dejaria el cursor corrido por la cantidad que el host
 * movio entre las dos. */
static int windowd_pointer_coalesce_selftest(void)
{
    struct savanxp_mouse_event raw[4];
    struct savanxp_mouse_event coalesced[4];
    size_t count;
    size_t index;
    int delta_total = 0;

    memset(raw, 0, sizeof(raw));
    memset(coalesced, 0, sizeof(coalesced));

    /* Cuatro muestras de un tablet que avanza 10 px por paso, con la rueda en
     * medio. Todas fundibles: mismo estado de botones, sin transicion. */
    for (index = 0; index < 4u; ++index)
    {
        raw[index].delta_x = 10;
        raw[index].absolute_x = 100 + (int)(index + 1u) * 10;
        raw[index].absolute_y = 500;
        raw[index].flags = SAVANXP_MOUSE_FLAG_ABSOLUTE;
    }
    raw[2].wheel = 3;

    count = coalesce_mouse_events(raw, 4u, coalesced, 4u, 0);
    if (count != 1u)
    {
        return 1;
    }
    if (coalesced[0].flags & SAVANXP_MOUSE_FLAG_ABSOLUTE)
    {
        if (coalesced[0].absolute_x != 140 || coalesced[0].absolute_y != 500)
        {
            return 1;
        }
    }
    for (index = 0; index < count; ++index)
    {
        delta_total += coalesced[index].delta_x;
    }
    if (delta_total != 40 || coalesced[0].wheel != 3)
    {
        return 1;
    }

    /* Al reves: un evento sin posicion (un PS/2, o la rueda antes del primer
     * EV_ABS) no puede dejar la posicion de otro en el fundido. Aplicarla seria
     * mover el cursor a un lugar del que nadie informo. */
    memset(raw, 0, sizeof(raw));
    memset(coalesced, 0, sizeof(coalesced));
    raw[0].delta_x = 5;
    raw[0].absolute_x = 700;
    raw[0].absolute_y = 400;
    raw[0].flags = SAVANXP_MOUSE_FLAG_ABSOLUTE;
    raw[1].delta_x = 5;

    count = coalesce_mouse_events(raw, 2u, coalesced, 2u, 0);
    if (count != 1u)
    {
        return 1;
    }
    return (coalesced[0].flags & SAVANXP_MOUSE_FLAG_ABSOLUTE) != 0u ? 1 : 0;
}

/* Descriptores que a windowd le tienen que sobrar con todas las ventanas
 * abiertas: el cliente de shell (la terminal), que todavia se puede abrir, mas
 * los fds transitorios de un launch y la lectura del .sxe. */
#define WINDOWD_SELFTEST_FD_HEADROOM 8
/* Una app recien arrancada tiene stdio, los cuatro del protocolo y lo que abra
 * su runtime. Si heredara la tabla de windowd serian decenas. */
#define WINDOWD_SELFTEST_APP_FD_MAX 20
#define WINDOWD_SELFTEST_START_DEADLINE_MS 15000ul
/* How far compositord has composed a client's frames: the header's
 * composed_sequence, which compositord writes. */
static uint64_t selftest_composed_sequence(const struct windowd_client *client)
{
    return client != 0 && client->header != 0 ? client->header->composed_sequence : 0u;
}

static int selftest_composed(const struct windowd_client *client)
{
    return selftest_composed_sequence(client) > 0;
}

/* Where the selftests park the cursor while they drive the session. */
#define WINDOWD_SELFTEST_CURSOR_X 40
#define WINDOWD_SELFTEST_CURSOR_Y 40

/* Launching is asynchronous since protocol v5: the slot is held until the
 * program connects. Waits for that, servicing connections like the loop does.
 * 0 once the window exists, -1 if the process died or ran out of time. */
static int selftest_wait_connected(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    const struct windowd_client *client)
{
    unsigned long deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;

    while (client->pid <= 0)
    {
        if (client->reserved_pid <= 0 || uptime_ms() >= deadline)
        {
            return -1;
        }
        service_connections(session, dirty);
        (void)reap_dead_clients(session, dirty);
        if (client->pid <= 0)
        {
            sleep_ms(5);
        }
    }
    return 0;
}

/* launch_overlay_client plus the wait: the slot of a window that exists, or -1. */
static int selftest_launch(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    const char *path,
    const char *argument)
{
    int slot = launch_overlay_client(session, path, argument);

    if (slot < 0 || selftest_wait_connected(session, dirty, &session->overlay_clients[slot]) < 0)
    {
        return -1;
    }
    return slot;
}

static int selftest_handle_count(long pid, uint32_t *handles)
{
    struct sxn_process_query query;

    if (sxn_process_query(pid, &query) != 0)
    {
        return -1;
    }
    *handles = query.handle_count;
    return 0;
}

/*
 * Session capacity (docs/desktop/WM_SUBSYSTEM.md, "Descriptor budget"): with the
 * desktop -- the client with the most channels -- showing its panel and a
 * popup, EVERY window slot must fill, windowd must have table to spare, and
 * every app must start with a clean one. With protocol v3 the session did not
 * get past half of the slots.
 */
static struct windowd_client *selftest_desktop_open_popup(struct windowd_session *session);

static int windowd_capacity_selftest(struct windowd_session *session)
{
    const char *filler_path = "/bin/aboutapp";
    unsigned long deadline = 0;
    uint32_t windowd_handles = 0;
    uint32_t handles_after_refusal = 0;
    uint32_t app_handles = 0;
    uint32_t compositor_handles = 0;
    long last_pid = -1;
    int windows = 0;
    int slot;
    struct windowd_dirty_rect dirty;

    memset(&dirty, 0, sizeof(dirty));
    /* The worst case the desktop takes: its background, its panel and a
     * popup, all open while the session fills up. */
    if (selftest_desktop_open_popup(session) == 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL capacity: the desktop did not open a popup\n");
        return 1;
    }

    while ((slot = find_free_overlay_slot(session)) >= 0)
    {
        if (launch_overlay_client(session, filler_path, 0) != slot)
        {
            printf("DESKTOP SMOKE FAIL capacity: the window of slot %d did not start\n", slot);
            return 1;
        }
        last_pid = session->overlay_clients[slot].reserved_pid;
    }

    /* Only once it submitted its first frame has the app connected and gone
     * through gfx_open: that is the table measured below. */
    deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
    for (;;)
    {
        int pending = 0;

        service_connections(session, &dirty);
        windowd_dirty_rect_reset(&dirty);
        windows = 0;
        for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
        {
            const struct windowd_client *client = &session->overlay_clients[slot];

            if (client->reserved_pid > 0)
            {
                ++pending;
                continue;
            }
            if (client->pid <= 0 || client->header == 0)
            {
                continue;
            }
            ++windows;
            if (client->header->submit_sequence == 0)
            {
                ++pending;
            }
        }
        if (pending == 0)
        {
            break;
        }
        if (uptime_ms() >= deadline)
        {
            printf("DESKTOP SMOKE FAIL capacidad: %d de %d ventanas nunca presentaron\n", pending, windows);
            return 1;
        }
        sleep_ms(20);
    }

    if (windows != WINDOWD_MAX_OVERLAY_CLIENTS)
    {
        printf("DESKTOP SMOKE FAIL capacidad: %d ventanas vivas de %d\n", windows, WINDOWD_MAX_OVERLAY_CLIENTS);
        return 1;
    }

    if (selftest_handle_count(savanxp_getpid(), &windowd_handles) != 0 ||
        selftest_handle_count(last_pid, &app_handles) != 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL capacidad: no se pudo leer la tabla de handles\n");
        return 1;
    }
    if (windowd_handles + WINDOWD_SELFTEST_FD_HEADROOM > WINDOWD_PROCESS_FD_LIMIT)
    {
        printf("DESKTOP SMOKE FAIL capacidad: windowd usa %u descriptores con la sesion llena\n",
            (unsigned)windowd_handles);
        return 1;
    }
    if (app_handles > WINDOWD_SELFTEST_APP_FD_MAX)
    {
        printf("DESKTOP SMOKE FAIL capacidad: la app arranco con %u descriptores heredados\n",
            (unsigned)app_handles);
        return 1;
    }
    /* compositord holds a wake event per attached surface: it pays per
     * window too, against the same 64-entry table. */
    if (session->compositor.pid <= 0 ||
        selftest_handle_count(session->compositor.pid, &compositor_handles) != 0 ||
        compositor_handles + WINDOWD_SELFTEST_FD_HEADROOM > WINDOWD_PROCESS_FD_LIMIT)
    {
        printf("DESKTOP SMOKE FAIL capacity: compositord holds %u descriptors with the session full\n",
            (unsigned)compositor_handles);
        return 1;
    }

    /* Con los slots llenos, un launch mas se rechaza sin dejar nada abierto. */
    if (launch_overlay_client(session, filler_path, 0) >= 0 ||
        selftest_handle_count(savanxp_getpid(), &handles_after_refusal) != 0 ||
        handles_after_refusal != windowd_handles)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL capacidad: el launch de mas no se rechazo limpio\n");
        return 1;
    }

    printf("DESKTOP SMOKE capacity windows=%d windowd_fds=%u compositord_fds=%u app_fds=%u\n",
        windows, (unsigned)windowd_handles, (unsigned)compositor_handles, (unsigned)app_handles);

    return 0;
}

/* Una vuelta de servicio para los overlays del selftest: pedidos del header y
 * frames, como el loop real, sin componer. Senalar los compuestos hace falta
 * igual: un cliente no presenta el frame siguiente hasta verlo. */
static void selftest_service_overlays(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    int slot;

    (void)event_reset(session->request_event_fd);
    service_connections(session, dirty);
    service_close_requests(session, dirty);
    service_desktop(session, dirty);
    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        service_client_window_requests(session, dirty, &session->overlay_clients[slot], slot);
        service_client_size_hints(session, dirty, &session->overlay_clients[slot], slot);
        service_client_presentation(session, dirty, &session->overlay_clients[slot]);
    }
    service_client_starts(session, dirty);
    /* compositord consumes the clients' frames and composes; the cursor stays
     * parked where the soak keeps it. */
    (void)windowd_commit(session, WINDOWD_SELFTEST_CURSOR_X, WINDOWD_SELFTEST_CURSOR_Y, dirty, 0);
}

/* First desktop window with `role` that has presented a frame, or 0. */
static struct windowd_client *selftest_desktop_window(struct windowd_session *session, uint32_t role)
{
    int index;

    for (index = 0; index < WINDOWD_DESKTOP_WINDOW_COUNT; ++index)
    {
        struct windowd_client *client = &session->desktop_windows[index];

        if (client->pid > 0 && client->role == role && client->started)
        {
            return client;
        }
    }
    return 0;
}

/* Services the session until the desktop has a window of `role` on screen,
 * or the deadline passes. */
static struct windowd_client *selftest_wait_desktop_window(struct windowd_session *session, uint32_t role)
{
    struct windowd_dirty_rect dirty = {0};
    unsigned long deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
    struct windowd_client *client = 0;

    while ((client = selftest_desktop_window(session, role)) == 0 && uptime_ms() < deadline)
    {
        selftest_service_overlays(session, &dirty);
        sleep_ms(20);
    }
    return client;
}

/* Opens the desktop's Start menu the way a user does: a press and a release
 * on the panel. Ten pixels in from the left edge is the Start button; that
 * is the desktop's layout, which only this test assumes. Returns the popup, or
 * 0. */
static struct windowd_client *selftest_desktop_open_popup(struct windowd_session *session)
{
    struct windowd_client *panel = selftest_wait_desktop_window(session, WINDOWD_ROLE_PANEL);
    struct windowd_client *popup = selftest_desktop_window(session, WINDOWD_ROLE_POPUP);

    if (panel == 0 || popup != 0)
    {
        return popup;
    }
    (void)route_pointer(panel, panel->window_x + 10, panel->window_y + panel->window_height / 2, 0,
        SAVANXP_MOUSE_BUTTON_LEFT);
    (void)route_pointer(panel, panel->window_x + 10, panel->window_y + panel->window_height / 2, 0, 0);
    return selftest_wait_desktop_window(session, WINDOWD_ROLE_POPUP);
}

/*
 * Window roles end to end (docs/desktop/DESKTOP_ARCHITECTURE.md#window-roles),
 * through the real desktop process: its panel sits on the bottom edge across
 * the whole display and reserves its strip; a press on the Start button opens
 * a popup above it; a press on an application window asks the popup to close
 * and the desktop closes it; and an application asking for a role is refused.
 */
static int windowd_desktop_selftest(struct windowd_session *session, int app_slot, int fullscreen_slot)
{
    struct windowd_dirty_rect dirty = {0};
    const struct savanxp_fb_info *display = &session->gfx.info;
    struct windowd_client *panel = selftest_wait_desktop_window(session, WINDOWD_ROLE_PANEL);
    struct windowd_client *popup = 0;
    struct windowd_client *app = overlay_client_at(session, app_slot);
    int reserved_top = 0;
    int reserved_bottom = 0;
    int popup_x = 0;
    int popup_y = 0;
    int popup_width = 0;
    int popup_height = 0;
    unsigned long deadline = 0;

    if (panel == 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL roles: the desktop never showed a panel\n");
        return 1;
    }
    if (panel->window_x != 0 || panel->window_width != (int)display->width ||
        panel->window_y + panel->window_height != (int)display->height)
    {
        printf("DESKTOP SMOKE FAIL roles: panel at %d,%d %dx%d, not on the bottom edge\n",
            panel->window_x, panel->window_y, panel->window_width, panel->window_height);
        return 1;
    }
    desktop_reserved_strips(session, &reserved_top, &reserved_bottom);
    if (reserved_top != 0 || reserved_bottom != panel->window_height)
    {
        printf("DESKTOP SMOKE FAIL roles: reserved strips %d/%d, panel is %d\n",
            reserved_top, reserved_bottom, panel->window_height);
        return 1;
    }

    popup = selftest_desktop_open_popup(session);
    if (popup == 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL roles: Start did not open a popup\n");
        return 1;
    }
    if ((popup->role_flags & SAVANXP_WM_POPUP_DISMISS_ON_OUTSIDE) == 0 ||
        popup->window_y + popup->window_height > panel->window_y)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL roles: the popup is not a dismissable one above the panel\n");
        return 1;
    }
    popup_x = popup->window_x;
    popup_y = popup->window_y;
    popup_width = popup->window_width;
    popup_height = popup->window_height;

    /* A press on an application window: the popup is asked to close, and the
     * desktop closes it. */
    if (app == 0 || app->pid <= 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL roles: no application window to press on\n");
        return 1;
    }
    {
        /* Any point where the application is the topmost window will do; the
         * popup or the panel may cover part of it. */
        struct sx_rect surface = windowd_client_surface_rect(app);
        int found = 0;
        int step_x;
        int step_y;

        for (step_y = 1; step_y < 8 && !found; ++step_y)
        {
            for (step_x = 1; step_x < 8 && !found; ++step_x)
            {
                int x = surface.x + (surface.width * step_x) / 8;
                int y = surface.y + (surface.height * step_y) / 8;
                const struct windowd_client *hit = top_client_at_point(session, x, y);

                if (hit != 0 && !is_desktop_window(session, hit))
                {
                    dismiss_popups_for_press(session, x, y);
                    found = 1;
                }
            }
        }
        if (!found)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL roles: no application window to press on\n");
            return 1;
        }
    }
    if (!popup->dismiss_requested || popup->header == 0 ||
        (popup->header->flags & SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN) == 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL roles: the press outside did not ask the popup to close\n");
        return 1;
    }
    deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
    while (popup->pid > 0 && uptime_ms() < deadline)
    {
        selftest_service_overlays(session, &dirty);
        sleep_ms(20);
    }
    if (popup->pid > 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL roles: the desktop did not close the dismissed popup\n");
        return 1;
    }

    /* Hotkeys. The desktop registers Ctrl+Esc and Ctrl+Shift+Esc when it
     * starts; windowd does not consume them itself, a reserved combination
     * cannot be registered, and Ctrl+Esc delivered as a hotkey makes the
     * desktop open its Start menu -- and again, close it. */
    {
        struct savanxp_input_event key_event;
        int count_before = 0;

        deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
        while (session->hotkey_count < 2 && uptime_ms() < deadline)
        {
            selftest_service_overlays(session, &dirty);
            sleep_ms(20);
        }
        if (session->hotkey_count < 2)
        {
            printf("DESKTOP SMOKE FAIL hotkeys: the desktop registered %d\n", session->hotkey_count);
            return 1;
        }
        count_before = session->hotkey_count;
        register_hotkey(session, SAVANXP_KEY_TAB, SAVANXP_KEY_MOD_ALT);
        if (session->hotkey_count != count_before)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL hotkeys: Alt+Tab could be registered\n");
            return 1;
        }

        memset(&key_event, 0, sizeof(key_event));
        key_event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
        key_event.key = SAVANXP_KEY_ESC;
        if (wm_handle_key(session, &key_event, &dirty) || deliver_hotkey(session, &key_event, &dirty))
        {
            puts_fd(2, "DESKTOP SMOKE FAIL hotkeys: a bare Esc did not reach the application\n");
            return 1;
        }
        key_event.modifiers = SAVANXP_KEY_MOD_CTRL;
        if (wm_handle_key(session, &key_event, &dirty) || !deliver_hotkey(session, &key_event, &dirty))
        {
            puts_fd(2, "DESKTOP SMOKE FAIL hotkeys: Ctrl+Esc did not go to the desktop\n");
            return 1;
        }
        /* Its KEY_UP follows it, even with Ctrl already released. */
        key_event.type = SAVANXP_INPUT_EVENT_KEY_UP;
        key_event.modifiers = 0;
        if (!deliver_hotkey(session, &key_event, &dirty) || session->hotkey_held_key != 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL hotkeys: the KEY_UP did not follow the hotkey\n");
            return 1;
        }
        if (selftest_wait_desktop_window(session, WINDOWD_ROLE_POPUP) == 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL hotkeys: Ctrl+Esc did not open the Start menu\n");
            return 1;
        }
        key_event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
        key_event.modifiers = SAVANXP_KEY_MOD_CTRL;
        (void)deliver_hotkey(session, &key_event, &dirty);
        key_event.type = SAVANXP_INPUT_EVENT_KEY_UP;
        key_event.modifiers = 0;
        (void)deliver_hotkey(session, &key_event, &dirty);
        deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
        while (selftest_desktop_window(session, WINDOWD_ROLE_POPUP) != 0 && uptime_ms() < deadline)
        {
            selftest_service_overlays(session, &dirty);
            sleep_ms(20);
        }
        if (selftest_desktop_window(session, WINDOWD_ROLE_POPUP) != 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL hotkeys: Ctrl+Esc again did not close the Start menu\n");
            return 1;
        }
    }

    /* A window opened while an application is fullscreen is laid out on the
     * native mode, not on the low one the scanout runs at (layout_display):
     * the Start menu opened in fullscreen lands where it lands on the desktop.
     * Measured against the low mode it was clamped into 640x400, and a
     * desktop started during a fullscreen game kept a 640-pixel taskbar. */
    {
        struct savanxp_input_event key_event;
        struct windowd_client *fullscreen_popup = 0;
        int same_place = 0;

        if (enter_overlay_fullscreen(session, &dirty, fullscreen_slot) != 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL layout: could not enter fullscreen\n");
            return 1;
        }
        fullscreen_popup = selftest_desktop_open_popup(session);
        if (fullscreen_popup != 0)
        {
            same_place = fullscreen_popup->window_x == popup_x && fullscreen_popup->window_y == popup_y &&
                fullscreen_popup->window_width == popup_width && fullscreen_popup->window_height == popup_height;
            if (!same_place)
            {
                printf("DESKTOP SMOKE FAIL layout: Start menu at %d,%d %dx%d in fullscreen, %d,%d %dx%d on the desktop\n",
                    fullscreen_popup->window_x, fullscreen_popup->window_y,
                    fullscreen_popup->window_width, fullscreen_popup->window_height,
                    popup_x, popup_y, popup_width, popup_height);
            }
            /* Ctrl+Esc toggles it closed. Delivering a hotkey also leaves
             * fullscreen, which makes the exit below a no-op. */
            memset(&key_event, 0, sizeof(key_event));
            key_event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
            key_event.key = SAVANXP_KEY_ESC;
            key_event.modifiers = SAVANXP_KEY_MOD_CTRL;
            (void)deliver_hotkey(session, &key_event, &dirty);
            key_event.type = SAVANXP_INPUT_EVENT_KEY_UP;
            key_event.modifiers = 0;
            (void)deliver_hotkey(session, &key_event, &dirty);
            deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
            while (selftest_desktop_window(session, WINDOWD_ROLE_POPUP) != 0 && uptime_ms() < deadline)
            {
                selftest_service_overlays(session, &dirty);
                sleep_ms(20);
            }
        }
        exit_overlay_fullscreen(session, &dirty);
        if (fullscreen_popup == 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL layout: Start did not open a popup in fullscreen\n");
            return 1;
        }
        if (!same_place)
        {
            return 1;
        }
        if (selftest_desktop_window(session, WINDOWD_ROLE_POPUP) != 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL layout: the Start menu opened in fullscreen did not close\n");
            return 1;
        }
    }

    /* Launching and the shell rule, end to end: Ctrl+Shift+Esc makes the
     * desktop spawn Task Manager, which connects on its own into a free slot,
     * labelled by its process name. Its parent is the desktop, so it could
     * never be the shell; the desktop, started here, could. */
    {
        struct savanxp_input_event key_event;
        long desktop_pid = session->desktop_windows[0].pid;
        int taskmgr_slot = -1;
        int slot;

        memset(&key_event, 0, sizeof(key_event));
        key_event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
        key_event.key = SAVANXP_KEY_ESC;
        key_event.modifiers = SAVANXP_KEY_MOD_CTRL | SAVANXP_KEY_MOD_SHIFT;
        (void)deliver_hotkey(session, &key_event, &dirty);
        key_event.type = SAVANXP_INPUT_EVENT_KEY_UP;
        key_event.modifiers = 0;
        (void)deliver_hotkey(session, &key_event, &dirty);
        deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
        while (taskmgr_slot < 0 && uptime_ms() < deadline)
        {
            selftest_service_overlays(session, &dirty);
            for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
            {
                const struct windowd_client *client = &session->overlay_clients[slot];

                if (client->pid > 0 && client->owner_slot < 0 && strcmp(client->path, "taskmgr") == 0)
                {
                    taskmgr_slot = slot;
                }
            }
            if (taskmgr_slot < 0)
            {
                sleep_ms(20);
            }
        }
        if (taskmgr_slot < 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL launch: Ctrl+Shift+Esc did not open Task Manager\n");
            return 1;
        }
        if (process_parent(session->overlay_clients[taskmgr_slot].pid) != desktop_pid ||
            started_by_session(session->overlay_clients[taskmgr_slot].pid) ||
            !started_by_session(desktop_pid))
        {
            puts_fd(2, "DESKTOP SMOKE FAIL launch: the shell rule judged the parentage wrong\n");
            return 1;
        }
        destroy_overlay_client(session, taskmgr_slot, 1);
        windowd_dirty_rect_reset(&dirty);
        printf("DESKTOP SMOKE launch: the desktop (pid %ld) started taskmgr\n", desktop_pid);
    }

    /* Closing does not kill (docs/desktop/DESKTOP_ARCHITECTURE.md#closing-a-
     * window). A program that claims the close request and ignores it keeps
     * its window; past the timeout windowd marks it not responding, the
     * desktop opens its End Program dialog, and End Now ends it -- which
     * windowd notices like any other exit. */
    {
        unsigned long saved_hung_ms = session->hung_close_ms;
        const struct windowd_client *dialog = 0;
        struct windowd_client *hung = 0;
        int hung_slot;
        long hung_pid = 0;
        int listed = 0;

        session->hung_close_ms = 300;
        hung_slot = selftest_launch(session, &dirty, "/bin/widgetsdemo", "--hang-on-close");
        if (hung_slot < 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL hung: the test program did not start\n");
            return 1;
        }
        hung = &session->overlay_clients[hung_slot];
        hung_pid = hung->pid;
        deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
        while (!hung->started && uptime_ms() < deadline)
        {
            selftest_service_overlays(session, &dirty);
            sleep_ms(10);
        }
        (void)close_overlay_window(session, &dirty, hung_slot);
        deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
        while (uptime_ms() < deadline)
        {
            const struct windowd_client *candidate = &session->desktop_windows[4];

            selftest_service_overlays(session, &dirty);
            (void)reap_dead_clients(session, &dirty);
            dialog = (candidate->pid > 0 && candidate->started) ? candidate : 0;
            if (hung->pid != hung_pid || (hung->not_responding && dialog != 0))
            {
                break;
            }
            sleep_ms(20);
        }
        if (session->desktop_windows[0].window_list != 0)
        {
            uint32_t index;
            const struct savanxp_wm_window_list *list = session->desktop_windows[0].window_list;

            for (index = 0; index < list->count && index < SAVANXP_WM_MAX_WINDOWS; ++index)
            {
                if (list->windows[index].window_id == (uint32_t)hung_pid &&
                    (list->windows[index].flags & SAVANXP_WM_WINDOW_FLAG_NOT_RESPONDING) != 0u)
                {
                    listed = 1;
                }
            }
        }
        if (hung->pid != hung_pid || !hung->not_responding || !listed || dialog == 0)
        {
            printf("DESKTOP SMOKE FAIL hung: alive=%d marked=%d listed=%d dialog=%d\n",
                hung->pid == hung_pid, hung->not_responding, listed, dialog != 0);
            return 1;
        }

        /* End Now, clicked: the button sits at the bottom right, left of Wait. */
        {
            struct sx_rect surface = windowd_client_surface_rect(dialog);
            int click_x = surface.x + surface.width - 12 - 84 - 8 - 84 / 2;
            int click_y = surface.y + surface.height - 12 - 24 / 2;

            (void)route_pointer(dialog, click_x, click_y, 0, SAVANXP_MOUSE_BUTTON_LEFT);
            (void)route_pointer(dialog, click_x, click_y, 0, 0);
        }
        deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
        while ((hung->pid == hung_pid || session->desktop_windows[4].pid > 0) && uptime_ms() < deadline)
        {
            selftest_service_overlays(session, &dirty);
            (void)reap_dead_clients(session, &dirty);
            sleep_ms(20);
        }
        session->hung_close_ms = saved_hung_ms;
        windowd_dirty_rect_reset(&dirty);
        if (hung->pid == hung_pid || session->desktop_windows[4].pid > 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL hung: End Now did not end the program and close the dialog\n");
            return 1;
        }
        puts_fd(1, "DESKTOP SMOKE hung: marked, dialog shown, End Now ended it\n");
    }

    /* An application may not ask for a role: the request is refused, as the
     * runtime would see it. */
    {
        struct savanxp_wm_client_requests *requests = &app->header->requests;
        uint32_t sequence = (requests->window_sequence | 1u) + 1u;

        memset(&requests->window, 0, sizeof(requests->window));
        requests->window.action = SAVANXP_WM_WINDOW_ACTION_OPEN;
        requests->window.window_id = 1;
        requests->window.width = 64;
        requests->window.height = 64;
        requests->window.role = SAVANXP_WM_WINDOW_ROLE_POPUP;
        requests->window_sequence = sequence;
        service_client_window_requests(session, &dirty, app, app_slot);
        if (requests->window_reply_sequence != sequence || requests->window_reply_status != -SAVANXP_EINVAL ||
            owned_window_slot(session, app_slot, 0) >= 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL roles: an application got a popup\n");
            return 1;
        }
    }

    printf("DESKTOP SMOKE roles panel=%d popup=%dx%d\n", panel->window_height, popup_width, popup_height);
    windowd_dirty_rect_reset(&dirty);
    return 0;
}

static int selftest_order_index(const struct windowd_session *session, int slot)
{
    int index;

    for (index = 0; index < session->overlay_count; ++index)
    {
        if (session->overlay_order[index] == slot)
        {
            return index;
        }
    }
    return -1;
}

/*
 * Ventanas con dueno de punta a punta (docs/desktop/OWNED_WINDOWS.md): widgetsdemo abre
 * su About por el runtime real -- pedido en el header, seccion por la cola de
 * handles del pipe, eventos con window_id --, y aca se asertan las reglas del
 * WM: familia en el z-order, dueno deshabilitado, fuera de la lista de tareas,
 * la X como pedido de cierre y ningun descriptor perdido al cerrar.
 */
static int windowd_owned_window_selftest(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    struct savanxp_wm_window_request request;
    struct savanxp_input_event alt_f4;
    const struct windowd_client *owner = 0;
    const struct windowd_client *dialog = 0;
    uint32_t handles_before_launch = 0;
    uint32_t handles_launched = 0;
    uint32_t handles_dialog = 0;
    uint32_t handles_after = 0;
    unsigned long deadline = 0;
    int owner_slot = find_free_overlay_slot(session);
    int dialog_slot = -1;
    int second_slot = -1;
    int tasks_before = windowd_task_count(session);

    if (!overlay_slot_valid(owner_slot) ||
        selftest_handle_count(savanxp_getpid(), &handles_before_launch) != 0 ||
        selftest_launch(session, dirty, "/bin/widgetsdemo", "--dialog-selftest") != owner_slot ||
        selftest_handle_count(savanxp_getpid(), &handles_launched) != 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL owned: no arranco widgetsdemo\n");
        return 1;
    }
    owner = &session->overlay_clients[owner_slot];

    deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
    for (;;)
    {
        selftest_service_overlays(session, dirty);
        dialog_slot = owned_window_slot(session, owner_slot, 0);
        if (dialog_slot >= 0 && session->overlay_clients[dialog_slot].started)
        {
            break;
        }
        if (owner->pid <= 0 || uptime_ms() >= deadline)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL owned: el dialogo nunca presento en su ventana\n");
            return 1;
        }
        sleep_ms(10);
    }
    dialog = &session->overlay_clients[dialog_slot];

    if (dialog->pid != owner->pid || dialog->window_id != 1u ||
        dialog->surface_info.width != 260u || dialog->surface_info.height != 104u)
    {
        printf("DESKTOP SMOKE FAIL owned: ventana id=%u %ux%u\n",
            (unsigned)dialog->window_id, (unsigned)dialog->surface_info.width, (unsigned)dialog->surface_info.height);
        return 1;
    }
    if (windowd_task_count(session) != tasks_before + 1)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL owned: el dialogo aparece como tarea\n");
        return 1;
    }
    if (!overlay_client_disabled(session, owner) || overlay_client_disabled(session, dialog) ||
        session->active_overlay_slot != dialog_slot)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL owned: el dueno no quedo deshabilitado con el dialogo activo\n");
        return 1;
    }
    /* Levantar al dueno trae la familia y deja el dialogo arriba y activo. */
    raise_overlay(session, owner_slot);
    if (session->active_overlay_slot != dialog_slot ||
        selftest_order_index(session, dialog_slot) <= selftest_order_index(session, owner_slot))
    {
        puts_fd(2, "DESKTOP SMOKE FAIL owned: levantar al dueno tapo a su dialogo\n");
        return 1;
    }
    if (!sx_rect_is_empty(windowd_client_minimize_button_rect(dialog)) ||
        sx_rect_is_empty(windowd_client_close_button_rect(dialog)) ||
        !windowd_client_fixed_size(dialog))
    {
        puts_fd(2, "DESKTOP SMOKE FAIL owned: el marco del dialogo no es de dialogo\n");
        return 1;
    }
    /* Dos duplicados por dialogo, nada mas: la seccion se cerro al encolarla. */
    if (selftest_handle_count(savanxp_getpid(), &handles_dialog) != 0 || handles_dialog != handles_launched + 2u)
    {
        printf("DESKTOP SMOKE FAIL owned: windowd paso de %u a %u descriptores al abrir el dialogo\n",
            (unsigned)handles_launched, (unsigned)handles_dialog);
        return 1;
    }

    /* Alt+F4 sobre el dialogo activo, que es el mismo camino que su X: el WM
     * pide, el proceso cancela el dialogo y manda el CLOSE. El dueno sigue. */
    memset(&alt_f4, 0, sizeof(alt_f4));
    alt_f4.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
    alt_f4.key = SAVANXP_KEY_F4;
    alt_f4.modifiers = SAVANXP_KEY_MOD_ALT;
    if (!wm_handle_key(session, &alt_f4, dirty))
    {
        puts_fd(2, "DESKTOP SMOKE FAIL owned: el WM no consumio Alt+F4\n");
        return 1;
    }
    deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
    while (owned_window_slot(session, owner_slot, 0) >= 0)
    {
        selftest_service_overlays(session, dirty);
        if (owner->pid <= 0 || uptime_ms() >= deadline)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL owned: Alt+F4 no cerro el dialogo\n");
            return 1;
        }
        sleep_ms(10);
    }
    if (session->active_overlay_slot != owner_slot || overlay_client_disabled(session, owner) ||
        selftest_handle_count(savanxp_getpid(), &handles_after) != 0 || handles_after != handles_launched)
    {
        printf("DESKTOP SMOKE FAIL owned: tras cerrar el dialogo activo=%d fds=%u (esperados %u)\n",
            session->active_overlay_slot, (unsigned)handles_after, (unsigned)handles_launched);
        return 1;
    }

    /* Un proceso que se muere con un dialogo abierto se lleva las dos ventanas.
     * La seccion queda encolada sin recibir: la suelta el pipe al cerrarse. */
    memset(&request, 0, sizeof(request));
    request.action = SAVANXP_WM_WINDOW_ACTION_OPEN;
    request.window_id = 2u;
    request.width = 120u;
    request.height = 60u;
    memcpy(request.title, "Selftest", sizeof("Selftest"));
    if (open_owned_window(session, dirty, owner_slot, &request) != 0 ||
        (second_slot = owned_window_slot(session, owner_slot, 2u)) < 0 ||
        open_owned_window(session, dirty, owner_slot, &request) == 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL owned: no se pudo abrir (o se duplico) el segundo dialogo\n");
        return 1;
    }
    destroy_overlay_client(session, owner_slot, 1);
    windowd_dirty_rect_reset(dirty);
    if (session->overlay_clients[owner_slot].pid > 0 || session->overlay_clients[second_slot].pid > 0 ||
        selftest_handle_count(savanxp_getpid(), &handles_after) != 0 || handles_after != handles_before_launch)
    {
        printf("DESKTOP SMOKE FAIL owned: al terminar el dueno quedaron ventanas o fds=%u (esperados %u)\n",
            (unsigned)handles_after, (unsigned)handles_before_launch);
        return 1;
    }

    /* Alt+F4 on a main window, the same gesture as its X, ASKS the process to
     * close: the window stays until the process ends, which the runtime does
     * on its own for a program that does not handle the request. */
    if (selftest_launch(session, dirty, "/bin/widgetsdemo", 0) != owner_slot ||
        session->active_overlay_slot != owner_slot ||
        !wm_handle_key(session, &alt_f4, dirty))
    {
        puts_fd(2, "DESKTOP SMOKE FAIL owned: Alt+F4 did not ask the main window to close\n");
        return 1;
    }
    deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
    while (session->overlay_clients[owner_slot].pid > 0 && uptime_ms() < deadline)
    {
        selftest_service_overlays(session, dirty);
        (void)reap_dead_clients(session, dirty);
        sleep_ms(10);
    }
    windowd_dirty_rect_reset(dirty);
    if (session->overlay_clients[owner_slot].pid > 0 ||
        selftest_handle_count(savanxp_getpid(), &handles_after) != 0 || handles_after != handles_before_launch)
    {
        printf("DESKTOP SMOKE FAIL owned: Alt+F4 did not close the main window (fds=%u, expected %u)\n",
            (unsigned)handles_after, (unsigned)handles_before_launch);
        return 1;
    }

    printf("DESKTOP SMOKE owned windows ok windowd_fds=%u\n", (unsigned)handles_dialog);
    return 0;
}

/* The first native window: sdk/nativedemo paints with SXN calls only, connects
 * to the port like any client and asks for its input on a ring. This selftest
 * launches it, composes its frame and samples the marker in the framebuffer.
 * It runs on the main session (the compositor is exclusive) and the window is
 * on top after it connects, so the centre is the demo's pixel. The colour also
 * lives in sdk/nativedemo/main.c (DEMO_MARKER).
 */
static int windowd_native_client_selftest(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    struct windowd_client *client = 0;
    int slot = -1;
    unsigned long deadline = 0;
    int iteration = 0;
    int magenta = 0;
    int green = 0;
    int presented = 0;

    if (session == 0 || dirty == 0)
    {
        return 1;
    }

    slot = find_free_overlay_slot(session);
    if (!overlay_slot_valid(slot) ||
        selftest_launch(session, dirty, "/disk/bin/nativedemo.sxe", 0) != slot)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL native: no arranco nativedemo\n");
        return 1;
    }
    client = &session->overlay_clients[slot];

    deadline = uptime_ms() + 60000;
    for (iteration = 0; iteration < 6000; ++iteration)
    {
        service_client_start(dirty, client);
        /* Every pass commits: compositord consumes the client's frames then. */
        {
            struct sx_rect surface = windowd_client_surface_rect(client);
            uint32_t stride = session->gfx.info.pitch / 4u;
            uint32_t *fb = session->compositor.framebuffer;
            uint32_t sample_x = (uint32_t)(surface.x + surface.width / 2);
            uint32_t sample_y = (uint32_t)(surface.y + surface.height / 2);

            if (windowd_commit(session, (int)session->gfx.info.width - 20, (int)session->gfx.info.height - 20, dirty, 1) < 0)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL native: commit\n");
                return 1;
            }
            ++presented;
            if (selftest_composed(client) &&
                sample_x < session->gfx.info.width && sample_y < session->gfx.info.height)
            {
                uint32_t sample = fb[sample_y * stride + sample_x];

                if (!magenta && sample == 0x00E01050u)
                {
                    /* La demo espera una tecla: se la inyecta por el camino
                     * real del router, que para nativos escribe al anillo. */
                    struct savanxp_input_event key;

                    magenta = 1;
                    memset(&key, 0, sizeof(key));
                    key.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
                    key.key = (uint32_t)'A';
                    key.ascii = (int32_t)'A';
                    (void)route_key(client, &key);
                }
                else if (magenta && !green && sample == 0x0000FF00u)
                {
                    green = 1;
                }
            }
        }
        /* After sampling: compositord composes on its own, and a client may
         * show its last frame and end before windowd looks. */
        (void)reap_dead_clients(session, dirty);
        if (green && client->pid <= 0)
        {
            break;
        }
        if (uptime_ms() >= deadline)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL native: timeout sin componer\n");
            return 1;
        }
        sleep_ms(4);
    }

    if (!green)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL native: el verde no aparecio\n");
        return 1;
    }

    printf("DESKTOP SMOKE native client ok presented=%d\n", presented);
    return 0;
}

/* Muestra un pixel de la superficie ya compuesta, en coords locales a la
 * ventana. Fuera de rango devuelve un valor imposible (los marcadores del
 * test son opacos, sin alfa). */
static uint32_t windowd_sample_surface_pixel(
    const struct windowd_session *session,
    const struct sx_rect *surface,
    int local_x,
    int local_y)
{
    uint32_t stride = 0;
    uint32_t sample_x = 0;
    uint32_t sample_y = 0;

    if (session == 0 || surface == 0)
    {
        return 0xFFFFFFFFu;
    }
    stride = session->gfx.info.pitch / 4u;
    sample_x = (uint32_t)(surface->x + local_x);
    sample_y = (uint32_t)(surface->y + local_y);
    if (sample_x >= session->gfx.info.width || sample_y >= session->gfx.info.height)
    {
        return 0xFFFFFFFFu;
    }
    return session->compositor.framebuffer[sample_y * stride + sample_x];
}

/* Layout de sdk/nativewidgets/main.c, en coords locales a la superficie. */
#define NATIVE_WIDGETS_BUTTON_CX 80
#define NATIVE_WIDGETS_BUTTON_CY 54
#define NATIVE_WIDGETS_FIELD_CX 120
#define NATIVE_WIDGETS_FIELD_CY 92
#define NATIVE_WIDGETS_SQ_1_CX 28
#define NATIVE_WIDGETS_SQ_2_CX 52
#define NATIVE_WIDGETS_SQ_CY 128
#define NATIVE_WIDGETS_SQ_OFF 0x00222222u
#define NATIVE_WIDGETS_SQ_ON 0x0000FF00u

/* Forma interactiva nativa: click en el boton, tipeo en el campo y Esc,
 * todo por el anillo de entrada, con los cuadrados de estado muestreados
 * del framebuffer. Corre despues del cliente solo-salida, en slot libre. */
static int windowd_native_widgets_selftest(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    struct windowd_client *client = 0;
    struct sx_rect surface = {0, 0, 0, 0};
    int slot = -1;
    unsigned long deadline = 0;
    int iteration = 0;
    int composed = 0;
    int clicked = 0;
    int typed = 0;
    int presented = 0;

    if (session == 0 || dirty == 0)
    {
        return 1;
    }

    slot = find_free_overlay_slot(session);
    if (!overlay_slot_valid(slot) ||
        selftest_launch(session, dirty, "/disk/bin/nativewidgets.sxe", 0) != slot)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL widgets: no arranco nativewidgets\n");
        return 1;
    }
    client = &session->overlay_clients[slot];

    deadline = uptime_ms() + 90000;
    for (iteration = 0; iteration < 9000; ++iteration)
    {
        service_client_start(dirty, client);
        {
            surface = windowd_client_surface_rect(client);
            if (windowd_commit(session, (int)session->gfx.info.width - 20, (int)session->gfx.info.height - 20, dirty, 1) < 0)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL widgets: commit\n");
                return 1;
            }
            ++presented;
            if (!composed && selftest_composed(client))
            {
                /* Forma pintada: los cuadrados arrancan apagados. */
                if (windowd_sample_surface_pixel(session, &surface, NATIVE_WIDGETS_SQ_1_CX, NATIVE_WIDGETS_SQ_CY) == NATIVE_WIDGETS_SQ_OFF &&
                    windowd_sample_surface_pixel(session, &surface, NATIVE_WIDGETS_SQ_2_CX, NATIVE_WIDGETS_SQ_CY) == NATIVE_WIDGETS_SQ_OFF)
                {
                    struct savanxp_input_event key;

                    composed = 1;
                    /* Click en el boton: down y up en su centro. */
                    (void)route_pointer(client,
                        surface.x + NATIVE_WIDGETS_BUTTON_CX, surface.y + NATIVE_WIDGETS_BUTTON_CY,
                        0, SAVANXP_MOUSE_BUTTON_LEFT);
                    (void)route_pointer(client,
                        surface.x + NATIVE_WIDGETS_BUTTON_CX, surface.y + NATIVE_WIDGETS_BUTTON_CY,
                        0, 0);
                    /* Foco al campo y dos letras. */
                    (void)route_pointer(client,
                        surface.x + NATIVE_WIDGETS_FIELD_CX, surface.y + NATIVE_WIDGETS_FIELD_CY,
                        0, SAVANXP_MOUSE_BUTTON_LEFT);
                    (void)route_pointer(client,
                        surface.x + NATIVE_WIDGETS_FIELD_CX, surface.y + NATIVE_WIDGETS_FIELD_CY,
                        0, 0);
                    memset(&key, 0, sizeof(key));
                    key.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
                    key.key = (uint32_t)'h';
                    key.ascii = (int32_t)'h';
                    (void)route_key(client, &key);
                    memset(&key, 0, sizeof(key));
                    key.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
                    key.key = (uint32_t)'i';
                    key.ascii = (int32_t)'i';
                    (void)route_key(client, &key);
                }
            }
            if (composed && !clicked &&
                windowd_sample_surface_pixel(session, &surface, NATIVE_WIDGETS_SQ_1_CX, NATIVE_WIDGETS_SQ_CY) == NATIVE_WIDGETS_SQ_ON)
            {
                clicked = 1;
                printf("DESKTOP SMOKE widgets click ok\n");
            }
            if (clicked && !typed &&
                windowd_sample_surface_pixel(session, &surface, NATIVE_WIDGETS_SQ_2_CX, NATIVE_WIDGETS_SQ_CY) == NATIVE_WIDGETS_SQ_ON)
            {
                struct savanxp_input_event key;

                typed = 1;
                printf("DESKTOP SMOKE widgets typed ok\n");
                memset(&key, 0, sizeof(key));
                key.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
                key.key = 27u;
                key.ascii = 27;
                (void)route_key(client, &key);
            }
        }
        (void)reap_dead_clients(session, dirty);
        if (typed && client->pid <= 0)
        {
            break;
        }
        if (uptime_ms() >= deadline)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL widgets: timeout sin completar\n");
            return 1;
        }
        sleep_ms(4);
    }

    if (!typed || client->pid > 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL widgets: no cerro\n");
        return 1;
    }

    printf("DESKTOP SMOKE native widgets ok presented=%d\n", presented);
    return 0;
}

/* Prueba de entrada de hardware real a un cliente nativo: la unica parte
 * sintetizada es el click que enfoca el campo (el puntero ya tiene su propio
 * pointer-smoke en el driver y comparte el mismo anillo). Las letras y el Esc
 * llegan por QMP -> PS/2 -> /dev/input0 y se drenan por el camino de
 * produccion (wm_handle_key al cliente activo), nunca por route_key directo:
 * si el verde del cuadrado 2 sale del framebuffer, el anillo nativo habla con
 * hardware de verdad. El host inyecta h, i, Esc al ver NATIVE HW READY. */
static int windowd_native_hw_selftest(void)
{
    struct windowd_session session;
    struct windowd_dirty_rect dirty = {0};
    struct windowd_client *client = 0;
    struct sx_rect surface = {0, 0, 0, 0};
    struct savanxp_input_event key_event;
    int slot = -1;
    unsigned long deadline = 0;
    int iteration = 0;
    int ready = 0;
    int typed = 0;
    int presented = 0;
    long count = 0;

    memset(&session, 0, sizeof(session));
    if (open_compositor_session(&session, 1) < 0)
    {
        puts_fd(2, "NATIVE HW FAIL compositor startup\n");
        close_compositor_session(&session);
        return 1;
    }

    slot = find_free_overlay_slot(&session);
    if (!overlay_slot_valid(slot) ||
        selftest_launch(&session, &dirty, "/disk/bin/nativewidgets.sxe", 0) != slot)
    {
        puts_fd(2, "NATIVE HW FAIL no arranco nativewidgets\n");
        close_compositor_session(&session);
        return 1;
    }
    client = &session.overlay_clients[slot];

    deadline = uptime_ms() + 120000;
    for (iteration = 0; iteration < 30000; ++iteration)
    {
        /* Camino de produccion: del driver al cliente activo, igual que en la
         * sesion real. wm_handle_key consume hotkeys; lo demas va al activo,
         * que tras el launch es nativewidgets (raise_overlay lo activa). */
        while ((count = savanxp_read(session.input_fd, &key_event, sizeof(key_event))) == (long)sizeof(key_event))
        {
            if (wm_handle_key(&session, &key_event, &dirty))
            {
                continue;
            }
            (void)route_key(active_client(&session), &key_event);
        }
        service_client_start(&dirty, client);
        {
            surface = windowd_client_surface_rect(client);
            if (windowd_commit(&session, (int)session.gfx.info.width - 20, (int)session.gfx.info.height - 20, &dirty, 1) < 0)
            {
                puts_fd(2, "NATIVE HW FAIL commit\n");
                close_compositor_session(&session);
                return 1;
            }
            ++presented;
            if (!ready && selftest_composed(client))
            {
                if (windowd_sample_surface_pixel(&session, &surface, NATIVE_WIDGETS_SQ_1_CX, NATIVE_WIDGETS_SQ_CY) == NATIVE_WIDGETS_SQ_OFF &&
                    windowd_sample_surface_pixel(&session, &surface, NATIVE_WIDGETS_SQ_2_CX, NATIVE_WIDGETS_SQ_CY) == NATIVE_WIDGETS_SQ_OFF)
                {
                    ready = 1;
                    /* Foco al campo por el camino sintetico; lo que se prueba
                     * por hardware es el teclado, no el puntero. */
                    (void)route_pointer(client,
                        surface.x + NATIVE_WIDGETS_FIELD_CX, surface.y + NATIVE_WIDGETS_FIELD_CY,
                        0, SAVANXP_MOUSE_BUTTON_LEFT);
                    (void)route_pointer(client,
                        surface.x + NATIVE_WIDGETS_FIELD_CX, surface.y + NATIVE_WIDGETS_FIELD_CY,
                        0, 0);
                    printf("NATIVE HW READY\n");
                }
            }
            if (ready && !typed &&
                windowd_sample_surface_pixel(&session, &surface, NATIVE_WIDGETS_SQ_2_CX, NATIVE_WIDGETS_SQ_CY) == NATIVE_WIDGETS_SQ_ON)
            {
                typed = 1;
                printf("NATIVE HW typed ok (real PS/2 through input ring)\n");
            }
        }
        (void)reap_dead_clients(&session, &dirty);
        if (typed && client->pid <= 0)
        {
            break;
        }
        if (uptime_ms() >= deadline)
        {
            puts_fd(2, "NATIVE HW FAIL timeout sin completar\n");
            close_compositor_session(&session);
            return 1;
        }
        sleep_ms(4);
    }

    close_compositor_session(&session);
    if (!typed)
    {
        puts_fd(2, "NATIVE HW FAIL sin verde por hardware\n");
        return 1;
    }
    printf("NATIVE HW PASS presented=%d\n", presented);
    return 0;
}

static int windowd_selftest(void)
{
    struct windowd_session session;
    struct windowd_dirty_rect dirty = {0};
    struct savanxp_gpu_present_timeline timeline = {0};
    const int kSlot = 0;
    /* progman se lanza despues de gfxdemo, asi que cae en el slot siguiente. */
    const int kProgmanSlot = 1;
    const int kMaxIterations = 4000;
    const int kTargetFrames = 60;
    const int kCursorX = 40;
    const int kCursorY = 40;
    int frames_presented = 0;
    int fullscreen_frames = 0;
    int iteration = 0;
    int failed = 0;
    uint64_t baseline_retired = 0;
    uint64_t consumed_submit = 0;
    /* Tamano generico que el WM le asigna a un overlay al lanzarlo: la
     * referencia contra la que se mide el size hint de progman. */
    uint32_t generic_surface_width = 0;
    uint32_t generic_surface_height = 0;

    if (windowd_region_selftest() != 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL region subtract primitive\n");
        return 1;
    }

    if (windowd_wheel_coalesce_selftest() != 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL wheel ticks lost in coalescing\n");
        return 1;
    }

    if (windowd_pointer_coalesce_selftest() != 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL pointer position lost in coalescing\n");
        return 1;
    }

    /* The presentation rules need no session; the end-to-end path (progman
     * publishing its own) is checked after the soak. */
    if (windowd_presentation_selftest() != 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL presentation rules\n");
        return 1;
    }

    if (open_compositor_session(&session, 1) < 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL compositor startup\n");
        close_compositor_session(&session);
        return 1;
    }
    windowd_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);

    /* gfxdemo declares launch_flags=fullscreen in its own .sxe and sends it in
     * its hello: the fullscreen subtest checks that path, not a catalog. */
    if (launch_overlay_client(&session, "/bin/gfxdemo", 0) != kSlot)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL launch gfxdemo\n");
        close_compositor_session(&session);
        return 1;
    }

    /* The desktop in the same soak: launched here, serviced in the loop, and
     * at the end its background must have composited at least one frame --
     * end-to-end proof that it connects, draws, and windowd stacks it at the
     * bottom. windowd_desktop_selftest covers its role windows. */
    if (launch_desktop_client(&session) < 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL launch desktop\n");
        close_compositor_session(&session);
        return 1;
    }

    /* A2.3: progman como cliente top-level normal. Asertar que compone un frame
     * prueba end-to-end que abre su sesion gfx, pinta el grid de programas y el
     * WM lo compone como una ventana mas. */
    if (launch_overlay_client(&session, "/bin/progman", 0) != kProgmanSlot)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL launch progman\n");
        close_compositor_session(&session);
        return 1;
    }

    /* A second instance: nothing stops the user from opening Program Manager
     * twice from the Start menu. Covered here because a single instance never
     * exercises two identical clients. */
    if (launch_overlay_client(&session, "/bin/progman", 0) < 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL launch segunda instancia de progman\n");
        close_compositor_session(&session);
        return 1;
    }

    /* All four connect on their own; the soak needs their windows. */
    if (selftest_wait_connected(&session, &dirty, &session.overlay_clients[kSlot]) < 0 ||
        selftest_wait_connected(&session, &dirty, &session.overlay_clients[kProgmanSlot]) < 0 ||
        selftest_wait_connected(&session, &dirty, &session.overlay_clients[kProgmanSlot + 1]) < 0 ||
        selftest_wait_connected(&session, &dirty, &session.desktop_windows[0]) < 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL a launched program never connected\n");
        close_compositor_session(&session);
        return 1;
    }
    if (!session.overlay_clients[kSlot].fullscreen_capable)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL gfxdemo's declared fullscreen flag did not reach the WM\n");
        close_compositor_session(&session);
        return 1;
    }

    generic_surface_width = session.overlay_clients[kProgmanSlot].surface_info.width;
    generic_surface_height = session.overlay_clients[kProgmanSlot].surface_info.height;

    if (windowd_compositor_get_timeline(&session.compositor, &timeline) == 0)
    {
        baseline_retired = timeline.retired_sequence;
    }

    for (iteration = 0; iteration < kMaxIterations; ++iteration)
    {
        struct windowd_client *client = &session.overlay_clients[kSlot];

        if (client->pid <= 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL client exited early\n");
            failed = 1;
            break;
        }

        service_client_size_hints(&session, &dirty, client, kSlot);
        service_client_presentation(&session, &dirty, client);
        service_client_cursor_hints(client);
        /* The desktop's frames and requests, so its background becomes
         * drawable and its panel opens. */
        service_desktop(&session, &dirty);
        /* Serve EVERY overlay, as the real windowd does: with one client
         * served, any other would sit waiting and the two-instance case would
         * not be exercised. */
        {
            int overlay_slot;

            for (overlay_slot = 0; overlay_slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++overlay_slot)
            {
                if (overlay_slot == kSlot)
                {
                    continue; /* served above */
                }
                service_client_size_hints(&session, &dirty, &session.overlay_clients[overlay_slot], overlay_slot);
                service_client_presentation(&session, &dirty, &session.overlay_clients[overlay_slot]);
            }
        }
        service_client_starts(&session, &dirty);

        /*
         * Exercise the window-management paths once up front (maximize/restore,
         * then a minimize/restore round trip), then drive sustained, fully
         * deterministic compositor load by oscillating the window position every
         * frame. Each move dirties the overlay frame so the compositor composites
         * the imported client surface and presents a real frame, instead of
         * depending on the client's own (input-driven) redraw cadence.
         */
        /* Exercise composited fullscreen mid-run: enter, present a few frames
         * with desktop chrome hidden, then exit. */
        if (iteration == 45)
        {
            if (enter_overlay_fullscreen(&session, &dirty, kSlot) != 0)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL enter fullscreen\n");
                failed = 1;
                break;
            }
            /* Fullscreen tiene que haber bajado el scanout a la resolucion de
             * la superficie del cliente: es lo que saca la pasada de escalado
             * por software. La asercion es incondicional porque los dos
             * backends que corre el smoke -- virtio-gpu y el framebuffer plano
             * con dispi -- saben cambiar de modo. En uno que no supiera, el
             * shell compone escalado (degradacion valida) y esto fallaria; el
             * dia que exista, hay que anunciar la capacidad hasta el shell. */
            if (session.gfx.info.width != WINDOWD_FULLSCREEN_MODE_WIDTH ||
                session.gfx.info.height != WINDOWD_FULLSCREEN_MODE_HEIGHT)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL fullscreen mode\n");
                failed = 1;
                break;
            }
        }
        else if (iteration == 52)
        {
            /* The low mode must still be set while the app is fullscreen. */
            if (session.gfx.info.width != WINDOWD_FULLSCREEN_MODE_WIDTH ||
                session.gfx.info.height != WINDOWD_FULLSCREEN_MODE_HEIGHT)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL fullscreen mode lost\n");
                failed = 1;
                break;
            }
            exit_overlay_fullscreen(&session, &dirty);
            /* Y al salir tiene que volver al nativo, pase lo que pase. */
            if (session.gfx.info.width != session.compositor.requested_info.width ||
                session.gfx.info.height != session.compositor.requested_info.height)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL restore mode\n");
                failed = 1;
                break;
            }
        }

        if (iteration == 20)
        {
            /* Restaurar tiene que dañar el area que ocupaba maximizada, no solo
             * el marco chico: si no, el fondo queda con restos de la ventana.
             * Se mide sobre un daño propio porque el acumulado del loop ya
             * puede traer el marco entero de los presents del cliente. */
            struct windowd_dirty_rect probe = {0};
            struct sx_rect maximized_frame = windowd_client_frame_rect(&session.overlay_clients[kSlot]);
            struct sx_rect probe_rect;
            size_t probe_index;

            toggle_overlay_client_maximized(&session, &probe, kSlot);
            if (session.overlay_clients[kSlot].maximized ||
                !sx_region_contains_point(&probe.region, maximized_frame.x, maximized_frame.y) ||
                !sx_region_contains_point(
                    &probe.region,
                    sx_rect_right(maximized_frame) - 1,
                    maximized_frame.y + maximized_frame.height - 1))
            {
                puts_fd(2, "DESKTOP SMOKE FAIL restore: el area maximizada quedo sin danar\n");
                failed = 1;
                break;
            }
            for (probe_index = 0; windowd_dirty_rect_at(&probe, probe_index, &probe_rect); ++probe_index)
            {
                windowd_dirty_rect_add(&dirty, &session.gfx.info, probe_rect.x, probe_rect.y, probe_rect.width, probe_rect.height);
            }
        }

        switch (iteration)
        {
        case 10:
            toggle_overlay_client_maximized(&session, &dirty, kSlot);
            break;
        case 30:
            minimize_overlay_client(&session, &dirty, kSlot);
            break;
        case 40:
            restore_overlay_client(&session, &dirty, kSlot);
            break;
        default:
            if (iteration > 40 && session.fullscreen_slot < 0)
            {
                int target_x = ((iteration & 1) != 0) ? (kCursorX + 40) : (kCursorX + 80);
                int target_y = ((iteration & 1) != 0) ? (kCursorY + 40) : (kCursorY + 80);
                move_overlay_client_window(&session, &dirty, kSlot, target_x, target_y);
            }
            else if (session.fullscreen_slot >= 0)
            {
                /* In fullscreen no window moves to make damage, and without
                   damage nothing is presented: force it, so frames are
                   presented in the low mode. */
                windowd_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
            }
            break;
        }

        if (selftest_composed_sequence(&session.overlay_clients[kSlot]) > consumed_submit)
        {
            consumed_submit = selftest_composed_sequence(&session.overlay_clients[kSlot]);
        }

        /* Without windowd damage, a commit still lets compositord consume the
         * clients' frames, so none stalls waiting for a free batch slot. */
        if (!windowd_dirty_rect_valid(&dirty))
        {
            if (windowd_commit(&session, kCursorX, kCursorY, &dirty, 0) < 0)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL commit\n");
                failed = 1;
                break;
            }
            sleep_ms(4);
            continue;
        }

        /* Synchronous: the frame is on the screen when the commit returns. */
        if (windowd_commit(&session, kCursorX, kCursorY, &dirty, 1) < 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL commit\n");
            failed = 1;
            break;
        }
        if (session.fullscreen_slot >= 0)
        {
            ++fullscreen_frames;
        }
        ++frames_presented;

        if (frames_presented >= kTargetFrames)
        {
            break;
        }
    }

    /* Before the back-pressure test below, which fills the desktop's event
     * pipe: the roles test needs its presses to arrive. */
    if (!failed && windowd_desktop_selftest(&session, kProgmanSlot, kSlot) != 0)
    {
        failed = 1;
    }

    /* Input back-pressure: the WM must never block on a client that does not
     * drain. The desktop drains its events once per loop, so between two
     * passes it is flooded until the pipe is full, and the write must FAIL
     * instead of blocking. Without non-blocking write ends this froze windowd,
     * and with it the whole session: it is the regression that hung the
     * desktop when an app was launched while the mouse moved. */
    if (!failed)
    {
        int dropped = 0;
        int index;

        for (index = 0; index < 65536; ++index)
        {
            if (!route_pointer(&session.desktop_windows[0], 10 + (index & 31), 10, 0, 0))
            {
                dropped = 1;
                break;
            }
        }
        if (!dropped)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL contrapresion: el pipe de input nunca se lleno\n");
            failed = 1;
        }
    }

    /* A minimized window is still a task, so Alt+Tab is a way back to it even
     * without the taskbar: minimize, find it among the tasks, draw a frame with
     * the switcher on it (the compositor's SWITCHER layer), and switching to
     * it restores it and closes the switcher. */
    if (!failed)
    {
        struct windowd_client *client = &session.overlay_clients[kSlot];
        int task_index = -1;
        int index;

        minimize_overlay_client(&session, &dirty, kSlot);
        if (!client->minimized)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL switcher: minimize did not apply\n");
            failed = 1;
        }
        for (index = 0; !failed && index < windowd_task_count(&session); ++index)
        {
            int slot = -1;

            if (windowd_task_client(&session, index, &slot) != 0 && slot == kSlot)
            {
                task_index = index;
            }
        }
        if (!failed && task_index < 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL switcher: the minimized window is not a task\n");
            failed = 1;
        }
        if (!failed)
        {
            session.switcher_open = 1;
            session.switcher_selected = task_index;
            windowd_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
            (void)windowd_commit(&session, kCursorX, kCursorY, &dirty, 1);
            switcher_switch_to(&session, &dirty, task_index);
            if (client->minimized)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL switcher: switching did not restore the window\n");
                failed = 1;
            }
            else if (session.switcher_open)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL switcher: the switcher stayed open\n");
                failed = 1;
            }
        }
    }

    /* Alt+Tab. Beyond cycling it covers that it starts from the ACTIVE window
     * -- so the first Tab lands on the one used last, not on any one -- that
     * releasing Alt confirms, and that Esc cancels without switching. */
    if (!failed)
    {
        struct savanxp_input_event key_event;
        int count;
        int active_index;
        int expected;

        switcher_close(&session, &dirty);

        count = windowd_task_count(&session);
        active_index = windowd_active_task_index(&session);
        if (count < 2)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: hacen falta dos ventanas para ciclar\n");
            failed = 1;
        }

        key_event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
        key_event.key = SAVANXP_KEY_TAB;
        key_event.ascii = 0;

        /* Tab pelado no es del WM: tiene que irse al cliente. */
        if (!failed)
        {
            key_event.modifiers = 0;
            if (wm_handle_key(&session, &key_event, &dirty) || session.switcher_open)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: Tab sin Alt no es del WM\n");
                failed = 1;
            }
        }

        /* Alt+Tab abre el switcher parado en la ventana anterior. */
        if (!failed)
        {
            key_event.modifiers = SAVANXP_KEY_MOD_ALT;
            expected = (active_index + count - 1) % count;
            if (!wm_handle_key(&session, &key_event, &dirty) || !session.switcher_open)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: no abrio el switcher\n");
                failed = 1;
            }
            else if (session.switcher_selected != expected)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: no arranco desde la ventana activa\n");
                failed = 1;
            }
        }

        /* El segundo Tab sigue bajando. */
        if (!failed)
        {
            expected = (session.switcher_selected + count - 1) % count;
            (void)wm_handle_key(&session, &key_event, &dirty);
            if (session.switcher_selected != expected)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: el segundo Tab no avanzo\n");
                failed = 1;
            }
        }

        /* Alt+Shift+Tab vuelve. */
        if (!failed)
        {
            expected = (session.switcher_selected + 1) % count;
            key_event.modifiers = SAVANXP_KEY_MOD_ALT | SAVANXP_KEY_MOD_SHIFT;
            (void)wm_handle_key(&session, &key_event, &dirty);
            if (session.switcher_selected != expected)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: Alt+Shift+Tab no volvio\n");
                failed = 1;
            }
        }

        /* Soltar Alt confirma. El KEY_UP llega SIN el flag de Alt, igual que
         * del driver real, que baja su estado antes de emitir. */
        if (!failed)
        {
            int target = session.switcher_selected;
            int slot = -1;

            (void)windowd_task_client(&session, target, &slot);
            key_event.type = SAVANXP_INPUT_EVENT_KEY_UP;
            key_event.key = SAVANXP_KEY_ALT;
            key_event.modifiers = 0;
            if (!wm_handle_key(&session, &key_event, &dirty) || session.switcher_open)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: soltar Alt no cerro el switcher\n");
                failed = 1;
            }
            else if (session.active_overlay_slot != slot)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: no activo la ventana elegida\n");
                failed = 1;
            }
        }

        /* Esc cancels: the switcher closes and the active window stays. */
        if (!failed)
        {
            int active_before = session.active_overlay_slot;

            key_event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
            key_event.key = SAVANXP_KEY_TAB;
            key_event.modifiers = SAVANXP_KEY_MOD_ALT;
            (void)wm_handle_key(&session, &key_event, &dirty);
            key_event.key = SAVANXP_KEY_ESC;
            key_event.modifiers = SAVANXP_KEY_MOD_ALT;
            if (!wm_handle_key(&session, &key_event, &dirty) || session.switcher_open ||
                session.active_overlay_slot != active_before)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: Esc did not cancel the switcher\n");
                failed = 1;
            }
            /* The Alt release that follows belongs to nobody now: it must not
             * switch either. */
            key_event.type = SAVANXP_INPUT_EVENT_KEY_UP;
            key_event.key = SAVANXP_KEY_ALT;
            key_event.modifiers = 0;
            (void)wm_handle_key(&session, &key_event, &dirty);
            if (!failed && session.active_overlay_slot != active_before)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: releasing Alt after Esc switched windows\n");
                failed = 1;
            }
        }
    }

    /* Subtest de size hint: la app pide el tamano que necesita su contenido y
     * el WM se lo da. Se mide sobre progman, que calcula el suyo desde el
     * registro de programas; alcanza con exigir que quede MAS CHICO que la
     * superficie generica del launch -- el numero exacto lo decide la app, y
     * clavarlo aca ataria el smoke a su layout. Tambien se exige que la
     * ventana entre entera en el area util: al encoger hay que recolocarla, y
     * el bug natural es dejarla anclada donde la puso el centrado viejo. */
    if (!failed)
    {
        const struct windowd_client *client = &session.overlay_clients[kProgmanSlot];
        struct sx_rect frame = windowd_client_frame_rect(client);
        int area_x = 0;
        int area_y = 0;
        int area_width = 0;
        int area_height = 0;

        windowd_work_area_bounds(&session.gfx.info, &area_x, &area_y, &area_width, &area_height);

        if (!client->size_hint_applied)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL size hint: progman nunca pidio su tamano\n");
            failed = 1;
        }
        else if (client->surface_info.width >= generic_surface_width ||
                 client->surface_info.height >= generic_surface_height)
        {
            printf("DESKTOP SMOKE FAIL size hint: superficie %ux%u no encogio desde %ux%u\n",
                (unsigned)client->surface_info.width,
                (unsigned)client->surface_info.height,
                (unsigned)generic_surface_width,
                (unsigned)generic_surface_height);
            failed = 1;
        }
        else if (frame.x < area_x || frame.y < area_y ||
                 sx_rect_right(frame) > area_x + area_width ||
                 sx_rect_bottom(frame) > area_y + area_height)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL size hint: la ventana quedo fuera del area util\n");
            failed = 1;
        }
    }

    /* Subtest de redimensionado por bordes (Fase C). La geometria se prueba
     * directa porque el soak no inyecta puntero; lo que importa es que el borde
     * OPUESTO al que se arrastra quede anclado y que el minimo recorte contra
     * ese ancla en vez de desplazar la ventana. */
    if (!failed)
    {
        const struct sx_rect origin = sx_rect_make(100, 80, 400, 300);
        struct sx_rect r;

        /* Borde derecho: crece a la derecha, el origen no se mueve. */
        r = resize_frame_for_drag(origin, WINDOWD_RESIZE_EDGE_RIGHT, 40, 0);
        if (r.x != origin.x || r.y != origin.y || r.width != origin.width + 40 || r.height != origin.height)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL resize: borde derecho movio el origen\n");
            failed = 1;
        }

        /* Borde izquierdo: se mueve el origen y el borde derecho queda fijo. */
        r = resize_frame_for_drag(origin, WINDOWD_RESIZE_EDGE_LEFT, 30, 0);
        if (!failed && (r.x != origin.x + 30 || sx_rect_right(r) != sx_rect_right(origin)))
        {
            puts_fd(2, "DESKTOP SMOKE FAIL resize: borde izquierdo no anclo el derecho\n");
            failed = 1;
        }

        /* Encoger de mas desde la izquierda: topa en el minimo SIN despegar el
         * borde derecho (el bug clasico es que la ventana empiece a moverse). */
        r = resize_frame_for_drag(origin, WINDOWD_RESIZE_EDGE_LEFT, 10000, 0);
        if (!failed && (r.width != WINDOWD_WINDOW_MIN_WIDTH || sx_rect_right(r) != sx_rect_right(origin)))
        {
            puts_fd(2, "DESKTOP SMOKE FAIL resize: el minimo despego el borde anclado\n");
            failed = 1;
        }

        /* Esquina: los dos ejes a la vez. */
        r = resize_frame_for_drag(origin, WINDOWD_RESIZE_EDGE_RIGHT | WINDOWD_RESIZE_EDGE_BOTTOM, 25, 35);
        if (!failed && (r.width != origin.width + 25 || r.height != origin.height + 35))
        {
            puts_fd(2, "DESKTOP SMOKE FAIL resize: esquina no aplico ambos ejes\n");
            failed = 1;
        }

        /* End-to-end sobre un cliente real: el marco nuevo tiene que llegar a la
         * superficie, que es lo que ve la app.
         *
         * Se usa progman y no gfxdemo: el soak lanza gfxdemo con el flag
         * FULLSCREEN, asi que su superficie se asigna al modo bajo (640x400) y
         * ya esta en el tope de capacidad -- crecer ahi clampea y no probaria
         * nada. Y se ENCOGE, que nunca topa contra la capacidad. */
        if (!failed)
        {
            struct windowd_client *client = &session.overlay_clients[kProgmanSlot];
            struct sx_rect frame = windowd_client_frame_rect(client);
            uint32_t before = client->surface_info.width;

            apply_overlay_client_frame(&session, &dirty, kProgmanSlot,
                resize_frame_for_drag(frame, WINDOWD_RESIZE_EDGE_RIGHT, -60, 0));
            if (client->surface_info.width != before - 60)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL resize: la superficie no siguio al marco\n");
                failed = 1;
            }
        }
    }

    /*
     * Subtest de ventana de tamano fijo. Se mide sobre progman poniendole y
     * sacandole el flag a mano, en vez de lanzar el buscaminas: lo que se
     * prueba es el GATEO, y hacerlo sobre la misma ventana antes y despues
     * descarta que el borde no agarre por alguna otra razon (geometria,
     * estado) en vez de por el flag.
     */
    if (!failed)
    {
        struct windowd_client *client = &session.overlay_clients[kProgmanSlot];
        struct sx_rect frame = windowd_client_frame_rect(client);
        /* Un punto sobre el borde derecho, dentro de la franja que agarra. */
        const int probe_x = sx_rect_right(frame) - 1;
        const int probe_y = frame.y + (frame.height / 2);
        const uint32_t saved_flags = client->presentation.window_flags;

        if (windowd_resize_edge_from_point(client, probe_x, probe_y) == WINDOWD_RESIZE_EDGE_NONE)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL fixed size: el borde no agarraba ni siendo redimensionable\n");
            failed = 1;
        }

        client->presentation.window_flags = saved_flags | SAVANXP_WM_WINDOW_STYLE_FIXED_SIZE;

        if (!failed && !windowd_client_fixed_size(client))
        {
            puts_fd(2, "DESKTOP SMOKE FAIL fixed size: el flag no se leyo de la presentacion\n");
            failed = 1;
        }
        if (!failed && windowd_resize_edge_from_point(client, probe_x, probe_y) != WINDOWD_RESIZE_EDGE_NONE)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL fixed size: el borde sigue agarrando\n");
            failed = 1;
        }
        /* El boton de maximizar NO desaparece: queda dibujado y deshabilitado.
         * Sacarlo dejaria un hueco entre minimizar y cerrar, asi que esta
         * asercion es la que fija esa decision. */
        if (!failed && !windowd_point_in_maximize_button(
                client,
                windowd_client_maximize_button_rect(client).x,
                windowd_client_maximize_button_rect(client).y))
        {
            puts_fd(2, "DESKTOP SMOKE FAIL fixed size: el maximizar se fue en vez de apagarse\n");
            failed = 1;
        }

        /*
         * La otra mitad del flag: en una ventana fija el size hint deja de ser
         * de una sola vez, que es lo que deja al buscaminas cambiar de nivel y
         * que la ventana lo siga. Se prueba sobre el predicado porque el hint
         * real lo escribe el cliente en el header, no el WM.
         *
         * El caso maximizada esta para fijar que "fija" no es un permiso que
         * pase por encima del resto del estado: una ventana maximizada no
         * acepta un tamano nuevo por mas fija que sea.
         */
        {
            const int saved_applied = client->size_hint_applied;

            /* El caso normal se mide SIN el flag: venimos de haberlo puesto
             * arriba, y dejarlo probaria dos veces la misma rama. */
            client->presentation.window_flags = saved_flags;
            client->size_hint_applied = 1;
            if (!failed && client_accepts_size_hint(client))
            {
                puts_fd(2, "DESKTOP SMOKE FAIL size hint: la ventana normal acepto un segundo hint\n");
                failed = 1;
            }

            client->presentation.window_flags = saved_flags | SAVANXP_WM_WINDOW_STYLE_FIXED_SIZE;
            if (!failed && !client_accepts_size_hint(client))
            {
                puts_fd(2, "DESKTOP SMOKE FAIL size hint: la ventana fija rechazo un segundo hint\n");
                failed = 1;
            }

            client->maximized = 1;
            if (!failed && client_accepts_size_hint(client))
            {
                puts_fd(2, "DESKTOP SMOKE FAIL size hint: la ventana fija acepto un hint estando maximizada\n");
                failed = 1;
            }
            client->maximized = 0;
            client->size_hint_applied = saved_applied;
        }

        client->presentation.window_flags = saved_flags;

        if (!failed && windowd_resize_edge_from_point(client, probe_x, probe_y) == WINDOWD_RESIZE_EDGE_NONE)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL fixed size: sacar el flag no devolvio el borde\n");
            failed = 1;
        }
    }

    if (!failed && frames_presented < kTargetFrames)
    {
        printf("DESKTOP SMOKE FAIL insufficient presented frames=%d iters=%d batches=%u\n",
            frames_presented, iteration, (unsigned)consumed_submit);
        failed = 1;
    }
    if (!failed && consumed_submit < 1)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL client surface never composited\n");
        failed = 1;
    }
    if (!failed && fullscreen_frames < 1)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL fullscreen frame never presented\n");
        failed = 1;
    }
    if (!failed && !selftest_composed(&session.desktop_windows[0]))
    {
        puts_fd(2, "DESKTOP SMOKE FAIL desktop background never composited\n");
        failed = 1;
    }
    if (!failed && !selftest_composed(&session.overlay_clients[kProgmanSlot]))
    {
        puts_fd(2, "DESKTOP SMOKE FAIL progman never composited\n");
        failed = 1;
    }
    /* Presentation from the client, end to end: progman's runtime read its own
     * .sxe (progman.sxres: name, accent 425c96, icon) and published it; windowd
     * opened no file. Before it published, the title was "progman". */
    if (!failed)
    {
        const struct windowd_presentation *shown = &session.overlay_clients[kProgmanSlot].presentation;

        if (strcmp(windowd_presentation_label(shown, 0), "Program Manager") != 0 ||
            !windowd_presentation_has_accent(shown) ||
            windowd_presentation_accent(shown) != 0x00425c96u ||
            shown->icon_extent != WINDOWD_PRESENTATION_ICON_EXTENT)
        {
            printf("DESKTOP SMOKE FAIL presentation: progman shows \"%s\" accent=%06x icon=%u\n",
                windowd_presentation_label(shown, 0), (unsigned)windowd_presentation_accent(shown),
                (unsigned)shown->icon_extent);
            failed = 1;
        }
    }
    if (!failed && !selftest_composed(&session.overlay_clients[kProgmanSlot + 1]))
    {
        puts_fd(2, "DESKTOP SMOKE FAIL segunda instancia de progman never composited\n");
        failed = 1;
    }
    if (!failed)
    {
        if (windowd_compositor_get_timeline(&session.compositor, &timeline) != 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL read present timeline\n");
            failed = 1;
        }
        else if (timeline.retired_sequence <= baseline_retired)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL present timeline did not advance\n");
            failed = 1;
        }
    }

    if (!failed && windowd_owned_window_selftest(&session, &dirty) != 0)
    {
        failed = 1;
    }

    /* Ventana nativa (sobre esta sesion: el compositor es exclusivo). */
    if (!failed && windowd_native_client_selftest(&session, &dirty) != 0)
    {
        failed = 1;
    }

    /* Forma nativa interactiva, mismo slot libre despues del reap. */
    if (!failed && windowd_native_widgets_selftest(&session, &dirty) != 0)
    {
        failed = 1;
    }

    /* Ultimo: llena la sesion, y lo que queda despues ya no se compone. */
    if (!failed && windowd_capacity_selftest(&session) != 0)
    {
        failed = 1;
    }

    close_compositor_session(&session);

    if (failed)
    {
        return 1;
    }
    printf("DESKTOP SMOKE PASS frames=%d batches=%u\n",
        frames_presented, (unsigned)consumed_submit);
    return 0;
}

/* --- Alt+Tab switcher -------------------------------------------------------
 *
 * NT's Alt+Tab: while Alt is held each Tab moves to the next window and a box
 * in the middle of the screen names it; releasing Alt switches to it. Tasks
 * are enumerated with windowd_task_client, minimized ones included, so Alt+Tab
 * is also a way back to a minimized window when the taskbar is not there.
 */

static void dirty_switcher(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    struct sx_rect box = windowd_switcher_rect(&session->gfx.info);

    windowd_dirty_rect_add(dirty, &session->gfx.info, box.x, box.y, box.width, box.height);
}

static void switcher_close(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    if (!session->switcher_open)
    {
        return;
    }
    session->switcher_open = 0;
    /* The box goes away: what it covered has to be repainted. */
    dirty_switcher(session, dirty);
}

/* Index of the active window among the tasks. Tasks are ordered from the
 * BOTTOM of the z-order up, so the active one is the last and "the previous
 * one" is one place back -- which is why Alt+Tab steps backwards. */
static int windowd_active_task_index(const struct windowd_session *session)
{
    int count = windowd_task_count(session);
    int index;

    for (index = 0; index < count; ++index)
    {
        int slot = -1;

        if (windowd_task_client(session, index, &slot) == 0)
        {
            continue;
        }
        /* With a dialog active, the active task is its owner's. */
        if (session->active_client_kind == WINDOWD_CLIENT_APP &&
            slot == overlay_root_slot(session, session->active_overlay_slot))
        {
            return index;
        }
    }
    return count > 0 ? count - 1 : 0;
}

/* Brings the chosen task to the front, restoring it if it was minimized. */
static void switcher_switch_to(struct windowd_session *session, struct windowd_dirty_rect *dirty, int task_index)
{
    int slot = -1;
    const struct windowd_client *client = windowd_task_client(session, task_index, &slot);

    switcher_close(session, dirty);
    if (client == 0 || !overlay_slot_valid(slot))
    {
        return;
    }
    if (session->overlay_clients[slot].minimized)
    {
        restore_overlay_client(session, dirty, slot);
    }
    raise_overlay(session, slot);
    windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
}

/*
 * Keyboard: the WM consumes its own keys (Alt+Tab, Alt+F4, F11) and everything
 * else goes on, to a registered hotkey or to the focused client.
 */
static int wm_handle_key(
    struct windowd_session *session,
    const struct savanxp_input_event *key_event,
    struct windowd_dirty_rect *dirty)
{
    /* Alt+Tab cycles through the windows. While Alt is held each Tab moves the
     * selection, and releasing Alt confirms -- the Windows gesture.
     * Alt+Shift+Tab goes the other way. */
    if (key_event->type == SAVANXP_INPUT_EVENT_KEY_DOWN &&
        key_event->key == SAVANXP_KEY_TAB &&
        (key_event->modifiers & SAVANXP_KEY_MOD_ALT) != 0)
    {
        int count = windowd_task_count(session);
        int step = (key_event->modifiers & SAVANXP_KEY_MOD_SHIFT) != 0 ? 1 : -1;

        if (count <= 0)
        {
            return 1;
        }
        if (!session->switcher_open)
        {
            /* A new cycle starts from the active window, so the first Tab
             * lands on the one used last, not on any one. */
            session->switcher_open = 1;
            session->switcher_selected = windowd_active_task_index(session);
        }
        session->switcher_selected = (session->switcher_selected + count + step) % count;
        dirty_switcher(session, dirty);
        return 1;
    }

    /* Releasing Alt confirms. It looks at the KEY, not the modifier: the
     * driver drops its state before emitting, so Alt's KEY_UP arrives without
     * the flag. */
    if (session->switcher_open &&
        key_event->type == SAVANXP_INPUT_EVENT_KEY_UP &&
        key_event->key == SAVANXP_KEY_ALT)
    {
        switcher_switch_to(session, dirty, session->switcher_selected);
        return 1;
    }

    /* While it is open the switcher owns the keyboard: Esc cancels, anything
     * else is swallowed so it does not reach the window being left. */
    if (session->switcher_open)
    {
        if (key_event->type == SAVANXP_INPUT_EVENT_KEY_DOWN && key_event->key == SAVANXP_KEY_ESC)
        {
            switcher_close(session, dirty);
        }
        return 1;
    }

    /* Alt+F4 cierra la ventana activa, igual que su X (close_overlay_window):
     * un dialogo recibe el pedido de cancelar, una ventana principal se cierra
     * con su proceso, y a pantalla completa tambien vale, que es donde no hay
     * marco que clickear. Con el shell activo no hay ventana que cerrar y la
     * tecla se consume igual: nadie espera que Alt+F4 le llegue a una app. */
    if (key_event->type == SAVANXP_INPUT_EVENT_KEY_DOWN &&
        key_event->key == SAVANXP_KEY_F4 &&
        (key_event->modifiers & SAVANXP_KEY_MOD_ALT) != 0)
    {
        if (session->active_client_kind == WINDOWD_CLIENT_APP)
        {
            (void)close_overlay_window(session, dirty, session->active_overlay_slot);
        }
        return 1;
    }

    if (key_event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || key_event->key != SAVANXP_KEY_F11)
    {
        return 0;
    }

    /* Toggle composited fullscreen for the active app. Intercepted even while
     * fullscreen so it can always be exited. */
    if (session->fullscreen_slot >= 0)
    {
        exit_overlay_fullscreen(session, dirty);
    }
    else if (session->active_client_kind == WINDOWD_CLIENT_APP &&
        overlay_slot_valid(session->active_overlay_slot))
    {
        (void)enter_overlay_fullscreen(session, dirty, session->active_overlay_slot);
    }
    return 1;
}

/* Repinta el cursor tras mover el puntero. Todo modal que consuma el evento
 * DEBE llamarlo: si no, el cursor se queda clavado en pantalla mientras el
 * dialogo esta abierto y parece que el mouse no responde. */
static void refresh_cursor_after_move(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    int cursor_x,
    int cursor_y,
    int previous_cursor_x,
    int previous_cursor_y)
{
    (void)dirty;
    if (previous_cursor_x == cursor_x && previous_cursor_y == cursor_y &&
        session->current_cursor_shape == session->previous_cursor_shape)
    {
        return;
    }
    /* compositord draws it: the next commit says where. */
    session->cursor_moved = 1;
}

static void handle_pointer_event(
    struct windowd_session *session,
    const struct savanxp_mouse_event *event,
    struct windowd_dirty_rect *dirty,
    int *io_cursor_x,
    int *io_cursor_y,
    uint32_t *io_last_buttons,
    int *io_drag_overlay_slot,
    int *io_drag_offset_x,
    int *io_drag_offset_y)
{
    int cursor_x = *io_cursor_x;
    int cursor_y = *io_cursor_y;
    uint32_t last_buttons = *io_last_buttons;
    int drag_overlay_slot = *io_drag_overlay_slot;
    int drag_offset_x = *io_drag_offset_x;
    int drag_offset_y = *io_drag_offset_y;
    struct savanxp_mouse_event mouse_event = *event;

    const struct windowd_client *previous_hover_client = 0;
    const struct windowd_client *current_hover_client = 0;
    uint32_t pressed_buttons = mouse_event.buttons;
    uint32_t left_pressed = pressed_buttons & SAVANXP_MOUSE_BUTTON_LEFT;
    uint32_t right_pressed = pressed_buttons & SAVANXP_MOUSE_BUTTON_RIGHT;
    uint32_t left_was_pressed = last_buttons & SAVANXP_MOUSE_BUTTON_LEFT;
    uint32_t right_was_pressed = last_buttons & SAVANXP_MOUSE_BUTTON_RIGHT;
    int previous_cursor_x = cursor_x;
    int previous_cursor_y = cursor_y;
    int previous_active_kind = session->active_client_kind;
    int previous_active_overlay_slot = session->active_overlay_slot;
    int drag_was_active = 0;
    int drag_active_now = 0;
    int mouse_routed = 0;

    mouse_event = *event;
    pressed_buttons = mouse_event.buttons;
    left_pressed = pressed_buttons & SAVANXP_MOUSE_BUTTON_LEFT;
    right_pressed = pressed_buttons & SAVANXP_MOUSE_BUTTON_RIGHT;
    (void)right_pressed;
    (void)right_was_pressed;

    if (!drag_overlay_slot_active(session, drag_overlay_slot))
    {
        drag_overlay_slot = -1;
    }
    drag_was_active = drag_overlay_slot_active(session, drag_overlay_slot);
    previous_hover_client = top_client_at_point(session, cursor_x, cursor_y);
    if ((mouse_event.flags & SAVANXP_MOUSE_FLAG_ABSOLUTE) != 0u)
    {
        /* Puntero absoluto: el host sabe donde esta y lo dice. Se asigna en vez
         * de sumar el delta, asi que un delta que se perdio en el camino (cola
         * del kernel llena, evento coalescido) no deja el cursor corrido: el
         * siguiente evento lo devuelve a la posicion verdadera. El clamp se
         * sigue aplicando porque el frame propio puede ser menor que la
         * pantalla. */
        cursor_x = windowd_clamp_int(mouse_event.absolute_x, 0, (int)session->gfx.info.width - 1);
        cursor_y = windowd_clamp_int(mouse_event.absolute_y, 0, (int)session->gfx.info.height - 1);
    }
    else
    {
        cursor_x = windowd_clamp_int(cursor_x + mouse_event.delta_x, 0, (int)session->gfx.info.width - 1);
        cursor_y = windowd_clamp_int(cursor_y + mouse_event.delta_y, 0, (int)session->gfx.info.height - 1);
    }
    current_hover_client = top_client_at_point(session, cursor_x, cursor_y);

    session->previous_cursor_shape = session->current_cursor_shape;
    session->current_cursor_shape = resolve_cursor_shape(
        session, current_hover_client, cursor_x, cursor_y, drag_was_active);
    if (session->current_cursor_shape != session->previous_cursor_shape)
    {
        session->cursor_moved = 1;
    }

    /* A press outside the desktop's popups asks them to close; the press
     * still goes to whatever it landed on. */
    if (left_pressed != 0 && left_was_pressed == 0)
    {
        dismiss_popups_for_press(session, cursor_x, cursor_y);
    }

    if (left_pressed != 0 && left_was_pressed == 0)
    {
        if (current_hover_client != 0)
        {
            if (current_hover_client->role != WINDOWD_ROLE_NORMAL)
            {
                /* A window of the desktop (background, panel, popup) is not an
                 * application window: it is not activated, not raised and has
                 * no frame. It only gets the press. Without this cut it would
                 * fall into the window path, where overlay_slot_for_client_ptr
                 * returns -1, the hover is dropped and the press is lost. */
                (void)route_pointer(current_hover_client, cursor_x, cursor_y, mouse_event.wheel, pressed_buttons);
                mouse_routed = 1;
                current_hover_client = 0;
            }
            else
            {
                int target_slot = overlay_slot_for_client_ptr(session, current_hover_client);
                int disabled = overlay_client_disabled(session, current_hover_client);

                raise_overlay(session, target_slot);
                /* Dueno deshabilitado: el click solo trajo la familia al frente
                 * y dejo activo al dialogo. No llega ni a la app ni a los
                 * botones del marco, y no arrastra. */
                current_hover_client = disabled ? 0 : overlay_client_at_const(session, target_slot);
            }
            if (current_hover_client != 0 &&
                windowd_point_in_minimize_button(current_hover_client, cursor_x, cursor_y))
            {
                int target_slot = overlay_slot_for_client_ptr(session, current_hover_client);
                if (overlay_slot_valid(target_slot))
                {
                    minimize_overlay_client(session, dirty, target_slot);
                    current_hover_client = 0;
                    drag_overlay_slot = -1;
                }
            }
            else if (current_hover_client != 0 &&
                     windowd_point_in_maximize_button(current_hover_client, cursor_x, cursor_y))
            {
                int target_slot = overlay_slot_for_client_ptr(session, current_hover_client);
                /* En una ventana de tamano fijo el boton se dibuja pero no
                 * hace nada: es el unico camino a maximizar, asi que alcanza
                 * con cortar aca para que el estado quede inalcanzable. */
                if (overlay_slot_valid(target_slot) && !windowd_client_fixed_size(current_hover_client))
                {
                    toggle_overlay_client_maximized(session, dirty, target_slot);
                }
            }
            else if (current_hover_client != 0 &&
                windowd_point_in_close_button(current_hover_client, cursor_x, cursor_y))
            {
                int target_slot = overlay_slot_for_client_ptr(session, current_hover_client);

                if (close_overlay_window(session, dirty, target_slot))
                {
                    current_hover_client = 0;
                    drag_overlay_slot = -1;
                }
            }
            /* Antes que el arrastre de la barra de titulo: la franja superior
             * del marco redimensiona, el resto de la barra mueve. */
            else if (current_hover_client != 0 &&
                     windowd_resize_edge_from_point(current_hover_client, cursor_x, cursor_y) != WINDOWD_RESIZE_EDGE_NONE)
            {
                int target_slot = overlay_slot_for_client_ptr(session, current_hover_client);
                if (overlay_slot_valid(target_slot))
                {
                    session->resize_slot = target_slot;
                    session->resize_edges = windowd_resize_edge_from_point(current_hover_client, cursor_x, cursor_y);
                    session->resize_origin_frame = windowd_client_frame_rect(current_hover_client);
                    session->resize_grab_x = cursor_x;
                    session->resize_grab_y = cursor_y;
                }
            }
            else if (current_hover_client != 0 &&
                     !current_hover_client->maximized &&
                     windowd_point_in_titlebar(current_hover_client, cursor_x, cursor_y))
            {
                int target_slot = overlay_slot_for_client_ptr(session, current_hover_client);
                if (drag_overlay_slot_active(session, target_slot))
                {
                    drag_overlay_slot = target_slot;
                    drag_offset_x = cursor_x - current_hover_client->window_x;
                    drag_offset_y = cursor_y - current_hover_client->window_y;
                }
            }
            else if (current_hover_client != 0 && current_hover_client->events_write_fd >= 0)
            {
                (void)route_pointer(current_hover_client, cursor_x, cursor_y, mouse_event.wheel, pressed_buttons);
                mouse_routed = 1;
            }
        }
    }
    /* Redimensionado en curso: recalcula el marco desde el origen guardado. */
    if (session->resize_slot >= 0)
    {
        struct windowd_client *resizing = overlay_client_at(session, session->resize_slot);

        if (left_pressed == 0 || resizing == 0 || resizing->pid <= 0 || !resizing->frame_visible)
        {
            session->resize_slot = -1;
            session->resize_edges = WINDOWD_RESIZE_EDGE_NONE;
        }
        else
        {
            apply_overlay_client_frame(
                session,
                dirty,
                session->resize_slot,
                resize_frame_for_drag(
                    session->resize_origin_frame,
                    session->resize_edges,
                    cursor_x - session->resize_grab_x,
                    cursor_y - session->resize_grab_y));
            /* Como cualquier camino que consume el evento: repintar el cursor,
             * o queda clavado mientras se arrastra. */
            refresh_cursor_after_move(session, dirty, cursor_x, cursor_y, previous_cursor_x, previous_cursor_y);
            last_buttons = pressed_buttons;
            goto done;
        }
    }

    drag_active_now = drag_overlay_slot_active(session, drag_overlay_slot);
    if (drag_active_now && left_pressed != 0)
    {
        move_overlay_client_window(
            session,
            dirty,
            drag_overlay_slot,
            cursor_x - drag_offset_x,
            cursor_y - drag_offset_y);
    }
    if (drag_was_active && left_pressed == 0)
    {
        drag_overlay_slot = -1;
        drag_active_now = 0;
    }

    if (!mouse_routed &&
        !drag_was_active &&
        !drag_active_now &&
        current_hover_client != 0 &&
        current_hover_client->events_write_fd >= 0 &&
        !overlay_client_disabled(session, current_hover_client) &&
        !(left_pressed != 0 && left_was_pressed == 0))
    {
        (void)route_pointer(current_hover_client, cursor_x, cursor_y, mouse_event.wheel, pressed_buttons);
    }
    else if (!mouse_routed &&
             !drag_was_active &&
             !drag_active_now &&
             current_hover_client == 0 &&
             previous_hover_client != 0 &&
             previous_hover_client->events_write_fd >= 0 &&
             !overlay_client_disabled(session, previous_hover_client) &&
             !(left_pressed != 0 && left_was_pressed == 0))
    {
        (void)route_pointer(previous_hover_client, cursor_x, cursor_y, mouse_event.wheel, pressed_buttons);
    }

    refresh_cursor_after_move(session, dirty, cursor_x, cursor_y, previous_cursor_x, previous_cursor_y);

    if (previous_active_kind != session->active_client_kind || previous_active_overlay_slot != session->active_overlay_slot)
    {
        if (overlay_slot_valid(previous_active_overlay_slot))
        {
            windowd_dirty_rect_add_client(dirty, overlay_client_at_const(session, previous_active_overlay_slot));
        }
        if (overlay_slot_valid(session->active_overlay_slot))
        {
            windowd_dirty_rect_add_client(dirty, overlay_client_at_const(session, session->active_overlay_slot));
        }
    }
    last_buttons = pressed_buttons;

done:
    *io_cursor_x = cursor_x;
    *io_cursor_y = cursor_y;
    *io_last_buttons = last_buttons;
    *io_drag_overlay_slot = drag_overlay_slot;
    *io_drag_offset_x = drag_offset_x;
    *io_drag_offset_y = drag_offset_y;
}

int main(int argc, char **argv)
{
    struct windowd_session session;
    struct savanxp_input_event key_event = {0};
    struct windowd_dirty_rect dirty = {0};
    int cursor_x = 24;
    int cursor_y = 24;
    uint32_t last_buttons = 0;
    int drag_overlay_slot = -1;
    int drag_offset_x = 0;
    int drag_offset_y = 0;

    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        return windowd_selftest();
    }

    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--cursor-repro") == 0)
    {
        return windowd_cursor_repro();
    }

    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--hw-native-test") == 0)
    {
        return windowd_native_hw_selftest();
    }

    if (open_compositor_session(&session, 0) < 0)
    {
        puts_fd(2, "desktop: compositor startup failed\n");
        close_compositor_session(&session);
        return 1;
    }
    session.cursor_moved = 1;
    /* Nothing is started here: sessiond starts the desktop and Program Manager
     * once the port exists, and they connect like anybody else. Without a
     * desktop the session lives on with a solid background. */
    windowd_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);

    for (;;)
    {
        /* Input, mouse, the session's submit event, the port and the pending
         * hellos. */
        struct savanxp_pollfd poll_fds[4 + WINDOWD_MAX_PENDING_CONNECTIONS];
        int input_poll_index = -1;
        int mouse_poll_index = -1;
        int request_poll_index = -1;
        int poll_count = 0;
        int slot;
        long count = 0;

        input_poll_index = poll_count;
        poll_fds[poll_count].fd = session.input_fd;
        poll_fds[poll_count].events = SAVANXP_POLLIN;
        poll_fds[poll_count].revents = 0;
        ++poll_count;

        if (session.mouse_fd >= 0)
        {
            mouse_poll_index = poll_count;
            poll_fds[poll_count].fd = session.mouse_fd;
            poll_fds[poll_count].events = SAVANXP_POLLIN;
            poll_fds[poll_count].revents = 0;
            ++poll_count;
        }

        /* Wake on header requests, not just the 16 ms tick. Frames do not
         * wake windowd: they are compositord's. The tick is what notices a
         * window's first frame, and the requests of a client that did not ask
         * for the request event. */
        request_poll_index = poll_count;
        poll_fds[poll_count].fd = session.request_event_fd;
        poll_fds[poll_count].events = SAVANXP_POLLIN;
        poll_fds[poll_count].revents = 0;
        ++poll_count;

        /* A program connecting, and the hellos still owed: a window opens
         * without waiting out the timeout. */
        poll_fds[poll_count].fd = session.port_fd;
        poll_fds[poll_count].events = SAVANXP_POLLIN;
        poll_fds[poll_count].revents = 0;
        ++poll_count;
        for (slot = 0; slot < WINDOWD_MAX_PENDING_CONNECTIONS; ++slot)
        {
            if (session.pending[slot].read_fd >= 0)
            {
                poll_fds[poll_count].fd = session.pending[slot].read_fd;
                poll_fds[poll_count].events = SAVANXP_POLLIN;
                poll_fds[poll_count].revents = 0;
                ++poll_count;
            }
        }

        if (savanxp_poll(poll_fds, (unsigned long)poll_count, 16) < 0)
        {
            break;
        }

        sweep_pending_reaps();

        /* Reset BEFORE servicing: a client that asks after this point re-arms
         * the event and wakes the next poll. Every client is serviced below on
         * every pass, so one shared event is enough. */
        if ((poll_fds[request_poll_index].revents & SAVANXP_POLLIN) != 0)
        {
            (void)event_reset(session.request_event_fd);
        }

        if (input_poll_index >= 0 && (poll_fds[input_poll_index].revents & SAVANXP_POLLIN) != 0)
        {
            while ((count = savanxp_read(session.input_fd, &key_event, sizeof(key_event))) == (long)sizeof(key_event))
            {
                /* Arbitration: the WM's own keys, then the hotkeys the
                 * desktop registered, then the focused application. */
                if (wm_handle_key(&session, &key_event, &dirty) ||
                    deliver_hotkey(&session, &key_event, &dirty))
                {
                    continue;
                }
                {
                    struct windowd_client *client = active_client(&session);
                    (void)route_key(client, &key_event);
                }
            }
        }

        /* F11 puede haber achicado el scanout debajo del puntero. Reencuadrarlo
         * aca evita que quede fuera de la pantalla nueva -- invisible y sin
         * poder hacer hit-test -- hasta que el usuario mueva el mouse, que es
         * cuando el clamp del handler lo volveria a meter en rango. */
        if (cursor_x >= (int)session.gfx.info.width || cursor_y >= (int)session.gfx.info.height)
        {
            cursor_x = windowd_clamp_int(cursor_x, 0, (int)session.gfx.info.width - 1);
            cursor_y = windowd_clamp_int(cursor_y, 0, (int)session.gfx.info.height - 1);
            session.cursor_moved = 1;
        }

        if (mouse_poll_index >= 0 && (poll_fds[mouse_poll_index].revents & SAVANXP_POLLIN) != 0)
        {
            struct savanxp_mouse_event raw_mouse_events[WINDOWD_MAX_MOUSE_EVENTS_PER_FRAME];
            struct savanxp_mouse_event coalesced_mouse_events[WINDOWD_MAX_MOUSE_EVENTS_PER_FRAME];
            size_t raw_mouse_count = 0;
            size_t coalesced_mouse_count = 0;
            size_t mouse_event_index = 0;

            count = savanxp_read(session.mouse_fd, raw_mouse_events, sizeof(raw_mouse_events));
            if (count > 0)
            {
                raw_mouse_count = (size_t)count / sizeof(raw_mouse_events[0]);
                coalesced_mouse_count = coalesce_mouse_events(
                    raw_mouse_events,
                    raw_mouse_count,
                    coalesced_mouse_events,
                    WINDOWD_MAX_MOUSE_EVENTS_PER_FRAME,
                    last_buttons);
            }

            for (mouse_event_index = 0; mouse_event_index < coalesced_mouse_count; ++mouse_event_index)
            {
                handle_pointer_event(&session, &coalesced_mouse_events[mouse_event_index], &dirty,
                    &cursor_x, &cursor_y, &last_buttons, &drag_overlay_slot, &drag_offset_x, &drag_offset_y);
            }
        }

        if (reap_dead_clients(&session, &dirty) < 0)
        {
            break;
        }
        service_connections(&session, &dirty);
        service_close_requests(&session, &dirty);
        service_desktop(&session, &dirty);
        for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
        {
            service_client_window_requests(&session, &dirty, &session.overlay_clients[slot], slot);
            service_client_size_hints(&session, &dirty, &session.overlay_clients[slot], slot);
            service_client_presentation(&session, &dirty, &session.overlay_clients[slot]);
            service_client_cursor_hints(&session.overlay_clients[slot]);
        }
        service_client_starts(&session, &dirty);

        if (!session.cursor_moved && !windowd_dirty_rect_valid(&dirty))
        {
            continue;
        }
        /* Without compositord the session is over: sessiond restarts all of
         * it. */
        if (windowd_commit(&session, cursor_x, cursor_y, &dirty, 0) < 0)
        {
            break;
        }
    }

    close_compositor_session(&session);
    return 1;
}
