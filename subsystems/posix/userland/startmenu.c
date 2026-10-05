#include "libc.h"
#include "savanxp/sxgui.h"

#include "progman_registry.h"
#include "desktop_icons.h"
#include "start_logo.h"

/*
 * Menu Inicio (verson recortada): cliente aparte, lanzado on-demand por la
 * taskbar via SAVANXP_DESKTOP_LAUNCH_FLAG_START_MENU y anclado abajo a la
 * izquierda por windowd (ver launch_start_menu_client). Sin .sxres a
 * proposito: sin categoria queda lanzable pero fuera del launcher, igual que
 * kbdlayoutpopup.
 *
 * Cada apertura es un proceso nuevo que escanea el catalogo de progman al
 * arrancar, asi que siempre esta fresco sin cache que invalidar. Lista plana
 * por grupos (cabecera + items con icono y nombre); sin cascada, sin teclado
 * y sin shutdown -- solo mouse, como el popup de layout. Se cierra solo
 * (exit) al lanzar, por toggle del boton Start, o por click afuera (el WM lo
 * destruye). Lo que no entra se scrollea con la rueda.
 */

#define STARTMENU_MARGIN 2
#define STARTMENU_ROW_HEIGHT 24
#define STARTMENU_HEADER_HEIGHT 20
#define STARTMENU_ICON_SIZE 16
#define STARTMENU_STRIP_WIDTH 24
#define STARTMENU_FOOTER_ROWS 2
/* Margen de separacion entre el listado y el pie: cara pelada, sin linea. */
#define STARTMENU_FOOTER_GAP 6
#define STARTMENU_FOOTER_HEIGHT (STARTMENU_FOOTER_GAP + 1 + STARTMENU_FOOTER_ROWS * STARTMENU_ROW_HEIGHT)
#define STARTMENU_MAX_ROWS (PROGMAN_MAX_GROUPS + PROGMAN_MAX_ITEMS)

#define STARTMENU_ROW_HEADER 0
#define STARTMENU_ROW_ITEM 1
#define STARTMENU_ROW_FOOTER 2

#define STARTMENU_ACTION_SHUTDOWN 0
#define STARTMENU_ACTION_REBOOT 1

struct startmenu_row
{
    int kind;
    int group;
    int item;
};

static struct startmenu_row g_rows[STARTMENU_MAX_ROWS];
static int g_row_count = 0;

/* Misma red que taskbar_bevel/popup_bevel: ninguno de los tres linkea el
 * toolkit, y son tres lineas. */
static void startmenu_bevel(struct sx_painter *painter, struct sx_rect rect, int sunken)
{
    uint32_t top_left = sunken ? SXGUI_COLOR_SHADOW : SXGUI_COLOR_LIGHT;
    uint32_t bottom_right = sunken ? SXGUI_COLOR_LIGHT : SXGUI_COLOR_SHADOW;

    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), bottom_right);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), bottom_right);
}

/* Franja lateral: degradado vertical en el tono de la base del titulo
 * (Windows Standard, sin acento), de oscuro arriba a claro abajo. 24 bandas
 * como el caption de windowd_render.c, por el mismo motivo. */
#define STARTMENU_STRIP_BANDS 24
static void startmenu_strip(struct sx_painter *painter, int x, int y, int height)
{
    int band;

    for (band = 0; band < STARTMENU_STRIP_BANDS; ++band)
    {
        int y0 = y + (height * band) / STARTMENU_STRIP_BANDS;
        int y1 = y + (height * (band + 1)) / STARTMENU_STRIP_BANDS;
        int amount = (STARTMENU_STRIP_BANDS > 1) ? (band * 255) / (STARTMENU_STRIP_BANDS - 1) : 0;
        uint32_t colour = SXCHROME_RGB(
            (uint8_t)((((int)16 - 0) * amount + 127) / 255),
            (uint8_t)((((int)132 - 0) * amount + 127) / 255),
            (uint8_t)(128 + (((int)208 - 128) * amount + 127) / 255));

        if (y1 > y0)
        {
            sx_painter_fill_rect(painter, sx_rect_make(x, y0, STARTMENU_STRIP_WIDTH, y1 - y0), colour);
        }
    }
}

