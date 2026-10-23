#include "desktop.h"

#include "progman_registry.h"
#include "desktop_icons.h"
#include "start_logo.h"

/*
 * The Start menu: a popup of the desktop, anchored at the bottom left above
 * the taskbar. It scans progman's catalog every time it opens, so it is always
 * fresh with no cache to invalidate. A flat list by group (header plus items
 * with icon and name), and Shut Down/Restart with a confirmation at the bottom;
 * no cascades and no keyboard -- mouse only. It closes when it launches a
 * program, when the Start button is clicked again, or on a press outside it
 * (windowd asks, the desktop closes it). What does not fit scrolls with the
 * wheel, or, as in Windows 98, by resting the pointer on the arrow strips the
 * list grows at its top and bottom when it overflows. The real Yes powers the
 * VM off and is not exercised in automation.
 */

#define STARTMENU_WIDTH 232
#define STARTMENU_HEIGHT 512
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
/* The arrow strips of an overflowing list, and how fast resting on one
 * scrolls it: 8 pixels every 40 ms, a 24px row in about a tenth of a second. */
#define STARTMENU_SCROLL_STRIP_HEIGHT 12
#define STARTMENU_SCROLL_STEP 8
#define STARTMENU_SCROLL_INTERVAL_MS 40ul
#define STARTMENU_ARROW_NONE (-1)
#define STARTMENU_ARROW_UP 0
#define STARTMENU_ARROW_DOWN 1

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

/* The part of the view the rows scroll in. When the catalog does not fit, the
 * view gives up a strip at the top and one at the bottom for the arrows. */
static int startmenu_overflows(int view_height)
{
    return startmenu_content_height() > view_height;
}

static void startmenu_list(int view_y, int view_height, int *list_y, int *list_height)
{
    int strip = startmenu_overflows(view_height) ? STARTMENU_SCROLL_STRIP_HEIGHT : 0;

    *list_y = view_y + strip;
    *list_height = view_height - strip * 2;
}

static struct sx_rect startmenu_arrow_rect(int view_x, int view_y, int view_width, int view_height, int arrow)
{
    int y = arrow == STARTMENU_ARROW_UP ? view_y : view_y + view_height - STARTMENU_SCROLL_STRIP_HEIGHT;

    return sx_rect_make(view_x, y, view_width, STARTMENU_SCROLL_STRIP_HEIGHT);
}

/* The arrow strip under the point, or STARTMENU_ARROW_NONE (also when the list
 * fits and there are no strips). */
static int startmenu_arrow_hit(int x, int y, int view_x, int view_y, int view_width, int view_height)
{
    int arrow;

    if (!startmenu_overflows(view_height))
    {
        return STARTMENU_ARROW_NONE;
    }
    for (arrow = STARTMENU_ARROW_UP; arrow <= STARTMENU_ARROW_DOWN; ++arrow)
    {
        struct sx_rect rect = startmenu_arrow_rect(view_x, view_y, view_width, view_height, arrow);

        if (x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height)
        {
            return arrow;
        }
    }
    return STARTMENU_ARROW_NONE;
}

static void startmenu_draw_up_arrow(struct sx_painter *painter, struct sx_rect rect, uint32_t colour)
{
    sxchrome_draw_arrow(painter, rect, SXCHROME_ARROW_UP, colour);
}

static void startmenu_draw_down_arrow(struct sx_painter *painter, struct sx_rect rect, uint32_t colour)
{
    sxchrome_draw_arrow(painter, rect, SXCHROME_ARROW_DOWN, colour);
}

/* An arrow that can still scroll is black; one at its end of the list is
 * etched grey, the way Windows greys a menu arrow that has nothing left. */
