#include "desktop.h"
#include "savanxp/audio_server.h"

#include <stdio.h>

/*
 * The volume popup: a popup of the desktop, anchored in the tray corner above
 * the speaker button. No widgets: it uses sxgui's palette, not the toolkit.
 * It has no terminal action (the sliders apply live), so it only closes when
 * the speaker button is clicked again or on a press outside it.
 *
 * At the top, as always, against /dev/audio0: master slider, mute and
 * persistence in /audio.cfg. Below, one row per application playing, with
 * its own slider, from the daemon's census (asynchronous query every second;
 * without the daemon, the text and nothing else). The per-application level
 * lasts for the session: it is not persisted.
 */

#define VOLUMEPOPUP_MARGIN 2
#define VOLUMEPOPUP_WIDTH 144
#define VOLUMEPOPUP_HEIGHT 140

#define VOLUMEPOPUP_MUTE_ROW_HEIGHT 20
#define VOLUMEPOPUP_MUTE_BOX 12
#define VOLUMEPOPUP_TROUGH_X 8
#define VOLUMEPOPUP_TROUGH_Y 32
#define VOLUMEPOPUP_TROUGH_WIDTH 128
#define VOLUMEPOPUP_TROUGH_HEIGHT 12
#define VOLUMEPOPUP_THUMB_WIDTH 10
#define VOLUMEPOPUP_THUMB_HEIGHT 16
#define VOLUMEPOPUP_WHEEL_STEP 5

#define VOLUMEPOPUP_APP_ROWS 4
#define VOLUMEPOPUP_APP_ROW_Y(index) (46 + (index) * 24)
#define VOLUMEPOPUP_APP_ROW_HEIGHT 20
#define VOLUMEPOPUP_APP_NAME_X 6
#define VOLUMEPOPUP_APP_NAME_WIDTH 52
#define VOLUMEPOPUP_APP_TROUGH_X 64
#define VOLUMEPOPUP_APP_TROUGH_WIDTH 72
#define VOLUMEPOPUP_APP_TROUGH_HEIGHT 10
#define VOLUMEPOPUP_APP_THUMB_WIDTH 8
#define VOLUMEPOPUP_APP_THUMB_HEIGHT 14

#define VOLUMEPOPUP_POLL_MS 1000u
#define VOLUMEPOPUP_QUERY_TIMEOUT_MS 300u

#define VOLUMEPOPUP_CONFIG_PATH "/audio.cfg"

static struct sx_rect popup_mute_rect(void)
{
    return sx_rect_make(
        VOLUMEPOPUP_MARGIN,
        VOLUMEPOPUP_MARGIN,
        VOLUMEPOPUP_WIDTH - (VOLUMEPOPUP_MARGIN * 2),
        VOLUMEPOPUP_MUTE_ROW_HEIGHT);
}

static int popup_slider_thumb_x(int volume, int trough_x, int travel);
static int popup_slider_volume_at(int x, int trough_x, int travel);

static struct sx_rect popup_thumb_rect(int volume)
{
    int trough_center_y = VOLUMEPOPUP_TROUGH_Y + VOLUMEPOPUP_TROUGH_HEIGHT / 2;
    int travel = VOLUMEPOPUP_TROUGH_WIDTH - VOLUMEPOPUP_THUMB_WIDTH;

    return sx_rect_make(
        popup_slider_thumb_x(volume, VOLUMEPOPUP_TROUGH_X, travel),
        trough_center_y - VOLUMEPOPUP_THUMB_HEIGHT / 2,
        VOLUMEPOPUP_THUMB_WIDTH,
        VOLUMEPOPUP_THUMB_HEIGHT);
}

static int popup_in_rect(struct sx_rect rect, int x, int y)
{
    return x >= rect.x && x < rect.x + rect.width &&
        y >= rect.y && y < rect.y + rect.height;
}

/* Una fila por app sonando: nombre a la izquierda, mini-slider a la derecha.
 * Hasta VOLUMEPOPUP_APP_ROWS; si hay mas, la ultima fila dice cuantos. */
struct popup_app_row {
    uint16_t port;
    uint8_t volume;
    char name[SAVANXP_AUDIOD_NAME_BYTES];
};

