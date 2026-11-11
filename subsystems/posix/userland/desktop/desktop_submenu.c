#include "desktop.h"

#include "progman_registry.h"
#include "desktop_icons.h"

/*
 * The Start menu's cascade: the popup that opens to the right of Programs or
 * Settings (docs/desktop/START_MENU.md). One level only: Programs lists every
 * group of progman's catalog except Settings, each under a grey header with
 * its items; Settings lists the Settings group's items, with no header.
 *
 * It lives in the tray popups' window (window id 3): a process has four owned
 * windows and the desktop uses them all, and the tray popups never need to be
 * open while the Start menu is. A press on it is a press on the desktop, so
 * windowd does not dismiss the Start menu for it, nor the other way round.
 *
 * The catalog is kept between openings and rebuilt only when
 * progman_registry_disk_signature() says the disk under it changed: building
 * it reads every binary in /bin, and doing that on each click is what made
 * the menu take a second to appear. What does not fit on the screen scrolls
 * with the wheel or, as in Windows 98, by resting the pointer on the arrow
 * strips the list grows at its top and bottom when it overflows.
 *
 * Launching a program closes the whole menu. Moving the pointer onto the
 * cascade tells the Start menu, so it keeps this one open and its parent row
 * lit while the pointer is here.
 */

#define SUBMENU_MARGIN 2
#define SUBMENU_ROW_HEIGHT 22
#define SUBMENU_HEADER_HEIGHT 20
#define SUBMENU_ICON_SIZE 16
#define SUBMENU_ICON_GAP 6
/* Room right of the longest name, as a Windows menu leaves. */
#define SUBMENU_TEXT_PAD 18
#define SUBMENU_MIN_WIDTH 140
#define SUBMENU_MAX_WIDTH 320
#define SUBMENU_MAX_ROWS (PROGMAN_MAX_GROUPS + PROGMAN_MAX_ITEMS)
/* The arrow strips of an overflowing list, and how fast resting on one
 * scrolls it: 8 pixels every 40 ms, a row in about a tenth of a second. */
#define SUBMENU_SCROLL_STRIP_HEIGHT 12
#define SUBMENU_SCROLL_STEP 8
#define SUBMENU_SCROLL_INTERVAL_MS 40ul
#define SUBMENU_ARROW_NONE (-1)
#define SUBMENU_ARROW_UP 0
#define SUBMENU_ARROW_DOWN 1

#define SUBMENU_ROW_HEADER 0
#define SUBMENU_ROW_ITEM 1

/* The progman group whose items Settings lists; every other group is a
 * program group. */
#define SUBMENU_SETTINGS_GROUP "Settings"

struct submenu_row
{
    int kind;
    int group;
    int item;
};

static struct submenu_row g_rows[SUBMENU_MAX_ROWS];
static int g_row_count = 0;
static int g_source = DESKTOP_SUBMENU_PROGRAMS;

static int g_width = SUBMENU_MIN_WIDTH;
static int g_height = SUBMENU_ROW_HEIGHT + SUBMENU_MARGIN * 2;
/* Where the rows scroll: the view minus the arrow strips, if any. */
static int g_list_y = 0;
static int g_list_height = 0;
static int g_scroll = 0;
/* The arrow strip the pointer rests on, and when it scrolls next. */
static int g_hot_arrow = SUBMENU_ARROW_NONE;
static unsigned long g_next_scroll_ms = 0;
static int g_hot_row = -1;
static int g_pressed_row = -1;
static uint32_t g_last_buttons = 0;
/* A program was launched: closing the cascade closes the Start menu too. */
static int g_launched = 0;

/* The catalog, and the disk signature taken right after building it. */
static int g_catalog_built = 0;
static uint32_t g_catalog_signature = 0;

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

/* The same stages, in the same order, as progman.c:build_catalog (see
 * progman_registry.h). Rebuilds only when the disk changed since last time. */
