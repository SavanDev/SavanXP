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
           widget->kind == SXGUI_LISTBOX || widget->kind == SXGUI_TEXTFIELD;
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

static void sxgui_paint_listbox(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    struct sx_rect inner = sxgui_inset(widget->rect, 2);
    int row_height = sxgui_row_height();
    int index;

    sx_painter_fill_rect(painter, widget->rect, SXGUI_COLOR_FIELD);
    sxgui_draw_sunken(painter, widget->rect);

    if (!sx_painter_push_clip(painter, inner))
    {
        return;
    }
    for (index = 0; index < widget->item_count; ++index)
    {
        int row_y = inner.y + index * row_height;
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

static void sxgui_fire(struct sxgui_widget *widget)
{
    if (widget->on_action != 0)
    {
        widget->on_action(widget, widget->user);
    }
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
                changed = 1;
                break;
            case SXGUI_CHECKBOX:
                widget->value = widget->value ? 0 : 1;
                sxgui_fire(widget);
                changed = 1;
                break;
            case SXGUI_LISTBOX:
            {
                struct sx_rect inner = sxgui_inset(widget->rect, 2);
                int row = (event->y - inner.y) / sxgui_row_height();
                if (row >= 0 && row < widget->item_count && row != widget->value)
                {
                    widget->value = row;
                    sxgui_fire(widget);
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
                    sxgui_fire(widget);
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
            }
            sxgui_fire(widget);
            return 1;
        }
        return 0;
    }

    if (widget->kind == SXGUI_LISTBOX)
    {
        if (event->key == SAVANXP_KEY_UP && widget->value > 0)
        {
            widget->value -= 1;
            sxgui_fire(widget);
            return 1;
        }
        if (event->key == SAVANXP_KEY_DOWN && widget->value + 1 < widget->item_count)
        {
            widget->value += 1;
            sxgui_fire(widget);
            return 1;
        }
        return 0;
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
