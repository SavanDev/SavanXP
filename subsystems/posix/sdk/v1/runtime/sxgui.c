#include "savanxp/sxgui.h"

#include <string.h>

/* ---- low level chrome helpers ------------------------------------------- */

static void sxgui_hline(struct sx_painter *painter, int x, int y, int width, uint32_t colour)
{
    sx_painter_fill_rect(painter, sx_rect_make(x, y, width, 1), colour);
}

static void sxgui_vline(struct sx_painter *painter, int x, int y, int height, uint32_t colour)
{
    sx_painter_fill_rect(painter, sx_rect_make(x, y, 1, height), colour);
}

/* Raised 3D border (buttons, window face). */
static void sxgui_draw_raised(struct sx_painter *painter, struct sx_rect rect)
{
    int right = rect.x + rect.width - 1;
    int bottom = rect.y + rect.height - 1;

    sxgui_hline(painter, rect.x, rect.y, rect.width, SXGUI_COLOR_LIGHT);
    sxgui_vline(painter, rect.x, rect.y, rect.height, SXGUI_COLOR_LIGHT);
    sxgui_hline(painter, rect.x, bottom, rect.width, SXGUI_COLOR_DARK);
    sxgui_vline(painter, right, rect.y, rect.height, SXGUI_COLOR_DARK);
    sxgui_hline(painter, rect.x + 1, bottom - 1, rect.width - 2, SXGUI_COLOR_SHADOW);
    sxgui_vline(painter, right - 1, rect.y + 1, rect.height - 2, SXGUI_COLOR_SHADOW);
}

/* Pressed 3D border (button held down). */
static void sxgui_draw_pressed(struct sx_painter *painter, struct sx_rect rect)
{
    sxgui_hline(painter, rect.x, rect.y, rect.width, SXGUI_COLOR_DARK);
    sxgui_vline(painter, rect.x, rect.y, rect.height, SXGUI_COLOR_DARK);
    sxgui_hline(painter, rect.x + 1, rect.y + 1, rect.width - 2, SXGUI_COLOR_SHADOW);
    sxgui_vline(painter, rect.x + 1, rect.y + 1, rect.height - 2, SXGUI_COLOR_SHADOW);
}

/* Sunken 3D border (text fields, list boxes). */
static void sxgui_draw_sunken(struct sx_painter *painter, struct sx_rect rect)
{
    int right = rect.x + rect.width - 1;
    int bottom = rect.y + rect.height - 1;

    sxgui_hline(painter, rect.x, rect.y, rect.width, SXGUI_COLOR_SHADOW);
    sxgui_vline(painter, rect.x, rect.y, rect.height, SXGUI_COLOR_SHADOW);
    sxgui_hline(painter, rect.x, bottom, rect.width, SXGUI_COLOR_LIGHT);
    sxgui_vline(painter, right, rect.y, rect.height, SXGUI_COLOR_LIGHT);
}

static struct sx_rect sxgui_inset(struct sx_rect rect, int amount)
{
    return sx_rect_make(rect.x + amount, rect.y + amount, rect.width - amount * 2, rect.height - amount * 2);
}

static int sxgui_row_height(void)
{
    return gfx_text_height() + 4;
}

/* ---- scrollbar machinery (shared by the widget and the listbox) --------- */

#define SXGUI_SCROLLBAR_THICKNESS 16
#define SXGUI_SCROLLBAR_MIN_THUMB 8

enum sxgui_scroll_part {
    SXGUI_SCROLL_NONE = 0,
    SXGUI_SCROLL_LINE_BACK,
    SXGUI_SCROLL_LINE_FORWARD,
    SXGUI_SCROLL_PAGE_BACK,
    SXGUI_SCROLL_PAGE_FORWARD,
    SXGUI_SCROLL_THUMB
};

struct sxgui_scroll_metrics {
    struct sx_rect button_back;
    struct sx_rect button_forward;
    struct sx_rect track;
    struct sx_rect thumb;
    int horizontal;
    int range_min;
    int range_max;
    int page;
    int value;
};

static int sxgui_clamp_int(int value, int low, int high)
{
    if (value < low)
    {
        return low;
    }
    if (value > high)
    {
        return high;
    }
    return value;
}

static void sxgui_scroll_metrics_init(
    struct sxgui_scroll_metrics *metrics,
    struct sx_rect rect,
    int horizontal,
    int range_min,
    int range_max,
    int page,
    int value)
{
    int length = horizontal ? rect.width : rect.height;
    int button = SXGUI_SCROLLBAR_THICKNESS;
    int track_length;
    int span;
    int thumb_length;
    int offset_range;
    int thumb_offset;

    if (button * 2 > length)
    {
        button = length / 2;
    }
    if (range_max < range_min)
    {
        range_max = range_min;
    }
    if (page < 1)
    {
        page = 1;
    }
    value = sxgui_clamp_int(value, range_min, range_max);

    metrics->horizontal = horizontal;
    metrics->range_min = range_min;
    metrics->range_max = range_max;
    metrics->page = page;
    metrics->value = value;

    if (horizontal)
    {
        metrics->button_back = sx_rect_make(rect.x, rect.y, button, rect.height);
        metrics->button_forward = sx_rect_make(rect.x + rect.width - button, rect.y, button, rect.height);
        metrics->track = sx_rect_make(rect.x + button, rect.y, rect.width - button * 2, rect.height);
    }
    else
    {
        metrics->button_back = sx_rect_make(rect.x, rect.y, rect.width, button);
        metrics->button_forward = sx_rect_make(rect.x, rect.y + rect.height - button, rect.width, button);
        metrics->track = sx_rect_make(rect.x, rect.y + button, rect.width, rect.height - button * 2);
    }

    track_length = horizontal ? metrics->track.width : metrics->track.height;
    span = range_max - range_min;
    thumb_length = span > 0 ? track_length * page / (span + page) : track_length;
    thumb_length = sxgui_clamp_int(thumb_length, SXGUI_SCROLLBAR_MIN_THUMB, track_length);
    offset_range = track_length - thumb_length;
    thumb_offset = (span > 0 && offset_range > 0) ? (int)((long)(value - range_min) * offset_range / span) : 0;

    if (horizontal)
    {
        metrics->thumb = sx_rect_make(metrics->track.x + thumb_offset, metrics->track.y, thumb_length, metrics->track.height);
    }
    else
    {
        metrics->thumb = sx_rect_make(metrics->track.x, metrics->track.y + thumb_offset, metrics->track.width, thumb_length);
    }
}

/* direction: 0 = up, 1 = down, 2 = left, 3 = right */
static void sxgui_paint_arrow(struct sx_painter *painter, struct sx_rect box, int direction, uint32_t colour)
{
    int rows = 4;
    int row;

    for (row = 0; row < rows; ++row)
    {
        int width = row * 2 + 1;
        switch (direction)
        {
        case 0:
            sxgui_hline(painter, box.x + (box.width - width) / 2, box.y + (box.height - rows) / 2 + row, width, colour);
            break;
        case 1:
            sxgui_hline(painter, box.x + (box.width - width) / 2, box.y + (box.height - rows) / 2 + rows - 1 - row, width, colour);
            break;
        case 2:
            sxgui_vline(painter, box.x + (box.width - rows) / 2 + row, box.y + (box.height - width) / 2, width, colour);
            break;
        default:
            sxgui_vline(painter, box.x + (box.width - rows) / 2 + rows - 1 - row, box.y + (box.height - width) / 2, width, colour);
            break;
        }
    }
}

static void sxgui_paint_scroll_metrics(struct sx_painter *painter, const struct sxgui_scroll_metrics *metrics, int enabled)
{
    uint32_t track_colour = SXGUI_RGB(222, 222, 222);
    uint32_t arrow_colour = enabled ? SXGUI_COLOR_TEXT : SXGUI_COLOR_DISABLED_TEXT;

    sx_painter_fill_rect(painter, metrics->track, track_colour);

    sx_painter_fill_rect(painter, metrics->button_back, SXGUI_COLOR_FACE);
    sxgui_draw_raised(painter, metrics->button_back);
    sx_painter_fill_rect(painter, metrics->button_forward, SXGUI_COLOR_FACE);
    sxgui_draw_raised(painter, metrics->button_forward);
    if (metrics->horizontal)
    {
        sxgui_paint_arrow(painter, metrics->button_back, 2, arrow_colour);
        sxgui_paint_arrow(painter, metrics->button_forward, 3, arrow_colour);
    }
    else
    {
        sxgui_paint_arrow(painter, metrics->button_back, 0, arrow_colour);
        sxgui_paint_arrow(painter, metrics->button_forward, 1, arrow_colour);
    }

    if (metrics->range_max > metrics->range_min)
    {
        sx_painter_fill_rect(painter, metrics->thumb, SXGUI_COLOR_FACE);
        sxgui_draw_raised(painter, metrics->thumb);
    }
}

static int sxgui_scroll_hit_part(const struct sxgui_scroll_metrics *metrics, int x, int y, int *grab_offset)
{
    if (sx_rect_contains_point(metrics->button_back, x, y))
    {
        return SXGUI_SCROLL_LINE_BACK;
    }
    if (sx_rect_contains_point(metrics->button_forward, x, y))
    {
        return SXGUI_SCROLL_LINE_FORWARD;
    }
    if (metrics->range_max <= metrics->range_min)
    {
        return SXGUI_SCROLL_NONE;
    }
    if (sx_rect_contains_point(metrics->thumb, x, y))
    {
        if (grab_offset != 0)
        {
            *grab_offset = metrics->horizontal ? x - metrics->thumb.x : y - metrics->thumb.y;
        }
        return SXGUI_SCROLL_THUMB;
    }
    if (sx_rect_contains_point(metrics->track, x, y))
    {
        int before = metrics->horizontal ? x < metrics->thumb.x : y < metrics->thumb.y;
        return before ? SXGUI_SCROLL_PAGE_BACK : SXGUI_SCROLL_PAGE_FORWARD;
    }
    return SXGUI_SCROLL_NONE;
}

static int sxgui_scroll_step_value(const struct sxgui_scroll_metrics *metrics, int part)
{
    int value = metrics->value;

    switch (part)
    {
    case SXGUI_SCROLL_LINE_BACK:
        value -= 1;
        break;
    case SXGUI_SCROLL_LINE_FORWARD:
        value += 1;
        break;
    case SXGUI_SCROLL_PAGE_BACK:
        value -= metrics->page;
        break;
    case SXGUI_SCROLL_PAGE_FORWARD:
        value += metrics->page;
        break;
    default:
        break;
    }
    return sxgui_clamp_int(value, metrics->range_min, metrics->range_max);
}

static int sxgui_scroll_value_from_drag(const struct sxgui_scroll_metrics *metrics, int x, int y, int grab_offset)
{
    int track_length = metrics->horizontal ? metrics->track.width : metrics->track.height;
    int thumb_length = metrics->horizontal ? metrics->thumb.width : metrics->thumb.height;
    int offset_range = track_length - thumb_length;
    int span = metrics->range_max - metrics->range_min;
    int pointer = metrics->horizontal ? x - metrics->track.x : y - metrics->track.y;
    int offset = pointer - grab_offset;
    long scaled;

    if (offset_range <= 0 || span <= 0)
    {
        return metrics->value;
    }
    offset = sxgui_clamp_int(offset, 0, offset_range);
    scaled = ((long)offset * span + offset_range / 2) / offset_range;
    return sxgui_clamp_int(metrics->range_min + (int)scaled, metrics->range_min, metrics->range_max);
}

static int sxgui_widget_enabled(const struct sxgui_widget *widget)
{
    return (widget->flags & SXGUI_FLAG_DISABLED) == 0;
}

static int sxgui_widget_visible(const struct sxgui_widget *widget)
{
    return (widget->flags & SXGUI_FLAG_VISIBLE) != 0;
}

static int sxgui_focusable(const struct sxgui_widget *widget)
{
    if (!sxgui_widget_visible(widget) || !sxgui_widget_enabled(widget))
    {
        return 0;
    }
    return widget->kind == SXGUI_BUTTON || widget->kind == SXGUI_CHECKBOX ||
           widget->kind == SXGUI_LISTBOX || widget->kind == SXGUI_TEXTFIELD ||
           widget->kind == SXGUI_SCROLLBAR || widget->kind == SXGUI_RADIO ||
           widget->kind == SXGUI_COMBOBOX || widget->kind == SXGUI_TEXTVIEW ||
           widget->kind == SXGUI_TEXTEDIT;
}

/* ---- painting ----------------------------------------------------------- */

static void sxgui_paint_label(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    uint32_t colour = sxgui_widget_enabled(widget) ? SXGUI_COLOR_TEXT : SXGUI_COLOR_DISABLED_TEXT;
    int text_y = widget->rect.y + (widget->rect.height - gfx_text_height()) / 2;
    int text_x = widget->rect.x;

    if ((widget->flags & SXGUI_FLAG_SUNKEN) != 0)
    {
        sx_painter_fill_rect(painter, widget->rect, SXGUI_COLOR_FACE);
        sxgui_draw_sunken(painter, widget->rect);
        text_x += 5;
    }
    if (widget->text != 0)
    {
        sx_painter_draw_text(painter, text_x, text_y, widget->text, colour);
    }
}

/* Etched frame with the caption punched into the top edge. */
static void sxgui_paint_groupbox(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    uint32_t colour = sxgui_widget_enabled(widget) ? SXGUI_COLOR_TEXT : SXGUI_COLOR_DISABLED_TEXT;
    struct sx_rect frame = widget->rect;

    frame.y += gfx_text_height() / 2;
    frame.height -= gfx_text_height() / 2;
    sx_painter_draw_frame(painter, sx_rect_make(frame.x + 1, frame.y + 1, frame.width - 1, frame.height - 1), SXGUI_COLOR_LIGHT);
    sx_painter_draw_frame(painter, sx_rect_make(frame.x, frame.y, frame.width - 1, frame.height - 1), SXGUI_COLOR_SHADOW);

    if (widget->text != 0)
    {
        int text_x = widget->rect.x + 8;
        struct sx_rect caption = sx_rect_make(text_x - 3, widget->rect.y, gfx_text_width(widget->text) + 6, gfx_text_height());
        sx_painter_fill_rect(painter, caption, SXGUI_COLOR_WINDOW);
        sx_painter_draw_text(painter, text_x, widget->rect.y, widget->text, colour);
    }
}

