/* SXGUI_ICONVIEW with group headers (SXGUI_GROUP_HEADER): where the rows go,
 * what a click hits, where the keys move the selection, and that scrolling
 * keeps the selected item in view. An icon view without headers keeps its
 * plain grid. Keys and clicks go in through the toolkit's real entry points. */
#include <stdio.h>
#include <string.h>
#include "sxgfx/sxgui.h"
#include "sxgfx/pixels.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static struct sxgui_context g_ctx;
static uint32_t g_pixels[400 * 420];
static struct savanxp_fb_info g_info = {400, 420, 400 * 4, 32, sizeof(g_pixels)};

static void key(unsigned int code)
{
    struct savanxp_input_event event = {SAVANXP_INPUT_EVENT_KEY_DOWN, code, 0, 0};
    sxgui_handle_key(&g_ctx, &event);
}

static void click(int x, int y)
{
    struct savanxp_gui_pointer_event press = {x, y, 0, 1};
    struct savanxp_gui_pointer_event release = {x, y, 0, 0};
    sxgui_handle_pointer(&g_ctx, &press);
    sxgui_handle_pointer(&g_ctx, &release);
}

/* Four columns: the sunken bevel (2 + 2), the margin (2 + 2) and 4 cells. */
#define WIDTH (SXGUI_ICONVIEW_CELL_WIDTH * 4 + 8)

/* Two groups: A with five items (a row of four, then one), B with one.
 *   row 0: header A          (item 0)
 *   row 1: a0 a1 a2 a3       (items 1-4)
 *   row 2: a4                (item 5)
 *   row 3: header B          (item 6)
 *   row 4: b0                (item 7) */
static const char *const k_grouped[] = {
    SXGUI_GROUP_HEADER_TEXT "Group A", "a0", "a1", "a2", "a3", "a4", SXGUI_GROUP_HEADER_TEXT "Group B", "b0",
};

static void test_layout_and_hits(struct sxgui_widget *w)
{
    struct sx_rect header_a = sxgui_iconview_item_rect(w, 0);
    struct sx_rect a0 = sxgui_iconview_item_rect(w, 1);
    struct sx_rect a3 = sxgui_iconview_item_rect(w, 4);
    struct sx_rect a4 = sxgui_iconview_item_rect(w, 5);
    struct sx_rect header_b = sxgui_iconview_item_rect(w, 6);
    struct sx_rect b0 = sxgui_iconview_item_rect(w, 7);

    /* A header is a whole row, lower than a row of icons. */
    CHECK(header_a.width > a0.width * 3);
    CHECK(header_a.height > 0 && header_a.height < a0.height);
    CHECK(a0.y == header_a.y + header_a.height);
    CHECK(a3.y == a0.y && a3.x == a0.x + SXGUI_ICONVIEW_CELL_WIDTH * 3);
    /* The fifth item wraps; the next header starts a row of its own. */
    CHECK(a4.x == a0.x && a4.y == a0.y + a0.height);
    CHECK(header_b.y == a4.y + a4.height);
    CHECK(b0.x == a0.x && b0.y == header_b.y + header_b.height);

    /* A header is never hit; an item's icon is. */
    CHECK(sxgui_listbox_row_at(w, header_a.x + 20, header_a.y + header_a.height / 2) == -1);
    CHECK(sxgui_listbox_row_at(w, b0.x + b0.width / 2, b0.y + 4 + SXGUI_ICONVIEW_ICON_SIZE / 2) == 7);
    CHECK(sxgui_listbox_row_at(w, a3.x + a3.width / 2, a3.y + 4 + SXGUI_ICONVIEW_ICON_SIZE / 2) == 4);
    click(header_b.x + 20, header_b.y + header_b.height / 2);
    CHECK(w->value == -1);
    click(b0.x + b0.width / 2, b0.y + 4 + SXGUI_ICONVIEW_ICON_SIZE / 2);
    CHECK(w->value == 7);
}

