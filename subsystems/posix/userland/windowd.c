#include "libc.h"
#include "windowd_session.h"
#include "windowd_appinfo.h"
#include "desktop_wallpaper.h"
#include "windowd_layout.h"
#include "windowd_render.h"
#include "windowd_stats.h"

#define WINDOWD_MAX_MOUSE_EVENTS_PER_FRAME 16
#define WINDOWD_SURFACE_PAGE_SIZE 4096u
/* Low render size used for composited fullscreen apps: the client renders here
 * and the shell scales it to the display when F11 fullscreen is active. */
#define WINDOWD_FULLSCREEN_MODE_WIDTH 640
#define WINDOWD_FULLSCREEN_MODE_HEIGHT 400

static const char *k_shellapp_path = "/bin/shellapp";
static const char *k_background_client_path = "/bin/shellui";
static const char *k_taskbar_client_path = "/bin/taskbar";
static const char *k_keyboard_popup_client_path = "/bin/kbdlayoutpopup";
static const char *k_progman_path = "/bin/progman";

/* Tamano del popup de layout de teclado (dos filas, ES/EN): fijo, no depende
 * de contenido -- lo que muestra el binario es un detalle suyo, windowd solo
 * le reserva el rect anclado arriba de la franja de la taskbar. */
#define WINDOWD_KEYBOARD_POPUP_WIDTH 96
#define WINDOWD_KEYBOARD_POPUP_HEIGHT 44

static int launch_overlay_client(
    struct windowd_session *session,
    const char *path,
    const char *argument,
    uint32_t launch_flags);
/* Definidas junto al resto del Task List, mas abajo; el selftest las usa antes. */
static void tasklist_open(struct windowd_session *session, struct windowd_dirty_rect *dirty);
static void tasklist_switch_to(struct windowd_session *session, struct windowd_dirty_rect *dirty, int task_index);
static void tasklist_close(struct windowd_session *session, struct windowd_dirty_rect *dirty);
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

/* Tamano de la tabla de descriptores de un proceso (process::kMaxFileHandles). */
#define WINDOWD_PROCESS_FD_LIMIT 64

/*
 * El hijo remapea con dup2 sus canales sobre los descriptores del protocolo
 * (savanxp/wm_protocol.h) antes del exec. Un descriptor de ORIGEN puede caer
 * dentro de esa ventana y quedar pisado por un dup2 anterior, asi que primero
 * se lo muda por encima del ultimo destino. dup() da el menor libre, de ahi el
 * reintento.
 */
static int child_relocate_above(int fd, int last_target)
{
    while (fd >= 0 && fd <= last_target)
    {
        long moved = savanxp_dup(fd);
        if (moved < 0)
        {
            return -1;
        }
        fd = (int)moved;
    }
    return fd;
}

/*
 * Despues de los dup2, el hijo cierra TODO lo que heredo por encima del
 * protocolo. fork copia la tabla entera de windowd, y exec no cierra nada: sin
 * esto cada app arrancaba con los canales de todas las ventanas anteriores
 * abiertos -- con la sesion llena, casi sin lugar en su propia tabla para abrir
 * un archivo. Cerrar un fd que no esta abierto es inofensivo.
 */
static void child_close_above(int last_target)
{
    int fd;

    for (fd = last_target + 1; fd < WINDOWD_PROCESS_FD_LIMIT; ++fd)
    {
        savanxp_close(fd);
    }
}

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
        if (session->overlay_clients[slot].pid <= 0)
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

    if (session->active_client_kind == WINDOWD_CLIENT_APP && session->active_overlay_slot < 0)
    {
        session->active_client_kind = WINDOWD_CLIENT_SHELL;
    }

    if (session->active_client_kind == WINDOWD_CLIENT_SHELL && visible_overlay_slot >= 0 && session->shell_client.pid <= 0)
    {
        session->active_client_kind = WINDOWD_CLIENT_APP;
        session->active_overlay_slot = visible_overlay_slot;
    }

    if (session->active_client_kind == WINDOWD_CLIENT_APP && session->active_overlay_slot < 0 && visible_overlay_slot >= 0)
    {
        session->active_overlay_slot = visible_overlay_slot;
    }

    if (session->active_client_kind == WINDOWD_CLIENT_SHELL)
    {
        session->active_overlay_slot = -1;
    }

    session->shell_client.active = session->shell_client.pid > 0 && session->active_client_kind == WINDOWD_CLIENT_SHELL;
    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        session->overlay_clients[slot].active =
            overlay_client_visible(&session->overlay_clients[slot]) &&
            session->active_client_kind == WINDOWD_CLIENT_APP &&
            slot == session->active_overlay_slot;
    }
}

static void activate_shell(struct windowd_session *session)
{
    if (session == 0)
    {
        return;
    }

    session->active_client_kind = WINDOWD_CLIENT_SHELL;
    session->active_overlay_slot = -1;
    refresh_active_state(session);
}

/* --- Ventanas con dueno (docs/OWNED_WINDOWS.md) ------------------------------
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
    return session->shell_client.pid > 0 ? &session->shell_client : 0;
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
        /* Unico lugar del WM que descuenta la barra de tareas. */
        windowd_maximize_area_bounds(
            &session->gfx.info,
            session->taskbar_client.pid > 0,
            &area_x, &area_y, &area_width, &area_height);
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
            &session->gfx.info,
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

    session->gfx.info = session->compositor.display_info;
    /* La superficie compartida todavia tiene los pixeles del modo anterior, que
     * con el stride nuevo se leen torcidos. Limpiarla evita mostrar un frame
     * corrupto en la ventana entre el cambio de modo y el primer repintado. */
    memset(session->compositor.framebuffer, 0, session->gfx.info.buffer_size);
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
        &session->gfx.info,
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

static const struct windowd_client *top_client_at_point(const struct windowd_session *session, int x, int y)
{
    const struct windowd_client *overlay;

    /* El popup de layout gana contra todo, incluso la taskbar: se dibuja
     * arriba de la franja y es lo primero que hay que poder clickear. */
    if (session != 0 && session->keyboard_popup_client.pid > 0 &&
        windowd_point_in_client(&session->keyboard_popup_client, x, y))
    {
        return &session->keyboard_popup_client;
    }

    /* La barra de tareas se compone por ENCIMA de las ventanas normales, asi
     * que gana el hit-test contra ellas. Con una app a pantalla completa queda
     * detras y no recibe nada: coincide con como se arma el z-order. */
    if (session != 0 && session->fullscreen_slot < 0 &&
        session->taskbar_client.pid > 0 &&
        windowd_point_in_client(&session->taskbar_client, x, y))
    {
        return &session->taskbar_client;
    }

    overlay = top_overlay_client_at_point(session, x, y);

    if (overlay != 0)
    {
        return overlay;
    }
    if (session != 0 && session->shell_client.pid > 0 && windowd_point_in_client(&session->shell_client, x, y))
    {
        return &session->shell_client;
    }
    return 0;
}

/* An overlay client forked but that hasn't submitted its first frame yet is
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
        if (client->pid > 0 && client->consumed_submit_sequence == 0)
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
    /* Sobre el Task List siempre flecha: es un dialogo del WM, no debe heredar
     * el cursor que pida la ventana que quedo debajo. */
    if (session != 0 && session->tasklist_open)
    {
        return SAVANXP_CURSOR_ARROW;
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
        current_hover_client != &session->shell_client &&
        !overlay_client_disabled(session, current_hover_client) &&
        windowd_point_in_client(current_hover_client, cursor_x, cursor_y))
    {
        return current_hover_client->last_cursor_hint_shape;
    }
    return SAVANXP_CURSOR_ARROW;
}

static int set_hw_cursor_position(struct windowd_session *session, int cursor_x, int cursor_y, int visible)
{
    if (session == 0 || !session->hw_cursor_enabled)
    {
        return -1;
    }

    return windowd_compositor_move_cursor(&session->compositor, cursor_x, cursor_y, visible) < 0 ? -1 : 0;
}

static int try_enable_hw_cursor(struct windowd_session *session, int cursor_x, int cursor_y)
{
    if (session == 0)
    {
        return 0;
    }
    if ((session->compositor.gpu_info.flags & SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE) == 0)
    {
        return 0;
    }

    if (windowd_compositor_enable_cursor(&session->compositor, cursor_x, cursor_y) < 0)
    {
        return 0;
    }

    session->hw_cursor_enabled = 1;
    return 1;
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

/* Cap on consecutive reconnects without a clean frame in between, so a daemon
   that dies on every spawn surfaces as a hard failure instead of a spin loop. */
#define WINDOWD_MAX_COMPOSITOR_RECOVERIES 8

/* Respawn compositord after it died mid-session. The display section and the
   shell's backbuffer outlive the daemon, so a successful reconnect re-displays
   the current frame and restores the cursor without a re-render. Returns 0 on
   success; the caller forces a full repaint so subsequent damage stays correct. */
static int recover_compositor(struct windowd_session *session)
{
    int result;

    if (session == 0)
    {
        return -1;
    }

    result = windowd_compositor_reconnect(&session->compositor);
    if (result < 0)
    {
        return windowd_stage_failed("reconnect compositord", result);
    }

    eprintf("desktop: compositord reconnected after fault\n");

    /* El daemon vuelve siempre en el modo nativo, porque su INIT pide
     * requested_info. Si el shell estaba en el modo bajo de fullscreen hay que
     * volver a pedirlo: componer con una geometria que el scanout ya no tiene
     * manda rects que no existen. Se pida o no con exito, la que vale es la que
     * quedo del lado del daemon, que es contra la que esta importada la
     * superficie. */
    if (session->gfx.info.width != session->compositor.display_info.width ||
        session->gfx.info.height != session->compositor.display_info.height)
    {
        (void)windowd_compositor_set_mode(
            &session->compositor, session->gfx.info.width, session->gfx.info.height);
        session->gfx.info = session->compositor.display_info;
        memset(session->compositor.framebuffer, 0, session->gfx.info.buffer_size);
    }
    return 0;
}

/* Devuelve 1 si el evento se entrego. Con el pipe lleno el write no bloquea
 * (extremo no-bloqueante) y el evento se DESCARTA: preferimos perder input de un
 * cliente que no drena antes que congelar la sesion. Es seguro para el puntero,
 * que lleva posicion absoluta -- el proximo evento corrige --, y aceptable para
 * el teclado, que se recupera solo en cuanto el cliente vuelve a leer. Siempre
 * se escribe un registro entero (ver savanxp_wm_event). */
static int route_event(const struct windowd_client *client, const struct savanxp_wm_event *event)
{
    struct savanxp_wm_event tagged;

    if (client == 0 || client->events_write_fd < 0 || event == 0)
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
        }

        current_buttons = event->buttons;
        last_was_button_transition = button_transition;
    }

    return coalesced_count;
}

static int windowd_process_alive(long pid)
{
    struct savanxp_process_info info;
    unsigned long index = 0;

    if (pid <= 0)
    {
        return 0;
    }

    for (;;)
    {
        long result = proc_info(index, &info);
        if (result <= 0)
        {
            return 0;
        }
        if ((long)info.pid == pid && info.state != SAVANXP_PROC_ZOMBIE)
        {
            return 1;
        }
        ++index;
    }
}