static void sxgui_paint_progress(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    struct sx_rect inner = sxgui_inset(widget->rect, 1);
    int span = widget->range_max - widget->range_min;
    int filled = 0;

    sx_painter_fill_rect(painter, widget->rect, SXGUI_COLOR_FIELD);
    sxgui_draw_sunken(painter, widget->rect);
    if (span > 0)
    {
        filled = (int)((long)(widget->value - widget->range_min) * inner.width / span);
    }
    else if (widget->value >= widget->range_max)
    {
        filled = inner.width;
    }
    filled = sxgui_clamp_int(filled, 0, inner.width);
    if (filled > 0)
    {
        sx_painter_fill_rect(painter, sx_rect_make(inner.x, inner.y, filled, inner.height), SXGUI_COLOR_SELECT);
    }
}

static void sxgui_paint_button(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    struct sx_rect rect = widget->rect;
    int pressed = widget->pressed && widget->hover;
    int text_x;
    int text_y;
    int offset = pressed ? 1 : 0;
    uint32_t colour = sxgui_widget_enabled(widget) ? SXGUI_COLOR_TEXT : SXGUI_COLOR_DISABLED_TEXT;

    sx_painter_fill_rect(painter, rect, SXGUI_COLOR_FACE);
    if (pressed)
    {
        sxgui_draw_pressed(painter, rect);
    }
    else
    {
        sxgui_draw_raised(painter, rect);
    }

    if (widget->text != 0)
    {
        text_x = rect.x + (rect.width - gfx_text_width(widget->text)) / 2 + offset;
        text_y = rect.y + (rect.height - gfx_text_height()) / 2 + offset;
        sx_painter_draw_text(painter, text_x, text_y, widget->text, colour);
    }
    if (widget->focused)
    {
        sx_painter_draw_frame(painter, sxgui_inset(rect, 3), SXGUI_COLOR_SHADOW);
    }
}

static void sxgui_paint_check_glyph(struct sx_painter *painter, struct sx_rect box)
{
    /* a small tick stamped from 2x2 squares */
    static const int points[][2] = {
        {2, 5}, {3, 6}, {4, 7}, {5, 6}, {6, 4}, {7, 2}, {8, 1}
    };
    int index;
    for (index = 0; index < (int)(sizeof(points) / sizeof(points[0])); ++index)
    {
        sx_painter_fill_rect(
            painter,
            sx_rect_make(box.x + points[index][0], box.y + points[index][1], 2, 2),
            SXGUI_COLOR_DARK);
    }
}

static void sxgui_paint_checkbox(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    int box_size = 13;
    struct sx_rect box = sx_rect_make(
        widget->rect.x,
        widget->rect.y + (widget->rect.height - box_size) / 2,
        box_size,
        box_size);
    uint32_t colour = sxgui_widget_enabled(widget) ? SXGUI_COLOR_TEXT : SXGUI_COLOR_DISABLED_TEXT;
    int text_y = widget->rect.y + (widget->rect.height - gfx_text_height()) / 2;

    sx_painter_fill_rect(painter, box, SXGUI_COLOR_FIELD);
    sxgui_draw_sunken(painter, box);
    if (widget->value)
    {
        sxgui_paint_check_glyph(painter, box);
    }
    if (widget->text != 0)
    {
        sx_painter_draw_text(painter, box.x + box_size + 6, text_y, widget->text, colour);
    }
    if (widget->focused && widget->text != 0)
    {
        struct sx_rect focus = sx_rect_make(
            box.x + box_size + 4,
            text_y - 1,
            gfx_text_width(widget->text) + 4,
            gfx_text_height() + 2);
        sx_painter_draw_frame(painter, focus, SXGUI_COLOR_SHADOW);
    }
}

/* ---- combobox popup helpers ---------------------------------------------- */

#define SXGUI_COMBOBOX_MAX_ROWS 8

static int sxgui_popup_visible_rows(const struct sxgui_widget *widget)
{
    int rows = widget->item_count < SXGUI_COMBOBOX_MAX_ROWS ? widget->item_count : SXGUI_COMBOBOX_MAX_ROWS;
    return rows > 0 ? rows : 1;
}

static void sxgui_popup_close(struct sxgui_context *ctx)
{
    ctx->popup_owner = -1;
}

static void sxgui_popup_ensure_hot_visible(struct sxgui_context *ctx, const struct sxgui_widget *widget)
{
    int rows = sxgui_popup_visible_rows(widget);
    int max_scroll = widget->item_count - rows;

    if (ctx->popup_hot < ctx->popup_scroll)
    {
        ctx->popup_scroll = ctx->popup_hot;
    }
    if (ctx->popup_hot >= ctx->popup_scroll + rows)
    {
        ctx->popup_scroll = ctx->popup_hot - rows + 1;
    }
    ctx->popup_scroll = sxgui_clamp_int(ctx->popup_scroll, 0, max_scroll > 0 ? max_scroll : 0);
}

/* Place the dropdown under the widget, clamped to the surface; open upwards
 * when it does not fit below. */
static void sxgui_popup_open(struct sxgui_context *ctx, int index)
{
    struct sxgui_widget *widget = &ctx->widgets[index];
    int rows = sxgui_popup_visible_rows(widget);
    int height = rows * sxgui_row_height() + 2;
    int width = widget->rect.width;
    int x = widget->rect.x;
    int y = widget->rect.y + widget->rect.height;
    int surface_width = (int)ctx->target.info.width;
    int surface_height = (int)ctx->target.info.height;

    if (y + height > surface_height)
    {
        y = widget->rect.y - height;
        if (y < 0)
        {
            y = 0;
        }
    }
    if (x + width > surface_width)
    {
        x = surface_width - width;
    }
    if (x < 0)
    {
        x = 0;
    }

    ctx->popup_owner = index;
    ctx->popup_rect = sx_rect_make(x, y, width, height);
    ctx->popup_hot = sxgui_clamp_int(widget->value, 0, widget->item_count > 0 ? widget->item_count - 1 : 0);
    ctx->popup_scroll = 0;
    sxgui_popup_ensure_hot_visible(ctx, widget);
}

/* ---- menu bar helpers ----------------------------------------------------- */

#define SXGUI_MENU_TITLE_PAD 8
#define SXGUI_MENU_SEPARATOR_HEIGHT 6
#define SXGUI_MENU_GUTTER 20

int sxgui_menubar_height(void)
{
    return gfx_text_height() + 8;
}

void sxgui_set_menubar(struct sxgui_context *ctx, struct sxgui_menubar *bar)
{
    if (ctx == 0)
    {
        return;
    }
    ctx->menubar = bar;
    if (bar != 0)
    {
        bar->open_menu = -1;
        bar->hot_item = -1;
    }
}

static struct sx_rect sxgui_menubar_title_rect(const struct sxgui_menubar *bar, int index)
{
    int x = 2;
    int i;

    for (i = 0; i < index; ++i)
    {
        x += gfx_text_width(bar->menus[i].title) + SXGUI_MENU_TITLE_PAD * 2;
    }
    return sx_rect_make(
        x,
        0,
        gfx_text_width(bar->menus[index].title) + SXGUI_MENU_TITLE_PAD * 2,
        sxgui_menubar_height());
}

static int sxgui_menubar_title_at(const struct sxgui_menubar *bar, int x, int y)
{
    int index;

    for (index = 0; index < bar->menu_count; ++index)
    {
        if (sx_rect_contains_point(sxgui_menubar_title_rect(bar, index), x, y))
        {
            return index;
        }
    }
    return -1;
}

static int sxgui_menu_item_height(const struct sxgui_menu_item *item)
{
    return item->text != 0 ? sxgui_row_height() : SXGUI_MENU_SEPARATOR_HEIGHT;
}

static struct sx_rect sxgui_menu_popup_rect(const struct sxgui_context *ctx, const struct sxgui_menubar *bar, int menu_index)
{
    const struct sxgui_menu *menu = &bar->menus[menu_index];
    struct sx_rect title = sxgui_menubar_title_rect(bar, menu_index);
    int surface_width = (int)ctx->target.info.width;
    int width = 0;
    int height = 2;
    int x = title.x;
    int index;

    for (index = 0; index < menu->item_count; ++index)
    {
        if (menu->items[index].text != 0)
        {
            int text_width = gfx_text_width(menu->items[index].text);
            if (text_width > width)
            {
                width = text_width;
            }
        }
        height += sxgui_menu_item_height(&menu->items[index]);
    }
    width += SXGUI_MENU_GUTTER + 12;
    if (x + width > surface_width)
    {
        x = surface_width - width;
    }
    if (x < 0)
    {
        x = 0;
    }
    return sx_rect_make(x, sxgui_menubar_height(), width, height);
}

/* Row index under y, or -1 over separators and outside the popup. */
static int sxgui_menu_item_at(struct sx_rect popup, const struct sxgui_menu *menu, int y)
{
    int row_y = popup.y + 1;
    int index;

    for (index = 0; index < menu->item_count; ++index)
    {
        int height = sxgui_menu_item_height(&menu->items[index]);
        if (y >= row_y && y < row_y + height)
        {
            return menu->items[index].text != 0 ? index : -1;
        }
        row_y += height;
    }
    return -1;
}

static int sxgui_menu_item_selectable(const struct sxgui_menu *menu, int index)
{
    return index >= 0 && index < menu->item_count &&
           menu->items[index].text != 0 &&
           (menu->items[index].flags & SXGUI_MENU_DISABLED) == 0;
}

/* Move hot_item up/down skipping separators, wrapping around. */
static int sxgui_menu_step_hot(const struct sxgui_menu *menu, int hot, int direction)
{
    int step;

    if (menu->item_count <= 0)
    {
        return -1;
    }
    if (hot < 0)
    {
        hot = direction > 0 ? -1 : 0;
    }
    for (step = 1; step <= menu->item_count; ++step)
    {
        int index = hot + direction * step;
        while (index < 0)
        {
            index += menu->item_count;
        }
        index %= menu->item_count;
        if (menu->items[index].text != 0)
        {
            return index;
        }
    }
    return -1;
}

static void sxgui_menu_fire(struct sxgui_menubar *bar, const struct sxgui_menu *menu, int index)
{
    bar->open_menu = -1;
    bar->hot_item = -1;
    if (bar->on_command != 0)
    {
        bar->on_command(menu->items[index].id, bar->user);
    }
}

/* ---- listbox scrolling helpers ------------------------------------------ */

/* Alto de la cabecera de columnas; 0 cuando el listbox no tiene columnas, que
 * es lo que hace que todo lo de abajo siga valiendo para una lista simple. */
static int sxgui_listbox_header_height(const struct sxgui_widget *widget)
{
    if (widget->kind != SXGUI_LISTBOX || widget->columns == 0 || widget->column_count <= 0)
    {
        return 0;
    }
    return gfx_text_height() + 5;
}

static int sxgui_listbox_visible_rows(const struct sxgui_widget *widget)
{
    int usable = widget->rect.height - 4 - sxgui_listbox_header_height(widget);
    int rows = usable / sxgui_row_height();
    return rows > 0 ? rows : 1;
}

static int sxgui_listbox_has_scrollbar(const struct sxgui_widget *widget)
{
    return widget->item_count > sxgui_listbox_visible_rows(widget);
}

static int sxgui_listbox_max_scroll(const struct sxgui_widget *widget)
{
    int max_scroll = widget->item_count - sxgui_listbox_visible_rows(widget);
    return max_scroll > 0 ? max_scroll : 0;
}

/* text area inside the sunken border, minus the embedded scrollbar column y la
 * cabecera de columnas. Como el pintado y el hit-test de filas parten los dos
 * de aca, correr el origen alcanza para que la cabecera no se solape. */
static struct sx_rect sxgui_listbox_inner(const struct sxgui_widget *widget)
{
    struct sx_rect inner = sxgui_inset(widget->rect, 2);
    int header = sxgui_listbox_header_height(widget);
    if (sxgui_listbox_has_scrollbar(widget))
    {
        inner.width -= SXGUI_SCROLLBAR_THICKNESS;
    }
    inner.y += header;
    inner.height -= header;
    if (inner.height < 0)
    {
        inner.height = 0;
    }
    return inner;
}

/* La cabecera ocupa el ancho util menos la columna del scrollbar, que corre de
 * punta a punta al costado -- igual que en la lista de detalles clasica. */
static struct sx_rect sxgui_listbox_header_rect(const struct sxgui_widget *widget)
{
    struct sx_rect inner = sxgui_inset(widget->rect, 2);
    if (sxgui_listbox_has_scrollbar(widget))
    {
        inner.width -= SXGUI_SCROLLBAR_THICKNESS;
    }
    inner.height = sxgui_listbox_header_height(widget);
    return inner;
}

/* Copia la celda `index` de un item separado por SXGUI_COLUMN_SEPARATOR.
 * Devuelve siempre buffer (cadena vacia si esa columna no existe en el item),
 * asi el llamador no necesita distinguir el caso. */
static const char *sxgui_column_cell(const char *text, int index, char *buffer, int capacity)
{
    int cell = 0;
    int length = 0;

    buffer[0] = 0;
    if (text == 0 || capacity <= 0)
    {
        return buffer;
    }
    while (*text != 0 && cell < index)
    {
        if (*text == SXGUI_COLUMN_SEPARATOR)
        {
            cell += 1;
        }
        text += 1;
    }
    if (cell != index)
    {
        return buffer;
    }
    while (*text != 0 && *text != SXGUI_COLUMN_SEPARATOR && length < capacity - 1)
    {
        buffer[length] = *text;
        length += 1;
        text += 1;
    }
    buffer[length] = 0;
    return buffer;
}

/* Una fila partida en celdas, cada una recortada a su columna para que un
 * nombre largo no se derrame sobre la de al lado. */
static void sxgui_paint_columns_row(
    struct sx_painter *painter,
    const struct sxgui_widget *widget,
    struct sx_rect inner,
    int row_y,
    int row_height,
    const char *label,
    uint32_t text_colour)
{
    int column;
    int cell_x = inner.x;

    for (column = 0; column < widget->column_count; ++column)
    {
        const struct sxgui_column *spec = &widget->columns[column];
        char cell[128];
        struct sx_rect cell_rect = sx_rect_make(cell_x, row_y, spec->width, row_height);
        int text_x;

        if (cell_x >= inner.x + inner.width)
        {
            break;
        }
        (void)sxgui_column_cell(label, column, cell, (int)sizeof(cell));
        if (cell[0] != 0 && sx_painter_push_clip(painter, cell_rect))
        {
            if ((spec->flags & SXGUI_COLUMN_RIGHT) != 0)
            {
                text_x = cell_x + spec->width - gfx_text_width(cell) - 6;
            }
            else
            {
                text_x = cell_x + 3;
            }
            sx_painter_draw_text(painter, text_x, row_y + 2, cell, text_colour);
            sx_painter_pop_clip(painter);
        }
        cell_x += spec->width;
    }
}

