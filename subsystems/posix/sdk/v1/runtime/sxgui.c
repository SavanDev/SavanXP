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
           widget->kind == SXGUI_SCROLLBAR;
}

/* ---- painting ----------------------------------------------------------- */

static void sxgui_paint_label(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    uint32_t colour = sxgui_widget_enabled(widget) ? SXGUI_COLOR_TEXT : SXGUI_COLOR_DISABLED_TEXT;
    int text_y = widget->rect.y + (widget->rect.height - gfx_text_height()) / 2;
    if (widget->text != 0)
    {
        sx_painter_draw_text(painter, widget->rect.x, text_y, widget->text, colour);
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

/* ---- listbox scrolling helpers ------------------------------------------ */

static int sxgui_listbox_visible_rows(const struct sxgui_widget *widget)
{
    int rows = (widget->rect.height - 4) / sxgui_row_height();
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

/* text area inside the sunken border, minus the embedded scrollbar column */
static struct sx_rect sxgui_listbox_inner(const struct sxgui_widget *widget)
{
    struct sx_rect inner = sxgui_inset(widget->rect, 2);
    if (sxgui_listbox_has_scrollbar(widget))
    {
        inner.width -= SXGUI_SCROLLBAR_THICKNESS;
    }
    return inner;
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
            sx_painter_draw_text(painter, inner.x + 3, row_y + 2, label, text_colour);
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
    if (widget->focused)
    {
        int caret_x = text_x + sxgui_text_prefix_width(widget->edit_buffer, widget->caret);
        sxgui_vline(painter, caret_x, text_y, gfx_text_height(), SXGUI_COLOR_TEXT);
    }
    sx_painter_pop_clip(painter);
}

void sxgui_paint(struct sxgui_context *ctx)
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
        switch (widget->kind)
        {
        case SXGUI_LABEL:
            sxgui_paint_label(&ctx->painter, widget);
            break;
        case SXGUI_BUTTON:
            sxgui_paint_button(&ctx->painter, widget);
            break;
        case SXGUI_CHECKBOX:
            sxgui_paint_checkbox(&ctx->painter, widget);
            break;
        case SXGUI_LISTBOX:
            sxgui_paint_listbox(&ctx->painter, widget);
            break;
        case SXGUI_TEXTFIELD:
            sxgui_paint_textfield(&ctx->painter, widget);
            break;
        case SXGUI_SCROLLBAR:
            sxgui_paint_scrollbar(&ctx->painter, widget);
            break;
        default:
            break;
        }
    }
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

/* Pointer moves routed to the captured widget while the button is held. */
static int sxgui_capture_motion(struct sxgui_context *ctx, struct sxgui_widget *widget, const struct savanxp_gui_pointer_event *event)
{
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
    if (widget->kind == SXGUI_LISTBOX && ctx->capture_part == SXGUI_SCROLL_THUMB)
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

    for (index = 0; index < ctx->widget_count; ++index)
    {
        struct sxgui_widget *widget = &ctx->widgets[index];
        int over;

        if (!sxgui_widget_visible(widget))
        {
            continue;
        }
        over = sxgui_widget_enabled(widget) && sxgui_hit(widget, event->x, event->y);
        if (over != widget->hover)
        {
            widget->hover = over;
            changed = 1;
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
            case SXGUI_LISTBOX:
            {
                if (sxgui_listbox_has_scrollbar(widget) &&
                    sx_rect_contains_point(sxgui_listbox_scrollbar_rect(widget), event->x, event->y))
                {
                    struct sxgui_scroll_metrics metrics;
                    int grab_offset = 0;
                    int part;

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
                        changed = 1;
                    }
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
                    struct sx_rect inner = sxgui_inset(widget->rect, 2);
                    int local_x = event->x - (inner.x + 3) + widget->scroll;
                    int text_length = (int)strlen(widget->edit_buffer);
                    int position;

                    widget->caret = text_length;
                    for (position = 0; position <= text_length; ++position)
                    {
                        if (sxgui_text_prefix_width(widget->edit_buffer, position) >= local_x)
                        {
                            widget->caret = position;
                            break;
                        }
                    }
                    sxgui_textfield_scroll_to_caret(widget);
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