static void startmenu_paint_arrow(struct sx_painter *painter, struct sx_rect rect, int arrow, int enabled)
{
    sxchrome_glyph_fn draw = arrow == STARTMENU_ARROW_UP ? startmenu_draw_up_arrow : startmenu_draw_down_arrow;

    sx_painter_fill_rect(painter, rect, SXGUI_COLOR_FACE);
    if (enabled)
    {
        draw(painter, rect, SXGUI_COLOR_TEXT);
    }
    else
    {
        sxchrome_draw_glyph_disabled(painter, rect, draw);
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
    /* A binary with only a large icon: shrinking averages (bilinear), which
     * is what docs/desktop/SXGFX_ROADMAP.md asks for icons. */
    sx_painter_draw_scaled_bitmap(
        painter,
        &bitmap,
        sx_rect_make(x, y, STARTMENU_ICON_SIZE, STARTMENU_ICON_SIZE),
        sx_rect_make(0, 0, (int)icon->width, (int)icon->height),
        SX_SCALE_BILINEAR);
}

static const char *k_footer_labels[STARTMENU_FOOTER_ROWS] = {"Shut Down", "Restart"};
static const char *k_footer_armed_labels[STARTMENU_FOOTER_ROWS] = {"Yes, shut down", "Yes, restart"};
static const char *k_footer_armed_cancel = "No";

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

static void startmenu_paint(struct savanxp_gfx_context *gfx, int hot_row, int hot_footer, int armed, int scroll)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame;
    struct sx_rect view;
    struct sx_rect list;
    int list_y;
    int list_height;
    int action;
    int index;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);

    frame = sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
    sx_painter_fill_rect(&painter, frame, SXGUI_COLOR_FACE);
    desktop_bevel(&painter, frame, 0);
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
    startmenu_list(view.y, view.height, &list_y, &list_height);
    list = sx_rect_make(view.x, list_y, view.width, list_height);
    if (startmenu_overflows(view.height))
    {
        startmenu_paint_arrow(
            &painter,
            startmenu_arrow_rect(view.x, view.y, view.width, view.height, STARTMENU_ARROW_UP),
            STARTMENU_ARROW_UP,
            scroll > 0);
        startmenu_paint_arrow(
            &painter,
            startmenu_arrow_rect(view.x, view.y, view.width, view.height, STARTMENU_ARROW_DOWN),
            STARTMENU_ARROW_DOWN,
            scroll < startmenu_max_scroll(list_height));
    }
    if (!sx_painter_push_clip(&painter, list))
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
            int y = list.y + startmenu_row_top(index) - scroll;
            struct sx_rect rect = sx_rect_make(list.x, y, list.width, height);

            /* A row entirely outside the list is not drawn at all. */
            if (y + height <= list.y || y >= list.y + list.height)
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
                /* The 16x16 drawing first; the 32x32 shrunk only when the
                 * binary has no small icon of its own. */
                icon = progman_item_icon_small(item);
                if (icon == 0)
                {
                    icon = progman_item_icon(item);
                }
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

    /* Pie fijo: separador y las dos acciones, pegadas a la barra. Sin armar
     * son Shut Down / Restart; armada una, la fila 0 confirma y la 1 cancela. */
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
        const char *label = k_footer_labels[action];
        int text_x = rect.x + 4 + STARTMENU_ICON_SIZE + 4;
        int text_y = rect.y + (rect.height - gfx_text_height()) / 2;

        sx_painter_fill_rect(&painter, rect, background);
        if (armed < 0)
        {
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
        }
        else if (action == 0)
        {
            label = k_footer_armed_labels[armed];
        }
        else
        {
            label = k_footer_armed_cancel;
        }
        sx_painter_draw_text(&painter, text_x, text_y, label, text_color);
    }
}

static int g_view_x = 0;
static int g_view_y = 0;
static int g_view_height = 0;
/* Where the rows scroll: the view minus the arrow strips, if any. */
static int g_list_y = 0;
static int g_list_height = 0;
static int g_scroll = 0;
/* The arrow strip the pointer rests on, and when it scrolls next. */
static int g_hot_arrow = STARTMENU_ARROW_NONE;
static unsigned long g_next_scroll_ms = 0;
static int g_armed = -1;
static int g_hot_row = -1;
static int g_hot_footer = -1;
static int g_pressed_row = -1;
static int g_pressed_footer = -1;
static uint32_t g_last_buttons = 0;

static int startmenu_open(void)
{
    struct savanxp_fb_info info;

    build_catalog();
    memset(&info, 0, sizeof(info));
    info.width = STARTMENU_WIDTH;
    info.height = STARTMENU_HEIGHT;
    startmenu_view(&info, &g_view_x, &g_view_y, &g_view_height);
    startmenu_list(g_view_y, g_view_height, &g_list_y, &g_list_height);
    g_scroll = 0;
    g_hot_arrow = STARTMENU_ARROW_NONE;
    g_next_scroll_ms = 0;
    g_armed = -1;
    g_hot_row = -1;
    g_hot_footer = -1;
    g_pressed_row = -1;
    g_pressed_footer = -1;
    g_last_buttons = 0;
    return 0;
}

static void startmenu_close(void)
{
}