static void sxgui_paint_listbox_header(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    struct sx_rect header = sxgui_listbox_header_rect(widget);
    int column;
    int cell_x = header.x;

    if (header.height <= 0 || !sx_painter_push_clip(painter, header))
    {
        return;
    }
    sx_painter_fill_rect(painter, header, SXGUI_COLOR_FACE);
    for (column = 0; column < widget->column_count; ++column)
    {
        const struct sxgui_column *spec = &widget->columns[column];
        struct sx_rect cell = sx_rect_make(cell_x, header.y, spec->width, header.height);

        sxgui_draw_raised(painter, cell);
        if (spec->title != 0 && sx_painter_push_clip(painter, cell))
        {
            sx_painter_draw_text(painter, cell.x + 3, cell.y + 2, spec->title, SXGUI_COLOR_TEXT);
            sx_painter_pop_clip(painter);
        }
        cell_x += spec->width;
    }
    /* Relleno hasta el borde: sin esto lo que sobra a la derecha de la ultima
     * columna queda del color del campo y se lee como una cabecera rota. */
    if (cell_x < header.x + header.width)
    {
        struct sx_rect rest = sx_rect_make(cell_x, header.y, header.x + header.width - cell_x, header.height);
        sxgui_draw_raised(painter, rest);
    }
    sx_painter_pop_clip(painter);
}

static struct sx_rect sxgui_listbox_scrollbar_rect(const struct sxgui_widget *widget)
{
    struct sx_rect inner = sxgui_inset(widget->rect, 2);
    return sx_rect_make(
        inner.x + inner.width - SXGUI_SCROLLBAR_THICKNESS,
        inner.y,
        SXGUI_SCROLLBAR_THICKNESS,
        inner.height);
}

static void sxgui_listbox_metrics(const struct sxgui_widget *widget, struct sxgui_scroll_metrics *metrics)
{
    sxgui_scroll_metrics_init(
        metrics,
        sxgui_listbox_scrollbar_rect(widget),
        0,
        0,
        sxgui_listbox_max_scroll(widget),
        sxgui_listbox_visible_rows(widget),
        widget->scroll);
}

static void sxgui_listbox_clamp_scroll(struct sxgui_widget *widget)
{
    widget->scroll = sxgui_clamp_int(widget->scroll, 0, sxgui_listbox_max_scroll(widget));
}

static void sxgui_listbox_ensure_visible(struct sxgui_widget *widget)
{
    int visible_rows = sxgui_listbox_visible_rows(widget);

    if (widget->value < widget->scroll)
    {
        widget->scroll = widget->value;
    }
    if (widget->value >= widget->scroll + visible_rows)
    {
        widget->scroll = widget->value - visible_rows + 1;
    }
    sxgui_listbox_clamp_scroll(widget);
}

/* Shared by listbox and textview: handle a press on the embedded scrollbar
 * column. Returns non-zero when the press landed on the scrollbar. */
static int sxgui_listbox_scrollbar_press(
    struct sxgui_context *ctx,
    int index,
    struct sxgui_widget *widget,
    const struct savanxp_gui_pointer_event *event,
    int *changed)
{
    struct sxgui_scroll_metrics metrics;
    int grab_offset = 0;
    int part;

    if (!sxgui_listbox_has_scrollbar(widget) ||
        !sx_rect_contains_point(sxgui_listbox_scrollbar_rect(widget), event->x, event->y))
    {
        return 0;
    }
    sxgui_listbox_metrics(widget, &metrics);
    part = sxgui_scroll_hit_part(&metrics, event->x, event->y, &grab_offset);
    if (part == SXGUI_SCROLL_THUMB)
    {
        ctx->capture_index = index;
        ctx->capture_part = part;
        ctx->capture_offset = grab_offset;
    }
    else if (part != SXGUI_SCROLL_NONE)
    {
        widget->scroll = sxgui_scroll_step_value(&metrics, part);
        *changed = 1;
    }
    return 1;
}

/* 12x12 circle stamped from per-row half widths; shadow on the upper arc,
 * light on the lower arc. */
static void sxgui_paint_radio(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    static const int half[12] = {2, 4, 5, 5, 6, 6, 6, 6, 5, 5, 4, 2};
    int box_size = 12;
    struct sx_rect box = sx_rect_make(
        widget->rect.x,
        widget->rect.y + (widget->rect.height - box_size) / 2,
        box_size,
        box_size);
    uint32_t colour = sxgui_widget_enabled(widget) ? SXGUI_COLOR_TEXT : SXGUI_COLOR_DISABLED_TEXT;
    int text_y = widget->rect.y + (widget->rect.height - gfx_text_height()) / 2;
    int row;

    for (row = 0; row < box_size; ++row)
    {
        int row_half = half[row];
        int x0 = box.x + box_size / 2 - row_half;
        int width = row_half * 2;
        uint32_t edge = row < box_size / 2 ? SXGUI_COLOR_SHADOW : SXGUI_COLOR_LIGHT;

        sx_painter_fill_rect(painter, sx_rect_make(x0, box.y + row, width, 1), SXGUI_COLOR_FIELD);
        if (row == 0 || row == box_size - 1)
        {
            sx_painter_fill_rect(painter, sx_rect_make(x0, box.y + row, width, 1), edge);
        }
        else
        {
            sx_painter_fill_rect(painter, sx_rect_make(x0, box.y + row, 1, 1), edge);
            sx_painter_fill_rect(painter, sx_rect_make(x0 + width - 1, box.y + row, 1, 1), edge);
        }
    }
    if (widget->value)
    {
        sx_painter_fill_rect(painter, sx_rect_make(box.x + 4, box.y + 4, 4, 4), SXGUI_COLOR_DARK);
    }
    if (widget->text != 0)
    {
        sx_painter_draw_text(painter, box.x + box_size + 6, text_y, widget->text, colour);
    }
    if (widget->focused && widget->text != 0)
    {
        struct sx_rect focus = sx_rect_make(
            box.x + box_size + 4,
            text_y - 1,
            gfx_text_width(widget->text) + 4,
            gfx_text_height() + 2);
        sx_painter_draw_frame(painter, focus, SXGUI_COLOR_SHADOW);
    }
}

static void sxgui_paint_listbox(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    struct sx_rect inner = sxgui_listbox_inner(widget);
    int row_height = sxgui_row_height();
    int index;

    sx_painter_fill_rect(painter, widget->rect, SXGUI_COLOR_FIELD);
    sxgui_draw_sunken(painter, widget->rect);

    if (!sx_painter_push_clip(painter, inner))
    {
        return;
    }
    for (index = widget->scroll; index < widget->item_count; ++index)
    {
        int row_y = inner.y + (index - widget->scroll) * row_height;
        const char *label = widget->items != 0 ? widget->items[index] : 0;
        uint32_t text_colour = SXGUI_COLOR_TEXT;

        if (row_y >= inner.y + inner.height)
        {
            break;
        }
        if (index == widget->value)
        {
            sx_painter_fill_rect(painter, sx_rect_make(inner.x, row_y, inner.width, row_height), SXGUI_COLOR_SELECT);
            text_colour = SXGUI_COLOR_SELECT_TEXT;
        }
        if (label != 0)
        {
            if (sxgui_listbox_header_height(widget) > 0)
            {
                sxgui_paint_columns_row(painter, widget, inner, row_y, row_height, label, text_colour);
            }
            else
            {
                sx_painter_draw_text(painter, inner.x + 3, row_y + 2, label, text_colour);
            }
        }
    }
    sx_painter_pop_clip(painter);

    sxgui_paint_listbox_header(painter, widget);

    if (sxgui_listbox_has_scrollbar(widget))
    {
        struct sxgui_scroll_metrics metrics;
        sxgui_listbox_metrics(widget, &metrics);
        sxgui_paint_scroll_metrics(painter, &metrics, sxgui_widget_enabled(widget));
    }
}

static void sxgui_paint_textview(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    struct sx_rect inner = sxgui_listbox_inner(widget);
    int row_height = sxgui_row_height();
    int index;

    sx_painter_fill_rect(painter, widget->rect, SXGUI_COLOR_FIELD);
    sxgui_draw_sunken(painter, widget->rect);

    if (!sx_painter_push_clip(painter, inner))
    {
        return;
    }
    for (index = widget->scroll; index < widget->item_count; ++index)
    {
        int row_y = inner.y + (index - widget->scroll) * row_height;
        const char *line = widget->items != 0 ? widget->items[index] : 0;

        if (row_y >= inner.y + inner.height)
        {
            break;
        }
        if (line != 0)
        {
            sx_painter_draw_text(painter, inner.x + 3, row_y + 2, line, SXGUI_COLOR_TEXT);
        }
    }
    sx_painter_pop_clip(painter);

    if (sxgui_listbox_has_scrollbar(widget))
    {
        struct sxgui_scroll_metrics metrics;
        sxgui_listbox_metrics(widget, &metrics);
        sxgui_paint_scroll_metrics(painter, &metrics, sxgui_widget_enabled(widget));
    }
}

static void sxgui_paint_combobox(struct sx_painter *painter, const struct sxgui_widget *widget, int open)
{
    struct sx_rect inner = sxgui_inset(widget->rect, 2);
    struct sx_rect button = sx_rect_make(
        widget->rect.x + widget->rect.width - 2 - SXGUI_SCROLLBAR_THICKNESS,
        inner.y,
        SXGUI_SCROLLBAR_THICKNESS,
        inner.height);
    struct sx_rect text_area = sx_rect_make(inner.x, inner.y, inner.width - button.width, inner.height);
    int text_y = inner.y + (inner.height - gfx_text_height()) / 2;
    uint32_t colour = sxgui_widget_enabled(widget) ? SXGUI_COLOR_TEXT : SXGUI_COLOR_DISABLED_TEXT;
    const char *label = 0;

    sx_painter_fill_rect(painter, widget->rect, SXGUI_COLOR_FIELD);
    sxgui_draw_sunken(painter, widget->rect);

    if (widget->items != 0 && widget->value >= 0 && widget->value < widget->item_count)
    {
        label = widget->items[widget->value];
    }
    if (label != 0 && sx_painter_push_clip(painter, text_area))
    {
        sx_painter_draw_text(painter, text_area.x + 3, text_y, label, colour);
        sx_painter_pop_clip(painter);
    }
    if (widget->focused)
    {
        sx_painter_draw_frame(painter, sxgui_inset(text_area, 1), SXGUI_COLOR_SHADOW);
    }

    sx_painter_fill_rect(painter, button, SXGUI_COLOR_FACE);
    if (open)
    {
        sxgui_draw_pressed(painter, button);
    }
    else
    {
        sxgui_draw_raised(painter, button);
    }
    sxgui_paint_arrow(painter, button, 1, colour);
}

/* Dropdown overlay; painted after every widget so it stays on top. */
static void sxgui_paint_combobox_popup(struct sxgui_context *ctx)
{
    struct sxgui_widget *widget = &ctx->widgets[ctx->popup_owner];
    struct sx_painter *painter = &ctx->painter;
    struct sx_rect inner = sxgui_inset(ctx->popup_rect, 1);
    int row_height = sxgui_row_height();
    int rows = sxgui_popup_visible_rows(widget);
    int index;

    sx_painter_fill_rect(painter, ctx->popup_rect, SXGUI_COLOR_FIELD);
    sx_painter_draw_frame(painter, ctx->popup_rect, SXGUI_COLOR_DARK);

    if (!sx_painter_push_clip(painter, inner))
    {
        return;
    }
    for (index = ctx->popup_scroll;
         index < widget->item_count && index < ctx->popup_scroll + rows;
         ++index)
    {
        int row_y = inner.y + (index - ctx->popup_scroll) * row_height;
        const char *label = widget->items != 0 ? widget->items[index] : 0;
        uint32_t text_colour = SXGUI_COLOR_TEXT;

        if (index == ctx->popup_hot)
        {
            sx_painter_fill_rect(painter, sx_rect_make(inner.x, row_y, inner.width, row_height), SXGUI_COLOR_SELECT);
            text_colour = SXGUI_COLOR_SELECT_TEXT;
        }
        if (label != 0)
        {
            sx_painter_draw_text(painter, inner.x + 3, row_y + 2, label, text_colour);
        }
    }
    sx_painter_pop_clip(painter);
}

static void sxgui_paint_menubar(struct sxgui_context *ctx)
{
    struct sxgui_menubar *bar = ctx->menubar;
    struct sx_painter *painter = &ctx->painter;
    int height = sxgui_menubar_height();
    int width = (int)ctx->target.info.width;
    int text_y = (height - gfx_text_height()) / 2;
    int index;

    sx_painter_fill_rect(painter, sx_rect_make(0, 0, width, height), SXGUI_COLOR_FACE);
    sxgui_hline(painter, 0, height - 1, width, SXGUI_COLOR_SHADOW);

    for (index = 0; index < bar->menu_count; ++index)
    {
        struct sx_rect title = sxgui_menubar_title_rect(bar, index);
        uint32_t colour = SXGUI_COLOR_TEXT;

        if (index == bar->open_menu)
        {
            sx_painter_fill_rect(painter, title, SXGUI_COLOR_SELECT);
            colour = SXGUI_COLOR_SELECT_TEXT;
        }
        sx_painter_draw_text(painter, title.x + SXGUI_MENU_TITLE_PAD, text_y, bar->menus[index].title, colour);
    }
}

static void sxgui_paint_menu_popup(struct sxgui_context *ctx)
{
    struct sxgui_menubar *bar = ctx->menubar;
    const struct sxgui_menu *menu = &bar->menus[bar->open_menu];
    struct sx_painter *painter = &ctx->painter;
    struct sx_rect popup = sxgui_menu_popup_rect(ctx, bar, bar->open_menu);
    int row_y = popup.y + 1;
    int index;

    sx_painter_fill_rect(painter, popup, SXGUI_COLOR_FACE);
    sxgui_draw_raised(painter, popup);

    for (index = 0; index < menu->item_count; ++index)
    {
        const struct sxgui_menu_item *item = &menu->items[index];
        int height = sxgui_menu_item_height(item);

        if (item->text == 0)
        {
            int line_y = row_y + height / 2 - 1;
            sxgui_hline(painter, popup.x + 2, line_y, popup.width - 4, SXGUI_COLOR_SHADOW);
            sxgui_hline(painter, popup.x + 2, line_y + 1, popup.width - 4, SXGUI_COLOR_LIGHT);
        }
        else
        {
            int enabled = (item->flags & SXGUI_MENU_DISABLED) == 0;
            uint32_t colour = enabled ? SXGUI_COLOR_TEXT : SXGUI_COLOR_DISABLED_TEXT;

            if (index == bar->hot_item && enabled)
            {
                sx_painter_fill_rect(painter, sx_rect_make(popup.x + 1, row_y, popup.width - 2, height), SXGUI_COLOR_SELECT);
                colour = SXGUI_COLOR_SELECT_TEXT;
            }
            if ((item->flags & SXGUI_MENU_CHECKED) != 0)
            {
                sxgui_paint_check_glyph(painter, sx_rect_make(popup.x + 3, row_y + (height - 13) / 2, 13, 13));
            }
            sx_painter_draw_text(painter, popup.x + SXGUI_MENU_GUTTER, row_y + 2, item->text, colour);
        }
        row_y += height;
    }
}