static void add_client_present_damage(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    const struct windowd_client *client,
    const struct savanxp_gpu_dirty_rect *rect)
{
    struct sx_rect surface_rect;

    if (session == 0 || dirty == 0 || client == 0 || rect == 0 || client->pid <= 0)
    {
        return;
    }

    surface_rect = windowd_client_surface_rect(client);
    if (client->fullscreen)
    {
        windowd_dirty_rect_add(
            dirty,
            &session->gfx.info,
            surface_rect.x,
            surface_rect.y,
            surface_rect.width,
            surface_rect.height);
        return;
    }
    windowd_dirty_rect_add(
        dirty,
        &session->gfx.info,
        surface_rect.x + (int)rect->x,
        surface_rect.y + (int)rect->y,
        (int)rect->width,
        (int)rect->height);
}

static void signal_client_retire(struct windowd_client *client, uint64_t retired_sequence)
{
    int advanced = 0;

    if (client == 0 || client->header == 0 || retired_sequence == 0)
    {
        return;
    }

    if (client->header->retired_sequence < retired_sequence)
    {
        client->header->retired_sequence = retired_sequence;
        advanced = 1;
    }
    if (advanced && client->wake_event_fd >= 0)
    {
        (void)event_set(client->wake_event_fd);
    }
}

static void signal_client_composed(struct windowd_client *client, uint64_t composed_sequence)
{
    int advanced = 0;

    if (client == 0 || client->header == 0 || composed_sequence == 0)
    {
        return;
    }

    if (client->header->composed_sequence < composed_sequence)
    {
        client->header->composed_sequence = composed_sequence;
        advanced = 1;
    }
    if (advanced && client->wake_event_fd >= 0)
    {
        (void)event_set(client->wake_event_fd);
    }
}

static void signal_composed_batches(struct windowd_session *session)
{
    int slot;

    if (session == 0)
    {
        return;
    }

    signal_client_composed(&session->background_client, session->background_client.consumed_submit_sequence);
    signal_client_composed(&session->taskbar_client, session->taskbar_client.consumed_submit_sequence);
    signal_client_composed(&session->keyboard_popup_client, session->keyboard_popup_client.consumed_submit_sequence);
    signal_client_composed(&session->shell_client, session->shell_client.consumed_submit_sequence);
    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        signal_client_composed(&session->overlay_clients[slot], session->overlay_clients[slot].consumed_submit_sequence);
    }
}

static void retire_presented_batches(struct windowd_session *session)
{
    int slot;

    if (session == 0)
    {
        return;
    }

    if (session->background_client.pending_retire_sequence != 0)
    {
        signal_client_retire(&session->background_client, session->background_client.pending_retire_sequence);
        session->background_client.pending_retire_sequence = 0;
    }

    if (session->taskbar_client.pending_retire_sequence != 0)
    {
        signal_client_retire(&session->taskbar_client, session->taskbar_client.pending_retire_sequence);
        session->taskbar_client.pending_retire_sequence = 0;
    }

    if (session->keyboard_popup_client.pending_retire_sequence != 0)
    {
        signal_client_retire(&session->keyboard_popup_client, session->keyboard_popup_client.pending_retire_sequence);
        session->keyboard_popup_client.pending_retire_sequence = 0;
    }

    if (session->shell_client.pending_retire_sequence != 0)
    {
        signal_client_retire(&session->shell_client, session->shell_client.pending_retire_sequence);
        session->shell_client.pending_retire_sequence = 0;
    }

    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        struct windowd_client *client = &session->overlay_clients[slot];
        if (client->pending_retire_sequence != 0)
        {
            signal_client_retire(client, client->pending_retire_sequence);
            client->pending_retire_sequence = 0;
        }
    }
}

static int consume_client_present_batches(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    struct windowd_client *client)
{
    uint64_t next_sequence = 0;
    int first_frame = 0;

    if (session == 0 || dirty == 0 || client == 0 || client->header == 0 || client->command_batches == 0)
    {
        return 0;
    }

    first_frame = (client->consumed_submit_sequence == 0);
    next_sequence = client->consumed_submit_sequence + 1u;
    while (next_sequence <= client->header->submit_sequence)
    {
        struct savanxp_gpu_dirty_rect_batch *batch = 0;
        uint32_t rect_index = 0;

        if (client->header->batch_capacity == 0)
        {
            eprintf("desktop: invalid client batch capacity for %s\n", client->path[0] != '\0' ? client->path : "?");
            return -1;
        }

        batch = &client->command_batches[(next_sequence - 1u) % client->header->batch_capacity];
        if (batch->submit_sequence != next_sequence ||
            batch->rect_count > client->header->rect_capacity ||
            batch->rect_count > SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS)
        {
            eprintf(
                "desktop: invalid client batch for %s seq=%u batch_seq=%u rects=%u\n",
                client->path[0] != '\0' ? client->path : "?",
                (unsigned int)next_sequence,
                (unsigned int)batch->submit_sequence,
                (unsigned int)batch->rect_count);
            return -1;
        }

        if ((batch->flags & SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_FULL_SURFACE) != 0)
        {
            struct sx_rect surface_rect = windowd_client_surface_rect(client);
            windowd_dirty_rect_add(
                dirty,
                &session->gfx.info,
                surface_rect.x,
                surface_rect.y,
                surface_rect.width,
                surface_rect.height);
        }
        else
        {
            const uint32_t surface_capacity_width = client_surface_capacity_width(client);
            const uint32_t surface_capacity_height = client_surface_capacity_height(client);
            for (rect_index = 0; rect_index < batch->rect_count; ++rect_index)
            {
                const struct savanxp_gpu_dirty_rect *rect = &batch->rects[rect_index];

                if (rect->width == 0 || rect->height == 0 ||
                    rect->x >= surface_capacity_width ||
                    rect->y >= surface_capacity_height ||
                    rect->width > (surface_capacity_width - rect->x) ||
                    rect->height > (surface_capacity_height - rect->y))
                {
                    eprintf(
                        "desktop: invalid client rect for %s seq=%u rect=%u,%u %ux%u surface=%ux%u\n",
                        client->path[0] != '\0' ? client->path : "?",
                        (unsigned int)next_sequence,
                        rect->x,
                        rect->y,
                        rect->width,
                        rect->height,
                        surface_capacity_width,
                        surface_capacity_height);
                    return -1;
                }
                add_client_present_damage(session, dirty, client, rect);
            }
        }

        client->consumed_submit_sequence = next_sequence;
        next_sequence += 1u;
    }

    /* Primer frame: la ventana pasa de no componerse a componerse, asi que hay
     * que repintar el MARCO entero. El damage de un batch cubre solo la
     * superficie del cliente; la barra de titulo y el borde los dibuja el WM y
     * nadie mas los ensuciaria. */
    if (first_frame && client->consumed_submit_sequence > 0)
    {
        struct sx_rect frame = windowd_client_frame_rect(client);
        windowd_dirty_rect_add(dirty, &session->gfx.info, frame.x, frame.y, frame.width, frame.height);
    }

    /* El evento de submit es de la sesion, no de este cliente: lo resetea el
     * loop principal antes de revisar a todos. */
    return 0;
}

static void snapshot_pending_retire_sequences(struct windowd_session *session)
{
    int slot;

    if (session == 0)
    {
        return;
    }

    session->background_client.pending_retire_sequence = session->background_client.consumed_submit_sequence;
    session->taskbar_client.pending_retire_sequence = session->taskbar_client.consumed_submit_sequence;
    session->keyboard_popup_client.pending_retire_sequence = session->keyboard_popup_client.consumed_submit_sequence;
    session->shell_client.pending_retire_sequence = session->shell_client.consumed_submit_sequence;
    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        session->overlay_clients[slot].pending_retire_sequence = session->overlay_clients[slot].consumed_submit_sequence;
    }
}

static int sync_pending_present(struct windowd_session *session, int wait_for_target, int *ready)
{
    int result;

    if (ready != 0)
    {
        *ready = 1;
    }
    if (session == 0 || session->compositor.pending_present_sequence == 0)
    {
        return 0;
    }

    result = windowd_compositor_sync_present(&session->compositor, wait_for_target, ready);
    if (result < 0)
    {
        return windowd_stage_failed("compositord sync present", result);
    }
    if (ready == 0 || *ready)
    {
        retire_presented_batches(session);
    }
    return 0;
}

static int present_frame(struct windowd_session *session, const struct windowd_dirty_rect *dirty)
{
    struct sx_rect rects[SAVANXP_GPU_SURFACE_PRESENT_BATCH_MAX_RECTS];
    size_t rect_count = 0;
    size_t index;
    int result;

    if (session == 0 || dirty == 0 || !windowd_dirty_rect_valid(dirty))
    {
        return 0;
    }

    snapshot_pending_retire_sequences(session);
    for (index = 0; index < windowd_dirty_rect_count(dirty); ++index)
    {
        struct sx_rect rect;

        if (!windowd_dirty_rect_at(dirty, index, &rect) || rect.width <= 0 || rect.height <= 0)
        {
            continue;
        }
        rects[rect_count++] = rect;
        if (rect_count >= SAVANXP_GPU_SURFACE_PRESENT_BATCH_MAX_RECTS)
        {
            result = windowd_compositor_present(&session->compositor, rects, rect_count);
            if (result < 0)
            {
                return windowd_stage_failed("compositord present", result);
            }
            rect_count = 0;
        }
    }

    if (rect_count != 0)
    {
        result = windowd_compositor_present(&session->compositor, rects, rect_count);
        if (result < 0)
        {
            return windowd_stage_failed("compositord present", result);
        }
    }
    return 0;
}

static void fill_client_surface_info(
    const struct windowd_session *session,
    enum windowd_client_kind kind,
    struct savanxp_fb_info *client_info)
{
    if (session == 0 || client_info == 0)
    {
        return;
    }

    if (kind == WINDOWD_CLIENT_APP)
    {
        windowd_fill_overlay_surface_info(&session->gfx.info, client_info);
    }
    else
    {
        windowd_fill_shell_surface_info(&session->gfx.info, client_info);
    }
}

