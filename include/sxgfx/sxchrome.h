#pragma once

/*
 * sxchrome - the system 3D edges and the disabled treatment, shared.
 *
 * WHY THIS IS NOT PART OF SXGUI-C: the widget toolkit is not the only thing
 * that paints Win9x chrome. `windowd` draws window frames and title bar
 * buttons, and an app that paints its own content (a board of cells, a meter,
 * a calculator display) draws boxes that have to match the toolkit's. Before
 * this module each of them kept a private copy of the same eight lines --
 * docs/userland/SYSTEM_LAYERING.md already states the rule this breaks ("SXGUI-C is the
 * canonical toolkit, and an app never reimplements a piece of it"), and
 * `sxgui_draw_raised_edge()` was the first patch on it.
 *
 * The toolkit could not simply be linked by `windowd`: the edges live in the
 * monolithic sxgui.c, so the window manager would have dragged menus, listbox
 * and textedit into its binary to get eight lines of bevel. So the edges come
 * DOWN here instead -- above SXGFX (it is the painter that draws them) and
 * below SXGUI-C (which now consumes them, like everyone else).
 *
 * It is small enough to sit in the base runtime of every userland binary,
 * next to gfx2d.c, so nothing has to opt in.
 */

#include "sxgfx/gfx2d.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXCHROME_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* Classic 3D system palette.
 *
 * These are the four tones of the "Windows Standard" scheme, read in this
 * order: DARK and SHADOW sink, BEVEL and LIGHT raise. A 3D border of the era
 * uses all FOUR -- two pixels per side, not one -- and BEVEL was the missing
 * one: without it the bevel is a single pixel and controls look flat. */
#define SXCHROME_COLOR_FACE          SXCHROME_RGB(192, 192, 192)  /* 3DFACE */
#define SXCHROME_COLOR_SHADOW        SXCHROME_RGB(128, 128, 128)  /* 3DSHADOW */
#define SXCHROME_COLOR_DARK          SXCHROME_RGB(0, 0, 0)        /* 3DDKSHADOW */
#define SXCHROME_COLOR_BEVEL         SXCHROME_RGB(223, 223, 223)  /* 3DLIGHT */
#define SXCHROME_COLOR_LIGHT         SXCHROME_RGB(255, 255, 255)  /* 3DHILIGHT */
#define SXCHROME_COLOR_TEXT          SXCHROME_RGB(0, 0, 0)
#define SXCHROME_COLOR_DISABLED_TEXT SXCHROME_RGB(128, 128, 128)
#define SXCHROME_COLOR_FIELD         SXCHROME_RGB(255, 255, 255)
#define SXCHROME_COLOR_SELECT        SXCHROME_RGB(0, 0, 128)
#define SXCHROME_COLOR_SELECT_TEXT   SXCHROME_RGB(255, 255, 255)
#define SXCHROME_COLOR_WINDOW        SXCHROME_RGB(192, 192, 192)

/* Windows Standard caption palette.  These are the base system colors: the
 * active title bar runs from navy to blue, while the inactive one runs from
 * dark to light gray.  The WM may blend an application's accent into the active
 * endpoints, but it never replaces this shared scheme. */
#define SXCHROME_COLOR_CAPTION_ACTIVE            SXCHROME_RGB(0, 0, 128)
#define SXCHROME_COLOR_CAPTION_ACTIVE_GRADIENT   SXCHROME_RGB(16, 132, 208)
#define SXCHROME_COLOR_CAPTION_INACTIVE          SXCHROME_RGB(128, 128, 128)
#define SXCHROME_COLOR_CAPTION_INACTIVE_GRADIENT SXCHROME_RGB(181, 181, 181)
#define SXCHROME_COLOR_CAPTION_TEXT              SXCHROME_RGB(255, 255, 255)
#define SXCHROME_COLOR_CAPTION_INACTIVE_TEXT     SXCHROME_RGB(192, 192, 192)