static void sxgui_paint_scrollbar(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    struct sxgui_scroll_metrics metrics;

    sxgui_scroll_metrics_init(
        &metrics,
        widget->rect,
        (widget->flags & SXGUI_FLAG_HSCROLL) != 0,
        widget->range_min,
        widget->range_max,
        widget->page,
        widget->value);
    sxgui_paint_scroll_metrics(painter, &metrics, sxgui_widget_enabled(widget));
    if (widget->focused)
    {
        sx_painter_draw_frame(painter, widget->rect, SXGUI_COLOR_SHADOW);
    }
}

/* Pixel width of the first `caret` characters. The buffer is caller-owned and
 * mutable, so a temporary NUL keeps the measurement allocation-free. */
static int sxgui_text_prefix_width(char *buffer, int caret)
{
    char saved;
    int width;

    if (buffer == 0 || caret <= 0)
    {
        return 0;
    }
    saved = buffer[caret];
    buffer[caret] = '\0';
    width = gfx_text_width(buffer);
    buffer[caret] = saved;
    return width;
}

static int sxgui_textfield_inner_width(const struct sxgui_widget *widget)
{
    return widget->rect.width - 2 * 2 - 3 * 2;
}

static void sxgui_textfield_clamp_caret(struct sxgui_widget *widget)
{
    int length = widget->edit_buffer != 0 ? (int)strlen(widget->edit_buffer) : 0;
    if (widget->caret < 0)
    {
        widget->caret = 0;
    }
    if (widget->caret > length)
    {
        widget->caret = length;
    }
}

static void sxgui_textfield_scroll_to_caret(struct sxgui_widget *widget)
{
    int inner_width = sxgui_textfield_inner_width(widget);
    int caret_x = sxgui_text_prefix_width(widget->edit_buffer, widget->caret);

    if (inner_width < 1)
    {
        inner_width = 1;
    }
    if (caret_x - widget->scroll > inner_width - 1)
    {
        widget->scroll = caret_x - (inner_width - 1);
    }
    if (caret_x - widget->scroll < 0)
    {
        widget->scroll = caret_x;
    }
    if (widget->scroll < 0)
    {
        widget->scroll = 0;
    }
}

/* ---- seleccion de texto (textfield y textedit) --------------------------------------
 *
 * Los comparten los dos widgets de texto: trabajan sobre edit_buffer, caret y
 * sel_anchor, y no saben de que widget son.
 *
 * El rango vive en dos enteros: sel_anchor (donde empezo) y caret (donde esta
 * ahora). Sin un buffer aparte, asi que no hay nada que mantener sincronizado
 * con las ediciones -- lo unico que hay que hacer al editar es soltar el ancla.
 */

static int sxgui_sel_active(const struct sxgui_widget *widget)
{
    return widget->sel_anchor >= 0 && widget->sel_anchor != widget->caret;
}

static int sxgui_sel_start(const struct sxgui_widget *widget)
{
    return widget->sel_anchor < widget->caret ? widget->sel_anchor : widget->caret;
}

static int sxgui_sel_end(const struct sxgui_widget *widget)
{
    return widget->sel_anchor > widget->caret ? widget->sel_anchor : widget->caret;
}

static void sxgui_sel_clear(struct sxgui_widget *widget)
{
    widget->sel_anchor = -1;
}

/* Antes de mover el caret: con shift se ancla (si no habia ancla) para que el
 * movimiento extienda; sin shift se suelta, que colapsa la seleccion. */
static void sxgui_sel_begin_move(struct sxgui_widget *widget, int extend)
{
    if (!extend)
    {
        sxgui_sel_clear(widget);
        return;
    }
    if (widget->sel_anchor < 0)
    {
        widget->sel_anchor = widget->caret;
    }
}

static int sxgui_sel_delete(struct sxgui_widget *widget)
{
    int start;
    int end;
    int length;

    if (!sxgui_sel_active(widget))
    {
        return 0;
    }
    start = sxgui_sel_start(widget);
    end = sxgui_sel_end(widget);
    length = (int)strlen(widget->edit_buffer);
    memmove(
        widget->edit_buffer + start,
        widget->edit_buffer + end,
        (size_t)(length - end + 1));
    widget->caret = start;
    sxgui_sel_clear(widget);
    widget->modified = 1;
    return 1;
}

/* Copiar al portapapeles sin buffer intermedio: se corta la cadena en el final
 * de la seleccion, se copia, y se repone el byte. El sistema es de un solo
 * hilo, asi que nadie puede ver el buffer truncado en el medio. */
static int sxgui_sel_copy(struct sxgui_widget *widget)
{
    int start;
    int end;
    char saved;
    int result;

    if (!sxgui_sel_active(widget))
    {
        return 0;
    }
    start = sxgui_sel_start(widget);
    end = sxgui_sel_end(widget);
    saved = widget->edit_buffer[end];
    widget->edit_buffer[end] = '\0';
    result = clipboard_set_text(widget->edit_buffer + start);
    widget->edit_buffer[end] = saved;
    return result >= 0;
}

/* El pegado necesita si o si una copia intermedia: el texto tiene que entrar en
 * el medio del buffer que se esta editando. Es estatica y no del stack porque
 * son 8 KiB; el costo es una vez por proceso, no por pegado. */
static char g_sxgui_paste[SAVANXP_CLIPBOARD_CAPACITY];

static int sxgui_sel_paste(struct sxgui_widget *widget, int single_line)
{
    int pasted;
    int length;
    int room;
    int insert;

    if (clipboard_get_text(g_sxgui_paste, (int)sizeof(g_sxgui_paste)) <= 0)
    {
        return 0;
    }
    if (single_line)
    {
        /* Un campo de una linea se queda con la primera. Meter el salto crudo
         * dejaria un caracter que el widget no sabe dibujar ni navegar. */
        int index;
        for (index = 0; g_sxgui_paste[index] != '\0'; ++index)
        {
            if (g_sxgui_paste[index] == '\n' || g_sxgui_paste[index] == '\r')
            {
                g_sxgui_paste[index] = '\0';
                break;
            }
        }
    }
    (void)sxgui_sel_delete(widget);

    pasted = (int)strlen(g_sxgui_paste);
    length = (int)strlen(widget->edit_buffer);
    room = widget->edit_capacity - 1 - length;
    /* Lo que no entra se recorta en vez de rechazar el pegado entero: a
     * diferencia de un write al portapapeles, aca el usuario VE lo que quedo. */
    insert = pasted < room ? pasted : room;
    if (insert <= 0)
    {
        return 0;
    }
    memmove(
        widget->edit_buffer + widget->caret + insert,
        widget->edit_buffer + widget->caret,
        (size_t)(length - widget->caret + 1));
    memcpy(widget->edit_buffer + widget->caret, g_sxgui_paste, (size_t)insert);
    widget->caret += insert;
    widget->modified = 1;
    sxgui_sel_clear(widget);
    return 1;
}

/* Espejo de sxgui_textedit_caret_from_point para el campo de una linea. Misma
 * razon: click y arrastre tienen que calcular identico. */
static int sxgui_textfield_caret_from_point(const struct sxgui_widget *widget, int x)
{
    struct sx_rect inner = sxgui_inset(widget->rect, 2);
    int local_x = x - (inner.x + 3) + widget->scroll;
    int length = (int)strlen(widget->edit_buffer);
    int position;

    for (position = 0; position <= length; ++position)
    {
        if (sxgui_text_prefix_width(widget->edit_buffer, position) >= local_x)
        {
            return position;
        }
    }
    return length;
}

static void sxgui_paint_textfield(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    struct sx_rect inner = sxgui_inset(widget->rect, 2);
    int text_y = inner.y + (inner.height - gfx_text_height()) / 2;
    int text_x = inner.x + 3 - widget->scroll;

    sx_painter_fill_rect(painter, widget->rect, SXGUI_COLOR_FIELD);
    sxgui_draw_sunken(painter, widget->rect);

    if (!sx_painter_push_clip(painter, inner))
    {
        return;
    }
    if (widget->edit_buffer != 0)
    {
        sx_painter_draw_text(painter, text_x, text_y, widget->edit_buffer, SXGUI_COLOR_TEXT);
    }
    /* Igual que en textedit: primero la linea entera y DESPUES el tramo
     * resaltado, o el texto normal lo taparia. */
    if (widget->edit_buffer != 0 && sxgui_sel_active(widget))
    {
        int from = sxgui_sel_start(widget);
        int to = sxgui_sel_end(widget);
        int x_from = text_x + sxgui_text_prefix_width(widget->edit_buffer, from);
        int x_to = text_x + sxgui_text_prefix_width(widget->edit_buffer, to);
        char run[256];
        int run_length = to - from;

        sx_painter_fill_rect(
            painter,
            sx_rect_make(x_from, text_y, x_to - x_from, gfx_text_height()),
            SXGUI_COLOR_SELECT);
        if (run_length > 0 && run_length < (int)sizeof(run))
        {
            memcpy(run, widget->edit_buffer + from, (size_t)run_length);
            run[run_length] = '\0';
            sx_painter_draw_text(painter, x_from, text_y, run, SXGUI_COLOR_SELECT_TEXT);
        }
    }
    if (widget->focused)
    {
        int caret_x = text_x + sxgui_text_prefix_width(widget->edit_buffer, widget->caret);
        sxgui_vline(painter, caret_x, text_y, gfx_text_height(), SXGUI_COLOR_TEXT);
    }
    sx_painter_pop_clip(painter);
}

/* ---- textedit (editor multilinea) ----------------------------------------
 *
 * El documento es el buffer del llamador, con lineas separadas por '\n'. No hay
 * indice de lineas: se recorre el buffer cada vez. Para los tamanos que maneja
 * un bloc de notas alcanza de sobra y evita mantener estructura duplicada que
 * se pueda desincronizar del texto.
 */

static int sxgui_textedit_line_count(const char *text)
{
    int count = 1;

    if (text == 0)
    {
        return 1;
    }
    while (*text != 0)
    {
        if (*text == '\n')
        {
            count += 1;
        }
        text += 1;
    }
    return count;
}

/* Indice del primer caracter de `line`; si la linea no existe, el final. */
static int sxgui_textedit_line_start(const char *text, int line)
{
    int index = 0;
    int current = 0;

    if (text == 0)
    {
        return 0;
    }
    while (text[index] != 0 && current < line)
    {
        if (text[index] == '\n')
        {
            current += 1;
        }
        index += 1;
    }
    return index;
}

static int sxgui_textedit_line_length(const char *text, int start)
{
    int length = 0;

    if (text == 0)
    {
        return 0;
    }
    while (text[start + length] != 0 && text[start + length] != '\n')
    {
        length += 1;
    }
    return length;
}

static int sxgui_textedit_caret_line(const char *text, int caret)
{
    int index = 0;
    int line = 0;

    if (text == 0)
    {
        return 0;
    }
    while (index < caret && text[index] != 0)
    {
        if (text[index] == '\n')
        {
            line += 1;
        }
        index += 1;
    }
    return line;
}

static int sxgui_textedit_visible_rows(const struct sxgui_widget *widget)
{
    int rows = (widget->rect.height - 4) / sxgui_row_height();
    return rows > 0 ? rows : 1;
}

static int sxgui_textedit_has_scrollbar(const struct sxgui_widget *widget)
{
    return sxgui_textedit_line_count(widget->edit_buffer) > sxgui_textedit_visible_rows(widget);
}

static struct sx_rect sxgui_textedit_inner(const struct sxgui_widget *widget)
{
    struct sx_rect inner = sxgui_inset(widget->rect, 2);
    if (sxgui_textedit_has_scrollbar(widget))
    {
        inner.width -= SXGUI_SCROLLBAR_THICKNESS;
    }
    return inner;
}

static void sxgui_textedit_metrics(const struct sxgui_widget *widget, struct sxgui_scroll_metrics *metrics)
{
    struct sx_rect inner = sxgui_inset(widget->rect, 2);
    int visible = sxgui_textedit_visible_rows(widget);
    int max_scroll = sxgui_textedit_line_count(widget->edit_buffer) - visible;

    sxgui_scroll_metrics_init(
        metrics,
        sx_rect_make(
            inner.x + inner.width - SXGUI_SCROLLBAR_THICKNESS,
            inner.y,
            SXGUI_SCROLLBAR_THICKNESS,
            inner.height),
        0,
        0,
        max_scroll > 0 ? max_scroll : 0,
        visible,
        widget->value);
}

/* `value` es la primera linea visible y `scroll` el desplazamiento horizontal
 * en pixeles -- el mismo reparto que usa el textfield para su scroll. */
static void sxgui_textedit_reveal_caret(struct sxgui_widget *widget)
{
    int line = sxgui_textedit_caret_line(widget->edit_buffer, widget->caret);
    int visible = sxgui_textedit_visible_rows(widget);
    int start = sxgui_textedit_line_start(widget->edit_buffer, line);
    int inner_width = sxgui_textedit_inner(widget).width - 6;
    int caret_x;

    if (line < widget->value)
    {
        widget->value = line;
    }
    if (line > widget->value + visible - 1)
    {
        widget->value = line - visible + 1;
    }
    if (widget->value < 0)
    {
        widget->value = 0;
    }

    if (inner_width < 1)
    {
        inner_width = 1;
    }
    caret_x = sxgui_text_prefix_width(widget->edit_buffer + start, widget->caret - start);
    if (caret_x - widget->scroll > inner_width - 1)
    {
        widget->scroll = caret_x - (inner_width - 1);
    }
    if (caret_x - widget->scroll < 0)
    {
        widget->scroll = caret_x;
    }
    if (widget->scroll < 0)
    {
        widget->scroll = 0;
    }
}

/* Posicion del caret para un punto de pantalla. Lo usan el click (que ancla) y
 * el arrastre (que extiende), y tienen que coincidir exactamente: si el
 * arrastre calculara distinto, soltar el boton moveria la seleccion. */