static int startmenu_row_height(const struct startmenu_row *row)
{
    return row != 0 && row->kind == STARTMENU_ROW_HEADER ? STARTMENU_HEADER_HEIGHT : STARTMENU_ROW_HEIGHT;
}

static int startmenu_content_height(void)
{
    int total = 0;
    int index;

    for (index = 0; index < g_row_count; ++index)
    {
        total += startmenu_row_height(&g_rows[index]);
    }
    return total;
}

/* Vista del contenido (lo que scrollea): marco menos margenes, franja y pie. */
static void startmenu_view(const struct savanxp_fb_info *info, int *x, int *y, int *height)
{
    if (x != 0)
    {
        *x = STARTMENU_MARGIN + STARTMENU_STRIP_WIDTH;
    }
    if (y != 0)
    {
        *y = STARTMENU_MARGIN;
    }
    if (height != 0)
    {
        *height = (int)info->height - (STARTMENU_MARGIN * 2) - STARTMENU_FOOTER_HEIGHT;
    }
}

static struct sx_rect startmenu_footer_rect(int view_x, int view_bottom, int view_width, int action);

static int startmenu_footer_hit(int x, int y, int view_x, int view_bottom, int view_width)
{
    int action;

    if (x < view_x || x >= view_x + view_width)
    {
        return -1;
    }
    for (action = 0; action < STARTMENU_FOOTER_ROWS; ++action)
    {
        struct sx_rect rect = startmenu_footer_rect(view_x, view_bottom, view_width, action);

        if (y >= rect.y && y < rect.y + rect.height)
        {
            return action;
        }
    }
    return -1;
}
static int startmenu_row_top(int index)
{
    int top = 0;
    int cursor;

    for (cursor = 0; cursor < index && cursor < g_row_count; ++cursor)
    {
        top += startmenu_row_height(&g_rows[cursor]);
    }
    return top;
}

static int startmenu_max_scroll(int view_height)
{
    int overflow = startmenu_content_height() - view_height;

    return overflow > 0 ? overflow : 0;
}

/* Fila bajo el punto (coordenadas de superficie), o -1. La franja lateral
 * no es clickeable. */
static int startmenu_hit(int x, int y, int scroll, int view_x, int view_y, int view_height)
{
    int index;
    int relative = y - view_y + scroll;

    if (x < view_x || relative < 0)
    {
        return -1;
    }
    for (index = 0; index < g_row_count; ++index)
    {
        int height = startmenu_row_height(&g_rows[index]);

        if (relative < height)
        {
            return (y - view_y < view_height) ? index : -1;
        }
        relative -= height;
    }
    return -1;
}

static int path_is_launchable(const char *path)
{
    long fd;

    if (path == 0 || path[0] == '\0')
    {
        return 0;
    }
    fd = savanxp_open(path);
    if (fd < 0)
    {
        return 0;
    }
    (void)savanxp_close((int)fd);
    return 1;
}

/* Mismo orden de etapas que progman.c:build_catalog (ver progman_registry.h).
 * Aca corre una sola vez por apertura, asi que no hay refresh. */
static void build_catalog(void)
{
    int group;
    int item;

    (void)progman_registry_load_file();
    (void)progman_registry_prune_missing(path_is_launchable);
    (void)progman_registry_scan_programs(path_is_launchable);
    if (progman_item_count() == 0)
    {
        progman_registry_load_defaults();
        (void)progman_registry_prune_missing(path_is_launchable);
    }
    (void)progman_registry_apply_sxe();

    g_row_count = 0;
    for (group = 0; group < progman_group_count(); ++group)
    {
        const struct progman_group *info = progman_group_at(group);

        if (info == 0 || g_row_count >= STARTMENU_MAX_ROWS)
        {
            break;
        }
        g_rows[g_row_count].kind = STARTMENU_ROW_HEADER;
        g_rows[g_row_count].group = group;
        g_rows[g_row_count].item = -1;
        ++g_row_count;
        for (item = 0; item < info->item_count; ++item)
        {
            if (progman_group_item_at(group, item) == 0 || g_row_count >= STARTMENU_MAX_ROWS)
            {
                break;
            }
            g_rows[g_row_count].kind = STARTMENU_ROW_ITEM;
            g_rows[g_row_count].group = group;
            g_rows[g_row_count].item = item;
            ++g_row_count;
        }
    }
}