static uint32_t startmenu_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    uint32_t down = pointer->buttons & ~g_last_buttons;
    uint32_t up = g_last_buttons & ~pointer->buttons;
    int view_bottom = g_view_y + g_view_height;
    int view_width = (int)gfx->info.width - g_view_x - STARTMENU_MARGIN;
    int row = startmenu_hit(pointer->x, pointer->y, g_scroll, g_view_x, g_list_y, g_list_height);
    int footer = startmenu_footer_hit(pointer->x, pointer->y, g_view_x, view_bottom, view_width);
    int arrow = startmenu_arrow_hit(pointer->x, pointer->y, g_view_x, g_view_y, view_width, g_view_height);
    uint32_t actions = DESKTOP_ACTION_NONE;

    g_last_buttons = pointer->buttons;
    if (arrow != g_hot_arrow)
    {
        /* Arriving on a strip scrolls at once; startmenu_tick keeps going. */
        g_hot_arrow = arrow;
        g_next_scroll_ms = 0;
    }
    /* Only program rows can be pressed; headers only light up on hover. */
    if (row >= 0 && row < g_row_count && g_rows[row].kind != STARTMENU_ROW_ITEM)
    {
        row = -1;
    }
    if (pointer->wheel != 0)
    {
        /* Three rows per notch, like the editor. */
        g_scroll -= pointer->wheel * 3 * STARTMENU_ROW_HEIGHT;
        if (g_scroll < 0)
        {
            g_scroll = 0;
        }
        if (g_scroll > startmenu_max_scroll(g_list_height))
        {
            g_scroll = startmenu_max_scroll(g_list_height);
        }
        row = startmenu_hit(pointer->x, pointer->y, g_scroll, g_view_x, g_list_y, g_list_height);
        if (row >= 0 && row < g_row_count && g_rows[row].kind != STARTMENU_ROW_ITEM)
        {
            row = -1;
        }
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if (row != g_hot_row || footer != g_hot_footer)
    {
        g_hot_row = row;
        g_hot_footer = footer;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        g_pressed_row = row;
        g_pressed_footer = footer;
    }
    else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        if (footer >= 0 && footer == g_pressed_footer)
        {
            /* Two-step confirmation: the first click arms, the second on row
             * 0 runs it and on row 1 cancels. The real Yes is not exercised
             * in automation (it powers the VM off). */
            if (g_armed < 0)
            {
                g_armed = footer;
            }
            else if (footer == 0)
            {
                if (g_armed == STARTMENU_ACTION_REBOOT)
                {
                    (void)power_reboot();
                }
                else
                {
                    (void)power_shutdown();
                }
            }
            else
            {
                g_armed = -1;
            }
            actions |= DESKTOP_ACTION_REPAINT;
        }
        else if (row >= 0 && row == g_pressed_row)
        {
            const struct progman_item *item = progman_group_item_at(g_rows[row].group, g_rows[row].item);

            if (item != 0 && desktop_launch(item->path, item->launch_flags) >= 0)
            {
                actions |= DESKTOP_ACTION_CLOSE;
            }
        }
        g_pressed_row = -1;
        g_pressed_footer = -1;
    }
    return actions;
}

/* Resting on an arrow strip scrolls without any pointer event arriving, so the
 * step is driven from the desktop loop. It stops at the end of the list. */
static uint32_t startmenu_tick(void)
{
    unsigned long now;
    int target;
    int limit;

    if (g_hot_arrow == STARTMENU_ARROW_NONE)
    {
        return DESKTOP_ACTION_NONE;
    }
    now = uptime_ms();
    if (now < g_next_scroll_ms)
    {
        return DESKTOP_ACTION_NONE;
    }
    limit = startmenu_max_scroll(g_list_height);
    target = g_scroll + (g_hot_arrow == STARTMENU_ARROW_UP ? -STARTMENU_SCROLL_STEP : STARTMENU_SCROLL_STEP);
    if (target < 0)
    {
        target = 0;
    }
    if (target > limit)
    {
        target = limit;
    }
    if (target == g_scroll)
    {
        return DESKTOP_ACTION_NONE;
    }
    g_scroll = target;
    g_next_scroll_ms = now + STARTMENU_SCROLL_INTERVAL_MS;
    return DESKTOP_ACTION_REPAINT;
}

/* The pointer went to the background or the taskbar: stop scrolling. */
static uint32_t startmenu_pointer_left(void)
{
    g_hot_arrow = STARTMENU_ARROW_NONE;
    return DESKTOP_ACTION_NONE;
}

static void startmenu_paint_window(struct savanxp_gfx_context *gfx)
{
    startmenu_paint(gfx, g_hot_row, g_hot_footer, g_armed, g_scroll);
}

const struct desktop_popup_ops desktop_startmenu_ops = {
    STARTMENU_WIDTH,
    STARTMENU_HEIGHT,
    0,
    startmenu_open,
    startmenu_close,
    startmenu_pointer,
    startmenu_tick,
    startmenu_paint_window,
    0,
    startmenu_pointer_left,
};
