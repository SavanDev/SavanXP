#pragma once

/*
 * sxgui - a minimal Win9x-flavoured widget toolkit on top of sxgfx.
 *
 * Retained but allocation-free: the application owns a flat array of
 * sxgui_widget and the toolkit paints them and dispatches input. Widgets draw
 * directly into the window backbuffer, confined with the painter clip stack.
 */

#include "savanxp/libc.h"
#include "savanxp/gfx2d.h"
#include "savanxp/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXGUI_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* Classic 3D system palette. */
#define SXGUI_COLOR_FACE          SXGUI_RGB(192, 192, 192)
#define SXGUI_COLOR_SHADOW        SXGUI_RGB(128, 128, 128)
#define SXGUI_COLOR_DARK          SXGUI_RGB(0, 0, 0)
#define SXGUI_COLOR_LIGHT         SXGUI_RGB(255, 255, 255)
#define SXGUI_COLOR_TEXT          SXGUI_RGB(0, 0, 0)
#define SXGUI_COLOR_DISABLED_TEXT SXGUI_RGB(128, 128, 128)
#define SXGUI_COLOR_FIELD         SXGUI_RGB(255, 255, 255)
#define SXGUI_COLOR_SELECT        SXGUI_RGB(0, 0, 128)
#define SXGUI_COLOR_SELECT_TEXT   SXGUI_RGB(255, 255, 255)
#define SXGUI_COLOR_WINDOW        SXGUI_RGB(192, 192, 192)

enum sxgui_kind {
    SXGUI_LABEL = 0,
    SXGUI_BUTTON,
    SXGUI_CHECKBOX,
    SXGUI_LISTBOX,
    SXGUI_TEXTFIELD
};

#define SXGUI_FLAG_VISIBLE  (1u << 0)
#define SXGUI_FLAG_DISABLED (1u << 1)

struct sxgui_widget {
    int kind;
    struct sx_rect rect;
    const char *text;            /* label / button caption */
    uint32_t flags;

    /* runtime interaction state, owned by the toolkit */
    int hover;
    int pressed;
    int focused;

    int value;                   /* checkbox: 0/1; listbox: selected row */

    /* listbox */
    const char *const *items;
    int item_count;

    /* textfield (caller-provided editable storage) */
    char *edit_buffer;
    int edit_capacity;
    int caret;

    void (*on_action)(struct sxgui_widget *widget, void *user);
    void *user;
};

struct sxgui_context {
    struct sx_bitmap target;
    struct sx_painter painter;
    struct sxgui_widget *widgets;
    int widget_count;
    int focus_index;
    uint32_t last_buttons;
    int pointer_x;
    int pointer_y;
};

/* Bind the toolkit to a window backbuffer and a widget array. */
void sxgui_context_init(
    struct sxgui_context *ctx,
    uint32_t *pixels,
    const struct savanxp_fb_info *info,
    struct sxgui_widget *widgets,
    int widget_count);

/* Feed compositor-routed input. Each returns non-zero when the UI changed and
 * a repaint is needed. */
int sxgui_handle_pointer(struct sxgui_context *ctx, const struct savanxp_gui_pointer_event *event);
int sxgui_handle_key(struct sxgui_context *ctx, const struct savanxp_input_event *event);

/* Paint every visible widget into the backbuffer. */
void sxgui_paint(struct sxgui_context *ctx);

/* Convenience widget constructors (fill an entry in the caller's array). */
struct sxgui_widget sxgui_label(struct sx_rect rect, const char *text);
struct sxgui_widget sxgui_button(struct sx_rect rect, const char *text, void (*on_action)(struct sxgui_widget *, void *), void *user);
struct sxgui_widget sxgui_checkbox(struct sx_rect rect, const char *text, int checked);
struct sxgui_widget sxgui_listbox(struct sx_rect rect, const char *const *items, int item_count);
struct sxgui_widget sxgui_textfield(struct sx_rect rect, char *edit_buffer, int edit_capacity);

#ifdef __cplusplus
}
#endif