static void startmenu_draw_icon(struct sx_painter *painter, const struct desktop_embedded_bitmap *icon, int x, int y)
{
    struct sx_bitmap bitmap;
    struct savanxp_fb_info info;

    if (icon == 0 || icon->pixels == 0 || icon->width == 0 || icon->height == 0)
    {
        return;
    }
    info.width = icon->width;
    info.height = icon->height;
    info.pitch = icon->width * (uint32_t)sizeof(uint32_t);
    info.bpp = 32u;
    info.buffer_size = info.pitch * icon->height;
    sx_bitmap_wrap(&bitmap, (uint32_t *)icon->pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    if (icon->width == STARTMENU_ICON_SIZE && icon->height == STARTMENU_ICON_SIZE)
    {
        sx_painter_blit_bitmap(painter, &bitmap, x, y);
        return;
    }
    /* El registro hornea a 32: al achicar se promedia (bilineal), que es lo
     * que pide docs/SXGFX_ROADMAP.md para iconos. */
    sx_painter_draw_scaled_bitmap(
        painter,
        &bitmap,
        sx_rect_make(x, y, STARTMENU_ICON_SIZE, STARTMENU_ICON_SIZE),
        sx_rect_make(0, 0, (int)icon->width, (int)icon->height),
        SX_SCALE_BILINEAR);
}

static const char *k_footer_labels[STARTMENU_FOOTER_ROWS] = {"Shut Down", "Restart"};

/* Rect de la fila de pie `action` (0 = Shut Down, 1 = Restart). El pie va
 * pegado abajo, sobre la vista, y no scrollea con el contenido: primero el
 * margen de separacion, despues la linea, despues las filas. */
static struct sx_rect startmenu_footer_rect(int view_x, int view_bottom, int view_width, int action)
{
    return sx_rect_make(
        view_x,
        view_bottom + 1 + STARTMENU_FOOTER_GAP + action * STARTMENU_ROW_HEIGHT,
        view_width,
        STARTMENU_ROW_HEIGHT);
}

static void startmenu_paint(struct savanxp_gfx_context *gfx, int hot_row, int hot_footer, int scroll)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame;
    struct sx_rect view;
    int action;
    int index;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);

    frame = sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
    sx_painter_fill_rect(&painter, frame, SXGUI_COLOR_FACE);
    startmenu_bevel(&painter, frame, 0);
    startmenu_strip(
        &painter,
        frame.x + STARTMENU_MARGIN,
        frame.y + STARTMENU_MARGIN,
        frame.height - (STARTMENU_MARGIN * 2));

    view = sx_rect_make(
        frame.x + STARTMENU_MARGIN + STARTMENU_STRIP_WIDTH,
        frame.y + STARTMENU_MARGIN,
        frame.width - (STARTMENU_MARGIN * 2) - STARTMENU_STRIP_WIDTH,
        frame.height - (STARTMENU_MARGIN * 2) - STARTMENU_FOOTER_HEIGHT);
    if (!sx_painter_push_clip(&painter, view))
    {
        return;
    }

    if (g_row_count == 0)
    {
        int text_y = view.y + (view.height - gfx_text_height()) / 2;

        sx_painter_draw_text(&painter, view.x + 4, text_y, "(no programs)", SXGUI_COLOR_DISABLED_TEXT);
        sx_painter_pop_clip(&painter);
    }
    else
    {
        for (index = 0; index < g_row_count; ++index)
        {
            const struct startmenu_row *row = &g_rows[index];
            int height = startmenu_row_height(row);
            int y = view.y + startmenu_row_top(index) - scroll;
            struct sx_rect rect = sx_rect_make(view.x, y, view.width, height);

            /* Fila entera fuera del area util: ni se dibuja. */
            if (y + height <= view.y || y >= view.y + view.height)
            {
                continue;
            }
            if (row->kind == STARTMENU_ROW_HEADER)
            {
                const struct progman_group *group = progman_group_at(row->group);
                const char *name = (group != 0 && group->name[0] != '\0') ? group->name : "Programs";
                int text_y = y + (height - gfx_text_height()) / 2;

                sx_painter_fill_rect(&painter, rect, SXGUI_COLOR_FACE);
                sx_painter_draw_text(&painter, rect.x + 4, text_y, name, SXGUI_COLOR_DISABLED_TEXT);
                sx_painter_hline(&painter, rect.x, y + height - 1, rect.width, SXGUI_COLOR_SHADOW);
            }
            else
            {
                const struct progman_item *item = progman_group_item_at(row->group, row->item);
                const struct desktop_embedded_bitmap *icon = 0;
                int selected = (index == hot_row);
                uint32_t background = selected ? SXGUI_COLOR_SELECT : SXGUI_COLOR_FACE;
                uint32_t text_color = selected ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT;
                int text_x;
                int text_y;

                if (item == 0)
                {
                    continue;
                }
                sx_painter_fill_rect(&painter, rect, background);
                icon = progman_item_icon(item);
                if (icon == 0)
                {
                    icon = desktop_icon_small((enum desktop_icon_id)item->icon_id);
                }
                startmenu_draw_icon(
                    &painter,
                    icon,
                    rect.x + 4,
                    y + (height - STARTMENU_ICON_SIZE) / 2);
                text_x = rect.x + 4 + STARTMENU_ICON_SIZE + 4;
                text_y = y + (height - gfx_text_height()) / 2;
                if (sx_painter_push_clip(&painter, sx_rect_make(text_x, y, rect.x + rect.width - 2 - text_x, height)))
                {
                    sx_painter_draw_text(&painter, text_x, text_y, item->name, text_color);
                    sx_painter_pop_clip(&painter);
                }
            }
        }
        sx_painter_pop_clip(&painter);
    }

    /* Pie fijo: separador y las dos acciones, pegadas a la barra. */
    sx_painter_hline(
        &painter,
        view.x,
        view.y + view.height,
        view.width,
        SXGUI_COLOR_SHADOW);
    for (action = 0; action < STARTMENU_FOOTER_ROWS; ++action)
    {
        struct sx_rect rect = startmenu_footer_rect(view.x, view.y + view.height, view.width, action);
        int selected = (action == hot_footer);
        uint32_t background = selected ? SXGUI_COLOR_SELECT : SXGUI_COLOR_FACE;
        uint32_t text_color = selected ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT;
        int text_x = rect.x + 4 + STARTMENU_ICON_SIZE + 4;
        int text_y = rect.y + (rect.height - gfx_text_height()) / 2;

        sx_painter_fill_rect(&painter, rect, background);
        if (action == STARTMENU_ACTION_SHUTDOWN)
        {
            struct savanxp_fb_info icon_info;
            struct sx_bitmap icon_bitmap;

            icon_info.width = k_shutdown_icon.width;
            icon_info.height = k_shutdown_icon.height;
            icon_info.pitch = k_shutdown_icon.width * 4u;
            icon_info.bpp = 32;
            icon_info.buffer_size = icon_info.pitch * k_shutdown_icon.height;
            sx_bitmap_wrap(&icon_bitmap, (uint32_t *)k_shutdown_icon.pixels, &icon_info, SX_PIXEL_FORMAT_BGRA8888);
            sx_painter_blit_bitmap(
                &painter,
                &icon_bitmap,
                rect.x + 4,
                rect.y + (rect.height - (int)k_shutdown_icon.height) / 2);
        }
        sx_painter_draw_text(&painter, text_x, text_y, k_footer_labels[action], text_color);
    }
}

