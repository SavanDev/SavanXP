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

static void sxgui_paint_textfield(struct sx_painter *painter, const struct sxgui_widget *widget)
{
    struct sx_rect inner = sxgui_inset(widget->rect, 2);
    int text_y = inner.y + (inner.height - gfx_text_height()) / 2;
    int text_x = inner.x + 3;

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
        int caret_x = text_x;
        if (widget->edit_buffer != 0)
        {
            caret_x += gfx_text_width(widget->edit_buffer);
        }
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

int sxgui_handle_key(struct sxgui_context *ctx, const struct savanxp_input_event *event)
{
    struct sxgui_widget *widget;
    int length;

    if (ctx == 0 || ctx->widgets == 0 || event == 0)
    {
        return 0;
    }
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return 0;
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
        length = (int)strlen(widget->edit_buffer);
        if (event->key == SAVANXP_KEY_BACKSPACE)
        {
            if (length > 0)
            {
                widget->edit_buffer[length - 1] = '\0';
                widget->caret = length - 1;
                return 1;
            }
            return 0;
        }
        if (event->ascii >= 32 && event->ascii < 127 && length < widget->edit_capacity - 1)
        {
            widget->edit_buffer[length] = (char)event->ascii;
            widget->edit_buffer[length + 1] = '\0';
            widget->caret = length + 1;
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
