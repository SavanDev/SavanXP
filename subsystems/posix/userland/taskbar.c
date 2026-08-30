#include "libc.h"
#include "savanxp/sxgui.h"
#include "savanxp/wm_shell_protocol.h"

#include "desktop_icons.h"

/*
 * Barra de tareas: un CLIENTE del WM, no chrome de windowd.
 *
 * Es el modelo de explorer.exe y el mismo layering que separo a shellui y
 * progman del window manager: windowd maneja ventanas, el shell las muestra.
 * Solo lista las ventanas abiertas -- sin menu inicio y sin area de
 * notificaciones, que es lo que se retiro con el chrome Win95 y no vuelve.
 *
 * Lo que no puede saber por su cuenta -- que ventanas hay, cual esta activa,
 * cual minimizada -- se lo cuenta el WM por la seccion compartida del fd 12
 * (ver savanxp/wm_shell_protocol.h), y los clicks vuelven como pedidos por el
 * fd 13. La barra NUNCA activa una ventana por su cuenta: pide, y el WM decide.
 */

#define TASKBAR_BUTTON_MAX_WIDTH 160
#define TASKBAR_BUTTON_MIN_WIDTH 40
#define TASKBAR_BUTTON_GAP 2
#define TASKBAR_MARGIN 2
#define TASKBAR_ICON_SIZE 16

/* Bisel 3D al estilo Win95, con las primitivas publicas del painter: sxgui solo
 * exporta widgets, y sus helpers de bisel son internos. La barra no usa ningun
 * widget, asi que no linkea el toolkit -- solo su paleta, que son macros. */
static void taskbar_bevel(struct sx_painter *painter, struct sx_rect rect, int sunken)
{
    uint32_t top_left = sunken ? SXGUI_COLOR_SHADOW : SXGUI_COLOR_LIGHT;
    uint32_t bottom_right = sunken ? SXGUI_COLOR_LIGHT : SXGUI_COLOR_SHADOW;

    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), bottom_right);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), bottom_right);
}

static struct savanxp_wm_window_list *g_list;
/* Copia estable para pintar: la seccion la escribe el WM en cualquier momento,
 * asi que se saca una foto consistente y se dibuja de ahi. */
static struct savanxp_wm_window_list g_snapshot;
static int g_pressed_index = -1;

/*
 * Lectura con seqlock: si la secuencia es impar el WM esta escribiendo, y si
 * cambio entre el antes y el despues la copia salio mezclada. En los dos casos
 * se reintenta. Sin esto, un repintado podria mostrar media lista vieja y media
 * nueva -- por ejemplo un titulo de una ventana con el flag de otra.
 */
static int taskbar_snapshot(void)
{
    int attempt;

    if (g_list == 0)
    {
        return 0;
    }
    for (attempt = 0; attempt < 8; ++attempt)
    {
        uint64_t before = g_list->sequence;
        uint64_t after;

        if ((before & 1u) != 0)
        {
            continue;
        }
        memcpy(&g_snapshot, g_list, sizeof(g_snapshot));
        after = g_list->sequence;
        if (before == after)
        {
            return 1;
        }
    }
    return 0;
}

static int taskbar_button_width(const struct savanxp_fb_info *info, int count)
{
    int usable;
    int width;

    if (count <= 0)
    {
        return 0;
    }
    usable = (int)info->width - (TASKBAR_MARGIN * 2) - (TASKBAR_BUTTON_GAP * (count - 1));
    width = usable / count;
    if (width > TASKBAR_BUTTON_MAX_WIDTH)
    {
        width = TASKBAR_BUTTON_MAX_WIDTH;
    }
    return width;
}

static struct sx_rect taskbar_button_rect(const struct savanxp_fb_info *info, int count, int index)
{
    int width = taskbar_button_width(info, count);
    int height = (int)info->height - (TASKBAR_MARGIN * 2);

    return sx_rect_make(
        TASKBAR_MARGIN + index * (width + TASKBAR_BUTTON_GAP),
        TASKBAR_MARGIN,
        width,
        height);
}

