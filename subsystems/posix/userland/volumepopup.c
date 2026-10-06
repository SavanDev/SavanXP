#include "libc.h"
#include "savanxp/audio_server.h"
#include "savanxp/sxgui.h"

#include <stdio.h>

/*
 * Popup de volumen: binario aparte, lanzado on-demand por windowd cuando la
 * taskbar pide SAVANXP_DESKTOP_LAUNCH_FLAG_VOLUME_POPUP. Mismo molde que
 * kbdlayoutpopup.c (cliente sin bordes anclado por windowd, sin widgets: no
 * linkea sxgui, solo su paleta) pero con toggle: no sale solo, el WM lo mata
 * al pedirlo de nuevo o al clickear afuera.
 *
 * Arriba, lo de siempre contra /dev/audio0: slider maestro, mute y
 * persistencia en /disk/audio.cfg. Debajo, una fila por app sonando con su
 * propio slider, desde el censo del demonio (encuesta async cada segundo;
 * sin demonio, texto y nada mas). El nivel por app es sesion: no se
 * persiste. Las medidas salen de windowd (WINDOWD_VOLUME_POPUP_WIDTH y
 * _HEIGHT): cambiarlas ahi sin cambiarlas aca descuadra el dibujo.
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

#define VOLUMEPOPUP_CONFIG_PATH "/disk/audio.cfg"

/* Mismo bisel que taskbar_bevel (taskbar.c) y popup_bevel
 * (kbdlayoutpopup.c) -- copiado, no compartido: ninguno de los tres linkea
 * el toolkit, y son tres lineas. */
static void popup_bevel(struct sx_painter *painter, struct sx_rect rect, int sunken)
{
    uint32_t top_left = sunken ? SXGUI_COLOR_SHADOW : SXGUI_COLOR_LIGHT;
    uint32_t bottom_right = sunken ? SXGUI_COLOR_LIGHT : SXGUI_COLOR_SHADOW;

    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), bottom_right);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), bottom_right);
}

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
    popup_bevel(&painter, frame, 0);

    /* Fila de mute: casilla mas etiqueta, y el nivel a la derecha. */
    sx_painter_fill_rect(
        &painter, mute_rect, mute_hot ? SXGUI_COLOR_SELECT : SXGUI_COLOR_FACE);
    sx_painter_fill_rect(&painter, box, SXGUI_COLOR_FIELD);
    popup_bevel(&painter, box, 1);
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
    popup_bevel(&painter, trough, 1);
    sx_painter_fill_rect(&painter, thumb, SXGUI_COLOR_FACE);
    popup_bevel(&painter, thumb, master_dragging);

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
        popup_bevel(&painter, row_trough, 1);
        sx_painter_fill_rect(&painter, row_thumb, SXGUI_COLOR_FACE);
        popup_bevel(&painter, row_thumb, row == drag_row);
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