void desktop_submenu_refresh_catalog(void)
{
    if (g_catalog_built && progman_registry_disk_signature() == g_catalog_signature)
    {
        return;
    }
    (void)progman_registry_load_file();
    (void)progman_registry_prune_missing(path_is_launchable);
    (void)progman_registry_scan_programs(path_is_launchable);
    if (progman_item_count() == 0)
    {
        progman_registry_load_defaults();
        (void)progman_registry_prune_missing(path_is_launchable);
    }
    (void)progman_registry_apply_sxe();
    g_catalog_signature = progman_registry_disk_signature();
    g_catalog_built = 1;
}

static int group_is_settings(const struct progman_group *group)
{
    return group != 0 && strcmp(group->name, SUBMENU_SETTINGS_GROUP) == 0;
}

static int group_in_source(const struct progman_group *group, int source)
{
    return source == DESKTOP_SUBMENU_SETTINGS ? group_is_settings(group) : !group_is_settings(group);
}

/* The rows `source` shows: a header per group only in Programs. */
static int build_rows(int source, struct submenu_row *rows, int capacity)
{
    int count = 0;
    int group;
    int item;

    for (group = 0; group < progman_group_count(); ++group)
    {
        const struct progman_group *info = progman_group_at(group);

        if (info == 0 || !group_in_source(info, source) || info->item_count == 0)
        {
            continue;
        }
        if (source == DESKTOP_SUBMENU_PROGRAMS && count < capacity)
        {
            rows[count].kind = SUBMENU_ROW_HEADER;
            rows[count].group = group;
            rows[count].item = -1;
            ++count;
        }
        for (item = 0; item < info->item_count && count < capacity; ++item)
        {
            if (progman_group_item_at(group, item) == 0)
            {
                break;
            }
            rows[count].kind = SUBMENU_ROW_ITEM;
            rows[count].group = group;
            rows[count].item = item;
            ++count;
        }
    }
    return count;
}

int desktop_submenu_is_empty(int source)
{
    struct submenu_row probe[1];

    return build_rows(source, probe, 1) == 0;
}

void desktop_submenu_select(int source)
{
    g_source = source == DESKTOP_SUBMENU_SETTINGS ? DESKTOP_SUBMENU_SETTINGS : DESKTOP_SUBMENU_PROGRAMS;
}

int desktop_submenu_source(void)
{
    return g_source;
}

static int row_height(const struct submenu_row *row)
{
    return row->kind == SUBMENU_ROW_HEADER ? SUBMENU_HEADER_HEIGHT : SUBMENU_ROW_HEIGHT;
}

static int content_height(void)
{
    int total = 0;
    int index;

    for (index = 0; index < g_row_count; ++index)
    {
        total += row_height(&g_rows[index]);
    }
    return total;
}

static int row_top(int index)
{
    int top = 0;
    int cursor;

    for (cursor = 0; cursor < index && cursor < g_row_count; ++cursor)
    {
        top += row_height(&g_rows[cursor]);
    }
    return top;
}

static const char *row_label(const struct submenu_row *row)
{
    if (row->kind == SUBMENU_ROW_HEADER)
    {
        const struct progman_group *group = progman_group_at(row->group);

        return (group != 0 && group->name[0] != '\0') ? group->name : "Programs";
    }
    else
    {
        const struct progman_item *item = progman_group_item_at(row->group, row->item);

        return item != 0 ? item->name : "";
    }
}

/* The view is the whole inside of the frame: no banner, no footer. */
static int view_height(void)
{
    return g_height - SUBMENU_MARGIN * 2;
}

static int overflows(void)
{
    return content_height() > view_height();
}

static int max_scroll(void)
{
    int overflow = content_height() - g_list_height;

    return overflow > 0 ? overflow : 0;
}

/* Sized to its content, as a Windows menu is: as wide as the longest name,
 * as tall as its rows, but never taller than the work area. */