static int sxgui_textedit_caret_from_point(const struct sxgui_widget *widget, int x, int y)
{
    struct sx_rect inner = sxgui_textedit_inner(widget);
    int row = widget->value + (y - inner.y) / sxgui_row_height();
    int line_count = sxgui_textedit_line_count(widget->edit_buffer);
    int local_x = x - (inner.x + 3) + widget->scroll;
    int start;
    int length;
    int position;

    if (row < 0)
    {
        row = 0;
    }
    if (row > line_count - 1)
    {
        row = line_count - 1;
    }
    start = sxgui_textedit_line_start(widget->edit_buffer, row);
    length = sxgui_textedit_line_length(widget->edit_buffer, start);
    for (position = 0; position <= length; ++position)
    {
        if (sxgui_text_prefix_width(widget->edit_buffer + start, position) >= local_x)
        {
            return start + position;
        }
    }
    return start + length;
}

static void sxgui_paint_textedit(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    struct sx_rect inner = sxgui_textedit_inner(widget);
    int row_height = sxgui_row_height();
    int line_count = sxgui_textedit_line_count(widget->edit_buffer);
    int caret_line = sxgui_textedit_caret_line(widget->edit_buffer, widget->caret);
    int line;

    sx_painter_fill_rect(painter, widget->rect, SXGUI_COLOR_FIELD);
    sxgui_draw_sunken(painter, widget->rect);

    if (!sx_painter_push_clip(painter, inner))
    {
        return;
    }
    for (line = widget->value; line < line_count; ++line)
    {
        int row_y = inner.y + (line - widget->value) * row_height;
        int start;
        int length;
        char text[256];
        int copy;

        if (row_y >= inner.y + inner.height)
        {
            break;
        }
        start = sxgui_textedit_line_start(widget->edit_buffer, line);
        length = sxgui_textedit_line_length(widget->edit_buffer, start);
        copy = length < (int)sizeof(text) - 1 ? length : (int)sizeof(text) - 1;
        memcpy(text, widget->edit_buffer + start, (size_t)copy);
        text[copy] = 0;

        if (copy > 0)
        {
            sx_painter_draw_text(painter, inner.x + 3 - widget->scroll, row_y + 2, text, SXGUI_COLOR_TEXT);
        }
        /* Resaltado: la interseccion de la seleccion con esta linea. Se pinta
         * la caja y se REDIBUJA solo ese tramo en el color invertido, en vez de
         * dibujar la linea en tres pedazos -- asi el texto de afuera conserva
         * el mismo kerning que tendria sin seleccion. */
        if (sxgui_sel_active(widget))
        {
            int sel_from = sxgui_sel_start(widget);
            int sel_to = sxgui_sel_end(widget);
            int from = sel_from > start ? sel_from - start : 0;
            int to = sel_to < start + length ? sel_to - start : length;

            if (sel_from <= start + length && sel_to >= start && to > from)
            {
                int x_from;
                int x_to;
                char run[256];
                int run_length;

                if (from > copy)
                {
                    from = copy;
                }
                if (to > copy)
                {
                    to = copy;
                }
                x_from = inner.x + 3 - widget->scroll + sxgui_text_prefix_width(text, from);
                x_to = inner.x + 3 - widget->scroll + sxgui_text_prefix_width(text, to);
                sx_painter_fill_rect(
                    painter,
                    sx_rect_make(x_from, row_y + 1, x_to - x_from, row_height - 1),
                    SXGUI_COLOR_SELECT);

                run_length = to - from;
                if (run_length > 0)
                {
                    memcpy(run, text + from, (size_t)run_length);
                    run[run_length] = '\0';
                    sx_painter_draw_text(painter, x_from, row_y + 2, run, SXGUI_COLOR_SELECT_TEXT);
                }
            }
        }
        if (widget->focused && line == caret_line)
        {
            int caret_x = inner.x + 3 - widget->scroll +
                sxgui_text_prefix_width(text, widget->caret - start <= copy ? widget->caret - start : copy);
            sxgui_vline(painter, caret_x, row_y + 2, gfx_text_height(), SXGUI_COLOR_TEXT);
        }
    }
    sx_painter_pop_clip(painter);

    if (sxgui_textedit_has_scrollbar(widget))
    {
        struct sxgui_scroll_metrics metrics;
        sxgui_textedit_metrics(widget, &metrics);
        sxgui_paint_scroll_metrics(painter, &metrics, sxgui_widget_enabled(widget));
    }
}

static void sxgui_paint_one(struct sx_painter *painter, const struct sxgui_widget *widget, int combobox_open)
{
    switch (widget->kind)
    {
    case SXGUI_LABEL:
        sxgui_paint_label(painter, widget);
        break;
    case SXGUI_BUTTON:
        sxgui_paint_button(painter, widget);
        break;
    case SXGUI_CHECKBOX:
        sxgui_paint_checkbox(painter, widget);
        break;
    case SXGUI_LISTBOX:
        sxgui_paint_listbox(painter, widget);
        break;
    case SXGUI_TEXTFIELD:
        sxgui_paint_textfield(painter, widget);
        break;
    case SXGUI_SCROLLBAR:
        sxgui_paint_scrollbar(painter, widget);
        break;
    case SXGUI_GROUPBOX:
        sxgui_paint_groupbox(painter, widget);
        break;
    case SXGUI_PROGRESS:
        sxgui_paint_progress(painter, widget);
        break;
    case SXGUI_RADIO:
        sxgui_paint_radio(painter, widget);
        break;
    case SXGUI_COMBOBOX:
        sxgui_paint_combobox(painter, widget, combobox_open);
        break;
    case SXGUI_TEXTVIEW:
        sxgui_paint_textview(painter, widget);
        break;
    case SXGUI_TEXTEDIT:
        sxgui_paint_textedit(painter, widget);
        break;
    default:
        break;
    }
}

#define SXGUI_DIALOG_BORDER 3

static int sxgui_dialog_title_height(void)
{
    return gfx_text_height() + 6;
}

static struct sx_point sxgui_dialog_client_origin(const struct sxgui_dialog *dialog)
{
    struct sx_point origin;
    origin.x = dialog->rect.x + SXGUI_DIALOG_BORDER;
    origin.y = dialog->rect.y + SXGUI_DIALOG_BORDER + sxgui_dialog_title_height();
    return origin;
}

static void sxgui_paint_dialog(struct sxgui_context *ctx)
{
    struct sxgui_dialog *dialog = ctx->modal;
    struct sx_painter *painter = &ctx->painter;
    int title_height = sxgui_dialog_title_height();
    struct sx_rect title = sx_rect_make(
        dialog->rect.x + SXGUI_DIALOG_BORDER,
        dialog->rect.y + SXGUI_DIALOG_BORDER,
        dialog->rect.width - SXGUI_DIALOG_BORDER * 2,
        title_height);
    struct sx_point origin = sxgui_dialog_client_origin(dialog);
    int index;

    sx_painter_fill_rect(painter, dialog->rect, SXGUI_COLOR_FACE);
    sxgui_draw_raised(painter, dialog->rect);
    sx_painter_fill_rect(painter, title, SXGUI_COLOR_SELECT);
    if (dialog->title != 0)
    {
        sx_painter_draw_text(
            painter,
            title.x + 4,
            title.y + (title_height - gfx_text_height()) / 2,
            dialog->title,
            SXGUI_COLOR_SELECT_TEXT);
    }

    /* El borde doble va DEBAJO del boton: se dibuja el marco alrededor de su
     * rect y despues el boton encima, que es como se veia el default. */
    if (dialog->default_button > 0 && dialog->default_button < dialog->widget_count)
    {
        const struct sxgui_widget *target = &dialog->widgets[dialog->default_button];
        struct sx_rect outline = sx_rect_make(
            origin.x + target->rect.x - 2,
            origin.y + target->rect.y - 2,
            target->rect.width + 4,
            target->rect.height + 4);
        sx_painter_draw_frame(painter, outline, SXGUI_COLOR_TEXT);
    }

    for (index = 0; index < dialog->widget_count; ++index)
    {
        struct sxgui_widget shifted = dialog->widgets[index];

        if (!sxgui_widget_visible(&shifted))
        {
            continue;
        }
        shifted.rect = sx_rect_translate(shifted.rect, origin.x, origin.y);
        sxgui_paint_one(painter, &shifted, 0);
    }
}

void sxgui_paint_content(struct sxgui_context *ctx)
{
    int index;

    if (ctx == 0 || ctx->widgets == 0)
    {
        return;
    }

    sx_painter_fill(&ctx->painter, SXGUI_COLOR_WINDOW);

    for (index = 0; index < ctx->widget_count; ++index)
    {
        struct sxgui_widget *widget = &ctx->widgets[index];
        if (!sxgui_widget_visible(widget))
        {
            continue;
        }
        sxgui_paint_one(&ctx->painter, widget, ctx->popup_owner == index);
    }
}

void sxgui_paint_overlay(struct sxgui_context *ctx)
{
    if (ctx == 0 || ctx->widgets == 0)
    {
        return;
    }

    /* overlay pass: chrome and popups paint above every widget */
    if (ctx->menubar != 0)
    {
        sxgui_paint_menubar(ctx);
    }
    if (ctx->popup_owner >= 0 && ctx->popup_owner < ctx->widget_count)
    {
        sxgui_paint_combobox_popup(ctx);
    }
    if (ctx->menubar != 0 && ctx->menubar->open_menu >= 0 &&
        ctx->menubar->open_menu < ctx->menubar->menu_count)
    {
        sxgui_paint_menu_popup(ctx);
    }
    if (ctx->modal != 0)
    {
        sxgui_paint_dialog(ctx);
    }
}

void sxgui_paint(struct sxgui_context *ctx)
{
    sxgui_paint_content(ctx);
    sxgui_paint_overlay(ctx);
}

void sxgui_dialog_begin(struct sxgui_context *ctx, struct sxgui_dialog *dialog, int width, int height)
{
    int total_width;
    int total_height;
    int x;
    int y;

    if (ctx == 0 || dialog == 0)
    {
        return;
    }
    total_width = width + SXGUI_DIALOG_BORDER * 2;
    total_height = height + SXGUI_DIALOG_BORDER * 2 + sxgui_dialog_title_height();
    x = ((int)ctx->target.info.width - total_width) / 2;
    y = ((int)ctx->target.info.height - total_height) / 2;
    if (x < 0)
    {
        x = 0;
    }
    if (y < 0)
    {
        y = 0;
    }
    dialog->rect = sx_rect_make(x, y, total_width, total_height);
    dialog->result = 0;
    dialog->saved_focus = ctx->focus_index;
    ctx->modal = dialog;
    /* Mientras hay modal, focus_index indexa el array del dialogo: el despacho
     * de teclado intercambia ctx->widgets por dialog->widgets. */
    ctx->focus_index = -1;
    if (dialog->initial_focus > 0 && dialog->initial_focus < dialog->widget_count)
    {
        int focus;
        for (focus = 0; focus < dialog->widget_count; ++focus)
        {
            dialog->widgets[focus].focused = (focus == dialog->initial_focus) ? 1 : 0;
        }
        ctx->focus_index = dialog->initial_focus;
    }
    ctx->capture_index = -1;
    sxgui_popup_close(ctx);
    if (ctx->menubar != 0)
    {
        ctx->menubar->open_menu = -1;
    }
}

void sxgui_dialog_end(struct sxgui_context *ctx, int result)
{
    struct sxgui_dialog *dialog;
    int index;

    if (ctx == 0 || ctx->modal == 0)
    {
        return;
    }
    dialog = ctx->modal;
    dialog->result = result;
    for (index = 0; index < dialog->widget_count; ++index)
    {
        dialog->widgets[index].focused = 0;
    }
    ctx->modal = 0;
    ctx->capture_index = -1;
    ctx->focus_index = dialog->saved_focus;
}

int sxgui_dialog_active(const struct sxgui_context *ctx)
{
    return ctx != 0 && ctx->modal != 0;
}

/* ---- input dispatch ----------------------------------------------------- */

static int sxgui_hit(const struct sxgui_widget *widget, int x, int y)
{
    return sx_rect_contains_point(widget->rect, x, y);
}

static void sxgui_set_focus(struct sxgui_context *ctx, int new_index)
{
    int index;
    if (ctx->focus_index == new_index)
    {
        return;
    }
    for (index = 0; index < ctx->widget_count; ++index)
    {
        ctx->widgets[index].focused = (index == new_index) ? 1 : 0;
    }
    ctx->focus_index = new_index;
}

static void sxgui_fire(struct sxgui_widget *widget, int action)
{
    widget->action = action;
    if (widget->on_action != 0)
    {
        widget->on_action(widget, widget->user);
    }
}

/* Check one radio and clear the rest of its group. Fires only on change. */
static int sxgui_radio_select(struct sxgui_context *ctx, int index)
{
    struct sxgui_widget *widget = &ctx->widgets[index];
    int other;

    if (widget->value)
    {
        return 0;
    }
    for (other = 0; other < ctx->widget_count; ++other)
    {
        if (other != index && ctx->widgets[other].kind == SXGUI_RADIO &&
            ctx->widgets[other].group == widget->group)
        {
            ctx->widgets[other].value = 0;
        }
    }
    widget->value = 1;
    sxgui_fire(widget, SXGUI_ACTION_CHANGE);
    return 1;
}

/* Pointer moves routed to the captured widget while the button is held. */
static int sxgui_capture_motion(struct sxgui_context *ctx, struct sxgui_widget *widget, const struct savanxp_gui_pointer_event *event)
{
    if (widget->kind == SXGUI_TEXTFIELD && widget->edit_buffer != 0)
    {
        int caret = sxgui_textfield_caret_from_point(widget, event->x);
        if (caret == widget->caret)
        {
            return 0;
        }
        widget->caret = caret;
        sxgui_textfield_scroll_to_caret(widget);
        return 1;
    }
    if (widget->kind == SXGUI_TEXTEDIT && widget->edit_buffer != 0)
    {
        /* Arrastrar mueve el caret y deja el ancla quieta: eso ES extender la
         * seleccion, sin ningun estado extra. */
        int caret = sxgui_textedit_caret_from_point(widget, event->x, event->y);
        if (caret == widget->caret)
        {
            return 0;
        }
        widget->caret = caret;
        sxgui_textedit_reveal_caret(widget);
        return 1;
    }
    if (widget->kind == SXGUI_BUTTON)
    {
        int over = sxgui_hit(widget, event->x, event->y);
        if (over != widget->hover)
        {
            widget->hover = over;
            return 1;
        }
        return 0;
    }
    if (widget->kind == SXGUI_SCROLLBAR && ctx->capture_part == SXGUI_SCROLL_THUMB)
    {
        struct sxgui_scroll_metrics metrics;
        int value;

        sxgui_scroll_metrics_init(
            &metrics,
            widget->rect,
            (widget->flags & SXGUI_FLAG_HSCROLL) != 0,
            widget->range_min,
            widget->range_max,
            widget->page,
            widget->value);
        value = sxgui_scroll_value_from_drag(&metrics, event->x, event->y, ctx->capture_offset);
        if (value != widget->value)
        {
            widget->value = value;
            sxgui_fire(widget, SXGUI_ACTION_CHANGE);
            return 1;
        }
        return 0;
    }
    if ((widget->kind == SXGUI_LISTBOX || widget->kind == SXGUI_TEXTVIEW) &&
        ctx->capture_part == SXGUI_SCROLL_THUMB)
    {
        struct sxgui_scroll_metrics metrics;
        int value;

        sxgui_listbox_metrics(widget, &metrics);
        value = sxgui_scroll_value_from_drag(&metrics, event->x, event->y, ctx->capture_offset);
        if (value != widget->scroll)
        {
            widget->scroll = value;
            return 1;
        }
        return 0;
    }
    return 0;
}