static void position_client_window(
    const struct windowd_session *session,
    struct windowd_client *client,
    enum windowd_client_kind kind,
    int cascade_index)
{
    if (session == 0 || client == 0)
    {
        return;
    }

    if (kind == WINDOWD_CLIENT_APP)
    {
        windowd_place_overlay_window(
            &session->gfx.info,
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

    /* Pedido de cierre: primero el flag, despues el wake. En ese orden, un
     * cliente que resetea el evento y vuelve a mirar el header no se lo pierde. */
    if (client->header != 0)
    {
        client->header->flags |= SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN;
    }
    if (client->wake_event_fd >= 0)
    {
        (void)event_set(client->wake_event_fd);
    }
    /* Una ventana con dueno comparte el pid del proceso, que sigue vivo y es de
     * su ventana principal: ni se mata ni se espera. */
    if (client->pid > 0 && client->owner_slot < 0)
    {
        if (terminate_client)
        {
            (void)savanxp_kill((int)client->pid, SAVANXP_SIGKILL);
        }
        (void)savanxp_waitpid((int)client->pid, &status);
    }
    close_fd_if_needed(&client->events_write_fd);
    close_fd_if_needed(&client->wake_event_fd);
    close_fd_if_needed(&client->shell_request_read_fd);
    if (client->mapped_view != 0 && !result_is_error((long)client->mapped_view))
    {
        (void)unmap_view(client->mapped_view);
    }
    if (client->window_list != 0)
    {
        (void)unmap_view(client->window_list);
    }
    reset_client(client);
}

/*
 * Lanza el proceso de un cliente y le cablea el protocolo v4
 * (savanxp/wm_protocol.h). windowd se queda con DOS descriptores por cliente:
 * el extremo de escritura del pipe de eventos y el evento de wake. La seccion
 * se cierra apenas el hijo la hereda -- el WM sigue usando su mapeo, que la
 * mantiene viva --, y el evento de submit es el de la sesion.
 *
 * `shell_role` cablea ademas los canales opcionales de
 * savanxp/wm_shell_protocol.h (lista de ventanas y pedidos). Solo lo pide la
 * barra de tareas, asi que su pipe de pedidos es un fd en todo el sistema y no
 * uno por ventana.
 */
/*
 * Crea la seccion de la superficie de `client` con el tamano de surface_info, la
 * mapea en el WM y le escribe el header v4. Devuelve el fd de la seccion, que el
 * llamador le entrega al cliente -- por fork o por la cola de handles -- y
 * despues cierra; el mapeo del WM la mantiene viva. -1 si falla, sin dejar
 * nada abierto ni mapeado.
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
    /* Entero, incluidos los pedidos: un launch_head basura seria una cola de
     * launches fantasma. */
    memset(header, 0, sizeof(*header));
    header->magic = SAVANXP_GPU_CLIENT_SURFACE_MAGIC;
    header->command_offset = (uint32_t)sizeof(*header);
    header->pixels_offset = (uint32_t)pixels_offset;
    header->info = client->surface_info;
    header->version = SAVANXP_GPU_CLIENT_SURFACE_VERSION_4;
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

static int start_client_process(
    struct windowd_client *client,
    const char *path,
    const char *argument,
    int submit_event_fd,
    int shell_role)
{
    int section_fd = -1;
    int window_list_section_fd = -1;
    int events_pipe[2] = {-1, -1};
    int shell_request_pipe[2] = {-1, -1};
    int wake_event = -1;
    const char *argv[3] = {path, argument, 0};
    int argc = (argument != 0 && argument[0] != '\0') ? 2 : 1;
    long pid;

    if (client == 0 || path == 0 || submit_event_fd < 0 ||
        client->surface_info.width == 0 || client->surface_info.height == 0 || client->surface_info.buffer_size == 0)
    {
        return -1;
    }

    section_fd = create_client_surface(client);
    if (section_fd < 0)
    {
        return -1;
    }

    wake_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    if (wake_event < 0 || savanxp_pipe(events_pipe) < 0)
    {
        goto fail;
    }
    /* El extremo de ESCRITURA va en no-bloqueante: el WM no puede quedar
     * bloqueado por un cliente que no drena sus eventos. Pasa de verdad al
     * lanzar una app -- raise_overlay la hace activa al instante, pero tarda en
     * empezar a leer (carga de disco, gfx_open copiando la superficie), y
     * mientras tanto cada movimiento del mouse va a su pipe. Con writes
     * bloqueantes, el pipe se llena y se congela la sesion entera. */
    if (savanxp_fcntl(events_pipe[1], SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0)
    {
        goto fail;
    }

    if (shell_role)
    {
        /* La lista va por SECCION y no por pipe: pesa cerca de un KiB y un pipe
         * puede aceptarla a medias si el cliente se atrasa, lo que
         * desincronizaria el stream para siempre. Con una seccion el WM pisa el
         * contenido y el cliente lee el ultimo estado, que es lo unico que una
         * barra de tareas necesita. */
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

        if (savanxp_pipe(shell_request_pipe) < 0)
        {
            goto fail;
        }
        /* El WM lee los pedidos sin bloquearse, igual que el resto de sus
         * canales de entrada. */
        if (savanxp_fcntl(shell_request_pipe[0], SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0)
        {
            goto fail;
        }
    }

    pid = savanxp_fork();
    if (pid < 0)
    {
        goto fail;
    }
    if (pid == 0)
    {
        const int last_target = shell_role ? SAVANXP_WM_SHELL_FD_LAST : SAVANXP_WM_FD_LAST;
        int child_section = child_relocate_above(section_fd, last_target);
        int child_events = child_relocate_above(events_pipe[0], last_target);
        int child_wake = child_relocate_above(wake_event, last_target);
        int child_submit = child_relocate_above(submit_event_fd, last_target);

        if (child_section < 0 || child_events < 0 || child_wake < 0 || child_submit < 0 ||
            savanxp_dup2(child_section, SAVANXP_WM_FD_SECTION) < 0 ||
            savanxp_dup2(child_events, SAVANXP_WM_FD_EVENTS) < 0 ||
            savanxp_dup2(child_wake, SAVANXP_WM_FD_WAKE_EVENT) < 0 ||
            savanxp_dup2(child_submit, SAVANXP_WM_FD_SUBMIT_EVENT) < 0)
        {
            exit(1);
        }
        if (shell_role)
        {
            int child_window_list = child_relocate_above(window_list_section_fd, last_target);
            int child_shell_request = child_relocate_above(shell_request_pipe[1], last_target);

            if (child_window_list < 0 || child_shell_request < 0 ||
                savanxp_dup2(child_window_list, SAVANXP_WM_FD_WINDOW_LIST) < 0 ||
                savanxp_dup2(child_shell_request, SAVANXP_WM_FD_SHELL_REQUEST) < 0)
            {
                exit(1);
            }
        }

        child_close_above(last_target);
        {
            long exec_result = exec(path, argv, argc);
            if (exec_result < 0)
            {
                eprintf("desktop: exec failed for %s (%s)\n", path, result_error_string(exec_result));
            }
        }
        exit(1);
    }

    /* Lo que ya tiene el hijo se suelta ANTES de leer el .sxe de abajo: con la
     * sesion llena, cada fd transitorio cuenta. */
    close_fd_if_needed(&section_fd);
    close_fd_if_needed(&window_list_section_fd);
    close_fd_if_needed(&events_pipe[0]);
    close_fd_if_needed(&shell_request_pipe[1]);

    {
        size_t path_length = strlen(path);
        if (path_length >= sizeof(client->path))
        {
            path_length = sizeof(client->path) - 1;
        }
        memcpy(client->path, path, path_length);
        client->path[path_length] = '\0';
    }
    /*
     * Unico lugar donde el WM toca disco por cliente: se lee el .sxe del
     * binario recien lanzado para sacar titulo, icono y accent
     * (docs/SXE_FORMAT.md, fase 4). El costo esta acotado por la cantidad de
     * ventanas abiertas, no por el tamano de un directorio, y se paga al crear
     * la ventana -- que ya es el momento mas caro del ciclo. No puede fallar:
     * sin recursos se queda con el fallback de la tabla.
     */
    windowd_presentation_load(&client->presentation, client->path);
    client->pid = pid;
    client->events_write_fd = events_pipe[1];
    client->wake_event_fd = wake_event;
    client->shell_request_read_fd = shell_request_pipe[0];
    client->launch_tail = 0;
    client->size_hint_consumed_sequence = 0;
    return 0;

fail:
    close_fd_if_needed(&shell_request_pipe[0]);
    close_fd_if_needed(&shell_request_pipe[1]);
    if (client->window_list != 0)
    {
        unmap_view(client->window_list);
        client->window_list = 0;
    }
    close_fd_if_needed(&window_list_section_fd);
    close_fd_if_needed(&events_pipe[0]);
    close_fd_if_needed(&events_pipe[1]);
    close_fd_if_needed(&wake_event);
    close_fd_if_needed(&section_fd);
    destroy_client_instance(client, 0);
    return -1;
}

static void destroy_shell_client(struct windowd_session *session, int terminate_client)
{
    if (session == 0)
    {
        return;
    }

    destroy_client_instance(&session->shell_client, terminate_client);
    activate_shell(session);
}

static void destroy_background_client(struct windowd_session *session, int terminate_client)
{
    if (session == 0)
    {
        return;
    }

    /* Pasivo: no toca foco ni active_client_kind (a diferencia del terminal). */
    destroy_client_instance(&session->background_client, terminate_client);
}

/* Lanza shellui como cliente de fondo: superficie frameless full-screen al
 * origen (kind SHELL) compuesta al fondo del z-order. No activa foco. */
static int launch_background_client(struct windowd_session *session)
{
    struct windowd_client *client = 0;

    if (session == 0)
    {
        return -1;
    }

    destroy_background_client(session, 1);
    client = &session->background_client;
    reset_client(client);
    fill_client_surface_info(session, WINDOWD_CLIENT_SHELL, &client->surface_info);
    position_client_window(session, client, WINDOWD_CLIENT_SHELL, 0);
    return start_client_process(client, k_background_client_path, 0, session->submit_event_fd, 0);
}

/* La barra de tareas es un cliente con rol de shell: su superficie ocupa la
 * franja al pie del display y se compone por encima de las ventanas normales,
 * pero por debajo de una app a pantalla completa. No recibe foco ni entra en la
 * lista de tareas: no es una ventana, es el shell. */
static int launch_taskbar_client(struct windowd_session *session)
{
    struct windowd_client *client = 0;
    struct sx_rect strip;

    if (session == 0)
    {
        return -1;
    }

    destroy_client_instance(&session->taskbar_client, 1);
    client = &session->taskbar_client;
    reset_client(client);

    strip = windowd_taskbar_rect(&session->gfx.info);
    client->surface_info = session->gfx.info;
    client->surface_info.width = (uint32_t)strip.width;
    client->surface_info.height = (uint32_t)strip.height;
    client->surface_info.pitch = (uint32_t)strip.width * 4u;
    client->surface_info.buffer_size = client->surface_info.pitch * (uint32_t)strip.height;
    client->window_x = strip.x;
    client->window_y = strip.y;
    client->window_width = strip.width;
    client->window_height = strip.height;
    client->frame_visible = 0;

    if (start_client_process(client, k_taskbar_client_path, 0, session->submit_event_fd, 1) < 0)
    {
        return -1;
    }
    return 0;
}

/* Popup de layout de teclado: mismo molde que la taskbar (rect a mano,
 * frame_visible=0, sin pasar por fill_client_surface_info/position_client_window
 * -- esas dos solo distinguen APP cascada-decorada de SHELL fullscreen/franja,
 * no sirven para un rect chico arbitrario). Anclado en la esquina inferior
 * derecha, pegado arriba de la franja de la taskbar. Un pedido repetido
 * reinicia la instancia en vez de duplicarla, igual que la taskbar. */
static int launch_keyboard_popup_client(struct windowd_session *session)
{
    struct windowd_client *client = 0;
    struct sx_rect strip;

    if (session == 0)
    {
        return -1;
    }

    destroy_client_instance(&session->keyboard_popup_client, 1);
    client = &session->keyboard_popup_client;
    reset_client(client);

    strip = windowd_taskbar_rect(&session->gfx.info);
    client->surface_info = session->gfx.info;
    client->surface_info.width = (uint32_t)WINDOWD_KEYBOARD_POPUP_WIDTH;
    client->surface_info.height = (uint32_t)WINDOWD_KEYBOARD_POPUP_HEIGHT;
    client->surface_info.pitch = (uint32_t)WINDOWD_KEYBOARD_POPUP_WIDTH * 4u;
    client->surface_info.buffer_size = client->surface_info.pitch * (uint32_t)WINDOWD_KEYBOARD_POPUP_HEIGHT;
    client->window_x = strip.x + strip.width - 4 - WINDOWD_KEYBOARD_POPUP_WIDTH;
    client->window_y = strip.y - WINDOWD_KEYBOARD_POPUP_HEIGHT;
    client->window_width = WINDOWD_KEYBOARD_POPUP_WIDTH;
    client->window_height = WINDOWD_KEYBOARD_POPUP_HEIGHT;
    client->frame_visible = 0;

    return start_client_process(client, k_keyboard_popup_client_path, 0, session->submit_event_fd, 0);
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

    if (session == 0 || session->taskbar_client.window_list == 0)
    {
        return;
    }
    list = session->taskbar_client.window_list;

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
    for (index = -1; index < WINDOWD_MAX_OVERLAY_CLIENTS && written < (int)SAVANXP_WM_MAX_WINDOWS; ++index)
    {
        int is_shell = index < 0;
        int slot = is_shell ? -1 : index;
        const struct windowd_client *client = is_shell
            ? &session->shell_client
            : &session->overlay_clients[index];
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
        if (is_shell)
        {
            if (session->active_client_kind == WINDOWD_CLIENT_SHELL)
            {
                entry->flags |= SAVANXP_WM_WINDOW_FLAG_ACTIVE;
            }
        }
        else
        {
            if (session->active_client_kind == WINDOWD_CLIENT_APP &&
                slot == overlay_root_slot(session, session->active_overlay_slot))
            {
                entry->flags |= SAVANXP_WM_WINDOW_FLAG_ACTIVE;
            }
            if (client->minimized)
            {
                entry->flags |= SAVANXP_WM_WINDOW_FLAG_MINIMIZED;
            }
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

/* Resuelve un window_id (pid) a un cliente. Devuelve el slot del overlay, o -1
 * con is_shell en 1 si el id es el del shell. */
static int resolve_window_id(const struct windowd_session *session, uint32_t window_id, int *is_shell)
{
    int slot;

    if (is_shell != 0)
    {
        *is_shell = 0;
    }
    if (session == 0 || window_id == 0)
    {
        return -1;
    }
    if (session->shell_client.pid > 0 && (uint32_t)session->shell_client.pid == window_id)
    {
        if (is_shell != 0)
        {
            *is_shell = 1;
        }
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

static void service_shell_requests(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    struct savanxp_wm_shell_request request;

    if (session == 0 || session->taskbar_client.shell_request_read_fd < 0)
    {
        return;
    }

    while (savanxp_read(session->taskbar_client.shell_request_read_fd, &request, sizeof(request)) ==
           (long)sizeof(request))
    {
        int is_shell = 0;
        int slot = resolve_window_id(session, request.window_id, &is_shell);

        if (request.action == SAVANXP_WM_SHELL_ACTIVATE)
        {
            if (is_shell)
            {
                activate_shell(session);
            }
            else if (overlay_slot_valid(slot))
            {
                if (session->overlay_clients[slot].minimized)
                {
                    restore_overlay_client(session, dirty, slot);
                }
                raise_overlay(session, slot);
                windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
            }
        }
        else if (request.action == SAVANXP_WM_SHELL_MINIMIZE)
        {
            if (!is_shell && overlay_slot_valid(slot) && !session->overlay_clients[slot].minimized)
            {
                minimize_overlay_client(session, dirty, slot);
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
 *   manda el CLOSE (docs/OWNED_WINDOWS.md).
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

    /* A pantalla completa la ventana tapaba todo, y cerrarla ademas devuelve el
     * modo de video: hay que repintar la pantalla entera, no su marco. */
    if (client->fullscreen)
    {
        destroy_overlay_client(session, slot, 1);
        windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
        return 1;
    }
    /* El danio se toma antes de destruir: con la ventana se van sus dialogos,
     * que pueden quedar fuera de su marco. */
    dirty_overlay_family(session, dirty, slot);
    destroy_overlay_client(session, slot, 1);
    /* La activacion pasa a otra ventana, y su barra de titulo cambia de color.
     * El camino del mouse la repinta al final de su evento; Alt+F4 no tiene ese
     * final, asi que se hace aca para los dos. */
    if (session->active_client_kind == WINDOWD_CLIENT_APP && overlay_slot_valid(session->active_overlay_slot))
    {
        windowd_dirty_rect_add_client(dirty, overlay_client_at_const(session, session->active_overlay_slot));
    }
    return 1;
}

static int launch_shell_client(struct windowd_session *session, const char *path)
{
    struct windowd_client *client = 0;

    if (session == 0 || path == 0)
    {
        return -1;
    }

    destroy_shell_client(session, 1);
    client = &session->shell_client;
    reset_client(client);
    fill_client_surface_info(session, WINDOWD_CLIENT_SHELL, &client->surface_info);
    position_client_window(session, client, WINDOWD_CLIENT_SHELL, 0);
    if (start_client_process(client, path, 0, session->submit_event_fd, 0) < 0)
    {
        return -1;
    }
    activate_shell(session);
    return 0;
}

/* launch_flags: SAVANXP_DESKTOP_LAUNCH_FLAG_*, declarados por quien pide el
 * launch. El WM ya no consulta ningun catalogo de aplicaciones para decidir
 * como dimensionar la superficie: si nadie declara el flag, la app arranca en
 * ventana. Consecuencia conocida: un lanzador que no pase flags (filesapp)
 * abre Doom sin pre-sizing y por lo tanto sin F11. El arreglo de fondo es que
 * el programa pida fullscreen en runtime -- el mode-setting ya funciona --, no
 * que el WM vuelva a conocer un catalogo. */
static int launch_overlay_client(
    struct windowd_session *session,
    const char *path,
    const char *argument,
    uint32_t launch_flags)
{
    struct windowd_client *client = 0;
    int slot = -1;

    if (session == 0 || path == 0)
    {
        return -1;
    }

    slot = find_free_overlay_slot(session);
    if (!overlay_slot_valid(slot))
    {
        eprintf("desktop: no free overlay slots for %s\n", path);
        return -1;
    }

    client = &session->overlay_clients[slot];
    reset_client(client);
    fill_client_surface_info(session, WINDOWD_CLIENT_APP, &client->surface_info);
    if ((launch_flags & SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN) != 0)
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
    position_client_window(session, client, WINDOWD_CLIENT_APP, client->cascade_index);
    if (start_client_process(client, path, argument, session->submit_event_fd, 0) < 0)
    {
        return -1;
    }
    append_overlay_to_order(session, slot);
    raise_overlay(session, slot);
    return 0;
}

static int relaunch_shell_client(struct windowd_session *session)
{
    return launch_shell_client(session, k_shellapp_path);
}

static int open_compositor_session(struct windowd_session *session)
{
    int result = 0;
    int slot;

    memset(session, 0, sizeof(*session));
    windowd_compositor_connection_init(&session->compositor);
    session->input_fd = -1;
    session->mouse_fd = -1;
    session->submit_event_fd = -1;
    session->hw_cursor_enabled = 0;
    session->active_client_kind = WINDOWD_CLIENT_SHELL;
    session->active_overlay_slot = -1;
    session->fullscreen_slot = -1;
    session->overlay_count = 0;
    /* -1 y no 0: con 0 el primer click sobre la fila 0 pareceria doble click. */
    session->tasklist_last_click_index = -1;
    /* Idem: memset dejaria el slot 0 como "redimensionando". */
    session->resize_slot = -1;
    session->resize_edges = WINDOWD_RESIZE_EDGE_NONE;
    reset_client(&session->background_client);
    reset_client(&session->taskbar_client);
    reset_client(&session->shell_client);
    reset_client(&session->keyboard_popup_client);
    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        reset_client(&session->overlay_clients[slot]);
        session->overlay_order[slot] = -1;
    }

    result = windowd_compositor_open(&session->compositor);
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

    /* Uno solo para toda la sesion: cada cliente lo recibe en
     * SAVANXP_WM_FD_SUBMIT_EVENT y el loop lo resetea antes de revisarlos a
     * todos. Tiene que existir antes del primer launch. */
    session->submit_event_fd = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    if (session->submit_event_fd < 0)
    {
        return windowd_stage_failed("create submit event", session->submit_event_fd);
    }

    memset(session->compositor.framebuffer, 0, session->gfx.info.buffer_size);
    windowd_set_backbuffer(session->compositor.framebuffer);
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
    destroy_shell_client(session, 1);
    destroy_background_client(session, 1);
    close_fd_if_needed(&session->input_fd);
    close_fd_if_needed(&session->mouse_fd);
    close_fd_if_needed(&session->submit_event_fd);
    (void)sync_pending_present(session, 1, 0);
    windowd_compositor_close(&session->compositor);
    windowd_set_backbuffer(0);
}


static int service_client_batches(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    struct windowd_client *client)
{
    int slot = -1;
    int is_shell = 0;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0)
    {
        return 0;
    }

    slot = overlay_slot_for_client_ptr(session, client);
    is_shell = client == &session->shell_client;
    if (consume_client_present_batches(session, dirty, client) < 0)
    {
        if (is_shell)
        {
            destroy_shell_client(session, 1);
            if (relaunch_shell_client(session) < 0)
            {
                puts_fd(2, "desktop: failed to relaunch shellapp\n");
                return -1;
            }
        }
        else if (overlay_slot_valid(slot))
        {
            destroy_overlay_client(session, slot, 1);
        }
        windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
    }

    return 0;
}

static int reap_dead_clients(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    int slot;

    if (session == 0 || dirty == 0)
    {
        return 0;
    }

    if (session->background_client.pid > 0 && !windowd_process_alive(session->background_client.pid))
    {
        /* Sin relaunch en A2.2: al morir el cliente de fondo caemos al wallpaper
         * dibujado por windowd (fallback), para no arriesgar un spin-loop si
         * shellui crashea al arrancar. */
        destroy_background_client(session, 0);
        windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
    }

    if (session->shell_client.pid > 0 && !windowd_process_alive(session->shell_client.pid))
    {
        destroy_shell_client(session, 0);
        if (relaunch_shell_client(session) < 0)
        {
            puts_fd(2, "desktop: failed to relaunch shellapp\n");
            return -1;
        }
        windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
    }

    if (session->keyboard_popup_client.pid > 0 && !windowd_process_alive(session->keyboard_popup_client.pid))
    {
        /* Se cierra solo tras elegir un layout (exit(0)): esto es el camino
         * normal de cierre, no una caida -- sin relaunch. */
        destroy_client_instance(&session->keyboard_popup_client, 0);
        windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
    }

    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        struct windowd_client *client = &session->overlay_clients[slot];
        if (client->pid > 0 && !windowd_process_alive(client->pid))
        {
            destroy_overlay_client(session, slot, 0);
            windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
        }
    }

    return 0;
}

/*
 * Drena la cola de launches del header (savanxp_wm_client_requests). El
 * cliente es otro proceso y escribe esa memoria cuando quiere, asi que cada
 * entrada se COPIA antes de validarla -- validar in situ y usar despues dejaria
 * que la cambie en el medio -- y la cola del WM es launch_tail propio, no el
 * del header.
 */
static int service_client_launch_requests(struct windowd_session *session, struct windowd_dirty_rect *dirty, struct windowd_client *client)
{
    struct savanxp_desktop_launch_request request;
    uint32_t head = 0;
    int woke_client = 0;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0 || client->header == 0)
    {
        return 0;
    }

    head = client->header->requests.launch_head;
    if (head - client->launch_tail > SAVANXP_WM_LAUNCH_QUEUE_CAPACITY)
    {
        /* Un head que se adelanta mas que la cola no lo produce el runtime:
         * se descarta lo pendiente en vez de leer entradas que no existen. */
        eprintf("desktop: invalid launch queue from %s\n", client->path[0] != '\0' ? client->path : "?");
        client->launch_tail = head;
        client->header->requests.launch_tail = head;
        return 0;
    }

    while (client->launch_tail != head)
    {
        memcpy(&request,
            &client->header->requests.launch[client->launch_tail % SAVANXP_WM_LAUNCH_QUEUE_CAPACITY],
            sizeof(request));
        client->launch_tail += 1u;
        client->header->requests.launch_tail = client->launch_tail;
        woke_client = 1;

        request.path[SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY - 1u] = '\0';
        request.argument[SAVANXP_DESKTOP_LAUNCH_ARG_CAPACITY - 1u] = '\0';
        /* El popup de layout de teclado no es una ventana mas: se ignora
         * path/argument (el binario es fijo, lo conoce el WM) y se posiciona
         * anclado, no cascada/decorado. */
        if ((request.flags & SAVANXP_DESKTOP_LAUNCH_FLAG_TASKBAR_POPUP) != 0)
        {
            if (launch_keyboard_popup_client(session) < 0)
            {
                eprintf("desktop: failed to launch keyboard layout popup\n");
                continue;
            }
            windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
            continue;
        }
        if (request.path[0] != '/')
        {
            eprintf("desktop: invalid launch request from %s\n", client->path[0] != '\0' ? client->path : "?");
            continue;
        }
        /* Se ignoran los bits desconocidos: un cliente viejo o mal formado no
         * debe poder pedir modos que el WM no entiende. */
        if (launch_overlay_client(
                session,
                request.path,
                request.argument,
                request.flags & SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN) < 0)
        {
            eprintf("desktop: failed to launch requested app %s\n", request.path);
            continue;
        }
        windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
    }

    /* Un cliente con la cola llena espera el wake para volver a mirar. */
    if (woke_client && client->wake_event_fd >= 0)
    {
        (void)event_set(client->wake_event_fd);
    }
    return 0;
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

    windowd_place_overlay_window(&session->gfx.info, &client->surface_info, client->cascade_index, &x, &y, &width, &height);
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
 * (docs/OWNED_WINDOWS.md). Todo lo que cuesta sale de lo que ya existe: un slot
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
        request->width + (uint32_t)(WINDOWD_WINDOW_BORDER * 2) > session->gfx.info.width ||
        request->height + (uint32_t)(WINDOWD_WINDOW_TITLEBAR_HEIGHT + WINDOWD_WINDOW_BORDER) > session->gfx.info.height)
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
        &session->gfx.info, client->window_width, client->window_height, &client->window_x, &client->window_y);
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

/* Drena el pedido de ventana del header (savanxp_wm_client_requests.window).
 * Como todo lo que viene del header: se copia, se valida y recien ahi se usa.
 * Solo la ventana PRINCIPAL de un overlay puede tener ventanas con dueno; para
 * cualquier otro cliente el pedido se contesta con error, que el runtime toma
 * como "segui dibujando el dialogo adentro". */
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

    if (!overlay_slot_valid(slot) || client->owner_slot >= 0)
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
 * static compositor-drawn content" report. Drives the exact failing scenario
 * headlessly: a static power-confirm dialog, cursor moves ONTO a probe point,
 * then OFF it with only cursor-sized damage, and we read the backbuffer at the
 * old position back. If the compose restores it, backbuffer == clean baseline;
 * any residue means the old cursor footprint was not repainted. */
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

    if (open_compositor_session(&session) < 0)
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
    windowd_draw_desktop(&session, 4, 4, &dirty);
    (void)present_frame(&session, &dirty);
    (void)sync_pending_present(&session, 1, 0);
    windowd_dirty_rect_reset(&dirty);
    for (row = 0; row < fh; ++row)
    {
        for (col = 0; col < fw; ++col)
        {
            baseline_block[(row * fw) + col] = fb[((size_t)(fy + row) * stride) + (size_t)(fx + col)];
        }
    }

    /* Frame B: move cursor ONTO Q. Only cursor-sized damage (old corner + Q). */
    windowd_dirty_rect_add_cursor(&dirty, info, 4, 4, SAVANXP_CURSOR_ARROW);
    windowd_dirty_rect_add_cursor(&dirty, info, qx, qy, SAVANXP_CURSOR_ARROW);
    windowd_draw_desktop(&session, qx, qy, &dirty);
    (void)present_frame(&session, &dirty);
    (void)sync_pending_present(&session, 1, 0);
    windowd_dirty_rect_reset(&dirty);
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

    /* Frame C: move cursor OFF Q to R. Only cursor damage (old Q + new R). The
     * backbuffer at Q must return to the clean baseline. */
    windowd_dirty_rect_add_cursor(&dirty, info, qx, qy, SAVANXP_CURSOR_ARROW);
    windowd_dirty_rect_add_cursor(&dirty, info, rx, ry, SAVANXP_CURSOR_ARROW);
    windowd_draw_desktop(&session, rx, ry, &dirty);
    (void)present_frame(&session, &dirty);
    (void)sync_pending_present(&session, 1, 0);
    windowd_dirty_rect_reset(&dirty);
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

/* Descriptores que a windowd le tienen que sobrar con todas las ventanas
 * abiertas: el cliente de shell (la terminal), que todavia se puede abrir, mas
 * los fds transitorios de un launch y la lectura del .sxe. */
#define WINDOWD_SELFTEST_FD_HEADROOM 8
/* Una app recien arrancada tiene stdio, los cuatro del protocolo y lo que abra
 * su runtime. Si heredara la tabla de windowd serian decenas. */
#define WINDOWD_SELFTEST_APP_FD_MAX 20
#define WINDOWD_SELFTEST_START_DEADLINE_MS 15000ul

static int selftest_handle_count(long pid, uint32_t *handles)
{
    struct savanxp_process_info info;
    unsigned long index = 0;

    memset(&info, 0, sizeof(info));
    while (proc_info(index, &info) > 0)
    {
        ++index;
        if (info.state != SAVANXP_PROC_UNUSED && (long)info.pid == pid)
        {
            *handles = info.handle_count;
            return 0;
        }
    }
    return -1;
}

/*
 * Capacidad de la sesion (docs/WM_SUBSYSTEM.md, "Descriptor budget"): con la
 * barra de tareas -- el cliente con mas canales -- y el popup de teclado vivos,
 * TODOS los slots de ventana se tienen que poder llenar, a windowd le tiene que
 * sobrar tabla, y cada app tiene que arrancar con la suya limpia. Con el
 * protocolo v3 la sesion no pasaba de la mitad de los slots.
 */
static int windowd_capacity_selftest(struct windowd_session *session)
{
    const char *filler_path = "/bin/aboutapp";
    unsigned long deadline = 0;
    uint32_t windowd_handles = 0;
    uint32_t handles_after_refusal = 0;
    uint32_t app_handles = 0;
    long last_pid = -1;
    int windows = 0;
    int slot;

    if (launch_taskbar_client(session) < 0 || launch_keyboard_popup_client(session) < 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL capacidad: no arrancaron la barra y el popup\n");
        return 1;
    }

    while ((slot = find_free_overlay_slot(session)) >= 0)
    {
        if (launch_overlay_client(session, filler_path, 0, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE) < 0)
        {
            printf("DESKTOP SMOKE FAIL capacidad: la ventana del slot %d no arranco\n", slot);
            return 1;
        }
        last_pid = session->overlay_clients[slot].pid;
    }

    /* Recien con el primer frame sometido la app paso el exec y gfx_open: antes
     * de eso su tabla todavia es la copia de la de windowd. */
    deadline = uptime_ms() + WINDOWD_SELFTEST_START_DEADLINE_MS;
    for (;;)
    {
        int pending = 0;

        windows = 0;
        for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
        {
            const struct windowd_client *client = &session->overlay_clients[slot];

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

    /* Con los slots llenos, un launch mas se rechaza sin dejar nada abierto. */
    if (launch_overlay_client(session, filler_path, 0, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE) == 0 ||
        selftest_handle_count(savanxp_getpid(), &handles_after_refusal) != 0 ||
        handles_after_refusal != windowd_handles)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL capacidad: el launch de mas no se rechazo limpio\n");
        return 1;
    }

    printf("DESKTOP SMOKE capacity windows=%d windowd_fds=%u app_fds=%u\n",
        windows, (unsigned)windowd_handles, (unsigned)app_handles);

    destroy_client_instance(&session->keyboard_popup_client, 1);
    destroy_client_instance(&session->taskbar_client, 1);
    return 0;
}

/* Una vuelta de servicio para los overlays del selftest: pedidos del header y
 * frames, como el loop real, sin componer. Senalar los compuestos hace falta
 * igual: un cliente no presenta el frame siguiente hasta verlo. */
static void selftest_service_overlays(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    int slot;

    (void)event_reset(session->submit_event_fd);
    for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
    {
        service_client_window_requests(session, dirty, &session->overlay_clients[slot], slot);
        service_client_size_hints(session, dirty, &session->overlay_clients[slot], slot);
        (void)service_client_batches(session, dirty, &session->overlay_clients[slot]);
    }
    signal_composed_batches(session);
    windowd_dirty_rect_reset(dirty);
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
 * Ventanas con dueno de punta a punta (docs/OWNED_WINDOWS.md): widgetsdemo abre
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
        launch_overlay_client(session, "/bin/widgetsdemo", "--dialog-selftest", SAVANXP_DESKTOP_LAUNCH_FLAG_NONE) < 0 ||
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
        if (dialog_slot >= 0 && session->overlay_clients[dialog_slot].consumed_submit_sequence > 0)
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

    /* Alt+F4 sobre una ventana principal la cierra con su proceso, como su X. */
    if (launch_overlay_client(session, "/bin/widgetsdemo", 0, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE) < 0 ||
        session->active_overlay_slot != owner_slot ||
        !wm_handle_key(session, &alt_f4, dirty) ||
        session->overlay_clients[owner_slot].pid > 0 ||
        selftest_handle_count(savanxp_getpid(), &handles_after) != 0 || handles_after != handles_before_launch)
    {
        printf("DESKTOP SMOKE FAIL owned: Alt+F4 no cerro la ventana principal (fds=%u, esperados %u)\n",
            (unsigned)handles_after, (unsigned)handles_before_launch);
        return 1;
    }
    windowd_dirty_rect_reset(dirty);

    printf("DESKTOP SMOKE owned windows ok windowd_fds=%u\n", (unsigned)handles_dialog);
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
    const int kKillDaemonAtFrame = 20;
    int frames_presented = 0;
    int fullscreen_frames = 0;
    int iteration = 0;
    int failed = 0;
    int recovery_armed = 1;
    int recovery_validated = 0;
    int fullscreen_recovery_armed = 1;
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

    /* Antes de levantar la sesion: la presentacion se resuelve leyendo del
     * disco y no depende del compositor (docs/SXE_FORMAT.md, fase 4). */
    if (windowd_presentation_selftest() != 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL presentacion desde recursos SXE\n");
        return 1;
    }

    if (open_compositor_session(&session) < 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL compositor startup\n");
        close_compositor_session(&session);
        return 1;
    }
    windowd_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);

    /* Flag explicito (no el fallback por catalogo): asi el subtest de
     * fullscreen valida el mecanismo nuevo y no depende de windowd_menu. */
    if (launch_overlay_client(&session, "/bin/gfxdemo", 0, SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN) < 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL launch gfxdemo\n");
        close_compositor_session(&session);
        return 1;
    }

    /* A2.2: ejercitar el cliente de fondo (shellui) en el mismo soak. Lo
     * lanzamos aca, lo servimos en el loop, y al final asertamos que compuso al
     * menos un frame -> prueba end-to-end de que shellui conecta, renderiza el
     * wallpaper, y windowd lo compone como capa de fondo. */
    if (launch_background_client(&session) < 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL launch background client\n");
        close_compositor_session(&session);
        return 1;
    }

    /* A2.3: progman como cliente top-level normal. Asertar que compone un frame
     * prueba end-to-end que abre su sesion gfx, pinta el grid de programas y el
     * WM lo compone como una ventana mas. */
    if (launch_overlay_client(&session, "/bin/progman", 0, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE) < 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL launch progman\n");
        close_compositor_session(&session);
        return 1;
    }

    /* Segunda instancia: progman ya arranca con la sesion (A2.4a), asi que
     * abrirlo de nuevo desde el launcher es un caso real. Cubrirlo aca porque
     * una sola instancia no ejercita el camino de dos clientes iguales. */
    if (launch_overlay_client(&session, "/bin/progman", 0, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE) < 0)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL launch segunda instancia de progman\n");
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
        service_client_cursor_hints(client);
        if (service_client_batches(&session, &dirty, client) < 0 ||
            service_client_launch_requests(&session, &dirty, client) < 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL servicing client batches\n");
            failed = 1;
            break;
        }
        /* Consumir los frames del cliente de fondo para que se vuelva drawable
         * y windowd lo componga como capa de fondo. */
        if (service_client_batches(&session, &dirty, &session.background_client) < 0)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL servicing background client\n");
            failed = 1;
            break;
        }
        /* Servir TODOS los overlays, como hace el windowd real: con un solo
         * cliente atendido, cualquier otro se quedaria esperando su composicion
         * y el escenario de dos instancias no se ejercitaria de verdad. */
        {
            int overlay_slot;
            int overlay_failed = 0;

            for (overlay_slot = 0; overlay_slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++overlay_slot)
            {
                if (overlay_slot == kSlot)
                {
                    continue; /* ya servido arriba */
                }
                service_client_size_hints(&session, &dirty, &session.overlay_clients[overlay_slot], overlay_slot);
                if (service_client_batches(&session, &dirty, &session.overlay_clients[overlay_slot]) < 0)
                {
                    overlay_failed = 1;
                    break;
                }
            }
            if (overlay_failed)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL servicing overlay clients\n");
                failed = 1;
                break;
            }
        }

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
            /* El modo bajo tiene que seguir puesto aun despues de la caida
               inyectada del daemon en el medio del fullscreen. */
            if (session.gfx.info.width != WINDOWD_FULLSCREEN_MODE_WIDTH ||
                session.gfx.info.height != WINDOWD_FULLSCREEN_MODE_HEIGHT)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL fullscreen mode after recovery\n");
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
                /* En fullscreen no hay movimiento de ventanas que genere dano, y
                   sin dano el shell no presenta. Sin presentar tampoco se entera
                   de que el daemon murio, porque la deteccion es por fallo de
                   present: forzar dano por iteracion mantiene el pipeline vivo,
                   que es lo que hace observable la recuperacion en modo bajo. */
                windowd_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
            }
            break;
        }

        /* Keep composed_sequence advancing so the client never stalls waiting
           for a free batch slot. */
        signal_composed_batches(&session);
        if (session.overlay_clients[kSlot].consumed_submit_sequence > consumed_submit)
        {
            consumed_submit = session.overlay_clients[kSlot].consumed_submit_sequence;
        }

        if (!windowd_dirty_rect_valid(&dirty))
        {
            sleep_ms(4);
            continue;
        }

        windowd_draw_desktop(&session, kCursorX, kCursorY, &dirty);
        signal_composed_batches(&session);
        if (present_frame(&session, &dirty) < 0)
        {
            /* A present that fails because the daemon link dropped (here, our own
               injected kill below) must recover transparently and keep going. */
            if (!windowd_compositor_connected(&session.compositor) &&
                recover_compositor(&session) == 0)
            {
                recovery_validated = 1;
                windowd_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
                continue;
            }
            puts_fd(2, "DESKTOP SMOKE FAIL present\n");
            failed = 1;
            break;
        }
        if (session.fullscreen_slot >= 0)
        {
            ++fullscreen_frames;
        }
        windowd_dirty_rect_reset(&dirty);

        /* Drain synchronously: block until this present retires, which fires the
           client retire event and frees its batch slots for the next frame. */
        if (sync_pending_present(&session, 1, 0) < 0)
        {
            if (!windowd_compositor_connected(&session.compositor) &&
                recover_compositor(&session) == 0)
            {
                recovery_validated = 1;
                windowd_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
                continue;
            }
            puts_fd(2, "DESKTOP SMOKE FAIL drain present\n");
            failed = 1;
            break;
        }
        ++frames_presented;

        /* Simulate a mid-session compositord crash once, then require the run to
           still reach its frame target so recovery is proven, not just compiled. */
        if (recovery_armed && frames_presented == kKillDaemonAtFrame)
        {
            recovery_armed = 0;
            if (session.compositor.pid > 0)
            {
                (void)savanxp_kill((int)session.compositor.pid, SAVANXP_SIGKILL);
            }
        }

        /* Segunda caida inyectada, esta vez con el scanout en el modo bajo de
           fullscreen. El daemon respawnea siempre en el nativo, asi que el
           shell tiene que volver a pedir su modo; si no, compondria contra una
           geometria que el scanout ya no tiene. */
        if (fullscreen_recovery_armed && session.fullscreen_slot >= 0)
        {
            fullscreen_recovery_armed = 0;
            if (session.compositor.pid > 0)
            {
                (void)savanxp_kill((int)session.compositor.pid, SAVANXP_SIGKILL);
            }
        }

        if (frames_presented >= kTargetFrames)
        {
            break;
        }
    }

    (void)sync_pending_present(&session, 1, 0);

    /* Subtest de contrapresion de input: el WM no debe bloquearse nunca por un
     * cliente que no drena. shellui drena sus eventos una vez por frame, asi que
     * entre dos pasadas le bombardeamos eventos hasta llenar el pipe y
     * verificamos que el write FALLA en vez de bloquear. Sin los extremos de
     * escritura en no-bloqueante esto congelaba windowd, y con el toda la
     * sesion: es la regresion que colgaba el escritorio al lanzar una app
     * mientras se movia el mouse. */
    if (!failed)
    {
        int dropped = 0;
        int index;

        for (index = 0; index < 65536; ++index)
        {
            if (!route_pointer(&session.background_client, 10 + (index & 31), 10, 0, 0))
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

    /* Subtest Task List (A2.4b): sin taskbar, es la unica via de vuelta para una
     * ventana minimizada. Minimizamos, verificamos que sigue enumerada, pintamos
     * un frame con el dialogo abierto (ejercita la capa TASKLIST del compositor)
     * y comprobamos que Switch To la restaura y cierra el dialogo. */
    if (!failed)
    {
        struct windowd_client *client = &session.overlay_clients[kSlot];
        int task_index = -1;
        int index;

        minimize_overlay_client(&session, &dirty, kSlot);
        if (!client->minimized)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL tasklist: minimize no aplico\n");
            failed = 1;
        }

        if (!failed)
        {
            tasklist_open(&session, &dirty);
            for (index = 0; index < windowd_task_count(&session); ++index)
            {
                int is_shell = 0;
                int slot = -1;

                if (windowd_task_client(&session, index, &is_shell, &slot) != 0 && slot == kSlot)
                {
                    task_index = index;
                    break;
                }
            }
            if (task_index < 0)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL tasklist: ventana minimizada ausente de la lista\n");
                failed = 1;
            }
        }

        if (!failed)
        {
            session.tasklist_selected = task_index;
            windowd_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
            windowd_draw_desktop(&session, kCursorX, kCursorY, &dirty);

            tasklist_switch_to(&session, &dirty, task_index);
            if (client->minimized)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL tasklist: Switch To no restauro la ventana\n");
                failed = 1;
            }
            else if (session.tasklist_open)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL tasklist: el dialogo quedo abierto tras Switch To\n");
                failed = 1;
            }
        }
    }

    /* Subtest del atajo Ctrl+Esc por el camino real de la tecla. El subtest de
     * arriba llama tasklist_open() directo, asi que no cubre la deteccion del
     * modificador -- que es justo lo que cambio al pasar de seguir Ctrl a mano
     * a leerlo de savanxp_input_event.modifiers. Se prueban los dos lados: con
     * el modificador abre, y sin el la tecla NO abre nada y se rutea al
     * cliente, que es lo que hace que ESC a secas siga sirviendo para cerrar
     * una ventana. */
    if (!failed)
    {
        struct savanxp_input_event key_event;
        int consumed;

        if (session.tasklist_open)
        {
            tasklist_close(&session, &dirty);
        }

        key_event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
        key_event.key = SAVANXP_KEY_ESC;
        key_event.ascii = 0;
        key_event.modifiers = 0;
        consumed = wm_handle_key(&session, &key_event, &dirty);
        if (consumed || session.tasklist_open)
        {
            puts_fd(2, "DESKTOP SMOKE FAIL ctrl+esc: ESC sin modificador no tendria que abrir el Task List\n");
            failed = 1;
        }

        if (!failed)
        {
            key_event.modifiers = SAVANXP_KEY_MOD_CTRL;
            consumed = wm_handle_key(&session, &key_event, &dirty);
            if (!consumed || !session.tasklist_open)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL ctrl+esc: el modificador no abrio el Task List\n");
                failed = 1;
            }
        }

        if (!failed)
        {
            /* Y vuelve a cerrar: el atajo alterna, no solo abre. */
            consumed = wm_handle_key(&session, &key_event, &dirty);
            if (!consumed || session.tasklist_open)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL ctrl+esc: el atajo no cerro el Task List\n");
                failed = 1;
            }
        }
    }

    /* Subtest de Alt+Tab. Mas alla del ciclado, cubre las dos cosas que lo
     * separan del Ctrl+Esc: que arranca desde la ventana ACTIVA -- para que el
     * primer Tab caiga en la ultima usada y no en una cualquiera -- y que
     * soltar Alt confirma. */
    if (!failed)
    {
        struct savanxp_input_event key_event;
        int count;
        int active_index;
        int expected;

        if (session.tasklist_open)
        {
            tasklist_close(&session, &dirty);
        }
        session.tasklist_alt_cycle = 0;

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
            if (wm_handle_key(&session, &key_event, &dirty) || session.tasklist_open)
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
            if (!wm_handle_key(&session, &key_event, &dirty) || !session.tasklist_open)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: no abrio el switcher\n");
                failed = 1;
            }
            else if (session.tasklist_selected != expected)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: no arranco desde la ventana activa\n");
                failed = 1;
            }
        }

        /* El segundo Tab sigue bajando. */
        if (!failed)
        {
            expected = (session.tasklist_selected + count - 1) % count;
            (void)wm_handle_key(&session, &key_event, &dirty);
            if (session.tasklist_selected != expected)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: el segundo Tab no avanzo\n");
                failed = 1;
            }
        }

        /* Alt+Shift+Tab vuelve. */
        if (!failed)
        {
            expected = (session.tasklist_selected + 1) % count;
            key_event.modifiers = SAVANXP_KEY_MOD_ALT | SAVANXP_KEY_MOD_SHIFT;
            (void)wm_handle_key(&session, &key_event, &dirty);
            if (session.tasklist_selected != expected)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: Alt+Shift+Tab no volvio\n");
                failed = 1;
            }
        }

        /* Soltar Alt confirma. El KEY_UP llega SIN el flag de Alt, igual que
         * del driver real, que baja su estado antes de emitir. */
        if (!failed)
        {
            int target = session.tasklist_selected;
            int is_shell = 0;
            int slot = -1;

            (void)windowd_task_client(&session, target, &is_shell, &slot);
            key_event.type = SAVANXP_INPUT_EVENT_KEY_UP;
            key_event.key = SAVANXP_KEY_ALT;
            key_event.modifiers = 0;
            if (!wm_handle_key(&session, &key_event, &dirty) || session.tasklist_open)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: soltar Alt no cerro el switcher\n");
                failed = 1;
            }
            else if (is_shell)
            {
                if (session.active_client_kind != WINDOWD_CLIENT_SHELL)
                {
                    puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: no activo el shell elegido\n");
                    failed = 1;
                }
            }
            else if (session.active_overlay_slot != slot)
            {
                puts_fd(2, "DESKTOP SMOKE FAIL alt+tab: no activo la ventana elegida\n");
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
    if (!failed && !recovery_validated)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL compositor reconnect not exercised\n");
        failed = 1;
    }
    /* Si la caida en modo bajo nunca se disparo, la asercion de arriba paso
       sin probar nada: la reconciliacion de modo quedaria sin cubrir. */
    if (!failed && fullscreen_recovery_armed)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL fullscreen reconnect not exercised\n");
        failed = 1;
    }
    if (!failed && session.background_client.consumed_submit_sequence < 1)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL background client never composited\n");
        failed = 1;
    }
    if (!failed && session.overlay_clients[kProgmanSlot].consumed_submit_sequence < 1)
    {
        puts_fd(2, "DESKTOP SMOKE FAIL progman never composited\n");
        failed = 1;
    }
    if (!failed && session.overlay_clients[kProgmanSlot + 1].consumed_submit_sequence < 1)
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