static struct sx_rect popup_app_rect(int row)
{
    return sx_rect_make(
        VOLUMEPOPUP_MARGIN,
        VOLUMEPOPUP_APP_ROW_Y(row),
        VOLUMEPOPUP_WIDTH - (VOLUMEPOPUP_MARGIN * 2),
        VOLUMEPOPUP_APP_ROW_HEIGHT);
}

static struct sx_rect popup_app_trough_rect(int row)
{
    int center_y = VOLUMEPOPUP_APP_ROW_Y(row) + VOLUMEPOPUP_APP_ROW_HEIGHT / 2;

    return sx_rect_make(
        VOLUMEPOPUP_APP_TROUGH_X,
        center_y - VOLUMEPOPUP_APP_TROUGH_HEIGHT / 2,
        VOLUMEPOPUP_APP_TROUGH_WIDTH,
        VOLUMEPOPUP_APP_TROUGH_HEIGHT);
}

static int popup_slider_thumb_x(int volume, int trough_x, int travel)
{
    if (volume < 0)
    {
        volume = 0;
    }
    if (volume > 100)
    {
        volume = 100;
    }
    return trough_x + (volume * travel) / 100;
}

static int popup_slider_volume_at(int x, int trough_x, int travel)
{
    int volume = ((x - trough_x) * 100) / travel;

    if (volume < 0)
    {
        volume = 0;
    }
    if (volume > 100)
    {
        volume = 100;
    }
    return volume;
}

static struct sx_rect popup_app_thumb_rect(int row, int volume)
{
    struct sx_rect trough = popup_app_trough_rect(row);
    int travel = VOLUMEPOPUP_APP_TROUGH_WIDTH - VOLUMEPOPUP_APP_THUMB_WIDTH;
    int center_y = trough.y + trough.height / 2;

    return sx_rect_make(
        popup_slider_thumb_x(volume, trough.x, travel),
        center_y - VOLUMEPOPUP_APP_THUMB_HEIGHT / 2,
        VOLUMEPOPUP_APP_THUMB_WIDTH,
        VOLUMEPOPUP_APP_THUMB_HEIGHT);
}

/* Copia con NUL garantizado: el demonio ya la manda asi, pero el cable
 * no se audita dos veces. */
static void popup_copy_name(char* out, const char* in)
{
    size_t index = 0;

    memset(out, 0, SAVANXP_AUDIOD_NAME_BYTES);
    if (in == 0)
    {
        return;
    }
    while (index + 1u < SAVANXP_AUDIOD_NAME_BYTES && in[index] != '\0')
    {
        out[index] = in[index];
        ++index;
    }
}

static int app_total_row_is_overflow(int row, int app_total)
{
    return app_total > VOLUMEPOPUP_APP_ROWS && row + 1 == VOLUMEPOPUP_APP_ROWS;
}

static int popup_find_row(const struct popup_app_row* apps, int count, uint16_t port)
{
    int row;

    for (row = 0; row < count && row < VOLUMEPOPUP_APP_ROWS; ++row)
    {
        if (apps[row].port == port)
        {
            return row;
        }
    }
    return -1;
}

/* Recorta el nombre al ancho dado, sin puntos suspensivos: el painter no
 * recorta y el texto se derramaria sobre el slider. */
static void popup_fit_name(char* out, size_t capacity, const char* name, int max_width)
{
    size_t length = 0;

    if (capacity == 0)
    {
        return;
    }
    while (name[length] != '\0' && length + 1u < capacity)
    {
        out[length] = name[length];
        out[length + 1u] = '\0';
        if (gfx_text_width(out) > max_width && length > 0)
        {
            out[length] = '\0';
            break;
        }
        ++length;
    }
}