static void taskbar_paint(struct savanxp_gfx_context *gfx)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    int count = (int)g_snapshot.count;
    int index;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);

    sx_painter_fill_rect(&painter, sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height), SXGUI_COLOR_FACE);
    /* Filo claro arriba: la barra se lee como una superficie levantada sobre el
     * escritorio, igual que en Win95. */
    sx_painter_fill_rect(&painter, sx_rect_make(0, 0, (int)gfx->info.width, 1), SXGUI_COLOR_LIGHT);

    for (index = 0; index < count && index < (int)SAVANXP_WM_MAX_WINDOWS; ++index)
    {
        const struct savanxp_wm_window_entry *entry = &g_snapshot.windows[index];
        struct sx_rect rect = taskbar_button_rect(&gfx->info, count, index);
        int active = (entry->flags & SAVANXP_WM_WINDOW_FLAG_ACTIVE) != 0;
        int sunken = active || index == g_pressed_index;
        int text_x = rect.x + 6;
        const struct desktop_embedded_bitmap *icon;

        if (rect.width < TASKBAR_BUTTON_MIN_WIDTH)
        {
            break;
        }

        sx_painter_fill_rect(&painter, rect, SXGUI_COLOR_FACE);
        taskbar_bevel(&painter, rect, sunken);

        icon = desktop_icon_small((enum desktop_icon_id)entry->icon_id);
        if (icon != 0 && rect.width > TASKBAR_ICON_SIZE + 24)
        {
            struct savanxp_fb_info icon_info;
            struct sx_bitmap icon_bitmap;

            icon_info.width = icon->width;
            icon_info.height = icon->height;
            icon_info.pitch = icon->width * 4u;
            icon_info.bpp = 32;
            icon_info.buffer_size = icon_info.pitch * icon->height;
            sx_bitmap_wrap(&icon_bitmap, (uint32_t *)icon->pixels, &icon_info, SX_PIXEL_FORMAT_BGRA8888);
            sx_painter_blit_bitmap(
                &painter,
                &icon_bitmap,
                text_x,
                rect.y + (rect.height - (int)icon->height) / 2);
            text_x += TASKBAR_ICON_SIZE + 4;
        }

        {
            /* El texto se recorta al ancho del boton: con muchas ventanas los
             * botones se angostan y un titulo largo se derramaria sobre el de
             * al lado. */
            struct sx_rect clip = sx_rect_make(
                text_x,
                rect.y,
                rect.x + rect.width - 4 - text_x,
                rect.height);
            if (sx_painter_push_clip(&painter, clip))
            {
                int text_y = rect.y + (rect.height - gfx_text_height()) / 2;
                /* Un boton hundido corre su contenido un pixel, como los de
                 * sxgui: es lo que da la sensacion de que se aprieta. */
                sx_painter_draw_text(
                    &painter,
                    text_x + (sunken ? 1 : 0),
                    text_y + (sunken ? 1 : 0),
                    entry->title,
                    SXGUI_COLOR_TEXT);
                sx_painter_pop_clip(&painter);
            }
        }
    }
}

static int taskbar_hit(const struct savanxp_fb_info *info, int x, int y)
{
    int count = (int)g_snapshot.count;
    int index;

    for (index = 0; index < count && index < (int)SAVANXP_WM_MAX_WINDOWS; ++index)
    {
        struct sx_rect rect = taskbar_button_rect(info, count, index);
        if (rect.width < TASKBAR_BUTTON_MIN_WIDTH)
        {
            break;
        }
        if (x >= rect.x && x < rect.x + rect.width &&
            y >= rect.y && y < rect.y + rect.height)
        {
            return index;
        }
    }
    return -1;
}

static void taskbar_request(uint32_t action, uint32_t window_id)
{
    struct savanxp_wm_shell_request request;

    request.action = action;
    request.window_id = window_id;
    (void)write(SAVANXP_WM_FD_SHELL_REQUEST, &request, sizeof(request));
}

int main(void)
{
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;
    struct savanxp_gui_pointer_event pointer;
    uint64_t last_sequence = 0;
    int needs_repaint = 1;
    uint32_t last_buttons = 0;

    if (gfx_open(&gfx) < 0)
    {
        puts_fd(2, "taskbar: gfx_open failed\n");
        return 1;
    }
    if (gfx_acquire(&gfx) < 0)
    {
        puts_fd(2, "taskbar: gfx_acquire failed\n");
        gfx_close(&gfx);
        return 1;
    }

    g_list = (struct savanxp_wm_window_list *)map_view(
        SAVANXP_WM_FD_WINDOW_LIST, SAVANXP_SECTION_READ);
    if (g_list == 0 || result_is_error((long)g_list))
    {
        puts_fd(2, "taskbar: no se pudo mapear la lista de ventanas\n");
        gfx_close(&gfx);
        return 1;
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
            int index = taskbar_hit(&gfx.info, pointer.x, pointer.y);

            if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
            {
                g_pressed_index = index;
                needs_repaint = 1;
            }
            else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
            {
                if (index >= 0 && index == g_pressed_index &&
                    index < (int)g_snapshot.count)
                {
                    const struct savanxp_wm_window_entry *entry = &g_snapshot.windows[index];
                    /* Click sobre la ventana YA activa la minimiza, como en
                     * Win95: el boton es un toggle y no solo un "traeme esto". */
                    int active = (entry->flags & SAVANXP_WM_WINDOW_FLAG_ACTIVE) != 0;
                    int minimized = (entry->flags & SAVANXP_WM_WINDOW_FLAG_MINIMIZED) != 0;
                    taskbar_request(
                        (active && !minimized) ? SAVANXP_WM_SHELL_MINIMIZE : SAVANXP_WM_SHELL_ACTIVATE,
                        entry->window_id);
                }
                g_pressed_index = -1;
                needs_repaint = 1;
            }
            last_buttons = pointer.buttons;
        }

        if (g_list->sequence != last_sequence && taskbar_snapshot())
        {
            last_sequence = g_snapshot.sequence;
            needs_repaint = 1;
        }

        if (needs_repaint)
        {
            taskbar_paint(&gfx);
            /* present falla cuando el WM cierra la sesion o el link cae. */
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

    gfx_close(&gfx);
    return 0;
}