static void submenu_measure(int *width, int *height)
{
    const struct savanxp_gfx_context *main_window = desktop_main_window();
    int work_height = (int)main_window->info.height - DESKTOP_TASKBAR_HEIGHT;
    int widest = 0;
    int index;

    desktop_submenu_refresh_catalog();
    g_row_count = build_rows(g_source, g_rows, SUBMENU_MAX_ROWS);
    for (index = 0; index < g_row_count; ++index)
    {
        int text = gfx_text_width(row_label(&g_rows[index]));

        if (text > widest)
        {
            widest = text;
        }
    }
    g_width = SUBMENU_MARGIN * 2 + SUBMENU_ICON_GAP + SUBMENU_ICON_SIZE + SUBMENU_ICON_GAP + widest + SUBMENU_TEXT_PAD;
    g_width = g_width < SUBMENU_MIN_WIDTH ? SUBMENU_MIN_WIDTH : g_width;
    g_width = g_width > SUBMENU_MAX_WIDTH ? SUBMENU_MAX_WIDTH : g_width;
    g_height = SUBMENU_MARGIN * 2 + (g_row_count > 0 ? content_height() : SUBMENU_ROW_HEIGHT);
    if (work_height > 0 && g_height > work_height)
    {
        g_height = work_height;
    }
    *width = g_width;
    *height = g_height;
}

static int submenu_open(void)
{
    int strip;

    strip = overflows() ? SUBMENU_SCROLL_STRIP_HEIGHT : 0;
    g_list_y = SUBMENU_MARGIN + strip;
    g_list_height = view_height() - strip * 2;
    g_scroll = 0;
    g_hot_arrow = SUBMENU_ARROW_NONE;
    g_next_scroll_ms = 0;
    g_hot_row = -1;
    g_pressed_row = -1;
    g_last_buttons = 0;
    g_launched = 0;
    return 0;
}

static void submenu_close(void)
{
    int launched = g_launched;

    g_launched = 0;
    desktop_startmenu_submenu_closed();
    if (launched)
    {
        desktop_close_popups();
    }
}

static struct sx_rect arrow_rect(int arrow)
{
    int y = arrow == SUBMENU_ARROW_UP ? SUBMENU_MARGIN : SUBMENU_MARGIN + view_height() - SUBMENU_SCROLL_STRIP_HEIGHT;

    return sx_rect_make(SUBMENU_MARGIN, y, g_width - SUBMENU_MARGIN * 2, SUBMENU_SCROLL_STRIP_HEIGHT);
}

/* The arrow strip under the point, or SUBMENU_ARROW_NONE (also when the list
 * fits and there are no strips). */
static int arrow_hit(int x, int y)
{
    int arrow;

    if (!overflows())
    {
        return SUBMENU_ARROW_NONE;
    }
    for (arrow = SUBMENU_ARROW_UP; arrow <= SUBMENU_ARROW_DOWN; ++arrow)
    {
        if (sx_rect_contains_point(arrow_rect(arrow), x, y))
        {
            return arrow;
        }
    }
    return SUBMENU_ARROW_NONE;
}

/* The item row under the point, or -1: headers and the frame are not rows
 * that can be chosen. */
static int item_hit(int x, int y)
{
    int relative = y - g_list_y + g_scroll;
    int index;

    if (x < SUBMENU_MARGIN || x >= g_width - SUBMENU_MARGIN || y < g_list_y || y >= g_list_y + g_list_height)
    {
        return -1;
    }
    for (index = 0; index < g_row_count; ++index)
    {
        int height = row_height(&g_rows[index]);

        if (relative < height)
        {
            return g_rows[index].kind == SUBMENU_ROW_ITEM ? index : -1;
        }
        relative -= height;
    }
    return -1;
}

static void clamp_scroll(void)
{
    if (g_scroll < 0)
    {
        g_scroll = 0;
    }
    if (g_scroll > max_scroll())
    {
        g_scroll = max_scroll();
    }
}