static void popup_paint(
    struct savanxp_gfx_context *gfx,
    int volume,
    int muted,
    int mute_hot,
    int master_dragging,
    const struct popup_app_row *apps,
    int app_total,
    int drag_row,
    int hot_row)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame;
    struct sx_rect mute_rect = popup_mute_rect();
    struct sx_rect trough = sx_rect_make(
        VOLUMEPOPUP_TROUGH_X,
        VOLUMEPOPUP_TROUGH_Y,
        VOLUMEPOPUP_TROUGH_WIDTH,
        VOLUMEPOPUP_TROUGH_HEIGHT);
    struct sx_rect thumb = popup_thumb_rect(volume);
    struct sx_rect box = sx_rect_make(
        mute_rect.x + 4,
        mute_rect.y + (mute_rect.height - VOLUMEPOPUP_MUTE_BOX) / 2,
        VOLUMEPOPUP_MUTE_BOX,
        VOLUMEPOPUP_MUTE_BOX);
    char percent[8];
    int text_y = mute_rect.y + (mute_rect.height - gfx_text_height()) / 2;
    int row;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);

    frame = sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
    sx_painter_fill_rect(&painter, frame, SXGUI_COLOR_FACE);
    desktop_bevel(&painter, frame, 0);

    /* Fila de mute: casilla mas etiqueta, y el nivel a la derecha. */
    sx_painter_fill_rect(
        &painter, mute_rect, mute_hot ? SXGUI_COLOR_SELECT : SXGUI_COLOR_FACE);
    sx_painter_fill_rect(&painter, box, SXGUI_COLOR_FIELD);
    desktop_bevel(&painter, box, 1);
    if (muted)
    {
        /* Sin primitiva de linea, una marca es un bloque macizo. */
        struct sx_rect mark = sx_rect_make(box.x + 3, box.y + 3, box.width - 6, box.height - 6);
        sx_painter_fill_rect(&painter, mark, SXGUI_COLOR_TEXT);
    }
    sx_painter_draw_text(
        &painter,
        box.x + box.width + 4,
        text_y,
        "Mute",
        mute_hot ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT);
    (void)snprintf(percent, sizeof(percent), "%d%%", volume);
    sx_painter_draw_text(
        &painter,
        mute_rect.x + mute_rect.width - 4 - gfx_text_width(percent),
        text_y,
        percent,
        mute_hot ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT);

    /* Slider: carril hundido, cursor levantado (hundido mientras se
     * arrastra, como los botones de la taskbar). */
    sx_painter_fill_rect(&painter, trough, SXGUI_COLOR_FACE);
    desktop_bevel(&painter, trough, 1);
    sx_painter_fill_rect(&painter, thumb, SXGUI_COLOR_FACE);
    desktop_bevel(&painter, thumb, master_dragging);

    /* Una fila por app sonando: nombre y mini-slider con su nivel. Sin
     * streams, una linea gris que lo dice en vez de filas vacias. */
    if (app_total == 0)
    {
        int empty_y = VOLUMEPOPUP_APP_ROW_Y(0) +
            (VOLUMEPOPUP_APP_ROW_HEIGHT - gfx_text_height()) / 2;
        sx_painter_draw_text(
            &painter, VOLUMEPOPUP_MARGIN + 4, empty_y,
            "(no audio)", SXGUI_COLOR_DISABLED_TEXT);
        return;
    }
    for (row = 0; row < app_total && row < VOLUMEPOPUP_APP_ROWS; ++row)
    {
        struct sx_rect row_rect = popup_app_rect(row);
        struct sx_rect row_trough = popup_app_trough_rect(row);
        struct sx_rect row_thumb = popup_app_thumb_rect(row, apps[row].volume);
        char name[16];
        int row_text_y = row_rect.y + (row_rect.height - gfx_text_height()) / 2;
        int hot = row == hot_row;

        if (app_total > VOLUMEPOPUP_APP_ROWS && row == VOLUMEPOPUP_APP_ROWS - 1)
        {
            char more[16];
            (void)snprintf(more, sizeof(more), "+%d more", app_total - VOLUMEPOPUP_APP_ROWS + 1);
            sx_painter_draw_text(&painter, row_rect.x + 4, row_text_y, more, SXGUI_COLOR_TEXT);
            break;
        }
        sx_painter_fill_rect(
            &painter, row_rect, hot ? SXGUI_COLOR_SELECT : SXGUI_COLOR_FACE);
        popup_fit_name(name, sizeof(name), apps[row].name[0] != '\0' ? apps[row].name : "(?)",
                       VOLUMEPOPUP_APP_NAME_WIDTH);
        sx_painter_draw_text(
            &painter, row_rect.x + 4, row_text_y, name,
            hot ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT);
        sx_painter_fill_rect(&painter, row_trough, SXGUI_COLOR_FACE);
        desktop_bevel(&painter, row_trough, 1);
        sx_painter_fill_rect(&painter, row_thumb, SXGUI_COLOR_FACE);
        desktop_bevel(&painter, row_thumb, row == drag_row);
    }
}

