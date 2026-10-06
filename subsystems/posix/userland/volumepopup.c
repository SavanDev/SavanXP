#include "libc.h"
#include "savanxp/sxgui.h"

#include <stdio.h>

/*
 * Popup de volumen: binario aparte, lanzado on-demand por windowd cuando la
 * taskbar pide SAVANXP_DESKTOP_LAUNCH_FLAG_VOLUME_POPUP. Mismo molde que
 * kbdlayoutpopup.c (cliente sin bordes anclado por windowd, sin widgets: no
 * linkea sxgui, solo su paleta) pero con toggle: no sale solo, el WM lo mata
 * al pedirlo de nuevo o al clickear afuera.
 *
 * Aplica en vivo contra /dev/audio0 y persiste en /disk/audio.cfg al soltar
 * el slider o al mutear, con el mismo formato que escriben `volume` y lee
 * init ("75 0"). Las medidas salen de windowd (WINDOWD_VOLUME_POPUP_WIDTH y
 * _HEIGHT): cambiarlas ahi sin cambiarlas aca descuadra el dibujo.
 */

#define VOLUMEPOPUP_MARGIN 2
#define VOLUMEPOPUP_WIDTH 144
#define VOLUMEPOPUP_HEIGHT 56

#define VOLUMEPOPUP_MUTE_ROW_HEIGHT 20
#define VOLUMEPOPUP_MUTE_BOX 12
#define VOLUMEPOPUP_TROUGH_X 8
#define VOLUMEPOPUP_TROUGH_Y 32
#define VOLUMEPOPUP_TROUGH_WIDTH 128
#define VOLUMEPOPUP_TROUGH_HEIGHT 12
#define VOLUMEPOPUP_THUMB_WIDTH 10
#define VOLUMEPOPUP_THUMB_HEIGHT 16
#define VOLUMEPOPUP_WHEEL_STEP 5

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

static int popup_thumb_x(int volume)
{
    if (volume < 0)
    {
        volume = 0;
    }
    if (volume > 100)
    {
        volume = 100;
    }
    return VOLUMEPOPUP_TROUGH_X +
        (volume * (VOLUMEPOPUP_TROUGH_WIDTH - VOLUMEPOPUP_THUMB_WIDTH)) / 100;
}

static struct sx_rect popup_thumb_rect(int volume)
{
    int trough_center_y = VOLUMEPOPUP_TROUGH_Y + VOLUMEPOPUP_TROUGH_HEIGHT / 2;

    return sx_rect_make(
        popup_thumb_x(volume),
        trough_center_y - VOLUMEPOPUP_THUMB_HEIGHT / 2,
        VOLUMEPOPUP_THUMB_WIDTH,
        VOLUMEPOPUP_THUMB_HEIGHT);
}

static int popup_volume_at(int x)
{
    int travel = VOLUMEPOPUP_TROUGH_WIDTH - VOLUMEPOPUP_THUMB_WIDTH;
    int volume = ((x - VOLUMEPOPUP_TROUGH_X) * 100) / travel;

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

static int popup_in_rect(struct sx_rect rect, int x, int y)
{
    return x >= rect.x && x < rect.x + rect.width &&
        y >= rect.y && y < rect.y + rect.height;
}

static void popup_paint(
    struct savanxp_gfx_context *gfx,
    int volume,
    int muted,
    int mute_hot,
    int dragging)
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
    popup_bevel(&painter, thumb, dragging);
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
    int dragging = 0;
    int drag_offset = 0;
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

    for (;;)
    {
        while (gfx_poll_event(&gfx, &event) > 0)
        {
            if (event.type == SAVANXP_INPUT_EVENT_RESIZED)
            {
                (void)gfx_apply_resize_event(&gfx, &event);
                needs_repaint = 1;
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

            /* Reconciliar con el estado real del boton, solo cuando el
             * evento no trae flanco: si se suelta afuera del popup, el up
             * lo recibe otro cliente y el arrastre o el armado quedarian
             * colgados hasta el proximo click. En el up propio no se toca
             * nada: la rama de abajo lo atiende con su persist. */
            if (((down | up) & SAVANXP_MOUSE_BUTTON_LEFT) == 0 &&
                (pointer.buttons & SAVANXP_MOUSE_BUTTON_LEFT) == 0)
            {
                if (dragging)
                {
                    dragging = 0;
                    popup_persist(volume, muted);
                    needs_repaint = 1;
                }
                mute_armed = 0;
            }

            if (in_mute != mute_hot)
            {
                mute_hot = in_mute;
                needs_repaint = 1;
            }
            if (pointer.wheel != 0)
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
            if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
            {
                if (in_mute)
                {
                    mute_armed = 1;
                }
                else if (in_trough)
                {
                    dragging = 1;
                    /* Agarrar el cursor por donde cayo en vez de
                     * recentrarlo: el salto se nota con 118 px de
                     * recorrido para 100 unidades. */
                    drag_offset = in_thumb ? pointer.x - thumb.x : VOLUMEPOPUP_THUMB_WIDTH / 2;
                    volume = popup_volume_at(pointer.x - drag_offset);
                    (void)audio_set_volume((int)audio_fd, volume);
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
                if (dragging)
                {
                    dragging = 0;
                    popup_persist(volume, muted);
                    needs_repaint = 1;
                }
            }
            else if (dragging)
            {
                int next = popup_volume_at(pointer.x - drag_offset);
                if (next != volume)
                {
                    volume = next;
                    (void)audio_set_volume((int)audio_fd, volume);
                    needs_repaint = 1;
                }
            }
            last_buttons = pointer.buttons;
        }

        if (needs_repaint)
        {
            popup_paint(&gfx, volume, muted, mute_hot, dragging);
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
