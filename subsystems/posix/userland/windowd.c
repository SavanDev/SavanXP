#include "libc.h"
#include "windowd_session.h"
#include "windowd_appinfo.h"
#include "desktop_wallpaper.h"
#include "windowd_layout.h"
#include "windowd_render.h"

#define WINDOWD_MAX_MOUSE_EVENTS_PER_FRAME 16
#define WINDOWD_SURFACE_PAGE_SIZE 4096u
/* Low render size used for composited fullscreen apps: the client renders here
 * and the shell scales it to the display when F11 fullscreen is active. */
#define WINDOWD_FULLSCREEN_MODE_WIDTH 640
#define WINDOWD_FULLSCREEN_MODE_HEIGHT 400

static const char *k_shellapp_path = "/bin/shellapp";
static const char *k_background_client_path = "/bin/shellui";
static const char *k_progman_path = "/bin/progman";

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
        close(*fd);
        *fd = -1;
    }
}

/*
 * El hijo remapea con dup2 los handles heredados sobre los descriptores
 * reservados del protocolo (savanxp/wm_protocol.h) antes del exec. Un
 * descriptor de ORIGEN puede caer el mismo dentro de esa ventana reservada
 * (p.ej. la seccion abierta en el fd 8 mientras el evento de shutdown se
 * dup2'ea sobre el 8), asi que cerrarlo por su numero viejo destruiria el
 * destino recien mapeado. Solo se liberan los origenes por encima de la
 * ventana; 0..2 son stdio y el resto son destinos vivos.
 *
 * El tope sale del protocolo, no de una constante propia: estaba clavado en 9
 * y se quedo corto cuando el cursor hint sumo el fd 10, dejando ese destino
 * expuesto a que lo cerraran por numero.
 */
#define WINDOWD_CLIENT_RESERVED_FD_MAX SAVANXP_WM_FD_LAST

static void close_client_setup_fd(int *fd)
{
    if (fd == 0)
    {
        return;
    }
    if (*fd > WINDOWD_CLIENT_RESERVED_FD_MAX)
    {
        close(*fd);
    }
    *fd = -1;
}

