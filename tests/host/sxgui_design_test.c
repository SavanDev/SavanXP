/* SXGUI's design surface: what a form designer paints and asks. Widgets
 * painted with no context behind them land where the painter's origin puts
 * them; grab handles are found where they are drawn; dragging one moves the
 * right edges; the grid and snapping round as a designer expects. */
#include <stdio.h>
#include <string.h>
#include "sxgfx/sxgui.h"
#include "sxgfx/pixels.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

#define W 200
#define H 150
static uint32_t pixels[W * H];

static uint32_t at(int x, int y)
{
    return pixels[y * W + x] & 0xFFFFFFu;
}

int main(void)
{
    struct savanxp_fb_info info = {W, H, W * 4, 32, sizeof(pixels)};
    struct sx_bitmap target;
    struct sx_painter painter;
    struct sxgui_widget widgets[2];
    struct sx_rect rect = sx_rect_make(20, 30, 60, 40);
    struct sx_rect moved;
    int x, y, count;

    sx_bitmap_wrap(&target, pixels, &info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &target);

    /* A progress bar full of its highlight colour, painted at (10, 10) of a
     * form whose client starts at (50, 40): it lands at (60, 50). A hidden
     * widget is not painted at all. */
    memset(pixels, 0xFF, sizeof(pixels));
    widgets[0] = sxgui_progress(sx_rect_make(10, 10, 40, 16), 0, 10, 10);
    widgets[1] = sxgui_button(sx_rect_make(0, 0, 30, 20), "Hidden", 0, 0);
    widgets[1].flags &= ~SXGUI_FLAG_VISIBLE;
    CHECK(sx_painter_push_origin(&painter, 50, 40));
    sxgui_paint_widgets(&painter, widgets, 2);
    sx_painter_pop_origin(&painter);
    CHECK(at(60 + 20, 50 + 8) == (SXGUI_COLOR_SELECT & 0xFFFFFFu));
    CHECK(at(10 + 20, 10 + 8) == 0xFFFFFFu);
    CHECK(at(50 + 5, 40 + 5) == 0xFFFFFFu); /* where the hidden button would be */

    /* Handles: drawn at the corners and the middles, found there, not inside. */
    memset(pixels, 0xFF, sizeof(pixels));
    sxgui_paint_grab_handles(&painter, rect);
    CHECK(at(20, 30) == 0x000000u);          /* NW */
    CHECK(at(20 + 30, 30) == 0x000000u);     /* N */
    CHECK(at(79, 69) == 0x000000u);          /* SE */
    CHECK(at(50, 50) == 0xFFFFFFu);          /* the middle stays the form's */
    CHECK(sxgui_grab_at(rect, 20, 30) == SXGUI_GRAB_NW);
    CHECK(sxgui_grab_at(rect, 79, 69) == SXGUI_GRAB_SE);
    CHECK(sxgui_grab_at(rect, 50, 31) == SXGUI_GRAB_N);
    CHECK(sxgui_grab_at(rect, 80, 50) == SXGUI_GRAB_E);
    CHECK(sxgui_grab_at(rect, 50, 50) == SXGUI_GRAB_NONE);

    /* Resizing moves only the edges the handle holds, down to a minimum. */
    moved = sxgui_grab_resize(rect, SXGUI_GRAB_SE, 10, 5, 8);
    CHECK(moved.x == 20 && moved.y == 30 && moved.width == 70 && moved.height == 45);
    moved = sxgui_grab_resize(rect, SXGUI_GRAB_NW, 10, 5, 8);
    CHECK(moved.x == 30 && moved.y == 35 && moved.width == 50 && moved.height == 35);
    moved = sxgui_grab_resize(rect, SXGUI_GRAB_W, 100, 0, 8);
    CHECK(moved.x == 72 && moved.width == 8 && moved.height == 40);
    moved = sxgui_grab_resize(rect, SXGUI_GRAB_N, 0, -10, 8);
    CHECK(moved.y == 20 && moved.height == 50 && moved.width == 60);

    /* The grid: one dot per step from the area's corner, nothing between. */
    memset(pixels, 0xFF, sizeof(pixels));
    sxgui_paint_snap_grid(&painter, sx_rect_make(0, 0, 32, 32), 8);
    count = 0;
    for (y = 0; y < 40; ++y)
    {
        for (x = 0; x < 40; ++x)
        {
            if (at(x, y) == 0x000000u) ++count;
        }
    }
    CHECK(count == 16 && at(8, 16) == 0x000000u && at(9, 16) == 0xFFFFFFu);
    CHECK(sxgui_snap(11, 8) == 8 && sxgui_snap(12, 8) == 16 && sxgui_snap(-5, 8) == -8 && sxgui_snap(7, 0) == 7);

    if (failures) return 1;
    puts("SXGUI DESIGN TEST PASS");
    return 0;
}