static uint32_t submenu_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    uint32_t down = pointer->buttons & ~g_last_buttons;
    uint32_t up = g_last_buttons & ~pointer->buttons;
    int arrow = arrow_hit(pointer->x, pointer->y);
    int row;
    uint32_t actions = DESKTOP_ACTION_NONE;

    (void)gfx;
    g_last_buttons = pointer->buttons;
    /* The pointer is here: the Start menu keeps this cascade open. */
    desktop_startmenu_submenu_entered();
    if (arrow != g_hot_arrow)
    {
        /* Arriving on a strip scrolls at once; submenu_tick keeps going. */
        g_hot_arrow = arrow;
        g_next_scroll_ms = 0;
    }
    if (pointer->wheel != 0)
    {
        /* Three rows per notch, like the editor. */
        g_scroll -= pointer->wheel * 3 * SUBMENU_ROW_HEIGHT;
        clamp_scroll();
        actions |= DESKTOP_ACTION_REPAINT;
    }
    row = item_hit(pointer->x, pointer->y);
    if (row != g_hot_row)
    {
        g_hot_row = row;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        g_pressed_row = row;
    }
    else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        if (row >= 0 && row == g_pressed_row)
        {
            const struct progman_item *item = progman_group_item_at(g_rows[row].group, g_rows[row].item);

            if (item != 0 && desktop_launch(item->path, item->launch_flags) >= 0)
            {
                g_launched = 1;
                actions |= DESKTOP_ACTION_CLOSE;
            }
        }
        g_pressed_row = -1;
    }
    return actions;
}

/* Resting on an arrow strip scrolls without any pointer event arriving, so the
 * step is driven from the desktop loop. It stops at the end of the list. */
static uint32_t submenu_tick(void)
{
    unsigned long now;
    int previous = g_scroll;

    if (g_hot_arrow == SUBMENU_ARROW_NONE)
    {
        return DESKTOP_ACTION_NONE;
    }
    now = uptime_ms();
    if (now < g_next_scroll_ms)
    {
        return DESKTOP_ACTION_NONE;
    }
    g_scroll += g_hot_arrow == SUBMENU_ARROW_UP ? -SUBMENU_SCROLL_STEP : SUBMENU_SCROLL_STEP;
    clamp_scroll();
    if (g_scroll == previous)
    {
        return DESKTOP_ACTION_NONE;
    }
    g_next_scroll_ms = now + SUBMENU_SCROLL_INTERVAL_MS;
    return DESKTOP_ACTION_REPAINT;
}

/* The pointer went to the background or the taskbar: stop scrolling. */
static uint32_t submenu_pointer_left(void)
{
    g_hot_arrow = SUBMENU_ARROW_NONE;
    if (g_hot_row >= 0)
    {
        g_hot_row = -1;
        return DESKTOP_ACTION_REPAINT;
    }
    return DESKTOP_ACTION_NONE;
}

static void draw_icon(struct sx_painter *painter, const struct desktop_embedded_bitmap *icon, int x, int y)
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
    if (icon->width == SUBMENU_ICON_SIZE && icon->height == SUBMENU_ICON_SIZE)
    {
        sx_painter_blit_bitmap(painter, &bitmap, x, y);
        return;
    }
    /* Shrinking averages (bilinear), which is what
     * docs/roadmaps/done/SXGFX_ROADMAP.md asks for icons; growing a 16x16
     * drawing doubles its pixels instead of blurring it. */
    sx_painter_draw_scaled_bitmap(
        painter,
        &bitmap,
        sx_rect_make(x, y, SUBMENU_ICON_SIZE, SUBMENU_ICON_SIZE),
        sx_rect_make(0, 0, (int)icon->width, (int)icon->height),
        (icon->width > SUBMENU_ICON_SIZE || icon->height > SUBMENU_ICON_SIZE) ? SX_SCALE_BILINEAR : SX_SCALE_NEAREST);
}

static void draw_up_arrow(struct sx_painter *painter, struct sx_rect rect, uint32_t colour)
{
    sxchrome_draw_arrow(painter, rect, SXCHROME_ARROW_UP, colour);
}

static void draw_down_arrow(struct sx_painter *painter, struct sx_rect rect, uint32_t colour)
{
    sxchrome_draw_arrow(painter, rect, SXCHROME_ARROW_DOWN, colour);
}

/* An arrow that can still scroll is black; one at its end of the list is
 * etched grey, the way Windows greys a menu arrow that has nothing left. */