/* Guarda el par que el kernel ya tiene: la fuente de verdad es el ioctl, el
 * fichero solo recuerda para el proximo arranque. Igual que `volume`. */
static void popup_persist(int volume, int muted)
{
    char text[16];
    long config_fd;

    (void)snprintf(text, sizeof(text), "%d %d\n", volume, muted);
    config_fd = savanxp_open_mode(
        VOLUMEPOPUP_CONFIG_PATH,
        SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (config_fd < 0)
    {
        return;
    }
    (void)savanxp_write((int)config_fd, text, strlen(text));
    savanxp_close((int)config_fd);
    (void)savanxp_sync();
}

static long g_audio_fd = -1;
static int g_volume = 100;
static int g_muted = 0;
static int g_mute_hot = 0;
static int g_mute_armed = 0;
static int g_drag_offset = 0;
/* -1 idle, -2 the master slider, 0..3 an application row. */
static int g_drag_row = -1;
static int g_hot_row = -1;
static uint16_t g_drag_port = 0;
static struct popup_app_row g_apps[VOLUMEPOPUP_APP_ROWS];
static int g_apps_total = 0;
static long g_census_sock = -1;
static uint64_t g_last_poll_ms = 0;
static int g_query_out = 0;
static uint64_t g_query_ms = 0;
static struct popup_app_row g_staging[VOLUMEPOPUP_APP_ROWS];
static int g_staging_total = -1;
static int g_staging_count = 0;
static uint32_t g_last_buttons = 0;

static void volume_close(void)
{
    if (g_census_sock >= 0)
    {
        savanxp_close((int)g_census_sock);
        g_census_sock = -1;
    }
    if (g_audio_fd >= 0)
    {
        savanxp_close((int)g_audio_fd);
        g_audio_fd = -1;
    }
}

static int volume_open(void)
{
    long status;

    g_mute_hot = 0;
    g_mute_armed = 0;
    g_drag_offset = 0;
    g_drag_row = -1;
    g_hot_row = -1;
    g_drag_port = 0;
    g_apps_total = 0;
    g_last_poll_ms = 0;
    g_query_out = 0;
    g_query_ms = 0;
    g_staging_total = -1;
    g_staging_count = 0;
    g_last_buttons = 0;

    /* Only for the ioctls: nothing is ever read or written through this fd. */
    g_audio_fd = (long)audio_open();
    if (g_audio_fd < 0)
    {
        puts_fd(2, "desktop: /dev/audio0 unavailable\n");
        return -1;
    }
    status = audio_get_volume((int)g_audio_fd);
    if (status < 0 || status > 100)
    {
        puts_fd(2, "desktop: AUDIO_IOC_GET_VOLUME failed\n");
        volume_close();
        return -1;
    }
    g_volume = (int)status;
    status = audio_get_muted((int)g_audio_fd);
    if (status != 0 && status != 1)
    {
        puts_fd(2, "desktop: AUDIO_IOC_GET_MUTED failed\n");
        volume_close();
        return -1;
    }
    g_muted = (int)status;

    /* The census socket: without it the popup still opens (master and mute
     * are kernel ioctls), only without rows. Non-blocking, so the 16 ms loop
     * never waits for replies. */
    {
        long sock = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
        if (sock >= 0)
        {
            if (savanxp_fcntl((int)sock, SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0)
            {
                savanxp_close((int)sock);
            }
            else
            {
                g_census_sock = sock;
            }
        }
    }
    return 0;
}

static uint32_t volume_tick(void)
{
    uint64_t now_ms = (uint64_t)uptime_ms();
    uint32_t actions = DESKTOP_ACTION_NONE;

    /* Asynchronous census: ask every second, collect replies without
     * blocking the 16 ms loop. Without the daemon the send fails and is
     * retried next cycle; the rows stay as they are. */
    if (g_census_sock >= 0 && !g_query_out &&
        (g_last_poll_ms == 0 || now_ms - g_last_poll_ms >= VOLUMEPOPUP_POLL_MS))
    {
        struct savanxp_audiod_list_query query;
        struct savanxp_sockaddr_in address;

        memset(&query, 0, sizeof(query));
        query.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
        query.version = SAVANXP_AUDIOD_CONTROL_VERSION;
        query.kind = SAVANXP_AUDIOD_LIST;
        address.ipv4 = SAVANXP_AUDIOD_HOST_IPV4;
        address.port = SAVANXP_AUDIOD_PORT;
        address.reserved0 = 0;
        if (savanxp_sendto(g_census_sock, &query, sizeof(query), &address) ==
            (long)sizeof(query))
        {
            g_query_out = 1;
            g_query_ms = now_ms;
            g_staging_total = -1;
            g_staging_count = 0;
        }
        else
        {
            g_last_poll_ms = now_ms;
        }
    }
    if (g_query_out)
    {
        int done = 0;
        int want = 0;

        for (;;)
        {
            struct savanxp_sockaddr_in source;
            unsigned char datagram[128];
            struct savanxp_audiod_list_reply reply;
            long got = savanxp_recvfrom(
                (int)g_census_sock, datagram, sizeof(datagram), &source, 0);

            if (got <= 0)
            {
                break;
            }
            if (got != (long)sizeof(reply))
            {
                continue;
            }
            memcpy(&reply, datagram, sizeof(reply));
            if (reply.magic != SAVANXP_AUDIOD_CONTROL_MAGIC ||
                reply.version != SAVANXP_AUDIOD_CONTROL_VERSION ||
                reply.kind != SAVANXP_AUDIOD_REPLY)
            {
                continue;
            }
            if (g_staging_total < 0)
            {
                g_staging_total = reply.count;
                if (g_staging_total == 0)
                {
                    done = 1;
                    break;
                }
            }
            else if (reply.count != (uint8_t)g_staging_total)
            {
                continue;
            }
            want = g_staging_total < VOLUMEPOPUP_APP_ROWS
                ? g_staging_total
                : VOLUMEPOPUP_APP_ROWS;
            if (reply.count > 0 && reply.index < reply.count &&
                reply.index < VOLUMEPOPUP_APP_ROWS)
            {
                g_staging[reply.index].port = reply.port;
                g_staging[reply.index].volume = reply.volume;
                popup_copy_name(g_staging[reply.index].name, reply.name);
                ++g_staging_count;
            }
            if (g_staging_count >= want)
            {
                done = 1;
                break;
            }
        }
        if (done)
        {
            int row;
            int shown = 0;

            if (g_staging_total > 0)
            {
                shown = g_staging_total < VOLUMEPOPUP_APP_ROWS
                    ? g_staging_total
                    : VOLUMEPOPUP_APP_ROWS;
                for (row = 0; row < shown; ++row)
                {
                    g_apps[row] = g_staging[row];
                }
                g_apps_total = g_staging_total;
            }
            else if (g_staging_total == 0)
            {
                g_apps_total = 0;
            }
            if (g_drag_row >= 0)
            {
                int at = popup_find_row(g_apps, g_apps_total, g_drag_port);
                if (at < 0)
                {
                    g_drag_row = -1;
                }
                else
                {
                    g_drag_row = at;
                }
            }
            g_query_out = 0;
            g_last_poll_ms = now_ms;
            actions |= DESKTOP_ACTION_REPAINT;
        }
        else if (now_ms - g_query_ms >= VOLUMEPOPUP_QUERY_TIMEOUT_MS)
        {
            /* Sin respuesta completa a tiempo se conserva el censo
             * anterior: mejor una fila vieja un segundo mas que
             * parpadeos. El proximo poll lo corrige. */
            g_query_out = 0;
            g_last_poll_ms = now_ms;
        }
    }

    return actions;
}

static uint32_t volume_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    uint32_t down = pointer->buttons & ~g_last_buttons;
    uint32_t up = g_last_buttons & ~pointer->buttons;
    struct sx_rect mute_rect = popup_mute_rect();
    struct sx_rect thumb = popup_thumb_rect(g_volume);
    int in_mute = popup_in_rect(mute_rect, pointer->x, pointer->y);
    int in_thumb = popup_in_rect(thumb, pointer->x, pointer->y);
    int in_trough = pointer->x >= VOLUMEPOPUP_TROUGH_X &&
        pointer->x < VOLUMEPOPUP_TROUGH_X + VOLUMEPOPUP_TROUGH_WIDTH &&
        pointer->y >= VOLUMEPOPUP_TROUGH_Y - 4 &&
        pointer->y < VOLUMEPOPUP_TROUGH_Y + VOLUMEPOPUP_TROUGH_HEIGHT + 4;
    int row;
    int in_row = -1;
    int in_row_trough = 0;
    int in_row_thumb = 0;
    struct sx_rect row_thumb = sx_rect_make(0, 0, 0, 0);
    uint32_t actions = DESKTOP_ACTION_NONE;

    (void)gfx;
    for (row = 0; row < g_apps_total && row < VOLUMEPOPUP_APP_ROWS; ++row)
    {
        if (app_total_row_is_overflow(row, g_apps_total))
        {
            break;
        }
        if (popup_in_rect(popup_app_rect(row), pointer->x, pointer->y))
        {
            struct sx_rect trough = popup_app_trough_rect(row);
            row_thumb = popup_app_thumb_rect(row, g_apps[row].volume);
            in_row = row;
            in_row_trough =
                pointer->x >= trough.x - 2 && pointer->x < trough.x + trough.width + 2 &&
                pointer->y >= trough.y - 4 && pointer->y < trough.y + trough.height + 4;
            in_row_thumb = popup_in_rect(row_thumb, pointer->x, pointer->y);
            break;
        }
    }

    /* Reconcile with the real button state, only when the event carries no
     * edge: if the button is released outside the popup, another window gets
     * the up, and the drag or the armed mute would hang until the next click.
     * The popup's own up is left alone: the branch below handles it, with its
     * persist. */
    if (((down | up) & SAVANXP_MOUSE_BUTTON_LEFT) == 0 &&
        (pointer->buttons & SAVANXP_MOUSE_BUTTON_LEFT) == 0)
    {
        if (g_drag_row == -2)
        {
            g_drag_row = -1;
            popup_persist(g_volume, g_muted);
            actions |= DESKTOP_ACTION_REPAINT;
        }
        else if (g_drag_row >= 0)
        {
            g_drag_row = -1;
            actions |= DESKTOP_ACTION_REPAINT;
        }
        g_mute_armed = 0;
    }

    if (in_mute != g_mute_hot)
    {
        g_mute_hot = in_mute;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if (in_row != g_hot_row)
    {
        g_hot_row = in_row;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if (pointer->wheel != 0)
    {
        if (in_row >= 0 && g_census_sock >= 0)
        {
            int next = (int)g_apps[in_row].volume + pointer->wheel * VOLUMEPOPUP_WHEEL_STEP;
            if (next < 0)
            {
                next = 0;
            }
            if (next > 100)
            {
                next = 100;
            }
            if (next != (int)g_apps[in_row].volume)
            {
                g_apps[in_row].volume = (uint8_t)next;
                (void)sx_audio_server_set_volume(
                    (int)g_census_sock, g_apps[in_row].port, next);
                actions |= DESKTOP_ACTION_REPAINT;
            }
        }
        else
        {
            int next = g_volume + pointer->wheel * VOLUMEPOPUP_WHEEL_STEP;
            if (next < 0)
            {
                next = 0;
            }
            if (next > 100)
            {
                next = 100;
            }
            if (next != g_volume)
            {
                g_volume = next;
                (void)audio_set_volume((int)g_audio_fd, g_volume);
            actions |= DESKTOP_ACTION_TASKBAR_CHANGED;
                popup_persist(g_volume, g_muted);
                actions |= DESKTOP_ACTION_REPAINT;
            }
        }
    }
    if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        if (in_mute)
        {
            g_mute_armed = 1;
        }
        else if (in_trough)
        {
            g_drag_row = -2;
            /* Agarrar el cursor por donde cayo en vez de
             * recentrarlo: el salto se nota con 118 px de
             * recorrido para 100 unidades. */
            g_drag_offset = in_thumb ? pointer->x - thumb.x : VOLUMEPOPUP_THUMB_WIDTH / 2;
            g_volume = popup_slider_volume_at(pointer->x - g_drag_offset, VOLUMEPOPUP_TROUGH_X, VOLUMEPOPUP_TROUGH_WIDTH - VOLUMEPOPUP_THUMB_WIDTH);
            (void)audio_set_volume((int)g_audio_fd, g_volume);
            actions |= DESKTOP_ACTION_TASKBAR_CHANGED;
            actions |= DESKTOP_ACTION_REPAINT;
        }
        else if (in_row >= 0 && in_row_trough && g_census_sock >= 0)
        {
            int travel =
                VOLUMEPOPUP_APP_TROUGH_WIDTH - VOLUMEPOPUP_APP_THUMB_WIDTH;
            g_drag_row = in_row;
            g_drag_port = g_apps[in_row].port;
            g_drag_offset = in_row_thumb
                ? pointer->x - row_thumb.x
                : VOLUMEPOPUP_APP_THUMB_WIDTH / 2;
            g_apps[in_row].volume = (uint8_t)popup_slider_volume_at(
                pointer->x - g_drag_offset, VOLUMEPOPUP_APP_TROUGH_X, travel);
            (void)sx_audio_server_set_volume(
                (int)g_census_sock, g_apps[in_row].port, g_apps[in_row].volume);
            actions |= DESKTOP_ACTION_REPAINT;
        }
    }
    else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        if (g_mute_armed)
        {
            if (in_mute)
            {
                g_muted = g_muted == 0 ? 1 : 0;
                (void)audio_set_muted((int)g_audio_fd, g_muted);
                actions |= DESKTOP_ACTION_TASKBAR_CHANGED;
                popup_persist(g_volume, g_muted);
            }
            g_mute_armed = 0;
            actions |= DESKTOP_ACTION_REPAINT;
        }
        if (g_drag_row == -2)
        {
            g_drag_row = -1;
            popup_persist(g_volume, g_muted);
            actions |= DESKTOP_ACTION_REPAINT;
        }
        else if (g_drag_row >= 0)
        {
            /* El nivel por app es sesion: no se persiste. */
            g_drag_row = -1;
            actions |= DESKTOP_ACTION_REPAINT;
        }
    }
    else if (g_drag_row == -2)
    {
        int next = popup_slider_volume_at(pointer->x - g_drag_offset, VOLUMEPOPUP_TROUGH_X, VOLUMEPOPUP_TROUGH_WIDTH - VOLUMEPOPUP_THUMB_WIDTH);
        if (next != g_volume)
        {
            g_volume = next;
            (void)audio_set_volume((int)g_audio_fd, g_volume);
            actions |= DESKTOP_ACTION_TASKBAR_CHANGED;
            actions |= DESKTOP_ACTION_REPAINT;
        }
    }
    else if (g_drag_row >= 0 && g_census_sock >= 0)
    {
        int travel = VOLUMEPOPUP_APP_TROUGH_WIDTH - VOLUMEPOPUP_APP_THUMB_WIDTH;
        int here = popup_find_row(g_apps, g_apps_total, g_drag_port);
        int next;
        if (here < 0)
        {
            g_drag_row = -1;
            actions |= DESKTOP_ACTION_REPAINT;
        }
        else
        {
            g_drag_row = here;
            next = popup_slider_volume_at(pointer->x - g_drag_offset, VOLUMEPOPUP_APP_TROUGH_X, travel);
            if (next != (int)g_apps[here].volume)
            {
                g_apps[here].volume = (uint8_t)next;
                (void)sx_audio_server_set_volume(
                    (int)g_census_sock, g_apps[here].port, next);
                actions |= DESKTOP_ACTION_REPAINT;
            }
        }
    }
    g_last_buttons = pointer->buttons;
    return actions;
}

static void volume_paint(struct savanxp_gfx_context *gfx)
{
    popup_paint(gfx, g_volume, g_muted, g_mute_hot, g_drag_row == -2, g_apps,
                g_apps_total, g_drag_row, g_hot_row);
}

const struct desktop_popup_ops desktop_volume_ops = {
    VOLUMEPOPUP_WIDTH,
    VOLUMEPOPUP_HEIGHT,
    1,
    volume_open,
    volume_close,
    volume_pointer,
    volume_tick,
    volume_paint,
    0,
    0,
};
