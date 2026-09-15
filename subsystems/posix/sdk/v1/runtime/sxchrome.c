#include "savanxp/sxchrome.h"

/* Las lineas de 1px las pone el painter (sx_painter_hline/vline): el chrome ya
 * no las arma con rects de altura 1. */

void sxchrome_draw_edge(
    struct sx_painter *painter,
    struct sx_rect rect,
    uint32_t outer_light,
    uint32_t outer_dark,
    uint32_t inner_light,
    uint32_t inner_dark)
{
    int right = rect.x + rect.width - 1;
    int bottom = rect.y + rect.height - 1;

    if (rect.width <= 0 || rect.height <= 0)
    {
        return;
    }

    /* anillo externo: la L clara arriba-izquierda y la oscura abajo-derecha */
    sx_painter_hline(painter, rect.x, rect.y, rect.width, outer_light);
    sx_painter_vline(painter, rect.x, rect.y, rect.height, outer_light);
    sx_painter_hline(painter, rect.x, bottom, rect.width, outer_dark);
    sx_painter_vline(painter, right, rect.y, rect.height, outer_dark);

    if (rect.width <= 2 || rect.height <= 2)
    {
        return;
    }

    /* anillo interno, corrido un pixel hacia adentro por los cuatro lados */
    sx_painter_hline(painter, rect.x + 1, rect.y + 1, rect.width - 2, inner_light);
    sx_painter_vline(painter, rect.x + 1, rect.y + 1, rect.height - 2, inner_light);
    sx_painter_hline(painter, rect.x + 1, bottom - 1, rect.width - 2, inner_dark);
    sx_painter_vline(painter, right - 1, rect.y + 1, rect.height - 2, inner_dark);
}

void sxchrome_draw_raised(struct sx_painter *painter, struct sx_rect rect)
{
    sxchrome_draw_edge(
        painter, rect,
        SXCHROME_COLOR_BEVEL, SXCHROME_COLOR_DARK,
        SXCHROME_COLOR_LIGHT, SXCHROME_COLOR_SHADOW);
}

void sxchrome_draw_sunken(struct sx_painter *painter, struct sx_rect rect)
{
    sxchrome_draw_edge(
        painter, rect,
        SXCHROME_COLOR_SHADOW, SXCHROME_COLOR_LIGHT,
        SXCHROME_COLOR_DARK, SXCHROME_COLOR_BEVEL);
}

void sxchrome_draw_inset(struct sx_painter *painter, struct sx_rect rect)
{
    int right = rect.x + rect.width - 1;
    int bottom = rect.y + rect.height - 1;

    if (rect.width <= 0 || rect.height <= 0)
    {
        return;
    }

    sx_painter_hline(painter, rect.x, rect.y, rect.width, SXCHROME_COLOR_SHADOW);
    sx_painter_vline(painter, rect.x, rect.y, rect.height, SXCHROME_COLOR_SHADOW);
    sx_painter_hline(painter, rect.x, bottom, rect.width, SXCHROME_COLOR_LIGHT);
    sx_painter_vline(painter, right, rect.y, rect.height, SXCHROME_COLOR_LIGHT);
}

void sxchrome_draw_etched(struct sx_painter *painter, struct sx_rect rect)
{
    if (rect.width <= 1 || rect.height <= 1)
    {
        return;
    }
    sx_painter_draw_frame(
        painter,
        sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 1, rect.height - 1),
        SXCHROME_COLOR_LIGHT);
    sx_painter_draw_frame(
        painter,
        sx_rect_make(rect.x, rect.y, rect.width - 1, rect.height - 1),
        SXCHROME_COLOR_SHADOW);
}

void sxchrome_fill_raised(struct sx_painter *painter, struct sx_rect rect, uint32_t face, int pressed)
{
    sx_painter_fill_rect(painter, rect, face);
    if (pressed)
    {
        sxchrome_draw_sunken(painter, rect);
        return;
    }
    sxchrome_draw_raised(painter, rect);
}

void sxchrome_draw_text_disabled(struct sx_painter *painter, int x, int y, const char *text)
{
    if (text == 0)
    {
        return;
    }
    sx_painter_draw_text(painter, x + 1, y + 1, text, SXCHROME_COLOR_LIGHT);
    sx_painter_draw_text(painter, x, y, text, SXCHROME_COLOR_DISABLED_TEXT);
}

void sxchrome_draw_glyph_disabled(struct sx_painter *painter, struct sx_rect rect, sxchrome_glyph_fn draw)
{
    if (draw == 0)
    {
        return;
    }
    draw(painter, sx_rect_translate(rect, 1, 1), SXCHROME_COLOR_LIGHT);
    draw(painter, rect, SXCHROME_COLOR_DISABLED_TEXT);
}