static void test_keys(struct sxgui_widget *w)
{
    /* From nothing, the first item, past the header; End the last. */
    w->value = -1;
    key(SAVANXP_KEY_DOWN);
    CHECK(w->value == 1);
    w->value = -1;
    key(SAVANXP_KEY_END);
    CHECK(w->value == 7);

    w->value = 1;
    key(SAVANXP_KEY_LEFT);
    CHECK(w->value == 1); /* nothing before the first item */
    key(SAVANXP_KEY_UP);
    CHECK(w->value == 1); /* only a header above */
    key(SAVANXP_KEY_RIGHT);
    key(SAVANXP_KEY_RIGHT);
    key(SAVANXP_KEY_RIGHT);
    CHECK(w->value == 4);
    /* Down to a shorter row lands on its last item. */
    key(SAVANXP_KEY_DOWN);
    CHECK(w->value == 5);
    /* Down over the header row, into group B; Right off the end stays. */
    key(SAVANXP_KEY_DOWN);
    CHECK(w->value == 7);
    key(SAVANXP_KEY_RIGHT);
    CHECK(w->value == 7);
    key(SAVANXP_KEY_DOWN);
    CHECK(w->value == 7);
    key(SAVANXP_KEY_UP);
    CHECK(w->value == 5);
    key(SAVANXP_KEY_UP);
    CHECK(w->value == 1);
    /* Left and Right step over a header between groups. */
    w->value = 7;
    key(SAVANXP_KEY_LEFT);
    CHECK(w->value == 5);
    key(SAVANXP_KEY_RIGHT);
    CHECK(w->value == 7);
    key(SAVANXP_KEY_HOME);
    CHECK(w->value == 1);
    key(SAVANXP_KEY_PAGE_DOWN);
    CHECK(w->value == 7);
    key(SAVANXP_KEY_PAGE_UP);
    CHECK(w->value == 1);
}

/* Lower than the five rows: it scrolls, and the selection stays in view. */
static void test_scroll(struct sxgui_widget *w)
{
    struct sx_rect b0;

    w->rect = sx_rect_make(0, 0, WIDTH, 120);
    w->value = 1;
    w->scroll = 0;
    key(SAVANXP_KEY_END);
    CHECK(w->value == 7);
    CHECK(w->scroll > 0);
    b0 = sxgui_iconview_item_rect(w, 7);
    CHECK(b0.y >= 0 && b0.y + b0.height <= w->rect.height);
    /* Back to the first item: its header comes into view with it. */
    key(SAVANXP_KEY_HOME);
    CHECK(w->value == 1 && w->scroll == 0);
    sxgui_paint_content(&g_ctx);
}

/* Without headers the grid is the plain one: item 4 opens row 1. */
static void test_plain(void)
{
    static const char *const plain[] = {"p0", "p1", "p2", "p3", "p4"};
    struct sxgui_widget widgets[1];
    struct sx_rect p0;
    struct sx_rect p4;

    widgets[0] = sxgui_iconview(sx_rect_make(0, 0, WIDTH, 400), plain, 5);
    sxgui_context_init(&g_ctx, g_pixels, &g_info, widgets, 1);
    sxgui_focus(&g_ctx, 0);
    p0 = sxgui_iconview_item_rect(&widgets[0], 0);
    p4 = sxgui_iconview_item_rect(&widgets[0], 4);
    CHECK(p4.x == p0.x && p4.y == p0.y + p0.height);
    widgets[0].value = 0;
    key(SAVANXP_KEY_DOWN);
    CHECK(widgets[0].value == 4);
    sxgui_paint_content(&g_ctx);
}

int main(void)
{
    struct sxgui_widget widgets[1];

    widgets[0] = sxgui_iconview(sx_rect_make(0, 0, WIDTH, 400), k_grouped, 8);
    widgets[0].flags |= SXGUI_FLAG_EMPTY_SELECTION;
    sxgui_context_init(&g_ctx, g_pixels, &g_info, widgets, 1);
    sxgui_focus(&g_ctx, 0);
    sxgui_paint_content(&g_ctx);

    test_layout_and_hits(&widgets[0]);
    test_keys(&widgets[0]);
    test_scroll(&widgets[0]);
    test_plain();

    if (failures != 0)
    {
        printf("sxgui-iconview-test: %d failure(s)\n", failures);
        return 1;
    }
    printf("sxgui-iconview-test: ok\n");
    return 0;
}