int sxgui_cursor_shape(const struct sxgui_context *ctx)
{
    return ctx != 0 ? ctx->cursor_shape : SAVANXP_CURSOR_ARROW;
}

int sxgui_handle_pointer(struct sxgui_context *ctx, const struct savanxp_gui_pointer_event *event)
{
    int changed = 0;
    int index;
    uint32_t left = SAVANXP_MOUSE_BUTTON_LEFT;
    int left_now;
    int left_before;

    if (ctx == 0 || ctx->widgets == 0 || event == 0)
    {
        return 0;
    }

    ctx->pointer_x = event->x;
    ctx->pointer_y = event->y;
    left_now = (event->buttons & left) != 0;
    left_before = (ctx->last_buttons & left) != 0;

    /* an active dialog captures the pointer: translate to its client area and
     * re-dispatch against the dialog's widget array */
    if (ctx->modal != 0 && !ctx->modal_route)
    {
        struct sxgui_dialog *dialog = ctx->modal;
        struct sx_point origin = sxgui_dialog_client_origin(dialog);
        struct savanxp_gui_pointer_event local = *event;
        struct sxgui_widget *saved_widgets = ctx->widgets;
        int saved_count = ctx->widget_count;

        local.x -= origin.x;
        local.y -= origin.y;
        ctx->widgets = dialog->widgets;
        ctx->widget_count = dialog->widget_count;
        ctx->modal_route = 1;
        changed = sxgui_handle_pointer(ctx, &local);
        ctx->modal_route = 0;
        ctx->widgets = saved_widgets;
        ctx->widget_count = saved_count;
        if (ctx->modal == 0)
        {
            /* a dialog callback ended it mid-dispatch: undo any focus the
             * dispatch left on dialog widgets */
            for (saved_count = 0; saved_count < dialog->widget_count; ++saved_count)
            {
                dialog->widgets[saved_count].focused = 0;
            }
            ctx->focus_index = dialog->saved_focus;
            ctx->capture_index = -1;
            changed = 1;
        }
        return changed;
    }

    /* an open menu owns the pointer: hovering the bar switches menus, click
     * on an item fires it, click anywhere else closes and is consumed */
    if (ctx->modal == 0 && ctx->menubar != 0 && ctx->menubar->open_menu >= 0 &&
        ctx->menubar->open_menu < ctx->menubar->menu_count)
    {
        struct sxgui_menubar *bar = ctx->menubar;
        const struct sxgui_menu *menu = &bar->menus[bar->open_menu];
        struct sx_rect popup = sxgui_menu_popup_rect(ctx, bar, bar->open_menu);

        if (event->y < sxgui_menubar_height())
        {
            int title_index = sxgui_menubar_title_at(bar, event->x, event->y);

            if (title_index >= 0 && title_index != bar->open_menu)
            {
                bar->open_menu = title_index;
                bar->hot_item = -1;
                changed = 1;
            }
            else if (left_now && !left_before && title_index == bar->open_menu)
            {
                bar->open_menu = -1;
                changed = 1;
            }
        }
        else if (sx_rect_contains_point(popup, event->x, event->y))
        {
            int item_index = sxgui_menu_item_at(popup, menu, event->y);

            if (item_index != bar->hot_item)
            {
                bar->hot_item = item_index;
                changed = 1;
            }
            if (left_now && !left_before && sxgui_menu_item_selectable(menu, item_index))
            {
                sxgui_menu_fire(bar, menu, item_index);
                changed = 1;
            }
        }
        else if (left_now && !left_before)
        {
            bar->open_menu = -1;
            changed = 1;
        }
        ctx->last_buttons = event->buttons;
        return changed;
    }

    /* closed menu bar: presses on the strip open menus and stay in the bar */
    if (ctx->modal == 0 && ctx->menubar != 0 && event->y < sxgui_menubar_height())
    {
        if (left_now && !left_before)
        {
            int title_index = sxgui_menubar_title_at(ctx->menubar, event->x, event->y);

            if (title_index >= 0)
            {
                sxgui_popup_close(ctx);
                ctx->menubar->open_menu = title_index;
                ctx->menubar->hot_item = -1;
                changed = 1;
            }
        }
        ctx->last_buttons = event->buttons;
        return changed;
    }

    /* an open dropdown owns the pointer: click outside closes and is consumed */
    if (ctx->popup_owner >= 0 && ctx->popup_owner < ctx->widget_count)
    {
        struct sxgui_widget *owner = &ctx->widgets[ctx->popup_owner];
        struct sx_rect inner = sxgui_inset(ctx->popup_rect, 1);

        if (sx_rect_contains_point(ctx->popup_rect, event->x, event->y))
        {
            int row = ctx->popup_scroll + (event->y - inner.y) / sxgui_row_height();

            if (event->y >= inner.y && row >= 0 && row < owner->item_count)
            {
                if (row != ctx->popup_hot)
                {
                    ctx->popup_hot = row;
                    changed = 1;
                }
                if (left_now && !left_before)
                {
                    if (owner->value != row)
                    {
                        owner->value = row;
                        sxgui_fire(owner, SXGUI_ACTION_CHANGE);
                    }
                    sxgui_popup_close(ctx);
                    changed = 1;
                }
            }
        }
        else if (left_now && !left_before)
        {
            sxgui_popup_close(ctx);
            changed = 1;
        }
        ctx->last_buttons = event->buttons;
        return changed;
    }

    if (ctx->capture_index >= 0 && ctx->capture_index < ctx->widget_count)
    {
        struct sxgui_widget *captured = &ctx->widgets[ctx->capture_index];

        if (left_now)
        {
            changed = sxgui_capture_motion(ctx, captured, event);
        }
        else
        {
            if (captured->kind == SXGUI_BUTTON && captured->pressed)
            {
                if (captured->hover)
                {
                    sxgui_fire(captured, SXGUI_ACTION_CLICK);
                }
                captured->pressed = 0;
            }
            ctx->capture_index = -1;
            ctx->capture_part = SXGUI_SCROLL_NONE;
            changed = 1;
        }
        ctx->last_buttons = event->buttons;
        return changed;
    }

    ctx->cursor_shape = SAVANXP_CURSOR_ARROW;
    for (index = 0; index < ctx->widget_count; ++index)
    {
        struct sxgui_widget *widget = &ctx->widgets[index];
        int over;
        int hit;

        if (!sxgui_widget_visible(widget))
        {
            continue;
        }
        hit = sxgui_hit(widget, event->x, event->y);
        over = sxgui_widget_enabled(widget) && hit;
        if (over != widget->hover)
        {
            widget->hover = over;
            changed = 1;
        }
        if (hit)
        {
            if (!sxgui_widget_enabled(widget))
            {
                ctx->cursor_shape = SAVANXP_CURSOR_UNAVAILABLE;
            }
            else if (widget->kind == SXGUI_TEXTFIELD || widget->kind == SXGUI_TEXTEDIT)
            {
                ctx->cursor_shape = SAVANXP_CURSOR_TEXT;
            }
        }
    }

    if (left_now && !left_before)
    {
        int focus_target = -1;
        for (index = 0; index < ctx->widget_count; ++index)
        {
            struct sxgui_widget *widget = &ctx->widgets[index];
            if (!sxgui_widget_visible(widget) || !sxgui_widget_enabled(widget) ||
                !sxgui_hit(widget, event->x, event->y))
            {
                continue;
            }
            if (sxgui_focusable(widget))
            {
                focus_target = index;
            }
            switch (widget->kind)
            {
            case SXGUI_BUTTON:
                widget->pressed = 1;
                ctx->capture_index = index;
                ctx->capture_part = SXGUI_SCROLL_NONE;
                changed = 1;
                break;
            case SXGUI_CHECKBOX:
                widget->value = widget->value ? 0 : 1;
                sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                changed = 1;
                break;
            case SXGUI_RADIO:
                if (sxgui_radio_select(ctx, index))
                {
                    changed = 1;
                }
                break;
            case SXGUI_COMBOBOX:
                if (widget->item_count > 0)
                {
                    sxgui_popup_open(ctx, index);
                    changed = 1;
                }
                break;
            case SXGUI_LISTBOX:
            {
                if (sxgui_listbox_scrollbar_press(ctx, index, widget, event, &changed))
                {
                    break;
                }
                {
                    struct sx_rect inner = sxgui_listbox_inner(widget);
                    int row = widget->scroll + (event->y - inner.y) / sxgui_row_height();
                    if (event->y >= inner.y && row >= 0 && row < widget->item_count)
                    {
                        unsigned long now_ms = uptime_ms();
                        if (row != widget->value)
                        {
                            widget->value = row;
                            sxgui_listbox_ensure_visible(widget);
                            sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                        }
                        else if (ctx->last_click_index == index &&
                                 now_ms - ctx->last_click_ms <= SXGUI_DOUBLE_CLICK_MS)
                        {
                            sxgui_fire(widget, SXGUI_ACTION_ACTIVATE);
                        }
                        ctx->last_click_index = index;
                        ctx->last_click_ms = now_ms;
                        changed = 1;
                    }
                }
                break;
            }
            case SXGUI_TEXTVIEW:
                (void)sxgui_listbox_scrollbar_press(ctx, index, widget, event, &changed);
                break;
            case SXGUI_SCROLLBAR:
            {
                struct sxgui_scroll_metrics metrics;
                int grab_offset = 0;
                int part;

                sxgui_scroll_metrics_init(
                    &metrics,
                    widget->rect,
                    (widget->flags & SXGUI_FLAG_HSCROLL) != 0,
                    widget->range_min,
                    widget->range_max,
                    widget->page,
                    widget->value);
                part = sxgui_scroll_hit_part(&metrics, event->x, event->y, &grab_offset);
                if (part == SXGUI_SCROLL_THUMB)
                {
                    ctx->capture_index = index;
                    ctx->capture_part = part;
                    ctx->capture_offset = grab_offset;
                }
                else if (part != SXGUI_SCROLL_NONE)
                {
                    int value = sxgui_scroll_step_value(&metrics, part);
                    if (value != widget->value)
                    {
                        widget->value = value;
                        sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                    }
                    changed = 1;
                }
                break;
            }
            case SXGUI_TEXTFIELD:
            {
                if (widget->edit_buffer != 0)
                {
                    if (!ctx->shift_down || widget->sel_anchor < 0)
                    {
                        widget->sel_anchor = sxgui_textfield_caret_from_point(widget, event->x);
                    }
                    widget->caret = sxgui_textfield_caret_from_point(widget, event->x);
                    ctx->capture_index = index;
                    sxgui_textfield_scroll_to_caret(widget);
                    changed = 1;
                }
                break;
            }
            case SXGUI_TEXTEDIT:
            {
                if (widget->edit_buffer != 0)
                {
                    /* Shift+click extiende desde el ancla que ya habia, como en
                     * cualquier editor; un click pelado ancla donde se hizo y
                     * deja lista una seleccion vacia que el arrastre estira. */
                    if (!ctx->shift_down || widget->sel_anchor < 0)
                    {
                        widget->sel_anchor = sxgui_textedit_caret_from_point(widget, event->x, event->y);
                    }
                    widget->caret = sxgui_textedit_caret_from_point(widget, event->x, event->y);
                    /* Captura: sin esto el movimiento con el boton apretado no
                     * llega a este widget y no se puede arrastrar para
                     * seleccionar. */
                    ctx->capture_index = index;
                    sxgui_textedit_reveal_caret(widget);
                    changed = 1;
                }
                break;
            }
            default:
                break;
            }
        }
        sxgui_set_focus(ctx, focus_target);
        changed = 1;
    }
    else if (!left_now && left_before)
    {
        for (index = 0; index < ctx->widget_count; ++index)
        {
            struct sxgui_widget *widget = &ctx->widgets[index];
            if (widget->kind == SXGUI_BUTTON && widget->pressed)
            {
                if (widget->hover)
                {
                    sxgui_fire(widget, SXGUI_ACTION_CLICK);
                }
                widget->pressed = 0;
                changed = 1;
            }
        }
    }

    ctx->last_buttons = event->buttons;
    return changed;
}

/* Move focus to the next/previous focusable widget, wrapping around. */
static int sxgui_focus_step(struct sxgui_context *ctx, int direction)
{
    int start = ctx->focus_index;
    int index;
    int step;

    if (start < 0 || start >= ctx->widget_count)
    {
        start = direction > 0 ? ctx->widget_count - 1 : 0;
    }
    for (step = 1; step <= ctx->widget_count; ++step)
    {
        index = start + direction * step;
        while (index < 0)
        {
            index += ctx->widget_count;
        }
        index %= ctx->widget_count;
        if (sxgui_focusable(&ctx->widgets[index]))
        {
            sxgui_set_focus(ctx, index);
            return 1;
        }
    }
    return 0;
}

