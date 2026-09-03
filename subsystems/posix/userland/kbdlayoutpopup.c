#include "libc.h"
#include "savanxp/sxgui.h"

/*
 * Popup del selector de layout de teclado (ES/EN): binario aparte, lanzado
 * on-demand por windowd cuando la taskbar pide SAVANXP_DESKTOP_LAUNCH_FLAG_TASKBAR_POPUP.
 * windowd lo ancla arriba de la franja, sin bordes -- ver launch_keyboard_popup_client
 * en windowd.c. No linkea sxgui (solo su paleta) ni recibe foco de teclado:
 * es puramente mouse-driven y se cierra solo (exit) al elegir una fila.
 */

#define KBDLAYOUTPOPUP_MARGIN 2
#define KBDLAYOUTPOPUP_ROW_COUNT 2
#define KBDLAYOUTPOPUP_ROW_HEIGHT 20

static const char *k_row_labels[KBDLAYOUTPOPUP_ROW_COUNT] = {"Espanol", "English"};

/* Mismo bisel que taskbar_bevel (taskbar.c) -- copiado, no compartido: ninguno
 * de los dos linkea el toolkit, y es tres lineas. */
static void popup_bevel(struct sx_painter *painter, struct sx_rect rect, int sunken)
{
    uint32_t top_left = sunken ? SXGUI_COLOR_SHADOW : SXGUI_COLOR_LIGHT;
    uint32_t bottom_right = sunken ? SXGUI_COLOR_LIGHT : SXGUI_COLOR_SHADOW;

    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), bottom_right);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), bottom_right);
}

static struct sx_rect popup_row_rect(const struct savanxp_fb_info *info, int row)
{
    return sx_rect_make(
        KBDLAYOUTPOPUP_MARGIN,
        KBDLAYOUTPOPUP_MARGIN + row * KBDLAYOUTPOPUP_ROW_HEIGHT,
        (int)info->width - (KBDLAYOUTPOPUP_MARGIN * 2),
        KBDLAYOUTPOPUP_ROW_HEIGHT);
}

static int popup_hit(const struct savanxp_fb_info *info, int x, int y)
{
    int row;

    for (row = 0; row < KBDLAYOUTPOPUP_ROW_COUNT; ++row)
    {
        struct sx_rect rect = popup_row_rect(info, row);
        if (x >= rect.x && x < rect.x + rect.width &&
            y >= rect.y && y < rect.y + rect.height)
        {
            return row;
        }
    }
    return -1;
}

static void popup_paint(struct savanxp_gfx_context *gfx, int current_layout, int hot_row)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame;
    int row;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);

    frame = sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
    sx_painter_fill_rect(&painter, frame, SXGUI_COLOR_FACE);
    popup_bevel(&painter, frame, 0);

    for (row = 0; row < KBDLAYOUTPOPUP_ROW_COUNT; ++row)
    {
        struct sx_rect rect = popup_row_rect(&gfx->info, row);
        int selected = row == hot_row;
        uint32_t background = selected ? SXGUI_COLOR_SELECT : SXGUI_COLOR_FACE;
        uint32_t text_color = selected ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT;
        int text_x = rect.x + 4;
        int text_y = rect.y + (rect.height - gfx_text_height()) / 2;

        sx_painter_fill_rect(&painter, rect, background);
        /* La fila del layout activo lleva una marca a la izquierda -- no hay
         * negrita en esta fuente, asi que un prefijo es la forma simple de
         * distinguirla sin depender de otra pasada de dibujo. */
        sx_painter_draw_text(
            &painter,
            text_x,
            text_y,
            row == current_layout ? "> " : "  ",
            text_color);
        sx_painter_draw_text(
            &painter,
            text_x + gfx_text_width("> "),
            text_y,
            k_row_labels[row],
            text_color);
    }
}

int main(void)
{
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;
    struct savanxp_gui_pointer_event pointer;
    long input_fd;
    int current_layout = SAVANXP_KEYBOARD_LAYOUT_ES;
    int pressed_row = -1;
    int hot_row = -1;
    int needs_repaint = 1;
    uint32_t last_buttons = 0;

    if (gfx_open(&gfx) < 0)
    {
        puts_fd(2, "kbdlayoutpopup: gfx_open failed\n");
        return 1;
    }
    if (gfx_acquire(&gfx) < 0)
    {
        puts_fd(2, "kbdlayoutpopup: gfx_acquire failed\n");
        gfx_close(&gfx);
        return 1;
    }

    /* Solo para el ioctl: jamas hay que leer de este fd -- la cola de
     * /dev/input0 es global y windowd es quien la drena de verdad. */
    input_fd = savanxp_open_mode("/dev/input0", SAVANXP_OPEN_READ);
    if (input_fd >= 0)
    {
        long layout = input_get_layout((int)input_fd);
        if (layout == SAVANXP_KEYBOARD_LAYOUT_ES || layout == SAVANXP_KEYBOARD_LAYOUT_EN)
        {
            current_layout = (int)layout;
        }
    }

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

        while (gfx_poll_pointer(SAVANXP_WM_FD_MOUSE, &pointer) > 0)
        {
            uint32_t down = pointer.buttons & ~last_buttons;
            uint32_t up = last_buttons & ~pointer.buttons;
            int row = popup_hit(&gfx.info, pointer.x, pointer.y);

            if (row != hot_row)
            {
                hot_row = row;
                needs_repaint = 1;
            }
            if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
            {
                pressed_row = row;
            }
            else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
            {
                if (row >= 0 && row == pressed_row)
                {
                    if (input_fd >= 0)
                    {
                        (void)input_set_layout((int)input_fd, row);
                    }
                    {
                        char digit = (char)('0' + row);
                        int config_fd = (int)savanxp_open_mode(
                            "/disk/keyboard.cfg",
                            SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
                        if (config_fd >= 0)
                        {
                            (void)savanxp_write(config_fd, &digit, 1);
                            savanxp_close(config_fd);
                        }
                    }
                    if (input_fd >= 0)
                    {
                        savanxp_close((int)input_fd);
                    }
                    gfx_close(&gfx);
                    return 0;
                }
                pressed_row = -1;
            }
            last_buttons = pointer.buttons;
        }

        if (needs_repaint)
        {
            popup_paint(&gfx, current_layout, hot_row);
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

    if (input_fd >= 0)
    {
        savanxp_close((int)input_fd);
    }
    gfx_close(&gfx);
    return 0;
}