/* --- Task List (Ctrl+Esc) ------------------------------------------------
 *
 * Conmutador de ventanas del WM, estilo NT 3.5. Reemplaza las dos funciones que
 * daba el taskbar: restaurar una ventana minimizada y cambiar entre ventanas.
 * Enumera la misma lista que el taskbar (shell + overlays en z-order, incluidas
 * las minimizadas) via windowd_taskbar_button_*.
 */

static void tasklist_close(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    if (!session->tasklist_open)
    {
        return;
    }
    session->tasklist_open = 0;
    /* El dialogo desaparece: hay que repintar lo que tapaba. */
    windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
}

/* Indice de la ventana activa dentro de la lista de tareas. El orden de la
 * lista es shell primero y despues los overlays de ABAJO hacia arriba, asi que
 * la activa es la ultima y "la anterior" esta un lugar mas atras -- de ahi que
 * Alt+Tab avance con paso negativo. */
static int windowd_active_task_index(const struct windowd_session *session)
{
    int count = windowd_task_count(session);
    int index;

    for (index = 0; index < count; ++index)
    {
        int is_shell = 0;
        int slot = -1;

        if (windowd_task_client(session, index, &is_shell, &slot) == 0)
        {
            continue;
        }
        if (session->active_client_kind == WINDOWD_CLIENT_SHELL && is_shell)
        {
            return index;
        }
        /* Con un dialogo activo, la tarea activa es la de su dueno. */
        if (session->active_client_kind == WINDOWD_CLIENT_APP && !is_shell &&
            slot == overlay_root_slot(session, session->active_overlay_slot))
        {
            return index;
        }
    }
    return count > 0 ? count - 1 : 0;
}