int main(void)
{
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;
    struct savanxp_gui_pointer_event pointer;
    long audio_fd;
    long status;
    int volume = 100;
    int muted = 0;
    int mute_hot = 0;
    int mute_armed = 0;
    int drag_offset = 0;
    /* -1 quieto, -2 el slider maestro, 0..3 una fila de app. */
    int drag_row = -1;
    int hot_row = -1;
    uint16_t drag_port = 0;
    struct popup_app_row apps[VOLUMEPOPUP_APP_ROWS];
    int apps_total = 0;
    long census_sock = -1;
    uint64_t last_poll_ms = 0;
    int query_out = 0;
    uint64_t query_ms = 0;
    struct popup_app_row staging[VOLUMEPOPUP_APP_ROWS];
    int staging_total = -1;
    int staging_count = 0;
    int needs_repaint = 1;
    uint32_t last_buttons = 0;

    if (gfx_open(&gfx) < 0)
    {
        puts_fd(2, "volumepopup: gfx_open failed\n");
        return 1;
    }
    if (gfx_acquire(&gfx) < 0)
    {
        puts_fd(2, "volumepopup: gfx_acquire failed\n");
        gfx_close(&gfx);
        return 1;
    }

    /* Solo para los ioctl: nunca hay que leer ni escribir por este fd. */
    audio_fd = (long)audio_open();
    if (audio_fd < 0)
    {
        puts_fd(2, "volumepopup: /dev/audio0 unavailable\n");
        gfx_close(&gfx);
        return 1;
    }
    status = audio_get_volume((int)audio_fd);
    if (status < 0 || status > 100)
    {
        puts_fd(2, "volumepopup: AUDIO_IOC_GET_VOLUME failed\n");
        savanxp_close((int)audio_fd);
        gfx_close(&gfx);
        return 1;
    }
    volume = (int)status;
    status = audio_get_muted((int)audio_fd);
    if (status != 0 && status != 1)
    {
        puts_fd(2, "volumepopup: AUDIO_IOC_GET_MUTED failed\n");
        savanxp_close((int)audio_fd);
        gfx_close(&gfx);
        return 1;
    }
    muted = (int)status;

    /* Socket del censo: sin el igual se abre el popup (maestro y mute son
     * ioctls al kernel), solo que sin filas. No bloqueante para no congelar
     * el loop de 16 ms esperando respuestas. */
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
                census_sock = sock;
            }
        }
    }

    for (;;)
    {
        uint64_t now_ms = (uint64_t)uptime_ms();

        while (gfx_poll_event(&gfx, &event) > 0)
        {
            if (event.type == SAVANXP_INPUT_EVENT_RESIZED)
            {
                (void)gfx_apply_resize_event(&gfx, &event);
                needs_repaint = 1;
            }
        }

        /* Censo async: pedir cada segundo, juntar respuestas sin bloquear
         * el loop de 16 ms. Sin demonio el envio falla y se reintenta en
         * el proximo ciclo; las filas quedan como estan. */
        if (census_sock >= 0 && !query_out &&
            (last_poll_ms == 0 || now_ms - last_poll_ms >= VOLUMEPOPUP_POLL_MS))
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
            if (savanxp_sendto(census_sock, &query, sizeof(query), &address) ==
                (long)sizeof(query))
            {
                query_out = 1;
                query_ms = now_ms;
                staging_total = -1;
                staging_count = 0;
            }
            else
            {
                last_poll_ms = now_ms;
            }
        }
        if (query_out)
        {
            int done = 0;
            int want = 0;

            for (;;)
            {
                struct savanxp_sockaddr_in source;
                unsigned char datagram[128];
                struct savanxp_audiod_list_reply reply;
                long got = savanxp_recvfrom(
                    (int)census_sock, datagram, sizeof(datagram), &source, 0);

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
                if (staging_total < 0)
                {
                    staging_total = reply.count;
                    if (staging_total == 0)
                    {
                        done = 1;
                        break;
                    }
                }
                else if (reply.count != (uint8_t)staging_total)
                {
                    continue;
                }
                want = staging_total < VOLUMEPOPUP_APP_ROWS
                    ? staging_total
                    : VOLUMEPOPUP_APP_ROWS;
                if (reply.count > 0 && reply.index < reply.count &&
                    reply.index < VOLUMEPOPUP_APP_ROWS)
                {
                    staging[reply.index].port = reply.port;
                    staging[reply.index].volume = reply.volume;
                    popup_copy_name(staging[reply.index].name, reply.name);
                    ++staging_count;
                }
                if (staging_count >= want)
                {
                    done = 1;
                    break;
                }
            }
            if (done)
            {
                int row;
                int shown = 0;

                if (staging_total > 0)
                {
                    shown = staging_total < VOLUMEPOPUP_APP_ROWS
                        ? staging_total
                        : VOLUMEPOPUP_APP_ROWS;
                    for (row = 0; row < shown; ++row)
                    {
                        apps[row] = staging[row];
                    }
                    apps_total = staging_total;
                }
                else if (staging_total == 0)
                {
                    apps_total = 0;
                }
                if (drag_row >= 0)
                {
                    int at = popup_find_row(apps, apps_total, drag_port);
                    if (at < 0)
                    {
                        drag_row = -1;
                    }
                    else
                    {
                        drag_row = at;
                    }
                }
                query_out = 0;
                last_poll_ms = now_ms;
                needs_repaint = 1;
            }
            else if (now_ms - query_ms >= VOLUMEPOPUP_QUERY_TIMEOUT_MS)
            {
                /* Sin respuesta completa a tiempo se conserva el censo
                 * anterior: mejor una fila vieja un segundo mas que
                 * parpadeos. El proximo poll lo corrige. */
                query_out = 0;
                last_poll_ms = now_ms;
            }
        }

        while (gfx_poll_pointer(SAVANXP_WM_FD_EVENTS, &pointer) > 0)
        {
            uint32_t down = pointer.buttons & ~last_buttons;
            uint32_t up = last_buttons & ~pointer.buttons;
            struct sx_rect mute_rect = popup_mute_rect();
            struct sx_rect thumb = popup_thumb_rect(volume);
            int in_mute = popup_in_rect(mute_rect, pointer.x, pointer.y);
            int in_thumb = popup_in_rect(thumb, pointer.x, pointer.y);
            int in_trough = pointer.x >= VOLUMEPOPUP_TROUGH_X &&
                pointer.x < VOLUMEPOPUP_TROUGH_X + VOLUMEPOPUP_TROUGH_WIDTH &&
                pointer.y >= VOLUMEPOPUP_TROUGH_Y - 4 &&
                pointer.y < VOLUMEPOPUP_TROUGH_Y + VOLUMEPOPUP_TROUGH_HEIGHT + 4;
            int row;
            int in_row = -1;
            int in_row_trough = 0;
            int in_row_thumb = 0;
            struct sx_rect row_thumb = sx_rect_make(0, 0, 0, 0);

            for (row = 0; row < apps_total && row < VOLUMEPOPUP_APP_ROWS; ++row)
            {
                if (app_total_row_is_overflow(row, apps_total))
                {
                    break;
                }
                if (popup_in_rect(popup_app_rect(row), pointer.x, pointer.y))
                {
                    struct sx_rect trough = popup_app_trough_rect(row);
                    row_thumb = popup_app_thumb_rect(row, apps[row].volume);
                    in_row = row;
                    in_row_trough =
                        pointer.x >= trough.x - 2 && pointer.x < trough.x + trough.width + 2 &&
                        pointer.y >= trough.y - 4 && pointer.y < trough.y + trough.height + 4;
                    in_row_thumb = popup_in_rect(row_thumb, pointer.x, pointer.y);
                    break;
                }
            }

            /* Reconciliar con el estado real del boton, solo cuando el
             * evento no trae flanco: si se suelta afuera del popup, el up
             * lo recibe otro cliente y el arrastre o el armado quedarian
             * colgados hasta el proximo click. En el up propio no se toca
             * nada: la rama de abajo lo atiende con su persist. */
            if (((down | up) & SAVANXP_MOUSE_BUTTON_LEFT) == 0 &&
                (pointer.buttons & SAVANXP_MOUSE_BUTTON_LEFT) == 0)
            {
                if (drag_row == -2)
                {
                    drag_row = -1;
                    popup_persist(volume, muted);
                    needs_repaint = 1;
                }
                else if (drag_row >= 0)
                {
                    drag_row = -1;
                    needs_repaint = 1;
                }
                mute_armed = 0;
            }

            if (in_mute != mute_hot)
            {
                mute_hot = in_mute;
                needs_repaint = 1;
            }
            if (in_row != hot_row)
            {
                hot_row = in_row;
                needs_repaint = 1;
            }
            if (pointer.wheel != 0)
            {
                if (in_row >= 0 && census_sock >= 0)
                {
                    int next = (int)apps[in_row].volume + pointer.wheel * VOLUMEPOPUP_WHEEL_STEP;
                    if (next < 0)
                    {
                        next = 0;
                    }
                    if (next > 100)
                    {
                        next = 100;
                    }
                    if (next != (int)apps[in_row].volume)
                    {
                        apps[in_row].volume = (uint8_t)next;
                        (void)sx_audio_server_set_volume(
                            (int)census_sock, apps[in_row].port, next);
                        needs_repaint = 1;
                    }
                }
                else
                {
                    int next = volume + pointer.wheel * VOLUMEPOPUP_WHEEL_STEP;
                    if (next < 0)
                    {
                        next = 0;
                    }
                    if (next > 100)
                    {
                        next = 100;
                    }
                    if (next != volume)
                    {
                        volume = next;
                        (void)audio_set_volume((int)audio_fd, volume);
                        popup_persist(volume, muted);
                        needs_repaint = 1;
                    }
                }
            }
            if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
            {
                if (in_mute)
                {
                    mute_armed = 1;
                }
                else if (in_trough)
                {
                    drag_row = -2;
                    /* Agarrar el cursor por donde cayo en vez de
                     * recentrarlo: el salto se nota con 118 px de
                     * recorrido para 100 unidades. */
                    drag_offset = in_thumb ? pointer.x - thumb.x : VOLUMEPOPUP_THUMB_WIDTH / 2;
                    volume = popup_slider_volume_at(pointer.x - drag_offset, VOLUMEPOPUP_TROUGH_X, VOLUMEPOPUP_TROUGH_WIDTH - VOLUMEPOPUP_THUMB_WIDTH);
                    (void)audio_set_volume((int)audio_fd, volume);
                    needs_repaint = 1;
                }
                else if (in_row >= 0 && in_row_trough && census_sock >= 0)
                {
                    int travel =
                        VOLUMEPOPUP_APP_TROUGH_WIDTH - VOLUMEPOPUP_APP_THUMB_WIDTH;
                    drag_row = in_row;
                    drag_port = apps[in_row].port;
                    drag_offset = in_row_thumb
                        ? pointer.x - row_thumb.x
                        : VOLUMEPOPUP_APP_THUMB_WIDTH / 2;
                    apps[in_row].volume = (uint8_t)popup_slider_volume_at(
                        pointer.x - drag_offset, VOLUMEPOPUP_APP_TROUGH_X, travel);
                    (void)sx_audio_server_set_volume(
                        (int)census_sock, apps[in_row].port, apps[in_row].volume);
                    needs_repaint = 1;
                }
            }
            else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
            {
                if (mute_armed)
                {
                    if (in_mute)
                    {
                        muted = muted == 0 ? 1 : 0;
                        (void)audio_set_muted((int)audio_fd, muted);
                        popup_persist(volume, muted);
                    }
                    mute_armed = 0;
                    needs_repaint = 1;
                }
                if (drag_row == -2)
                {
                    drag_row = -1;
                    popup_persist(volume, muted);
                    needs_repaint = 1;
                }
                else if (drag_row >= 0)
                {
                    /* El nivel por app es sesion: no se persiste. */
                    drag_row = -1;
                    needs_repaint = 1;
                }
            }
            else if (drag_row == -2)
            {
                int next = popup_slider_volume_at(pointer.x - drag_offset, VOLUMEPOPUP_TROUGH_X, VOLUMEPOPUP_TROUGH_WIDTH - VOLUMEPOPUP_THUMB_WIDTH);
                if (next != volume)
                {
                    volume = next;
                    (void)audio_set_volume((int)audio_fd, volume);
                    needs_repaint = 1;
                }
            }
            else if (drag_row >= 0 && census_sock >= 0)
            {
                int travel = VOLUMEPOPUP_APP_TROUGH_WIDTH - VOLUMEPOPUP_APP_THUMB_WIDTH;
                int here = popup_find_row(apps, apps_total, drag_port);
                int next;
                if (here < 0)
                {
                    drag_row = -1;
                    needs_repaint = 1;
                }
                else
                {
                    drag_row = here;
                    next = popup_slider_volume_at(pointer.x - drag_offset, VOLUMEPOPUP_APP_TROUGH_X, travel);
                    if (next != (int)apps[here].volume)
                    {
                        apps[here].volume = (uint8_t)next;
                        (void)sx_audio_server_set_volume(
                            (int)census_sock, apps[here].port, next);
                        needs_repaint = 1;
                    }
                }
            }
            last_buttons = pointer.buttons;
        }

        if (needs_repaint)
        {
            popup_paint(&gfx, volume, muted, mute_hot, drag_row == -2, apps,
                        apps_total, drag_row, hot_row);
            if (gfx_present(&gfx, gfx.pixels) < 0)
            {
                break;
            }
            needs_repaint = 0;
        }
        else
        {
            sleep_ms(16);
        }
    }

    savanxp_close((int)audio_fd);
    gfx_close(&gfx);
    return 0;
}