int main(void)
{
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;
    struct savanxp_gui_pointer_event pointer;
    int view_x = 0;
    int view_y = 0;
    int view_height = 0;
    int scroll = 0;
    int hot_row = -1;
    int hot_footer = -1;
    int pressed_row = -1;
    int pressed_footer = -1;
    int needs_repaint = 1;
    uint32_t last_buttons = 0;

    if (gfx_open(&gfx) < 0)
    {
        puts_fd(2, "startmenu: gfx_open failed\n");
        return 1;
    }
    if (gfx_acquire(&gfx) < 0)
    {
        puts_fd(2, "startmenu: gfx_acquire failed\n");
        gfx_close(&gfx);
        return 1;
    }

    build_catalog();
    startmenu_view(&gfx.info, &view_x, &view_y, &view_height);

    for (;;)
    {
        while (gfx_poll_event(&gfx, &event) > 0)
        {
            if (event.type == SAVANXP_INPUT_EVENT_RESIZED)
            {
                (void)gfx_apply_resize_event(&gfx, &event);
                startmenu_view(&gfx.info, &view_x, &view_y, &view_height);
                scroll = 0;
                hot_row = -1;
                hot_footer = -1;
                pressed_row = -1;
                pressed_footer = -1;
                needs_repaint = 1;
            }
        }

        while (gfx_poll_pointer(SAVANXP_WM_FD_EVENTS, &pointer) > 0)
        {
            uint32_t down = pointer.buttons & ~last_buttons;
            uint32_t up = last_buttons & ~pointer.buttons;
            int view_bottom = view_y + view_height;
            int view_width = (int)gfx.info.width - view_x - STARTMENU_MARGIN;
            int row = startmenu_hit(pointer.x, pointer.y, scroll, view_x, view_y, view_height);
            int footer = startmenu_footer_hit(pointer.x, pointer.y, view_x, view_bottom, view_width);

            /* Solo las filas de programa se pueden apretar; las cabeceras
             * solo se iluminan al pasar. */
            if (row >= 0 && row < g_row_count && g_rows[row].kind != STARTMENU_ROW_ITEM)
            {
                row = -1;
            }
            if (pointer.wheel != 0)
            {
                /* Tres filas por muesca, como el editor. */
                scroll -= pointer.wheel * 3 * STARTMENU_ROW_HEIGHT;
                if (scroll < 0)
                {
                    scroll = 0;
                }
                if (scroll > startmenu_max_scroll(view_height))
                {
                    scroll = startmenu_max_scroll(view_height);
                }
                row = startmenu_hit(pointer.x, pointer.y, scroll, view_x, view_y, view_height);
                if (row >= 0 && row < g_row_count && g_rows[row].kind != STARTMENU_ROW_ITEM)
                {
                    row = -1;
                }
                needs_repaint = 1;
            }
            if (row != hot_row || footer != hot_footer)
            {
                hot_row = row;
                hot_footer = footer;
                needs_repaint = 1;
            }
            if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
            {
                pressed_row = row;
                pressed_footer = footer;
            }
            else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
            {
                if (footer >= 0 && footer == pressed_footer)
                {
                    /* Sin confirmacion en la version recortada: el click es
                     * deliberado sobre la fila del pie. */
                    if (footer == STARTMENU_ACTION_REBOOT)
                    {
                        (void)power_reboot();
                    }
                    else
                    {
                        (void)power_shutdown();
                    }
                }
                else if (row >= 0 && row == pressed_row)
                {
                    const struct progman_item *item = progman_group_item_at(g_rows[row].group, g_rows[row].item);

                    if (item != 0 && gfx_desktop_launch_ex(&gfx, item->path, item->launch_flags) == 0)
                    {
                        gfx_close(&gfx);
                        return 0;
                    }
                }
                pressed_row = -1;
                pressed_footer = -1;
            }
            last_buttons = pointer.buttons;
        }

        if (needs_repaint)
        {
            startmenu_paint(&gfx, hot_row, hot_footer, scroll);
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