static void tasklist_open(struct windowd_session *session, struct windowd_dirty_rect *dirty)
{
    int count = windowd_task_count(session);

    session->tasklist_open = 1;
    if (session->tasklist_selected < 0 || session->tasklist_selected >= count)
    {
        session->tasklist_selected = 0;
    }
    windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
}

/* Trae al frente la tarea seleccionada, restaurandola si estaba minimizada. */
static void tasklist_switch_to(struct windowd_session *session, struct windowd_dirty_rect *dirty, int task_index)
{
    int is_shell = 0;
    int slot = -1;
    const struct windowd_client *client = windowd_task_client(session, task_index, &is_shell, &slot);

    if (client == 0)
    {
        return;
    }
    tasklist_close(session, dirty);
    if (is_shell)
    {
        activate_shell(session);
        return;
    }
    if (!overlay_slot_valid(slot))
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

/* End Task solo aplica a overlays: el shell_client se relanza solo, asi que
 * "terminarlo" desde aca no tendria efecto observable. */
static void tasklist_end_task(struct windowd_session *session, struct windowd_dirty_rect *dirty, int task_index)
{
    int is_shell = 0;
    int slot = -1;
    const struct windowd_client *client = windowd_task_client(session, task_index, &is_shell, &slot);

    if (client == 0 || is_shell || !overlay_slot_valid(slot))
    {
        return;
    }
    destroy_overlay_client(session, slot, 1);
    if (session->tasklist_selected >= windowd_task_count(session))
    {
        session->tasklist_selected = windowd_task_count(session) - 1;
    }
    if (session->tasklist_selected < 0)
    {
        session->tasklist_selected = 0;
    }
    windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
}

/*
 * Teclado: el WM consume sus hotkeys globales (Ctrl+Esc, F11) y lo demas se
 * rutea al cliente activo. Retirado el chrome (A2.4c) ya no hay un segundo
 * turno: el shell es un proceso cliente y recibe input como cualquier app.
 */
static int wm_handle_key(
    struct windowd_session *session,
    const struct savanxp_input_event *key_event,
    struct windowd_dirty_rect *dirty)
{
    /* El estado de Ctrl viene en el evento (savanxp_input_event.modifiers), no
     * se sigue a mano: el driver es el que lo sabe de verdad, y el seguimiento
     * por KEY_DOWN/KEY_UP se trababa si se perdia un KEY_UP. La tecla igual se
     * rutea al cliente, que la necesita para sus propios atajos. */
    if (key_event->type == SAVANXP_INPUT_EVENT_KEY_DOWN &&
        key_event->key == SAVANXP_KEY_ESC &&
        (key_event->modifiers & SAVANXP_KEY_MOD_CTRL) != 0)
    {
        if (session->tasklist_open)
        {
            tasklist_close(session, dirty);
        }
        else
        {
            tasklist_open(session, dirty);
        }
        return 1;
    }

    /* Alt+Tab cicla por las ventanas usando el Task List como switcher, que es
     * exactamente para lo que existe: ya enumera lo mismo y ya sabe pintarse.
     * Mientras Alt siga apretado cada Tab mueve la seleccion, y al soltar Alt
     * se confirma -- el gesto de Windows, sin UI nueva. Alt+Shift+Tab va al
     * reves. */
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
        if (!session->tasklist_open)
        {
            /* Ciclo nuevo: se arranca desde la ventana activa para que el
             * primer Tab caiga en la ultima que se uso, no en una cualquiera. */
            tasklist_open(session, dirty);
            session->tasklist_selected = windowd_active_task_index(session);
            session->tasklist_alt_cycle = 1;
        }
        session->tasklist_selected = (session->tasklist_selected + count + step) % count;
        windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
        return 1;
    }

    /* Soltar Alt confirma el ciclo. Se mira la TECLA y no el modificador: el
     * driver baja su estado antes de emitir, asi que el KEY_UP de Alt llega ya
     * sin el flag puesto. Se exige que el Task List siga abierto porque un ESC
     * en el medio del ciclo lo cierra y ahi no hay nada que confirmar. */
    if (key_event->type == SAVANXP_INPUT_EVENT_KEY_UP &&
        key_event->key == SAVANXP_KEY_ALT &&
        session->tasklist_alt_cycle)
    {
        session->tasklist_alt_cycle = 0;
        if (session->tasklist_open)
        {
            tasklist_switch_to(session, dirty, session->tasklist_selected);
        }
        return 1;
    }

    /* Mientras esta abierto, el Task List captura el teclado: es modal. */
    if (session->tasklist_open)
    {
        int count = windowd_task_count(session);

        if (key_event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
        {
            return 1;
        }
        if (key_event->key == SAVANXP_KEY_ESC)
        {
            tasklist_close(session, dirty);
        }
        else if (count > 0 && key_event->key == SAVANXP_KEY_UP)
        {
            session->tasklist_selected = (session->tasklist_selected + count - 1) % count;
            windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
        }
        else if (count > 0 && key_event->key == SAVANXP_KEY_DOWN)
        {
            session->tasklist_selected = (session->tasklist_selected + 1) % count;
            windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
        }
        else if (count > 0 && key_event->key == SAVANXP_KEY_ENTER)
        {
            tasklist_switch_to(session, dirty, session->tasklist_selected);
        }
        else if (count > 0 && key_event->key == SAVANXP_KEY_DELETE)
        {
            tasklist_end_task(session, dirty, session->tasklist_selected);
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
    if (previous_cursor_x == cursor_x && previous_cursor_y == cursor_y &&
        session->current_cursor_shape == session->previous_cursor_shape)
    {
        return;
    }

    if (session->hw_cursor_enabled)
    {
        (void)set_hw_cursor_position(session, cursor_x, cursor_y, 1);
    }
    else
    {
        windowd_dirty_rect_add_cursor(dirty, &session->gfx.info, previous_cursor_x, previous_cursor_y, session->previous_cursor_shape);
        windowd_dirty_rect_add_cursor(dirty, &session->gfx.info, cursor_x, cursor_y, session->current_cursor_shape);
    }
}

/* Modal de puntero del WM: el Task List consume el evento entero mientras esta
 * abierto, y tiene precedencia sobre los modales del shell (es UI del WM).
 * Doble click sobre una fila equivale a Switch To. */
static int wm_pointer_handle_tasklist(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    int cursor_x,
    int cursor_y,
    int previous_cursor_x,
    int previous_cursor_y,
    uint32_t left_pressed,
    uint32_t left_was_pressed)
{
    int count;
    int task_index;
    int button_index;

    if (!session->tasklist_open)
    {
        return 0;
    }
    /* Antes de cualquier salida: el dialogo consume todos los eventos, asi que
     * si no repintamos aca el cursor se congela. */
    refresh_cursor_after_move(session, dirty, cursor_x, cursor_y, previous_cursor_x, previous_cursor_y);
    if (left_pressed == 0 || left_was_pressed != 0)
    {
        return 1; /* Consumido igual: el dialogo es modal. */
    }

    count = windowd_task_count(session);
    button_index = windowd_tasklist_button_from_point(&session->gfx.info, count, cursor_x, cursor_y);
    if (button_index >= 0)
    {
        if (button_index == 0)
        {
            tasklist_switch_to(session, dirty, session->tasklist_selected);
        }
        else if (button_index == 1)
        {
            tasklist_end_task(session, dirty, session->tasklist_selected);
        }
        else
        {
            tasklist_close(session, dirty);
        }
        return 1;
    }

    task_index = windowd_tasklist_item_from_point(
        &session->gfx.info, count, session->tasklist_selected, cursor_x, cursor_y);
    if (task_index >= 0)
    {
        unsigned long now = uptime_ms();
        int double_click = (session->tasklist_last_click_index == task_index) &&
            (now - session->tasklist_last_click_ms <= 450UL);

        session->tasklist_selected = task_index;
        windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
        if (double_click)
        {
            tasklist_switch_to(session, dirty, task_index);
            session->tasklist_last_click_index = -1;
        }
        else
        {
            session->tasklist_last_click_index = task_index;
            session->tasklist_last_click_ms = now;
        }
        return 1;
    }

    /* Click fuera del dialogo: cerrar, como cualquier modal. */
    if (!sx_rect_contains_point(windowd_tasklist_rect(&session->gfx.info, count), cursor_x, cursor_y))
    {
        tasklist_close(session, dirty);
    }
    return 1;
}

/*
 * Manejo de un evento de puntero coalescido (Fase A, ver docs/WM_SUBSYSTEM.md).
 * Saca el cuerpo per-evento del for(;;) de main() a una unidad nombrada con
 * contrato de estado explicito. Adentro, el update de cursor/hover es del WM,
 * los modales del shell (shell_pointer_handle_*) consumen, y el dispatch de
 * click izquierdo sigue siendo el chain mixto chrome/ventana que A2 bisectara.
 * El estado que persiste entre eventos (cursor, botones, drag) entra y sale por
 * puntero; se copia a locales y se escribe de vuelta en 'done'.
 */
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
    cursor_x = windowd_clamp_int(cursor_x + mouse_event.delta_x, 0, (int)session->gfx.info.width - 1);
    cursor_y = windowd_clamp_int(cursor_y + mouse_event.delta_y, 0, (int)session->gfx.info.height - 1);
    current_hover_client = top_client_at_point(session, cursor_x, cursor_y);

    session->previous_cursor_shape = session->current_cursor_shape;
    session->current_cursor_shape = resolve_cursor_shape(
        session, current_hover_client, cursor_x, cursor_y, drag_was_active);
    if (session->current_cursor_shape != session->previous_cursor_shape && session->hw_cursor_enabled)
    {
        (void)windowd_compositor_set_cursor_shape(&session->compositor, session->current_cursor_shape);
    }

    /* Unico modal que queda: el Task List (UI del WM). */
    if (wm_pointer_handle_tasklist(session, dirty, cursor_x, cursor_y,
            previous_cursor_x, previous_cursor_y, left_pressed, left_was_pressed))
    {
        last_buttons = pressed_buttons;
        goto done;
    }

    if (left_pressed != 0 && left_was_pressed == 0)
    {
        /* Sin chrome: un click solo puede caer sobre una ventana o sobre el
         * fondo, y el fondo (shellui) no recibe input. */
        if (current_hover_client != 0)
        {
            if (current_hover_client == &session->keyboard_popup_client)
            {
                /* Mismo corte que la taskbar unas lineas mas abajo, y por la
                 * misma razon: el popup tampoco es una ventana overlay, y sin
                 * este caso especial el click caia en el camino generico de
                 * ventana -- donde overlay_slot_for_client_ptr devuelve -1,
                 * anula el hover, y se pierde el click entero. */
                (void)route_pointer(current_hover_client, cursor_x, cursor_y, mouse_event.wheel, pressed_buttons);
                mouse_routed = 1;
                current_hover_client = 0;
            }
            else if (current_hover_client == &session->taskbar_client)
            {
                /* La barra de tareas no es una ventana: no se activa, no se
                 * levanta en el z-order y no tiene botones de marco. Solo recibe
                 * el click. Sin este corte caia en el camino de ventana, donde
                 * overlay_slot_for_client_ptr devuelve -1 y el hover se anulaba
                 * -- y con el se perdian TODAS las ramas siguientes, incluida la
                 * que rutea el evento al cliente. */
                (void)route_pointer(current_hover_client, cursor_x, cursor_y, mouse_event.wheel, pressed_buttons);
                mouse_routed = 1;
                current_hover_client = 0;
            }
            else if (current_hover_client == &session->shell_client)
            {
                activate_shell(session);
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
                current_hover_client != &session->shell_client &&
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
                     current_hover_client != &session->shell_client &&
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
                     current_hover_client != &session->shell_client &&
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
                     current_hover_client != &session->shell_client &&
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
                     current_hover_client != &session->shell_client &&
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
    int compositor_recoveries = 0;
    struct windowd_stats stats;

    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        return windowd_selftest();
    }

    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--cursor-repro") == 0)
    {
        return windowd_cursor_repro();
    }

    if (open_compositor_session(&session) < 0)
    {
        puts_fd(2, "desktop: compositor startup failed\n");
        close_compositor_session(&session);
        return 1;
    }
    (void)try_enable_hw_cursor(&session, cursor_x, cursor_y);
    /* windowd conserva su propio wallpaper_init para el fallback (si el cliente
     * de fondo no esta listo aun o murio, windowd dibuja el wallpaper). */
    desktop_wallpaper_init();
    /* Barra de tareas: opcional. Si no arranca, la sesion sigue viva y el Task
     * List (Ctrl+Esc) con Alt+Tab siguen siendo la via para cambiar de ventana;
     * no es motivo para tirar la sesion abajo. */
    if (launch_taskbar_client(&session) < 0)
    {
        puts_fd(2, "desktop: la barra de tareas no arranco\n");
    }
    /* Cliente de fondo (shellui): no-fatal si falla; se usa el fallback. */
    if (launch_background_client(&session) < 0)
    {
        puts_fd(2, "desktop: cliente de fondo (shellui) no arranco; uso fallback\n");
    }
    /* Program Manager: el launcher de la sesion. Es una ventana normal, sin
     * trato especial -- pero ahora es el UNICO camino para lanzar programas, asi
     * que si no arranca la sesion queda sin launcher (queda el Task List para
     * manejar lo que ya este abierto). */
    if (launch_overlay_client(&session, k_progman_path, 0, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE) < 0)
    {
        puts_fd(2, "desktop: Program Manager no arranco; sesion sin launcher\n");
    }
    windowd_stats_open(&stats);
    windowd_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);

    for (;;)
    {
        /* input + mouse + el evento de submit de la sesion. */
        struct savanxp_pollfd poll_fds[3];
        int input_poll_index = -1;
        int mouse_poll_index = -1;
        int submit_poll_index = -1;
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

        /* Wake on client frame submissions and header requests, not just the
         * 16 ms timeout (kept as a backstop). The timeout still bounds latency
         * if a wakeup is missed. */
        submit_poll_index = poll_count;
        poll_fds[poll_count].fd = session.submit_event_fd;
        poll_fds[poll_count].events = SAVANXP_POLLIN;
        poll_fds[poll_count].revents = 0;
        ++poll_count;

        if (savanxp_poll(poll_fds, (unsigned long)poll_count, 16) < 0)
        {
            break;
        }

        /* Reset BEFORE servicing: a client that submits after this point
         * re-arms the event and wakes the next poll. Every client is serviced
         * below on every pass, so one shared event is enough. */
        if ((poll_fds[submit_poll_index].revents & SAVANXP_POLLIN) != 0)
        {
            (void)event_reset(session.submit_event_fd);
        }

        if (input_poll_index >= 0 && (poll_fds[input_poll_index].revents & SAVANXP_POLLIN) != 0)
        {
            while ((count = savanxp_read(session.input_fd, &key_event, sizeof(key_event))) == (long)sizeof(key_event))
            {
                /* Sin chrome, la arbitracion se reduce a: hotkeys del WM y, si
                 * no consume, ruteo al cliente activo. */
                if (wm_handle_key(&session, &key_event, &dirty))
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
            (void)set_hw_cursor_position(&session, cursor_x, cursor_y, 1);
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
        /* Cliente de fondo (pasivo): consumimos sus frames y (a futuro, A2.3) sus
         * launch requests desde los iconos del escritorio. */
        if (service_client_batches(&session, &dirty, &session.background_client) < 0)
        {
            break;
        }
        if (service_client_launch_requests(&session, &dirty, &session.background_client) < 0)
        {
            break;
        }
        service_client_cursor_hints(&session.background_client);
        service_client_size_hints(&session, &dirty, &session.background_client, -1);
        /* Barra de tareas: sus frames, sus pedidos, y la lista republicada solo
         * cuando algo cambio -- no en cada vuelta. */
        if (service_client_batches(&session, &dirty, &session.taskbar_client) < 0)
        {
            break;
        }
        if (service_client_launch_requests(&session, &dirty, &session.taskbar_client) < 0)
        {
            break;
        }
        service_shell_requests(&session, &dirty);
        publish_window_list(&session);
        /* Popup de layout de teclado: solo presenta sus frames -- no lanza
         * nada por su cuenta, no tiene cursor hints ni size hints (rect fijo,
         * nunca los pide). */
        if (service_client_batches(&session, &dirty, &session.keyboard_popup_client) < 0)
        {
            break;
        }
        if (service_client_batches(&session, &dirty, &session.shell_client) < 0)
        {
            break;
        }
        if (service_client_launch_requests(&session, &dirty, &session.shell_client) < 0)
        {
            break;
        }
        service_client_cursor_hints(&session.shell_client);
        service_client_size_hints(&session, &dirty, &session.shell_client, -1);
        service_client_window_requests(&session, &dirty, &session.shell_client, -1);
        for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
        {
            service_client_window_requests(&session, &dirty, &session.overlay_clients[slot], slot);
            service_client_size_hints(&session, &dirty, &session.overlay_clients[slot], slot);
            if (service_client_batches(&session, &dirty, &session.overlay_clients[slot]) < 0)
            {
                break;
            }
            if (service_client_launch_requests(&session, &dirty, &session.overlay_clients[slot]) < 0)
            {
                break;
            }
            service_client_cursor_hints(&session.overlay_clients[slot]);
        }
        if (slot < WINDOWD_MAX_OVERLAY_CLIENTS)
        {
            break;
        }


        {
            int frame_ready = 1;
            int sync_result;

            windowd_stats_sync_begin(&stats);
            sync_result = sync_pending_present(&session, 0, &frame_ready);
            windowd_stats_sync_end(&stats);
            if (sync_result < 0)
            {
                if (!windowd_compositor_connected(&session.compositor) &&
                    compositor_recoveries < WINDOWD_MAX_COMPOSITOR_RECOVERIES &&
                    recover_compositor(&session) == 0)
                {
                    ++compositor_recoveries;
                    windowd_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
                    frame_ready = 1;
                }
                else
                {
                    break;
                }
            }
            if (!windowd_dirty_rect_valid(&dirty))
            {
                signal_composed_batches(&session);
                continue;
            }
            if (!frame_ready)
            {
                continue;
            }
        }

        windowd_stats_frame_begin(&stats);
        windowd_draw_desktop(&session, cursor_x, cursor_y, &dirty);
        windowd_stats_compose_done(&stats);
        signal_composed_batches(&session);
        if (present_frame(&session, &dirty) < 0)
        {
            if (!windowd_compositor_connected(&session.compositor) &&
                compositor_recoveries < WINDOWD_MAX_COMPOSITOR_RECOVERIES &&
                recover_compositor(&session) == 0)
            {
                ++compositor_recoveries;
                windowd_dirty_rect_reset(&dirty);
                continue;
            }
            puts_fd(2, "desktop: present failed\n");
            break;
        }
        compositor_recoveries = 0;
        /* El danio se mide ANTES del reset: es el del frame que se acaba
           de presentar. */
        windowd_stats_frame_end(&stats, &dirty, &session.compositor.last_present_timing);
        windowd_stats_report(&stats, 0);
        windowd_dirty_rect_reset(&dirty);
    }

    windowd_stats_report(&stats, 1);
    windowd_stats_close(&stats);
    close_compositor_session(&session);
    return 1;
}