int sxgui_handle_key(struct sxgui_context *ctx, const struct savanxp_input_event *event)
{
    struct sxgui_widget *widget;
    int length;

    if (ctx == 0 || ctx->widgets == 0 || event == 0)
    {
        return 0;
    }
    if (event->type == SAVANXP_INPUT_EVENT_KEY_UP)
    {
        if (event->key == SAVANXP_KEY_SHIFT)
        {
            ctx->shift_down = 0;
        }
        return 0;
    }
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return 0;
    }
    if (event->key == SAVANXP_KEY_SHIFT)
    {
        ctx->shift_down = 1;
        return 0;
    }
    /* an active dialog swallows the keyboard; ESC ends it with result 0 */
    if (ctx->modal != 0 && !ctx->modal_route)
    {
        struct sxgui_dialog *dialog = ctx->modal;
        struct sxgui_widget *saved_widgets;
        int saved_count;
        int changed;

        if (event->key == SAVANXP_KEY_ESC)
        {
            sxgui_dialog_end(ctx, 0);
            return 1;
        }
        /* Enter dispara el boton por defecto, salvo que el foco ya este sobre
         * OTRO boton -- ahi gana el que el usuario eligio con Tab, que es lo
         * que uno espera despues de haberse movido a proposito. */
        if (event->key == SAVANXP_KEY_ENTER)
        {
            int target = dialog->default_button;
            if (ctx->focus_index > 0 && ctx->focus_index < dialog->widget_count &&
                dialog->widgets[ctx->focus_index].kind == SXGUI_BUTTON)
            {
                target = ctx->focus_index;
            }
            if (target > 0 && target < dialog->widget_count &&
                dialog->widgets[target].kind == SXGUI_BUTTON &&
                sxgui_widget_enabled(&dialog->widgets[target]))
            {
                sxgui_fire(&dialog->widgets[target], SXGUI_ACTION_CLICK);
                return 1;
            }
        }
        saved_widgets = ctx->widgets;
        saved_count = ctx->widget_count;
        ctx->widgets = dialog->widgets;
        ctx->widget_count = dialog->widget_count;
        ctx->modal_route = 1;
        changed = sxgui_handle_key(ctx, event);
        ctx->modal_route = 0;
        ctx->widgets = saved_widgets;
        ctx->widget_count = saved_count;
        if (ctx->modal == 0)
        {
            int index;
            for (index = 0; index < dialog->widget_count; ++index)
            {
                dialog->widgets[index].focused = 0;
            }
            ctx->focus_index = dialog->saved_focus;
            ctx->capture_index = -1;
        }
        (void)changed;
        return 1;
    }

    /* an open menu swallows the keyboard; ESC closes it (consumed so the app
     * frame does not treat it as quit) */
    if (ctx->modal == 0 && ctx->menubar != 0 && ctx->menubar->open_menu >= 0 &&
        ctx->menubar->open_menu < ctx->menubar->menu_count)
    {
        struct sxgui_menubar *bar = ctx->menubar;
        const struct sxgui_menu *menu = &bar->menus[bar->open_menu];

        if (event->key == SAVANXP_KEY_ESC)
        {
            bar->open_menu = -1;
            return 1;
        }
        if (event->key == SAVANXP_KEY_LEFT || event->key == SAVANXP_KEY_RIGHT)
        {
            int direction = event->key == SAVANXP_KEY_RIGHT ? 1 : -1;
            bar->open_menu = (bar->open_menu + direction + bar->menu_count) % bar->menu_count;
            bar->hot_item = -1;
            return 1;
        }
        if (event->key == SAVANXP_KEY_UP || event->key == SAVANXP_KEY_DOWN)
        {
            bar->hot_item = sxgui_menu_step_hot(menu, bar->hot_item, event->key == SAVANXP_KEY_DOWN ? 1 : -1);
            return 1;
        }
        if (event->key == SAVANXP_KEY_ENTER && sxgui_menu_item_selectable(menu, bar->hot_item))
        {
            sxgui_menu_fire(bar, menu, bar->hot_item);
            return 1;
        }
        return 1;
    }
    /* an open dropdown swallows the keyboard; ESC closes it (and is consumed
     * here so the app frame does not treat it as quit) */
    if (ctx->popup_owner >= 0 && ctx->popup_owner < ctx->widget_count)
    {
        struct sxgui_widget *owner = &ctx->widgets[ctx->popup_owner];

        if (event->key == SAVANXP_KEY_ESC)
        {
            sxgui_popup_close(ctx);
            return 1;
        }
        if (event->key == SAVANXP_KEY_UP && ctx->popup_hot > 0)
        {
            ctx->popup_hot -= 1;
            sxgui_popup_ensure_hot_visible(ctx, owner);
            return 1;
        }
        if (event->key == SAVANXP_KEY_DOWN && ctx->popup_hot + 1 < owner->item_count)
        {
            ctx->popup_hot += 1;
            sxgui_popup_ensure_hot_visible(ctx, owner);
            return 1;
        }
        if (event->key == SAVANXP_KEY_ENTER)
        {
            if (owner->value != ctx->popup_hot)
            {
                owner->value = ctx->popup_hot;
                sxgui_fire(owner, SXGUI_ACTION_CHANGE);
            }
            sxgui_popup_close(ctx);
            return 1;
        }
        return 1;
    }
    if (event->key == SAVANXP_KEY_TAB)
    {
        return sxgui_focus_step(ctx, ctx->shift_down ? -1 : 1);
    }
    if (ctx->focus_index < 0 || ctx->focus_index >= ctx->widget_count)
    {
        return 0;
    }

    widget = &ctx->widgets[ctx->focus_index];

    if (widget->kind == SXGUI_BUTTON || widget->kind == SXGUI_CHECKBOX)
    {
        if (event->key == SAVANXP_KEY_ENTER || event->ascii == ' ')
        {
            if (widget->kind == SXGUI_CHECKBOX)
            {
                widget->value = widget->value ? 0 : 1;
                sxgui_fire(widget, SXGUI_ACTION_CHANGE);
            }
            else
            {
                sxgui_fire(widget, SXGUI_ACTION_CLICK);
            }
            return 1;
        }
        return 0;
    }

    if (widget->kind == SXGUI_RADIO)
    {
        if (event->key == SAVANXP_KEY_ENTER || event->ascii == ' ')
        {
            return sxgui_radio_select(ctx, ctx->focus_index);
        }
        return 0;
    }

    if (widget->kind == SXGUI_COMBOBOX)
    {
        if (event->key == SAVANXP_KEY_ENTER || event->ascii == ' ')
        {
            if (widget->item_count > 0)
            {
                sxgui_popup_open(ctx, ctx->focus_index);
                return 1;
            }
            return 0;
        }
        if (event->key == SAVANXP_KEY_UP && widget->value > 0)
        {
            widget->value -= 1;
            sxgui_fire(widget, SXGUI_ACTION_CHANGE);
            return 1;
        }
        if (event->key == SAVANXP_KEY_DOWN && widget->value + 1 < widget->item_count)
        {
            widget->value += 1;
            sxgui_fire(widget, SXGUI_ACTION_CHANGE);
            return 1;
        }
        return 0;
    }

    if (widget->kind == SXGUI_LISTBOX)
    {
        int visible_rows = sxgui_listbox_visible_rows(widget);
        int new_value = widget->value;

        if (widget->item_count <= 0)
        {
            return 0;
        }
        if (event->key == SAVANXP_KEY_ENTER)
        {
            sxgui_fire(widget, SXGUI_ACTION_ACTIVATE);
            return 1;
        }
        switch (event->key)
        {
        case SAVANXP_KEY_UP:
            new_value -= 1;
            break;
        case SAVANXP_KEY_DOWN:
            new_value += 1;
            break;
        case SAVANXP_KEY_PAGE_UP:
            new_value -= visible_rows;
            break;
        case SAVANXP_KEY_PAGE_DOWN:
            new_value += visible_rows;
            break;
        case SAVANXP_KEY_HOME:
            new_value = 0;
            break;
        case SAVANXP_KEY_END:
            new_value = widget->item_count - 1;
            break;
        default:
            return 0;
        }
        new_value = sxgui_clamp_int(new_value, 0, widget->item_count - 1);
        if (new_value == widget->value)
        {
            return 0;
        }
        widget->value = new_value;
        sxgui_listbox_ensure_visible(widget);
        sxgui_fire(widget, SXGUI_ACTION_CHANGE);
        return 1;
    }

    if (widget->kind == SXGUI_TEXTVIEW)
    {
        int visible_rows = sxgui_listbox_visible_rows(widget);
        int new_scroll = widget->scroll;

        switch (event->key)
        {
        case SAVANXP_KEY_UP:
            new_scroll -= 1;
            break;
        case SAVANXP_KEY_DOWN:
            new_scroll += 1;
            break;
        case SAVANXP_KEY_PAGE_UP:
            new_scroll -= visible_rows;
            break;
        case SAVANXP_KEY_PAGE_DOWN:
            new_scroll += visible_rows;
            break;
        case SAVANXP_KEY_HOME:
            new_scroll = 0;
            break;
        case SAVANXP_KEY_END:
            new_scroll = sxgui_listbox_max_scroll(widget);
            break;
        default:
            return 0;
        }
        new_scroll = sxgui_clamp_int(new_scroll, 0, sxgui_listbox_max_scroll(widget));
        if (new_scroll == widget->scroll)
        {
            return 0;
        }
        widget->scroll = new_scroll;
        return 1;
    }

    if (widget->kind == SXGUI_SCROLLBAR)
    {
        int new_value = widget->value;

        switch (event->key)
        {
        case SAVANXP_KEY_UP:
        case SAVANXP_KEY_LEFT:
            new_value -= 1;
            break;
        case SAVANXP_KEY_DOWN:
        case SAVANXP_KEY_RIGHT:
            new_value += 1;
            break;
        case SAVANXP_KEY_PAGE_UP:
            new_value -= widget->page;
            break;
        case SAVANXP_KEY_PAGE_DOWN:
            new_value += widget->page;
            break;
        case SAVANXP_KEY_HOME:
            new_value = widget->range_min;
            break;
        case SAVANXP_KEY_END:
            new_value = widget->range_max;
            break;
        default:
            return 0;
        }
        new_value = sxgui_clamp_int(new_value, widget->range_min, widget->range_max);
        if (new_value == widget->value)
        {
            return 0;
        }
        widget->value = new_value;
        sxgui_fire(widget, SXGUI_ACTION_CHANGE);
        return 1;
    }

    if (widget->kind == SXGUI_TEXTFIELD && widget->edit_buffer != 0 && widget->edit_capacity > 0)
    {
        sxgui_textfield_clamp_caret(widget);
        length = (int)strlen(widget->edit_buffer);

        /* Misma maquinaria que textedit: los helpers de seleccion trabajan
         * sobre edit_buffer/caret/sel_anchor y no saben de que widget son. */
        if ((event->modifiers & SAVANXP_KEY_MOD_CTRL) != 0 &&
            (event->modifiers & SAVANXP_KEY_MOD_ALT_GR) == 0)
        {
            int done = 0;

            switch (event->ascii)
            {
            case 'c':
            case 'C':
                done = sxgui_sel_copy(widget);
                break;
            case 'x':
            case 'X':
                if (sxgui_sel_copy(widget) && sxgui_sel_delete(widget))
                {
                    sxgui_textfield_scroll_to_caret(widget);
                    sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                }
                done = 1;
                break;
            case 'v':
            case 'V':
                if (sxgui_sel_paste(widget, 1))
                {
                    sxgui_textfield_scroll_to_caret(widget);
                    sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                }
                done = 1;
                break;
            case 'a':
            case 'A':
                widget->sel_anchor = 0;
                widget->caret = length;
                sxgui_textfield_scroll_to_caret(widget);
                done = 1;
                break;
            default:
                break;
            }
            if (done || (event->ascii >= 32 && event->ascii < 127))
            {
                return 1;
            }
        }

        if (sxgui_sel_active(widget))
        {
            int destructive =
                event->key == SAVANXP_KEY_BACKSPACE ||
                event->key == SAVANXP_KEY_DELETE ||
                (event->ascii >= 32 && event->ascii < 127);

            if (destructive)
            {
                sxgui_sel_delete(widget);
                length = (int)strlen(widget->edit_buffer);
                sxgui_textfield_scroll_to_caret(widget);
                sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                if (event->key == SAVANXP_KEY_BACKSPACE || event->key == SAVANXP_KEY_DELETE)
                {
                    return 1;
                }
            }
        }

        switch (event->key)
        {
        case SAVANXP_KEY_LEFT:
        case SAVANXP_KEY_RIGHT:
        case SAVANXP_KEY_HOME:
        case SAVANXP_KEY_END:
            sxgui_sel_begin_move(widget, (event->modifiers & SAVANXP_KEY_MOD_SHIFT) != 0);
            break;
        default:
            sxgui_sel_clear(widget);
            break;
        }

        if (event->key == SAVANXP_KEY_LEFT)
        {
            if (widget->caret > 0)
            {
                widget->caret -= 1;
                sxgui_textfield_scroll_to_caret(widget);
                return 1;
            }
            return 0;
        }
        if (event->key == SAVANXP_KEY_RIGHT)
        {
            if (widget->caret < length)
            {
                widget->caret += 1;
                sxgui_textfield_scroll_to_caret(widget);
                return 1;
            }
            return 0;
        }
        if (event->key == SAVANXP_KEY_HOME)
        {
            widget->caret = 0;
            sxgui_textfield_scroll_to_caret(widget);
            return 1;
        }
        if (event->key == SAVANXP_KEY_END)
        {
            widget->caret = length;
            sxgui_textfield_scroll_to_caret(widget);
            return 1;
        }
        if (event->key == SAVANXP_KEY_BACKSPACE)
        {
            if (widget->caret > 0)
            {
                memmove(
                    widget->edit_buffer + widget->caret - 1,
                    widget->edit_buffer + widget->caret,
                    (size_t)(length - widget->caret + 1));
                widget->caret -= 1;
                sxgui_textfield_scroll_to_caret(widget);
                sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                return 1;
            }
            return 0;
        }
        if (event->key == SAVANXP_KEY_DELETE)
        {
            if (widget->caret < length)
            {
                memmove(
                    widget->edit_buffer + widget->caret,
                    widget->edit_buffer + widget->caret + 1,
                    (size_t)(length - widget->caret));
                sxgui_textfield_scroll_to_caret(widget);
                sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                return 1;
            }
            return 0;
        }
        if (event->ascii >= 32 && event->ascii < 127 && length < widget->edit_capacity - 1)
        {
            memmove(
                widget->edit_buffer + widget->caret + 1,
                widget->edit_buffer + widget->caret,
                (size_t)(length - widget->caret + 1));
            widget->edit_buffer[widget->caret] = (char)event->ascii;
            widget->caret += 1;
            sxgui_textfield_scroll_to_caret(widget);
            sxgui_fire(widget, SXGUI_ACTION_CHANGE);
            return 1;
        }
        return 0;
    }

    if (widget->kind == SXGUI_TEXTEDIT && widget->edit_buffer != 0 && widget->edit_capacity > 0)
    {
        int line;
        int start;
        int line_length;
        int column;

        length = (int)strlen(widget->edit_buffer);
        if (widget->caret < 0)
        {
            widget->caret = 0;
        }
        if (widget->caret > length)
        {
            widget->caret = length;
        }
        /* Atajos de portapapeles. AltGr llega como Ctrl+Alt en varias
         * distribuciones, asi que se exige Ctrl SIN AltGr para no comerse una
         * tecla que el usuario quiso escribir. */
        if ((event->modifiers & SAVANXP_KEY_MOD_CTRL) != 0 &&
            (event->modifiers & SAVANXP_KEY_MOD_ALT_GR) == 0)
        {
            int done = 0;

            switch (event->ascii)
            {
            case 'c':
            case 'C':
                done = sxgui_sel_copy(widget);
                break;
            case 'x':
            case 'X':
                if (sxgui_sel_copy(widget) && sxgui_sel_delete(widget))
                {
                    sxgui_textedit_reveal_caret(widget);
                    sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                }
                done = 1;
                break;
            case 'v':
            case 'V':
                if (sxgui_sel_paste(widget, 0))
                {
                    sxgui_textedit_reveal_caret(widget);
                    sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                }
                done = 1;
                break;
            case 'a':
            case 'A':
                widget->sel_anchor = 0;
                widget->caret = length;
                sxgui_textedit_reveal_caret(widget);
                done = 1;
                break;
            default:
                break;
            }
            /* Cualquier Ctrl+imprimible se consume aunque no lo manejemos: sin
             * esto un Ctrl+B terminaria escribiendo una 'b' en el documento. */
            if (done || (event->ascii >= 32 && event->ascii < 127))
            {
                return 1;
            }
        }

        /* Editar con algo seleccionado lo reemplaza, que es lo que hace
         * cualquier editor. Borrar cambia el largo, asi que hay que recalcular
         * todo lo que se dedujo de el. */
        if (sxgui_sel_active(widget))
        {
            int destructive =
                event->key == SAVANXP_KEY_BACKSPACE ||
                event->key == SAVANXP_KEY_DELETE ||
                event->key == SAVANXP_KEY_ENTER ||
                (event->ascii >= 32 && event->ascii < 127);

            if (destructive)
            {
                sxgui_sel_delete(widget);
                length = (int)strlen(widget->edit_buffer);
                sxgui_textedit_reveal_caret(widget);
                sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                /* Backspace y Delete ya hicieron lo suyo: borrar la seleccion
                 * ES la operacion. Enter y los imprimibles siguen de largo para
                 * insertarse donde quedo el caret. */
                if (event->key == SAVANXP_KEY_BACKSPACE || event->key == SAVANXP_KEY_DELETE)
                {
                    return 1;
                }
            }
        }

        /* Shift + movimiento extiende; el movimiento pelado colapsa. Se decide
         * antes de mover porque el ancla tiene que quedar donde ESTABA el
         * caret. */
        switch (event->key)
        {
        case SAVANXP_KEY_LEFT:
        case SAVANXP_KEY_RIGHT:
        case SAVANXP_KEY_UP:
        case SAVANXP_KEY_DOWN:
        case SAVANXP_KEY_HOME:
        case SAVANXP_KEY_END:
        case SAVANXP_KEY_PAGE_UP:
        case SAVANXP_KEY_PAGE_DOWN:
            sxgui_sel_begin_move(widget, (event->modifiers & SAVANXP_KEY_MOD_SHIFT) != 0);
            break;
        default:
            /* Un ancla vieja sobre un buffer que se acaba de editar puede
             * pintar una seleccion fantasma. */
            sxgui_sel_clear(widget);
            break;
        }

        line = sxgui_textedit_caret_line(widget->edit_buffer, widget->caret);
        start = sxgui_textedit_line_start(widget->edit_buffer, line);
        line_length = sxgui_textedit_line_length(widget->edit_buffer, start);
        column = widget->caret - start;

        switch (event->key)
        {
        case SAVANXP_KEY_LEFT:
            if (widget->caret > 0)
            {
                widget->caret -= 1;
                sxgui_textedit_reveal_caret(widget);
                return 1;
            }
            return 0;
        case SAVANXP_KEY_RIGHT:
            if (widget->caret < length)
            {
                widget->caret += 1;
                sxgui_textedit_reveal_caret(widget);
                return 1;
            }
            return 0;
        case SAVANXP_KEY_UP:
        case SAVANXP_KEY_DOWN:
        {
            /* Subir y bajar conservan la columna hasta donde llegue la linea
             * destino, que es lo que hace cualquier editor. */
            int target = event->key == SAVANXP_KEY_UP ? line - 1 : line + 1;
            int target_start;
            int target_length;

            if (target < 0 || target > sxgui_textedit_line_count(widget->edit_buffer) - 1)
            {
                return 0;
            }
            target_start = sxgui_textedit_line_start(widget->edit_buffer, target);
            target_length = sxgui_textedit_line_length(widget->edit_buffer, target_start);
            widget->caret = target_start + (column < target_length ? column : target_length);
            sxgui_textedit_reveal_caret(widget);
            return 1;
        }
        case SAVANXP_KEY_HOME:
            widget->caret = start;
            sxgui_textedit_reveal_caret(widget);
            return 1;
        case SAVANXP_KEY_END:
            widget->caret = start + line_length;
            sxgui_textedit_reveal_caret(widget);
            return 1;
        case SAVANXP_KEY_PAGE_UP:
        case SAVANXP_KEY_PAGE_DOWN:
        {
            int step = sxgui_textedit_visible_rows(widget);
            int target = event->key == SAVANXP_KEY_PAGE_UP ? line - step : line + step;
            int target_start;
            int target_length;

            if (target < 0)
            {
                target = 0;
            }
            if (target > sxgui_textedit_line_count(widget->edit_buffer) - 1)
            {
                target = sxgui_textedit_line_count(widget->edit_buffer) - 1;
            }
            target_start = sxgui_textedit_line_start(widget->edit_buffer, target);
            target_length = sxgui_textedit_line_length(widget->edit_buffer, target_start);
            widget->caret = target_start + (column < target_length ? column : target_length);
            sxgui_textedit_reveal_caret(widget);
            return 1;
        }
        case SAVANXP_KEY_BACKSPACE:
            if (widget->caret > 0)
            {
                memmove(
                    widget->edit_buffer + widget->caret - 1,
                    widget->edit_buffer + widget->caret,
                    (size_t)(length - widget->caret + 1));
                widget->caret -= 1;
                widget->modified = 1;
                sxgui_textedit_reveal_caret(widget);
                sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                return 1;
            }
            return 0;
        case SAVANXP_KEY_DELETE:
            if (widget->caret < length)
            {
                memmove(
                    widget->edit_buffer + widget->caret,
                    widget->edit_buffer + widget->caret + 1,
                    (size_t)(length - widget->caret));
                widget->modified = 1;
                sxgui_textedit_reveal_caret(widget);
                sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                return 1;
            }
            return 0;
        default:
            break;
        }

        /* Enter parte la linea; el resto de los imprimibles se insertan. Ambos
         * son la misma operacion sobre el buffer. */
        {
            int inserted = 0;
            if (event->key == SAVANXP_KEY_ENTER)
            {
                inserted = '\n';
            }
            else if (event->ascii >= 32 && event->ascii < 127)
            {
                inserted = event->ascii;
            }
            if (inserted != 0 && length < widget->edit_capacity - 1)
            {
                memmove(
                    widget->edit_buffer + widget->caret + 1,
                    widget->edit_buffer + widget->caret,
                    (size_t)(length - widget->caret + 1));
                widget->edit_buffer[widget->caret] = (char)inserted;
                widget->caret += 1;
                widget->modified = 1;
                sxgui_textedit_reveal_caret(widget);
                sxgui_fire(widget, SXGUI_ACTION_CHANGE);
                return 1;
            }
            if (inserted != 0)
            {
                /* Buffer lleno: se consume igual, para no dejar que la tecla
                 * caiga al manejador global de la app (ESC-como-cerrar y demas). */
                return 1;
            }
        }
        return 0;
    }

    return 0;
}