/* Thickness of each bevel. It enters the arithmetic of anything that divides
 * up a control's usable area: the interior is the rect minus this on each
 * side. */
#define SXCHROME_BORDER_RAISED 2 /* buttons, headers, popups */
#define SXCHROME_BORDER_SUNKEN 2 /* fields, lists, editors */
#define SXCHROME_BORDER_INSET  1 /* status bar panels */

/*
 * The primitive: TWO one-pixel rings, each with its own top-left and
 * bottom-right tone. Four colours in all. The outer ring separates the control
 * from the background and the inner one gives it thickness.
 *
 * All the rest of the chrome comes from here by changing the ORDER of the
 * four, which is what makes sunken and raised exactly the reverse of each
 * other rather than two similar drawings.
 *
 * It is exported with the four tones open, not only through the wrappers
 * below, because the WM composes surfaces that may need the tones in another
 * order. Sharing the primitive lets every surface of the system keep the same
 * two-pixel construction without copying it.
 *
 * It draws nothing for an empty rect; with a width or height <= 2 it draws
 * only the outer ring, since the inner one no longer fits.
 */
void sxchrome_draw_edge(
    struct sx_painter *painter,
    struct sx_rect rect,
    uint32_t outer_light,
    uint32_t outer_dark,
    uint32_t inner_light,
    uint32_t inner_dark);

/* Raised border (buttons, window face) in the system palette. */
void sxchrome_draw_raised(struct sx_painter *painter, struct sx_rect rect);

/* Its exact reverse, shared by the pressed button and the fields: that is
 * what gives a sunken button and a text box the same thickness. */
void sxchrome_draw_sunken(struct sx_painter *painter, struct sx_rect rect);

/* ONE-pixel bevel, for what has no thickness: status bar panels, meters and
 * boxes that only separate from the background. */
void sxchrome_draw_inset(struct sx_painter *painter, struct sx_rect rect);

/* "Etched" line: the dark and light frames offset one pixel diagonally. It is
 * the border of the group box and of menu separators. */
void sxchrome_draw_etched(struct sx_painter *painter, struct sx_rect rect);

/* Face + border of a control: fills with `face` and adds the raised or the
 * sunken bevel. It is the whole button minus its label. */
void sxchrome_fill_raised(struct sx_painter *painter, struct sx_rect rect, uint32_t face, int pressed);

/*
 * Label of a disabled control: grey with a white copy offset one pixel down
 * and to the right. It is the "etched" text of the era -- it reads as off
 * without disappearing, which is exactly what flat grey on grey fails to do.
 */
void sxchrome_draw_text_disabled(struct sx_painter *painter, int x, int y, const char *text);

/*
 * The same offset white copy, for a glyph that is not text: a combo's arrow, a
 * maximize square. The caller passes the function that draws it, and it is
 * called TWICE, first the highlight and then the grey.
 *
 * It exists because the etched relief is what tells a disabled control from a
 * badly painted one, and until then only text had it: any glyph that wanted to
 * look disabled had to reimplement it, which is the problem this file closes.
 */
typedef void (*sxchrome_glyph_fn)(struct sx_painter *painter, struct sx_rect rect, uint32_t colour);
void sxchrome_draw_glyph_disabled(struct sx_painter *painter, struct sx_rect rect, sxchrome_glyph_fn draw);

/* Which way sxchrome_draw_arrow points. */
enum sxchrome_arrow
{
    SXCHROME_ARROW_UP = 0,
    SXCHROME_ARROW_DOWN = 1,
    SXCHROME_ARROW_LEFT = 2,
    SXCHROME_ARROW_RIGHT = 3,
};

/*
 * The small solid triangle of the period: four rows, seven pixels at the
 * base, centred in `rect`. Scroll bar buttons, combo boxes and the scroll
 * strips of a menu that does not fit all draw this one, so they match.
 */
void sxchrome_draw_arrow(struct sx_painter *painter, struct sx_rect rect, enum sxchrome_arrow direction, uint32_t colour);

#ifdef __cplusplus
}
#endif