static void reset_client(struct windowd_client *client)
{
    if (client == 0)
    {
        return;
    }

    memset(client, 0, sizeof(*client));
    client->section_fd = -1;
    client->input_write_fd = -1;
    client->mouse_write_fd = -1;
    client->submit_event_fd = -1;
    client->retire_event_fd = -1;
    client->shutdown_event_fd = -1;
    client->launch_read_fd = -1;
    client->cursor_hint_read_fd = -1;
    client->size_hint_read_fd = -1;
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

static void raise_overlay(struct windowd_session *session, int slot)
{
    if (session == 0 || !overlay_slot_valid(slot) || session->overlay_clients[slot].pid <= 0)
    {
        refresh_active_state(session);
        return;
    }

    session->overlay_clients[slot].minimized = 0;
    append_overlay_to_order(session, slot);
    session->active_client_kind = WINDOWD_CLIENT_APP;
    session->active_overlay_slot = slot;
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

static void minimize_overlay_client(struct windowd_session *session, struct windowd_dirty_rect *dirty, int slot)
{
    struct windowd_client *client = overlay_client_at(session, slot);
    struct sx_rect frame_rect;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0 || client->minimized)
    {
        return;
    }

    frame_rect = windowd_client_frame_rect(client);
    windowd_dirty_rect_add(dirty, &session->gfx.info, frame_rect.x, frame_rect.y, frame_rect.width, frame_rect.height);
    client->minimized = 1;
    refresh_active_state(session);
}

static void restore_overlay_client(struct windowd_session *session, struct windowd_dirty_rect *dirty, int slot)
{
    struct windowd_client *client = overlay_client_at(session, slot);
    struct sx_rect frame_rect;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0)
    {
        return;
    }

    client->minimized = 0;
    frame_rect = windowd_client_frame_rect(client);
    windowd_dirty_rect_add(dirty, &session->gfx.info, frame_rect.x, frame_rect.y, frame_rect.width, frame_rect.height);
    raise_overlay(session, slot);
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

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0)
    {
        return;
    }

    if (!client->maximized)
    {
        client->restore_window_x = client->window_x;
        client->restore_window_y = client->window_y;
        client->restore_window_width = client->window_width;
        client->restore_window_height = client->window_height;
        windowd_work_area_bounds(&session->gfx.info, &area_x, &area_y, &area_width, &area_height);
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
    const struct windowd_client *overlay = top_overlay_client_at_point(session, x, y);

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
    if (current_hover_client != 0 &&
        current_hover_client != &session->shell_client &&
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
 * el teclado, que se recupera solo en cuanto el cliente vuelve a leer. */
static int route_packet(int fd, const void *packet, size_t size)
{
    if (fd < 0)
    {
        return 0;
    }
    return write(fd, packet, size) == (long)size;
}

/* Deliver the pointer to a client in its own surface-local coordinates, so the
 * app hit-tests in local space and stays aligned with the system cursor the
 * compositor draws. */
static int route_pointer(const struct windowd_client *client, int cursor_x, int cursor_y, uint32_t buttons)
{
    struct sx_rect surface_rect;
    struct savanxp_gui_pointer_event event;

    if (client == 0 || client->mouse_write_fd < 0)
    {
        return 0;
    }
    surface_rect = windowd_client_surface_rect(client);
    event.x = cursor_x - surface_rect.x;
    event.y = cursor_y - surface_rect.y;
    event.buttons = buttons;
    return route_packet(client->mouse_write_fd, &event, sizeof(event));
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
    if (advanced && client->retire_event_fd >= 0)
    {
        (void)event_set(client->retire_event_fd);
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
    if (advanced && client->retire_event_fd >= 0)
    {
        (void)event_set(client->retire_event_fd);
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

    if (client->submit_event_fd >= 0 &&
        client->consumed_submit_sequence >= client->header->submit_sequence)
    {
        (void)event_reset(client->submit_event_fd);
    }

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
        const struct sx_rect *rect = windowd_dirty_rect_at(dirty, index);
        if (rect == 0 || rect->width <= 0 || rect->height <= 0)
        {
            continue;
        }
        rects[rect_count++] = *rect;
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

    if (client->shutdown_event_fd >= 0)
    {
        (void)event_set(client->shutdown_event_fd);
    }
    if (client->pid > 0)
    {
        if (terminate_client)
        {
            (void)kill((int)client->pid, SAVANXP_SIGKILL);
        }
        (void)waitpid((int)client->pid, &status);
    }
    close_fd_if_needed(&client->input_write_fd);
    close_fd_if_needed(&client->mouse_write_fd);
    close_fd_if_needed(&client->submit_event_fd);
    close_fd_if_needed(&client->retire_event_fd);
    close_fd_if_needed(&client->shutdown_event_fd);
    close_fd_if_needed(&client->launch_read_fd);
    close_fd_if_needed(&client->cursor_hint_read_fd);
    close_fd_if_needed(&client->size_hint_read_fd);
    if (client->mapped_view != 0 && !result_is_error((long)client->mapped_view))
    {
        (void)unmap_view(client->mapped_view);
    }
    close_fd_if_needed(&client->section_fd);
    reset_client(client);
}

static int start_client_process(struct windowd_client *client, const char *path, const char *argument)
{
    struct savanxp_gpu_client_surface_header *header;
    unsigned long command_bytes = 0;
    unsigned long pixels_offset = 0;
    unsigned long section_size = 0;
    int input_pipe[2] = {-1, -1};
    int mouse_pipe[2] = {-1, -1};
    int launch_pipe[2] = {-1, -1};
    int cursor_hint_pipe[2] = {-1, -1};
    int size_hint_pipe[2] = {-1, -1};
    int submit_event = -1;
    int retire_event = -1;
    int shutdown_event = -1;
    const char *argv[3] = {path, argument, 0};
    int argc = (argument != 0 && argument[0] != '\0') ? 2 : 1;
    long pid;

    if (client == 0 || path == 0 ||
        client->surface_info.width == 0 || client->surface_info.height == 0 || client->surface_info.buffer_size == 0)
    {
        return -1;
    }

    command_bytes = (unsigned long)(SAVANXP_GPU_CLIENT_BATCH_CAPACITY * sizeof(struct savanxp_gpu_dirty_rect_batch));
    /* Page-align the pixel region. Older fullscreen-exclusive scanout used this
     * directly; keeping the alignment preserves the v3 client ABI. */
    pixels_offset = ((unsigned long)sizeof(*header) + command_bytes + (WINDOWD_SURFACE_PAGE_SIZE - 1u)) & ~(unsigned long)(WINDOWD_SURFACE_PAGE_SIZE - 1u);
    section_size = pixels_offset + client->surface_info.buffer_size;
    client->section_fd = (int)section_create(section_size, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (client->section_fd < 0)
    {
        return -1;
    }
    client->mapped_view = map_view(client->section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)client->mapped_view))
    {
        close_fd_if_needed(&client->section_fd);
        return -1;
    }

    header = (struct savanxp_gpu_client_surface_header *)client->mapped_view;
    header->magic = SAVANXP_GPU_CLIENT_SURFACE_MAGIC;
    header->command_offset = (uint32_t)sizeof(*header);
    header->pixels_offset = (uint32_t)pixels_offset;
    header->info = client->surface_info;
    header->version = SAVANXP_GPU_CLIENT_SURFACE_VERSION_3;
    header->flags = 0;
    header->pixel_format = SAVANXP_GPU_SURFACE_FORMAT_BGRX8888;
    header->reserved0 = 0;
    header->batch_capacity = SAVANXP_GPU_CLIENT_BATCH_CAPACITY;
    header->rect_capacity = SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS;
    header->reserved1 = 0;
    header->submit_sequence = 0;
    header->retired_sequence = 0;
    header->composed_sequence = 0;
    client->header = header;
    client->command_batches = (struct savanxp_gpu_dirty_rect_batch *)((unsigned char *)client->mapped_view + header->command_offset);
    client->pixels = (uint32_t *)((unsigned char *)client->mapped_view + header->pixels_offset);
    memset(client->command_batches, 0, command_bytes);
    memset(client->pixels, 0, client->surface_info.buffer_size);

    submit_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    retire_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    shutdown_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    if (pipe(input_pipe) < 0 || pipe(mouse_pipe) < 0 || pipe(launch_pipe) < 0 || pipe(cursor_hint_pipe) < 0 ||
        pipe(size_hint_pipe) < 0 || submit_event < 0 || retire_event < 0 || shutdown_event < 0)
    {
        goto fail;
    }
    /* Los extremos de ESCRITURA de teclado y mouse tambien van en no-bloqueante:
     * el WM no puede quedar bloqueado por un cliente que no drena su input. Pasa
     * de verdad al lanzar una app -- raise_overlay la hace activa al instante,
     * pero tarda en empezar a leer (carga de disco, gfx_open copiando la
     * superficie), y mientras tanto cada movimiento del mouse va a su pipe. Con
     * writes bloqueantes, el pipe se llena y se congela la sesion entera. */
    if (fcntl(launch_pipe[0], SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0 ||
        fcntl(cursor_hint_pipe[0], SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0 ||
        fcntl(size_hint_pipe[0], SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0 ||
        fcntl(input_pipe[1], SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0 ||
        fcntl(mouse_pipe[1], SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0)
    {
        goto fail;
    }

    pid = fork();
    if (pid < 0)
    {
        goto fail;
    }
    if (pid == 0)
    {
        /* Establece el contrato de fds del protocolo WM<->cliente
         * (savanxp/wm_protocol.h) antes del exec. */
        if (dup2(client->section_fd, SAVANXP_WM_FD_SECTION) < 0 ||
            dup2(input_pipe[0], SAVANXP_WM_FD_INPUT) < 0 ||
            dup2(mouse_pipe[0], SAVANXP_WM_FD_MOUSE) < 0 ||
            dup2(submit_event, SAVANXP_WM_FD_SUBMIT_EVENT) < 0 ||
            dup2(retire_event, SAVANXP_WM_FD_RETIRE_EVENT) < 0 ||
            dup2(shutdown_event, SAVANXP_WM_FD_SHUTDOWN_EVENT) < 0 ||
            dup2(launch_pipe[1], SAVANXP_WM_FD_LAUNCH) < 0 ||
            dup2(cursor_hint_pipe[1], SAVANXP_WM_FD_CURSOR_HINT) < 0 ||
            dup2(size_hint_pipe[1], SAVANXP_WM_FD_SIZE_HINT) < 0)
        {
            exit(1);
        }

        close_client_setup_fd(&input_pipe[0]);
        close_client_setup_fd(&input_pipe[1]);
        close_client_setup_fd(&mouse_pipe[0]);
        close_client_setup_fd(&mouse_pipe[1]);
        close_client_setup_fd(&submit_event);
        close_client_setup_fd(&retire_event);
        close_client_setup_fd(&shutdown_event);
        close_client_setup_fd(&launch_pipe[0]);
        close_client_setup_fd(&launch_pipe[1]);
        close_client_setup_fd(&cursor_hint_pipe[0]);
        close_client_setup_fd(&cursor_hint_pipe[1]);
        close_client_setup_fd(&size_hint_pipe[0]);
        close_client_setup_fd(&size_hint_pipe[1]);
        close_client_setup_fd(&client->section_fd);
        {
            long exec_result = exec(path, argv, argc);
            if (exec_result < 0)
            {
                eprintf("desktop: exec failed for %s (%s)\n", path, result_error_string(exec_result));
            }
        }
        exit(1);
    }

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
    client->input_write_fd = input_pipe[1];
    client->mouse_write_fd = mouse_pipe[1];
    client->submit_event_fd = submit_event;
    client->retire_event_fd = retire_event;
    client->shutdown_event_fd = shutdown_event;
    client->launch_read_fd = launch_pipe[0];
    client->cursor_hint_read_fd = cursor_hint_pipe[0];
    client->size_hint_read_fd = size_hint_pipe[0];

    close_fd_if_needed(&input_pipe[0]);
    close_fd_if_needed(&mouse_pipe[0]);
    close_fd_if_needed(&launch_pipe[1]);
    close_fd_if_needed(&cursor_hint_pipe[1]);
    close_fd_if_needed(&size_hint_pipe[1]);
    return 0;

fail:
    close_fd_if_needed(&input_pipe[0]);
    close_fd_if_needed(&input_pipe[1]);
    close_fd_if_needed(&mouse_pipe[0]);
    close_fd_if_needed(&mouse_pipe[1]);
    close_fd_if_needed(&launch_pipe[0]);
    close_fd_if_needed(&launch_pipe[1]);
    close_fd_if_needed(&cursor_hint_pipe[0]);
    close_fd_if_needed(&cursor_hint_pipe[1]);
    close_fd_if_needed(&size_hint_pipe[0]);
    close_fd_if_needed(&size_hint_pipe[1]);
    close_fd_if_needed(&submit_event);
    close_fd_if_needed(&retire_event);
    close_fd_if_needed(&shutdown_event);
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
    return start_client_process(client, k_background_client_path, 0);
}

static void destroy_overlay_client(struct windowd_session *session, int slot, int terminate_client)
{
    struct windowd_client *client = overlay_client_at(session, slot);

    if (client == 0)
    {
        return;
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
    if (start_client_process(client, path, 0) < 0)
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
    if (start_client_process(client, path, argument) < 0)
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
    reset_client(&session->shell_client);
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
    session->input_fd = (int)open_mode("/dev/input0", SAVANXP_OPEN_READ);
    if (session->input_fd < 0)
    {
        return windowd_stage_failed("open /dev/input0", session->input_fd);
    }

    session->mouse_fd = (int)open_mode("/dev/mouse0", SAVANXP_OPEN_READ);
    if (session->mouse_fd < 0)
    {
        eprintf("desktop: /dev/mouse0 unavailable (%s), continuing keyboard-only\n", result_error_string(session->mouse_fd));
        session->mouse_fd = -1;
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

static int service_client_launch_requests(struct windowd_session *session, struct windowd_dirty_rect *dirty, struct windowd_client *client)
{
    struct savanxp_desktop_launch_request request;
    long read_result = 0;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0 || client->launch_read_fd < 0)
    {
        return 0;
    }

    for (;;)
    {
        memset(&request, 0, sizeof(request));
        read_result = read(client->launch_read_fd, &request, sizeof(request));
        if (read_result == 0)
        {
            return 0;
        }
        if (read_result < 0)
        {
            if (result_error_code(read_result) == SAVANXP_EAGAIN)
            {
                return 0;
            }
            eprintf("desktop: launch request read failed for %s (%s)\n",
                client->path[0] != '\0' ? client->path : "?",
                result_error_string(read_result));
            return 0;
        }
        if (read_result != (long)sizeof(request) || request.path[0] != '/')
        {
            eprintf("desktop: invalid launch request from %s\n", client->path[0] != '\0' ? client->path : "?");
            return 0;
        }
        request.path[SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY - 1u] = '\0';
        request.argument[SAVANXP_DESKTOP_LAUNCH_ARG_CAPACITY - 1u] = '\0';
        /* Se ignoran los bits desconocidos: un cliente viejo o mal formado no
         * debe poder pedir modos que el WM no entiende. */
        if (launch_overlay_client(
                session,
                request.path,
                request.argument,
                request.flags & SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN) < 0)
        {
            eprintf("desktop: failed to launch requested app %s\n", request.path);
            return 0;
        }
        windowd_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
    }
}

/* Drains cursor-shape hints an app reports about its own widgets (e.g. the
 * pointer entering a textfield). Just caches the latest value on the client;
 * resolve_cursor_shape() decides whether it is actually shown, and only
 * while this exact client is the one under the pointer. No repaint here --
 * the shape-resolution pass on the next mouse event picks it up. */
static void service_client_cursor_hints(struct windowd_client *client)
{
    struct savanxp_desktop_cursor_hint hint;
    long read_result = 0;

    if (client == 0 || client->pid <= 0 || client->cursor_hint_read_fd < 0)
    {
        return;
    }

    for (;;)
    {
        read_result = read(client->cursor_hint_read_fd, &hint, sizeof(hint));
        if (read_result == 0)
        {
            return;
        }
        if (read_result < 0)
        {
            if (result_error_code(read_result) != SAVANXP_EAGAIN)
            {
                eprintf("desktop: cursor hint read failed for %s (%s)\n",
                    client->path[0] != '\0' ? client->path : "?",
                    result_error_string(read_result));
            }
            return;
        }
        if (read_result != (long)sizeof(hint) || hint.shape >= (uint32_t)SAVANXP_CURSOR_SHAPE_COUNT)
        {
            eprintf("desktop: invalid cursor hint from %s\n", client->path[0] != '\0' ? client->path : "?");
            return;
        }
        client->last_cursor_hint_shape = (int)hint.shape;
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
 * slot < 0 = drenar y descartar. El contrato de fds es uno solo, asi que el
 * canal existe tambien para el shell y el cliente de fondo, que son
 * full-screen por definicion; si nadie lo leyera, un cliente que escribiera
 * ahi terminaria bloqueado con el pipe lleno. */
static void service_client_size_hints(
    struct windowd_session *session,
    struct windowd_dirty_rect *dirty,
    struct windowd_client *client,
    int slot)
{
    struct savanxp_desktop_size_hint hint;
    long read_result = 0;

    if (client == 0 || client->pid <= 0 || client->size_hint_read_fd < 0)
    {
        return;
    }

    for (;;)
    {
        read_result = read(client->size_hint_read_fd, &hint, sizeof(hint));
        if (read_result == 0)
        {
            return;
        }
        if (read_result < 0)
        {
            if (result_error_code(read_result) != SAVANXP_EAGAIN)
            {
                eprintf("desktop: size hint read failed for %s (%s)\n",
                    client->path[0] != '\0' ? client->path : "?",
                    result_error_string(read_result));
            }
            return;
        }
        if (read_result != (long)sizeof(hint) || hint.width == 0 || hint.height == 0)
        {
            eprintf("desktop: invalid size hint from %s\n", client->path[0] != '\0' ? client->path : "?");
            return;
        }
        if (session == 0 || dirty == 0 || !overlay_slot_valid(slot) ||
            client->size_hint_applied || client->maximized || client->fullscreen || !client->frame_visible)
        {
            continue;
        }
        client->size_hint_applied = 1;
        resize_overlay_client_surface(session, dirty, slot, (int)hint.width, (int)hint.height);
        reposition_overlay_client_window(session, dirty, client);
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

        switch (iteration)
        {
        case 10:
            toggle_overlay_client_maximized(&session, &dirty, kSlot);
            break;
        case 20:
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
                (void)kill((int)session.compositor.pid, SAVANXP_SIGKILL);
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
                (void)kill((int)session.compositor.pid, SAVANXP_SIGKILL);
            }
        }

        if (frames_presented >= kTargetFrames)
        {
            break;
        }
    }

    (void)sync_pending_present(&session, 1, 0);

    /* Subtest de contrapresion de input: el WM no debe bloquearse nunca por un
     * cliente que no drena. shellui sirve de cliente "sordo" -- nunca abre su
     * pipe de puntero --, asi que le bombardeamos eventos hasta llenar el pipe y
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
            if (!route_pointer(&session.background_client, 10 + (index & 31), 10, 0))
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

    close_compositor_session(&session);

    if (failed)
    {
        return 1;
    }
    printf("DESKTOP SMOKE PASS frames=%d batches=%u\n",
        frames_presented, (unsigned)consumed_submit);
    return 0;
}

/* Append a live client's submit event to the poll set so a client frame
 * submission wakes the compositor immediately instead of waiting out the poll
 * timeout. Returns the new descriptor count. poll_clients[] maps each poll slot
 * back to its client so the level-triggered event can be reset afterwards. */
static int add_client_submit_pollfd(
    struct savanxp_pollfd *poll_fds,
    struct windowd_client **poll_clients,
    int poll_count,
    struct windowd_client *client)
{
    if (client == 0 || client->pid <= 0 || client->submit_event_fd < 0)
    {
        return poll_count;
    }
    poll_fds[poll_count].fd = client->submit_event_fd;
    poll_fds[poll_count].events = SAVANXP_POLLIN;
    poll_fds[poll_count].revents = 0;
    poll_clients[poll_count] = client;
    return poll_count + 1;
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
        if (session->active_client_kind == WINDOWD_CLIENT_APP && !is_shell &&
            slot == session->active_overlay_slot)
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
            if (current_hover_client == &session->shell_client)
            {
                activate_shell(session);
            }
            else
            {
                int target_slot = overlay_slot_for_client_ptr(session, current_hover_client);
                raise_overlay(session, target_slot);
                current_hover_client = overlay_client_at_const(session, target_slot);
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
                if (overlay_slot_valid(target_slot))
                {
                    toggle_overlay_client_maximized(session, dirty, target_slot);
                }
            }
            else if (current_hover_client != 0 &&
                     current_hover_client != &session->shell_client &&
                windowd_point_in_close_button(current_hover_client, cursor_x, cursor_y))
            {
                int target_slot = overlay_slot_for_client_ptr(session, current_hover_client);
                struct sx_rect closed_frame = windowd_client_frame_rect(current_hover_client);

                if (overlay_slot_valid(target_slot))
                {
                    destroy_overlay_client(session, target_slot, 1);
                    windowd_dirty_rect_add(
                        dirty,
                        &session->gfx.info,
                        closed_frame.x,
                        closed_frame.y,
                        closed_frame.width,
                        closed_frame.height);
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
            else if (current_hover_client != 0 && current_hover_client->mouse_write_fd >= 0)
            {
                (void)route_pointer(current_hover_client, cursor_x, cursor_y, pressed_buttons);
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
        current_hover_client->mouse_write_fd >= 0 &&
        !(left_pressed != 0 && left_was_pressed == 0))
    {
        (void)route_pointer(current_hover_client, cursor_x, cursor_y, pressed_buttons);
    }
    else if (!mouse_routed &&
             !drag_was_active &&
             !drag_active_now &&
             current_hover_client == 0 &&
             previous_hover_client != 0 &&
             previous_hover_client->mouse_write_fd >= 0 &&
             !(left_pressed != 0 && left_was_pressed == 0))
    {
        (void)route_pointer(previous_hover_client, cursor_x, cursor_y, pressed_buttons);
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
    windowd_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);

    for (;;)
    {
        /* input + mouse + background_client + shell_client + overlays. */
        struct savanxp_pollfd poll_fds[4 + WINDOWD_MAX_OVERLAY_CLIENTS];
        struct windowd_client *poll_clients[4 + WINDOWD_MAX_OVERLAY_CLIENTS];
        int input_poll_index = -1;
        int mouse_poll_index = -1;
        int poll_count = 0;
        int client_poll_start = 0;
        int poll_idx;
        int slot;
        long count = 0;

        input_poll_index = poll_count;
        poll_fds[poll_count].fd = session.input_fd;
        poll_fds[poll_count].events = SAVANXP_POLLIN;
        poll_fds[poll_count].revents = 0;
        poll_clients[poll_count] = 0;
        ++poll_count;

        if (session.mouse_fd >= 0)
        {
            mouse_poll_index = poll_count;
            poll_fds[poll_count].fd = session.mouse_fd;
            poll_fds[poll_count].events = SAVANXP_POLLIN;
            poll_fds[poll_count].revents = 0;
            poll_clients[poll_count] = 0;
            ++poll_count;
        }

        /* Wake on client frame submissions, not just the 16 ms timeout (kept as
         * a backstop). The timeout still bounds latency if a wakeup is missed. */
        client_poll_start = poll_count;
        poll_count = add_client_submit_pollfd(poll_fds, poll_clients, poll_count, &session.background_client);
        poll_count = add_client_submit_pollfd(poll_fds, poll_clients, poll_count, &session.shell_client);
        for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
        {
            poll_count = add_client_submit_pollfd(poll_fds, poll_clients, poll_count, &session.overlay_clients[slot]);
        }

        if (poll(poll_fds, (unsigned long)poll_count, 16) < 0)
        {
            break;
        }

        /* Clear the level-triggered submit events we observed; a fresh submit
         * re-arms the event and wakes the next poll. Draining of the actual
         * frame content happens below via the shared submit_sequence. */
        for (poll_idx = client_poll_start; poll_idx < poll_count; ++poll_idx)
        {
            if ((poll_fds[poll_idx].revents & SAVANXP_POLLIN) != 0 &&
                poll_clients[poll_idx] != 0 &&
                poll_clients[poll_idx]->submit_event_fd >= 0)
            {
                (void)event_reset(poll_clients[poll_idx]->submit_event_fd);
            }
        }

        if (input_poll_index >= 0 && (poll_fds[input_poll_index].revents & SAVANXP_POLLIN) != 0)
        {
            while ((count = read(session.input_fd, &key_event, sizeof(key_event))) == (long)sizeof(key_event))
            {
                /* Sin chrome, la arbitracion se reduce a: hotkeys del WM y, si
                 * no consume, ruteo al cliente activo. */
                if (wm_handle_key(&session, &key_event, &dirty))
                {
                    continue;
                }
                {
                    struct windowd_client *client = active_client(&session);
                    if (client != 0 && client->input_write_fd >= 0)
                    {
                        (void)route_packet(client->input_write_fd, &key_event, sizeof(key_event));
                    }
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

            count = read(session.mouse_fd, raw_mouse_events, sizeof(raw_mouse_events));
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
        for (slot = 0; slot < WINDOWD_MAX_OVERLAY_CLIENTS; ++slot)
        {
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

            if (sync_pending_present(&session, 0, &frame_ready) < 0)
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

        windowd_draw_desktop(&session, cursor_x, cursor_y, &dirty);
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
        windowd_dirty_rect_reset(&dirty);
    }

    close_compositor_session(&session);
    return 1;
}