static void paint_arrow(struct sx_painter *painter, int arrow, int enabled)
{
    struct sx_rect rect = arrow_rect(arrow);
    sxchrome_glyph_fn draw = arrow == SUBMENU_ARROW_UP ? draw_up_arrow : draw_down_arrow;

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

static void paint_row(struct sx_painter *painter, int index, struct sx_rect rect)
{
    const struct submenu_row *row = &g_rows[index];
    int text_y = rect.y + (rect.height - gfx_text_height()) / 2;

    if (row->kind == SUBMENU_ROW_HEADER)
    {
        sx_painter_fill_rect(painter, rect, SXGUI_COLOR_FACE);
        sx_painter_draw_text(painter, rect.x + 4, text_y, row_label(row), SXGUI_COLOR_DISABLED_TEXT);
        sx_painter_hline(painter, rect.x, rect.y + rect.height - 1, rect.width, SXGUI_COLOR_SHADOW);
    }
    else
    {
        const struct progman_item *item = progman_group_item_at(row->group, row->item);
        const struct desktop_embedded_bitmap *icon;
        int selected = (index == g_hot_row);
        int text_x = rect.x + SUBMENU_ICON_GAP + SUBMENU_ICON_SIZE + SUBMENU_ICON_GAP;

        if (item == 0)
        {
            return;
        }
        sx_painter_fill_rect(painter, rect, selected ? SXGUI_COLOR_SELECT : SXGUI_COLOR_FACE);
        /* The 16x16 drawing first, the size this menu shows; the large one is
         * reduced only when the binary has no small icon. */
        icon = progman_item_icon_small(item);
        if (icon == 0)
        {
            icon = progman_item_icon(item);
        }
        if (icon == 0)
        {
            icon = desktop_icon_small((enum desktop_icon_id)item->icon_id);
        }
        draw_icon(painter, icon, rect.x + SUBMENU_ICON_GAP, rect.y + (rect.height - SUBMENU_ICON_SIZE) / 2);
        if (sx_painter_push_clip(painter, sx_rect_make(text_x, rect.y, rect.x + rect.width - 2 - text_x, rect.height)))
        {
            sx_painter_draw_text(
                painter,
                text_x,
                text_y,
                item->name,
                selected ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT);
            sx_painter_pop_clip(painter);
        }
    }
}

static void submenu_paint(struct savanxp_gfx_context *gfx)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame = sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
    struct sx_rect list;
    int index;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);
    sx_painter_fill_rect(&painter, frame, SXGUI_COLOR_FACE);
    sxchrome_draw_raised(&painter, frame);
    if (overflows())
    {
        paint_arrow(&painter, SUBMENU_ARROW_UP, g_scroll > 0);
        paint_arrow(&painter, SUBMENU_ARROW_DOWN, g_scroll < max_scroll());
    }
    list = sx_rect_make(SUBMENU_MARGIN, g_list_y, frame.width - SUBMENU_MARGIN * 2, g_list_height);
    if (!sx_painter_push_clip(&painter, list))
    {
        return;
    }
    if (g_row_count == 0)
    {
        sx_painter_draw_text(
            &painter,
            list.x + SUBMENU_ICON_GAP,
            list.y + (SUBMENU_ROW_HEIGHT - gfx_text_height()) / 2,
            "(Empty)",
            SXGUI_COLOR_DISABLED_TEXT);
    }
    for (index = 0; index < g_row_count; ++index)
    {
        int height = row_height(&g_rows[index]);
        int y = list.y + row_top(index) - g_scroll;

        /* A row entirely outside the list is not drawn at all. */
        if (y + height <= list.y || y >= list.y + list.height)
        {
            continue;
        }
        paint_row(&painter, index, sx_rect_make(list.x, y, list.width, height));
    }
    sx_painter_pop_clip(&painter);
}

const struct desktop_popup_ops desktop_submenu_ops = {
    SUBMENU_MIN_WIDTH,
    SUBMENU_ROW_HEIGHT,
    0,
    submenu_open,
    submenu_close,
    submenu_pointer,
    submenu_tick,
    submenu_paint,
    0,
    submenu_pointer_left,
    submenu_measure,
};