/* ---- setup & constructors ----------------------------------------------- */

void sxgui_context_init(
    struct sxgui_context *ctx,
    uint32_t *pixels,
    const struct savanxp_fb_info *info,
    struct sxgui_widget *widgets,
    int widget_count)
{
    if (ctx == 0)
    {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    sx_bitmap_wrap(&ctx->target, pixels, info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&ctx->painter, &ctx->target);
    ctx->widgets = widgets;
    ctx->widget_count = widget_count;
    ctx->focus_index = -1;
    ctx->capture_index = -1;
    ctx->last_click_index = -1;
    ctx->popup_owner = -1;
}

void sxgui_context_retarget(struct sxgui_context *ctx, uint32_t *pixels, const struct savanxp_fb_info *info)
{
    if (ctx == 0)
    {
        return;
    }
    sx_bitmap_wrap(&ctx->target, pixels, info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&ctx->painter, &ctx->target);
}

static struct sxgui_widget sxgui_make(int kind, struct sx_rect rect, const char *text)
{
    struct sxgui_widget widget;
    memset(&widget, 0, sizeof(widget));
    widget.kind = kind;
    widget.rect = rect;
    widget.text = text;
    widget.flags = SXGUI_FLAG_VISIBLE;
    /* El memset dejaria sel_anchor en 0, que significaria "seleccionado desde
     * el principio del buffer" en vez de "sin seleccion". */
    widget.sel_anchor = -1;
    return widget;
}

struct sxgui_widget sxgui_label(struct sx_rect rect, const char *text)
{
    return sxgui_make(SXGUI_LABEL, rect, text);
}

struct sxgui_widget sxgui_button(struct sx_rect rect, const char *text, void (*on_action)(struct sxgui_widget *, void *), void *user)
{
    struct sxgui_widget widget = sxgui_make(SXGUI_BUTTON, rect, text);
    widget.on_action = on_action;
    widget.user = user;
    return widget;
}

struct sxgui_widget sxgui_checkbox(struct sx_rect rect, const char *text, int checked)
{
    struct sxgui_widget widget = sxgui_make(SXGUI_CHECKBOX, rect, text);
    widget.value = checked ? 1 : 0;
    return widget;
}

struct sxgui_widget sxgui_listbox(struct sx_rect rect, const char *const *items, int item_count)
{
    struct sxgui_widget widget = sxgui_make(SXGUI_LISTBOX, rect, 0);
    widget.items = items;
    widget.item_count = item_count;
    return widget;
}

struct sxgui_widget sxgui_textfield(struct sx_rect rect, char *edit_buffer, int edit_capacity)
{
    struct sxgui_widget widget = sxgui_make(SXGUI_TEXTFIELD, rect, 0);
    widget.edit_buffer = edit_buffer;
    widget.edit_capacity = edit_capacity;
    if (edit_buffer != 0)
    {
        widget.caret = (int)strlen(edit_buffer);
    }
    return widget;
}

struct sxgui_widget sxgui_scrollbar(struct sx_rect rect, int range_min, int range_max, int page, int value)
{
    struct sxgui_widget widget = sxgui_make(SXGUI_SCROLLBAR, rect, 0);
    widget.range_min = range_min;
    widget.range_max = range_max >= range_min ? range_max : range_min;
    widget.page = page > 0 ? page : 1;
    widget.value = sxgui_clamp_int(value, widget.range_min, widget.range_max);
    return widget;
}

struct sxgui_widget sxgui_groupbox(struct sx_rect rect, const char *text)
{
    return sxgui_make(SXGUI_GROUPBOX, rect, text);
}

struct sxgui_widget sxgui_radio(struct sx_rect rect, const char *text, int group, int checked)
{
    struct sxgui_widget widget = sxgui_make(SXGUI_RADIO, rect, text);
    widget.group = group;
    widget.value = checked ? 1 : 0;
    return widget;
}

struct sxgui_widget sxgui_combobox(struct sx_rect rect, const char *const *items, int item_count, int selected)
{
    struct sxgui_widget widget = sxgui_make(SXGUI_COMBOBOX, rect, 0);
    widget.items = items;
    widget.item_count = item_count;
    widget.value = sxgui_clamp_int(selected, 0, item_count > 0 ? item_count - 1 : 0);
    return widget;
}

struct sxgui_widget sxgui_textview(struct sx_rect rect, const char *const *lines, int line_count)
{
    struct sxgui_widget widget = sxgui_make(SXGUI_TEXTVIEW, rect, 0);
    widget.items = lines;
    widget.item_count = line_count;
    return widget;
}

void sxgui_focus(struct sxgui_context *ctx, int index)
{
    if (ctx == 0 || index < -1 || index >= ctx->widget_count)
    {
        return;
    }
    sxgui_set_focus(ctx, index);
}

int sxgui_textedit_has_selection(const struct sxgui_widget *widget)
{
    if (widget == 0 || widget->edit_buffer == 0 ||
        (widget->kind != SXGUI_TEXTEDIT && widget->kind != SXGUI_TEXTFIELD))
    {
        return 0;
    }
    return sxgui_sel_active(widget);
}

int sxgui_textedit_copy(struct sxgui_widget *widget)
{
    if (!sxgui_textedit_has_selection(widget))
    {
        return 0;
    }
    return sxgui_sel_copy(widget);
}

int sxgui_textedit_cut(struct sxgui_widget *widget)
{
    if (!sxgui_textedit_has_selection(widget))
    {
        return 0;
    }
    if (!sxgui_sel_copy(widget))
    {
        return 0;
    }
    return sxgui_sel_delete(widget);
}

int sxgui_textedit_paste(struct sxgui_widget *widget)
{
    if (widget == 0 || widget->edit_buffer == 0 ||
        (widget->kind != SXGUI_TEXTEDIT && widget->kind != SXGUI_TEXTFIELD))
    {
        return 0;
    }
    return sxgui_sel_paste(widget, widget->kind == SXGUI_TEXTFIELD);
}

void sxgui_textedit_select_all(struct sxgui_widget *widget)
{
    if (widget == 0 || widget->edit_buffer == 0 ||
        (widget->kind != SXGUI_TEXTEDIT && widget->kind != SXGUI_TEXTFIELD))
    {
        return;
    }
    widget->sel_anchor = 0;
    widget->caret = (int)strlen(widget->edit_buffer);
}

struct sxgui_widget sxgui_textedit(struct sx_rect rect, char *buffer, int capacity)
{
    struct sxgui_widget widget = sxgui_make(SXGUI_TEXTEDIT, rect, 0);
    widget.edit_buffer = buffer;
    widget.edit_capacity = capacity;
    return widget;
}

struct sxgui_widget sxgui_progress(struct sx_rect rect, int range_min, int range_max, int value)
{
    struct sxgui_widget widget = sxgui_make(SXGUI_PROGRESS, rect, 0);
    widget.range_min = range_min;
    widget.range_max = range_max >= range_min ? range_max : range_min;
    widget.value = sxgui_clamp_int(value, widget.range_min, widget.range_max);
    return widget;
}
