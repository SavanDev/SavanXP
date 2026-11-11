#include "desktop.h"

#include "progman_registry.h"
#include "desktop_icons.h"
#include "shared/version.h"
#include "start_logo.h"

/*
 * The Start menu: a popup of the desktop, anchored at the bottom left above
 * the taskbar. It keeps progman's catalog between openings and rebuilds it only
 * when progman_registry_disk_signature() says the disk under it changed:
 * building it reads every binary in /bin, and doing that on each click is what
 * made the menu take a second to appear. A flat list by group (header plus items
 * with icon and name), and Shut Down.../Restart... at the bottom, which open
 * the Shut Down dialog (desktop_shutdown.c); no cascades and no keyboard --
 * mouse only. It closes when it launches a
 * program, when the Start button is clicked again, or on a press outside it
 * (windowd asks, the desktop closes it). What does not fit scrolls with the
 * wheel, or, as in Windows 98, by resting the pointer on the arrow strips the
 * list grows at its top and bottom when it overflows.
 *
 * The look is Windows NT 4.0's Start menu: a raised 2px frame, the vertical
 * banner black at the top and blue at the bottom with the system name and its
 * edition rotated along it (read bottom to top), 16x16 icons on 28px rows, and
 * an etched separator above Shut Down.
 */

#define STARTMENU_WIDTH 232
#define STARTMENU_HEIGHT 512
#define STARTMENU_MARGIN 2
#define STARTMENU_ROW_HEIGHT 28
#define STARTMENU_HEADER_HEIGHT 20
#define STARTMENU_ICON_SIZE 16
#define STARTMENU_ICON_GAP 6
#define STARTMENU_STRIP_WIDTH 24
#define STARTMENU_FOOTER_ROWS 2
/* Between the list and the footer: the etched separator (2px) and the gap
 * under it, before the first footer row. */
#define STARTMENU_FOOTER_GAP 5
#define STARTMENU_FOOTER_HEIGHT (2 + STARTMENU_FOOTER_GAP + STARTMENU_FOOTER_ROWS * STARTMENU_ROW_HEIGHT)
/* The banner caption is the system name in the caption face followed by the
 * edition in the UI face, rotated bottom to top, as in NT 4. */
#define STARTMENU_CAPTION_GAP 4
/* Off-screen strike the caption is drawn into before it is turned 90 degrees.
 * The caption face is 17px tall, the UI one 13; the strike leaves room for
 * both. */
#define STARTMENU_CAPTION_MAX_WIDTH 200
#define STARTMENU_CAPTION_HEIGHT 20
#define STARTMENU_MAX_ROWS (PROGMAN_MAX_GROUPS + PROGMAN_MAX_ITEMS)
/* The arrow strips of an overflowing list, and how fast resting on one
 * scrolls it: 8 pixels every 40 ms, a 28px row in about a tenth of a second. */
#define STARTMENU_SCROLL_STRIP_HEIGHT 12
#define STARTMENU_SCROLL_STEP 8
#define STARTMENU_SCROLL_INTERVAL_MS 40ul
#define STARTMENU_ARROW_NONE (-1)
#define STARTMENU_ARROW_UP 0
#define STARTMENU_ARROW_DOWN 1

#define STARTMENU_ROW_HEADER 0
#define STARTMENU_ROW_ITEM 1
#define STARTMENU_ROW_FOOTER 2

#define STARTMENU_ACTION_SHUTDOWN 0
#define STARTMENU_ACTION_REBOOT 1

struct startmenu_row
{
    int kind;
    int group;
    int item;
};

static struct startmenu_row g_rows[STARTMENU_MAX_ROWS];
static int g_row_count = 0;

/* The banner: the NT 4 gradient, black across the top and blue at the bottom.
 * The ramp is squared so the black holds most of the strip's height and the
 * blue appears only near the bottom, which is what the NT 4 banner looks like.
 * A light line closes it above the menu's bottom edge. */
#define STARTMENU_STRIP_BANDS 32
static void startmenu_strip(struct sx_painter *painter, int x, int y, int height)
{
    int band;

    for (band = 0; band < STARTMENU_STRIP_BANDS; ++band)
    {
        int y0 = y + (height * band) / STARTMENU_STRIP_BANDS;
        int y1 = y + (height * (band + 1)) / STARTMENU_STRIP_BANDS;
        int amount = (STARTMENU_STRIP_BANDS > 1) ? (band * 255) / (STARTMENU_STRIP_BANDS - 1) : 0;
        int ramp = (amount * amount) / 255;
        uint32_t colour = SXCHROME_RGB(
            (uint8_t)((16 * ramp + 127) / 255),
            (uint8_t)((132 * ramp + 127) / 255),
            (uint8_t)((208 * ramp + 127) / 255));

        if (y1 > y0)
        {
            sx_painter_fill_rect(painter, sx_rect_make(x, y0, STARTMENU_STRIP_WIDTH, y1 - y0), colour);
        }
    }
    sx_painter_fill_rect(painter, sx_rect_make(x, y + height - 2, STARTMENU_STRIP_WIDTH, 2), SXCHROME_COLOR_LIGHT);
}

/* The caption strike, painted once: white glyphs on black, taken later pixel
 * by pixel and turned a quarter turn counter-clockwise. */
static uint32_t g_caption_source[STARTMENU_CAPTION_MAX_WIDTH * STARTMENU_CAPTION_HEIGHT];
static int g_caption_width = 0;
/* The rows of the strike the glyphs really use: the banner centres that band,
 * not the font's line box, which is what puts both words on one centreline. */
static int g_caption_top = 0;
static int g_caption_bottom = -1;
static int g_caption_ready = 0;

/* First and last row with a pixel between columns [x0, x1). */
static void startmenu_measure_caption(int x0, int x1, int *top, int *bottom)
{
    int x;
    int y;

    *top = 0;
    *bottom = -1;
    for (y = 0; y < STARTMENU_CAPTION_HEIGHT; ++y)
    {
        for (x = x0; x < x1 && x < STARTMENU_CAPTION_MAX_WIDTH; ++x)
        {
            if (g_caption_source[y * STARTMENU_CAPTION_MAX_WIDTH + x] != 0)
            {
                if (*bottom < 0)
                {
                    *top = y;
                }
                *bottom = y;
                break;
            }
        }
    }
}

static void startmenu_build_caption(void)
{
    struct savanxp_fb_info info;
    struct sx_bitmap source;
    struct sx_painter painter;
    int title_width;
    int edition_width;
    int edition_x;
    int shift = 0;
    int title_top;
    int title_bottom;
    int edition_top;
    int edition_bottom;

    if (g_caption_ready)
    {
        return;
    }
    memset(g_caption_source, 0, sizeof(g_caption_source));
    memset(&info, 0, sizeof(info));
    info.width = STARTMENU_CAPTION_MAX_WIDTH;
    info.height = STARTMENU_CAPTION_HEIGHT;
    info.pitch = info.width * (uint32_t)sizeof(uint32_t);
    info.bpp = 32u;
    info.buffer_size = info.pitch * info.height;
    sx_bitmap_wrap(&source, g_caption_source, &info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &source);

    /* First pass, both parts at the top, only to measure each one's glyph
     * band: RetroSans and the UI face have different line boxes, so aligning
     * their boxes leaves the words off each other's centreline. */
    (void)sx_painter_set_font(&painter, SX_FONT_UI_TITLE);
    sx_painter_draw_text(&painter, 0, 0, SAVANXP_SYSTEM_NAME, SXCHROME_COLOR_LIGHT);
    title_width = sx_painter_text_width(&painter, SAVANXP_SYSTEM_NAME);
    (void)sx_painter_set_font(&painter, SX_FONT_UI);
    edition_x = title_width + STARTMENU_CAPTION_GAP;
    sx_painter_draw_text(&painter, edition_x, 0, SAVANXP_EDITION, SXCHROME_COLOR_LIGHT);
    edition_width = sx_painter_text_width(&painter, SAVANXP_EDITION);
    startmenu_measure_caption(0, title_width, &title_top, &title_bottom);
    startmenu_measure_caption(edition_x, edition_x + edition_width, &edition_top, &edition_bottom);

    if (title_bottom >= title_top && edition_bottom >= edition_top)
    {
        shift = ((title_top + title_bottom) - (edition_top + edition_bottom)) / 2;
        if (shift > 0 && edition_bottom + shift >= STARTMENU_CAPTION_HEIGHT)
        {
            shift = STARTMENU_CAPTION_HEIGHT - 1 - edition_bottom;
        }
        if (shift < 0 && edition_top + shift < 0)
        {
            shift = -edition_top;
        }
    }

    /* Second pass with the edition on the name's centreline. */
    memset(g_caption_source, 0, sizeof(g_caption_source));
    (void)sx_painter_set_font(&painter, SX_FONT_UI_TITLE);
    sx_painter_draw_text(&painter, 0, 0, SAVANXP_SYSTEM_NAME, SXCHROME_COLOR_LIGHT);
    (void)sx_painter_set_font(&painter, SX_FONT_UI);
    sx_painter_draw_text(&painter, edition_x, shift, SAVANXP_EDITION, SXCHROME_COLOR_LIGHT);

    g_caption_width = edition_x + edition_width;
    if (g_caption_width > STARTMENU_CAPTION_MAX_WIDTH)
    {
        g_caption_width = STARTMENU_CAPTION_MAX_WIDTH;
    }
    startmenu_measure_caption(0, g_caption_width, &g_caption_top, &g_caption_bottom);
    g_caption_ready = 1;
}

/* Draws the caption along the banner, starting at its bottom: a 90-degree
 * counter-clockwise turn, so it is read bottom to top with the glyph tops on
 * the left, as in NT 4. */
static void startmenu_paint_caption(struct sx_painter *painter, int strip_x, int strip_y, int strip_height)
{
    int column;
    int row;
    int band_width;
    int block_x;
    int block_y;

    startmenu_build_caption();
    if (g_caption_width == 0 || g_caption_bottom < g_caption_top)
    {
        return;
    }
    band_width = g_caption_bottom - g_caption_top + 1;
    block_x = strip_x + (STARTMENU_STRIP_WIDTH - band_width) / 2 - g_caption_top;
    if (block_x < strip_x)
    {
        block_x = strip_x;
    }
    block_y = strip_y + strip_height - 10 - g_caption_width;
    if (block_y < strip_y)
    {
        block_y = strip_y;
    }
    for (row = 0; row < g_caption_width; ++row)
    {
        int y = block_y + g_caption_width - 1 - row;

        if (y < strip_y || y >= strip_y + strip_height)
        {
            continue;
        }
        for (column = 0; column < STARTMENU_CAPTION_HEIGHT; ++column)
        {
            int x = block_x + column;

            if (x >= strip_x && x < strip_x + STARTMENU_STRIP_WIDTH &&
                g_caption_source[column * STARTMENU_CAPTION_MAX_WIDTH + row] != 0)
            {
                sx_painter_set_pixel(painter, x, y, SXCHROME_COLOR_CAPTION_TEXT);
            }
        }
    }
}

static int startmenu_row_height(const struct startmenu_row *row)
{
    return row != 0 && row->kind == STARTMENU_ROW_HEADER ? STARTMENU_HEADER_HEIGHT : STARTMENU_ROW_HEIGHT;
}

static int startmenu_content_height(void)
{
    int total = 0;
    int index;

    for (index = 0; index < g_row_count; ++index)
    {
        total += startmenu_row_height(&g_rows[index]);
    }
    return total;
}

/* Vista del contenido (lo que scrollea): marco menos margenes, franja y pie. */
static void startmenu_view(const struct savanxp_fb_info *info, int *x, int *y, int *height)
{
    if (x != 0)
    {
        *x = STARTMENU_MARGIN + STARTMENU_STRIP_WIDTH;
    }
    if (y != 0)
    {
        *y = STARTMENU_MARGIN;
    }
    if (height != 0)
    {
        *height = (int)info->height - (STARTMENU_MARGIN * 2) - STARTMENU_FOOTER_HEIGHT;
    }
}

/* The part of the view the rows scroll in. When the catalog does not fit, the
 * view gives up a strip at the top and one at the bottom for the arrows. */
static int startmenu_overflows(int view_height)
{
    return startmenu_content_height() > view_height;
}

static void startmenu_list(int view_y, int view_height, int *list_y, int *list_height)
{
    int strip = startmenu_overflows(view_height) ? STARTMENU_SCROLL_STRIP_HEIGHT : 0;

    *list_y = view_y + strip;
    *list_height = view_height - strip * 2;
}

static struct sx_rect startmenu_arrow_rect(int view_x, int view_y, int view_width, int view_height, int arrow)
{
    int y = arrow == STARTMENU_ARROW_UP ? view_y : view_y + view_height - STARTMENU_SCROLL_STRIP_HEIGHT;

    return sx_rect_make(view_x, y, view_width, STARTMENU_SCROLL_STRIP_HEIGHT);
}

/* The arrow strip under the point, or STARTMENU_ARROW_NONE (also when the list
 * fits and there are no strips). */
static int startmenu_arrow_hit(int x, int y, int view_x, int view_y, int view_width, int view_height)
{
    int arrow;

    if (!startmenu_overflows(view_height))
    {
        return STARTMENU_ARROW_NONE;
    }
    for (arrow = STARTMENU_ARROW_UP; arrow <= STARTMENU_ARROW_DOWN; ++arrow)
    {
        struct sx_rect rect = startmenu_arrow_rect(view_x, view_y, view_width, view_height, arrow);

        if (x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height)
        {
            return arrow;
        }
    }
    return STARTMENU_ARROW_NONE;
}

static void startmenu_draw_up_arrow(struct sx_painter *painter, struct sx_rect rect, uint32_t colour)
{
    sxchrome_draw_arrow(painter, rect, SXCHROME_ARROW_UP, colour);
}

static void startmenu_draw_down_arrow(struct sx_painter *painter, struct sx_rect rect, uint32_t colour)
{
    sxchrome_draw_arrow(painter, rect, SXCHROME_ARROW_DOWN, colour);
}

/* An arrow that can still scroll is black; one at its end of the list is
 * etched grey, the way Windows greys a menu arrow that has nothing left. */
static void startmenu_paint_arrow(struct sx_painter *painter, struct sx_rect rect, int arrow, int enabled)
{
    sxchrome_glyph_fn draw = arrow == STARTMENU_ARROW_UP ? startmenu_draw_up_arrow : startmenu_draw_down_arrow;

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

static struct sx_rect startmenu_footer_rect(int view_x, int view_bottom, int view_width, int action);

static int startmenu_footer_hit(int x, int y, int view_x, int view_bottom, int view_width)
{
    int action;

    if (x < view_x || x >= view_x + view_width)
    {
        return -1;
    }
    for (action = 0; action < STARTMENU_FOOTER_ROWS; ++action)
    {
        struct sx_rect rect = startmenu_footer_rect(view_x, view_bottom, view_width, action);

        if (y >= rect.y && y < rect.y + rect.height)
        {
            return action;
        }
    }
    return -1;
}
static int startmenu_row_top(int index)
{
    int top = 0;
    int cursor;

    for (cursor = 0; cursor < index && cursor < g_row_count; ++cursor)
    {
        top += startmenu_row_height(&g_rows[cursor]);
    }
    return top;
}

static int startmenu_max_scroll(int view_height)
{
    int overflow = startmenu_content_height() - view_height;

    return overflow > 0 ? overflow : 0;
}

/* Fila bajo el punto (coordenadas de superficie), o -1. La franja lateral
 * no es clickeable. */
static int startmenu_hit(int x, int y, int scroll, int view_x, int view_y, int view_height)
{
    int index;
    int relative = y - view_y + scroll;

    if (x < view_x || relative < 0)
    {
        return -1;
    }
    for (index = 0; index < g_row_count; ++index)
    {
        int height = startmenu_row_height(&g_rows[index]);

        if (relative < height)
        {
            return (y - view_y < view_height) ? index : -1;
        }
        relative -= height;
    }
    return -1;
}

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

/* The catalog the rows below describe, and the disk signature taken right
 * after building it. */
static int g_catalog_built = 0;
static uint32_t g_catalog_signature = 0;

/* The same stages, in the same order, as progman.c:build_catalog (see
 * progman_registry.h). Rebuilds only when the disk changed since last time. */
static void build_catalog(void)
{
    int group;
    int item;

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

    g_row_count = 0;
    for (group = 0; group < progman_group_count(); ++group)
    {
        const struct progman_group *info = progman_group_at(group);

        if (info == 0 || g_row_count >= STARTMENU_MAX_ROWS)
        {
            break;
        }
        g_rows[g_row_count].kind = STARTMENU_ROW_HEADER;
        g_rows[g_row_count].group = group;
        g_rows[g_row_count].item = -1;
        ++g_row_count;
        for (item = 0; item < info->item_count; ++item)
        {
            if (progman_group_item_at(group, item) == 0 || g_row_count >= STARTMENU_MAX_ROWS)
            {
                break;
            }
            g_rows[g_row_count].kind = STARTMENU_ROW_ITEM;
            g_rows[g_row_count].group = group;
            g_rows[g_row_count].item = item;
            ++g_row_count;
        }
    }
    g_catalog_signature = progman_registry_disk_signature();
    g_catalog_built = 1;
}

static void startmenu_draw_icon(struct sx_painter *painter, const struct desktop_embedded_bitmap *icon, int x, int y)
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
    if (icon->width == STARTMENU_ICON_SIZE && icon->height == STARTMENU_ICON_SIZE)
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
        sx_rect_make(x, y, STARTMENU_ICON_SIZE, STARTMENU_ICON_SIZE),
        sx_rect_make(0, 0, (int)icon->width, (int)icon->height),
        (icon->width > STARTMENU_ICON_SIZE || icon->height > STARTMENU_ICON_SIZE)
            ? SX_SCALE_BILINEAR
            : SX_SCALE_NEAREST);
}

/* A bitmap from start_logo.h (the Shut Down and Restart icons). */
static void startmenu_draw_asset(
    struct sx_painter *painter,
    const struct savanxp_embedded_bitmap_asset *asset,
    int x,
    int y)
{
    struct savanxp_fb_info info;
    struct sx_bitmap bitmap;

    if (asset == 0 || asset->pixels == 0 || asset->width == 0 || asset->height == 0)
    {
        return;
    }
    info.width = asset->width;
    info.height = asset->height;
    info.pitch = asset->width * 4u;
    info.bpp = 32;
    info.buffer_size = info.pitch * asset->height;
    sx_bitmap_wrap(&bitmap, (uint32_t *)asset->pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    sx_painter_blit_bitmap(painter, &bitmap, x, y);
}

static const char *k_footer_labels[STARTMENU_FOOTER_ROWS] = {"Shut Down...", "Restart..."};
static const struct savanxp_embedded_bitmap_asset *const k_footer_icons[STARTMENU_FOOTER_ROWS] = {
    &k_shutdown_icon,
    &k_restart_icon,
};

/* The rect of footer row `action` (0 = Shut Down, 1 = Restart). The footer
 * sits at the bottom, below the view, and does not scroll with the content:
 * first the etched line, then the gap, then the rows. */
static struct sx_rect startmenu_footer_rect(int view_x, int view_bottom, int view_width, int action)
{
    return sx_rect_make(
        view_x,
        view_bottom + 2 + STARTMENU_FOOTER_GAP + action * STARTMENU_ROW_HEIGHT,
        view_width,
        STARTMENU_ROW_HEIGHT);
}

static void startmenu_paint(struct savanxp_gfx_context *gfx, int hot_row, int hot_footer, int scroll)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame;
    struct sx_rect view;
    struct sx_rect list;
    int list_y;
    int list_height;
    int action;
    int index;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);

    frame = sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
    sx_painter_fill_rect(&painter, frame, SXGUI_COLOR_FACE);
    /* The NT 4 frame is two pixels of light and shadow, not the taskbar's
     * single line. */
    sxchrome_draw_raised(&painter, frame);
    startmenu_strip(
        &painter,
        frame.x + STARTMENU_MARGIN,
        frame.y + STARTMENU_MARGIN,
        frame.height - (STARTMENU_MARGIN * 2));
    startmenu_paint_caption(
        &painter,
        frame.x + STARTMENU_MARGIN,
        frame.y + STARTMENU_MARGIN,
        frame.height - (STARTMENU_MARGIN * 2));

    view = sx_rect_make(
        frame.x + STARTMENU_MARGIN + STARTMENU_STRIP_WIDTH,
        frame.y + STARTMENU_MARGIN,
        frame.width - (STARTMENU_MARGIN * 2) - STARTMENU_STRIP_WIDTH,
        frame.height - (STARTMENU_MARGIN * 2) - STARTMENU_FOOTER_HEIGHT);
    startmenu_list(view.y, view.height, &list_y, &list_height);
    list = sx_rect_make(view.x, list_y, view.width, list_height);
    if (startmenu_overflows(view.height))
    {
        startmenu_paint_arrow(
            &painter,
            startmenu_arrow_rect(view.x, view.y, view.width, view.height, STARTMENU_ARROW_UP),
            STARTMENU_ARROW_UP,
            scroll > 0);
        startmenu_paint_arrow(
            &painter,
            startmenu_arrow_rect(view.x, view.y, view.width, view.height, STARTMENU_ARROW_DOWN),
            STARTMENU_ARROW_DOWN,
            scroll < startmenu_max_scroll(list_height));
    }
    if (!sx_painter_push_clip(&painter, list))
    {
        return;
    }

    if (g_row_count == 0)
    {
        int text_y = view.y + (view.height - gfx_text_height()) / 2;

        sx_painter_draw_text(&painter, view.x + 4, text_y, "(no programs)", SXGUI_COLOR_DISABLED_TEXT);
        sx_painter_pop_clip(&painter);
    }
    else
    {
        for (index = 0; index < g_row_count; ++index)
        {
            const struct startmenu_row *row = &g_rows[index];
            int height = startmenu_row_height(row);
            int y = list.y + startmenu_row_top(index) - scroll;
            struct sx_rect rect = sx_rect_make(list.x, y, list.width, height);

            /* A row entirely outside the list is not drawn at all. */
            if (y + height <= list.y || y >= list.y + list.height)
            {
                continue;
            }
            if (row->kind == STARTMENU_ROW_HEADER)
            {
                const struct progman_group *group = progman_group_at(row->group);
                const char *name = (group != 0 && group->name[0] != '\0') ? group->name : "Programs";
                int text_y = y + (height - gfx_text_height()) / 2;

                sx_painter_fill_rect(&painter, rect, SXGUI_COLOR_FACE);
                sx_painter_draw_text(&painter, rect.x + 4, text_y, name, SXGUI_COLOR_DISABLED_TEXT);
                sx_painter_hline(&painter, rect.x, y + height - 1, rect.width, SXGUI_COLOR_SHADOW);
            }
            else
            {
                const struct progman_item *item = progman_group_item_at(row->group, row->item);
                const struct desktop_embedded_bitmap *icon = 0;
                int selected = (index == hot_row);
                uint32_t background = selected ? SXGUI_COLOR_SELECT : SXGUI_COLOR_FACE;
                uint32_t text_color = selected ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT;
                int text_x;
                int text_y;

                if (item == 0)
                {
                    continue;
                }
                sx_painter_fill_rect(&painter, rect, background);
                /* The 16x16 drawing first, the size this menu shows; the
                 * large one is reduced only when the binary has no small icon. */
                icon = progman_item_icon_small(item);
                if (icon == 0)
                {
                    icon = progman_item_icon(item);
                }
                if (icon == 0)
                {
                    icon = desktop_icon_small((enum desktop_icon_id)item->icon_id);
                }
                startmenu_draw_icon(
                    &painter,
                    icon,
                    rect.x + STARTMENU_ICON_GAP,
                    y + (height - STARTMENU_ICON_SIZE) / 2);
                text_x = rect.x + STARTMENU_ICON_GAP + STARTMENU_ICON_SIZE + STARTMENU_ICON_GAP;
                text_y = y + (height - gfx_text_height()) / 2;
                if (sx_painter_push_clip(&painter, sx_rect_make(text_x, y, rect.x + rect.width - 2 - text_x, height)))
                {
                    sx_painter_draw_text(&painter, text_x, text_y, item->name, text_color);
                    sx_painter_pop_clip(&painter);
                }
            }
        }
        sx_painter_pop_clip(&painter);
    }

    /* Fixed footer: the NT 4 etched separator and Shut Down... / Restart...
     * above the taskbar. */
    sx_painter_hline(&painter, view.x, view.y + view.height, view.width, SXGUI_COLOR_SHADOW);
    sx_painter_hline(&painter, view.x, view.y + view.height + 1, view.width, SXGUI_COLOR_LIGHT);
    for (action = 0; action < STARTMENU_FOOTER_ROWS; ++action)
    {
        struct sx_rect rect = startmenu_footer_rect(view.x, view.y + view.height, view.width, action);
        int selected = (action == hot_footer);
        uint32_t background = selected ? SXGUI_COLOR_SELECT : SXGUI_COLOR_FACE;
        uint32_t text_color = selected ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT;
        const struct savanxp_embedded_bitmap_asset *icon = k_footer_icons[action];
        const char *label = k_footer_labels[action];
        int text_x = rect.x + STARTMENU_ICON_GAP + STARTMENU_ICON_SIZE + STARTMENU_ICON_GAP;
        int text_y = rect.y + (rect.height - gfx_text_height()) / 2;

        sx_painter_fill_rect(&painter, rect, background);
        startmenu_draw_asset(
            &painter,
            icon,
            rect.x + STARTMENU_ICON_GAP,
            rect.y + (rect.height - (int)icon->height) / 2);
        sx_painter_draw_text(&painter, text_x, text_y, label, text_color);
    }
}

static int g_view_x = 0;
static int g_view_y = 0;
static int g_view_height = 0;
/* Where the rows scroll: the view minus the arrow strips, if any. */
static int g_list_y = 0;
static int g_list_height = 0;
static int g_scroll = 0;
/* The arrow strip the pointer rests on, and when it scrolls next. */
static int g_hot_arrow = STARTMENU_ARROW_NONE;
static unsigned long g_next_scroll_ms = 0;
/* The footer action chosen: its dialog opens once the menu has closed. */
static int g_pending_action = -1;
static int g_hot_row = -1;
static int g_hot_footer = -1;
static int g_pressed_row = -1;
static int g_pressed_footer = -1;
static uint32_t g_last_buttons = 0;

static int startmenu_open(void)
{
    struct savanxp_fb_info info;

    build_catalog();
    memset(&info, 0, sizeof(info));
    info.width = STARTMENU_WIDTH;
    info.height = STARTMENU_HEIGHT;
    startmenu_view(&info, &g_view_x, &g_view_y, &g_view_height);
    startmenu_list(g_view_y, g_view_height, &g_list_y, &g_list_height);
    g_scroll = 0;
    g_hot_arrow = STARTMENU_ARROW_NONE;
    g_next_scroll_ms = 0;
    g_pending_action = -1;
    g_hot_row = -1;
    g_hot_footer = -1;
    g_pressed_row = -1;
    g_pressed_footer = -1;
    g_last_buttons = 0;
    return 0;
}

static void startmenu_close(void)
{
    int action = g_pending_action;

    g_pending_action = -1;
    /* The menu's window is free by now, and the dialog takes it. */
    if (action == STARTMENU_ACTION_SHUTDOWN)
    {
        desktop_shutdown_dialog(DESKTOP_SHUTDOWN_POWER_OFF);
    }
    else if (action == STARTMENU_ACTION_REBOOT)
    {
        desktop_shutdown_dialog(DESKTOP_SHUTDOWN_RESTART);
    }
}

static uint32_t startmenu_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    uint32_t down = pointer->buttons & ~g_last_buttons;
    uint32_t up = g_last_buttons & ~pointer->buttons;
    int view_bottom = g_view_y + g_view_height;
    int view_width = (int)gfx->info.width - g_view_x - STARTMENU_MARGIN;
    int row = startmenu_hit(pointer->x, pointer->y, g_scroll, g_view_x, g_list_y, g_list_height);
    int footer = startmenu_footer_hit(pointer->x, pointer->y, g_view_x, view_bottom, view_width);
    int arrow = startmenu_arrow_hit(pointer->x, pointer->y, g_view_x, g_view_y, view_width, g_view_height);
    uint32_t actions = DESKTOP_ACTION_NONE;

    g_last_buttons = pointer->buttons;
    if (arrow != g_hot_arrow)
    {
        /* Arriving on a strip scrolls at once; startmenu_tick keeps going. */
        g_hot_arrow = arrow;
        g_next_scroll_ms = 0;
    }
    /* Only program rows can be pressed; headers only light up on hover. */
    if (row >= 0 && row < g_row_count && g_rows[row].kind != STARTMENU_ROW_ITEM)
    {
        row = -1;
    }
    if (pointer->wheel != 0)
    {
        /* Three rows per notch, like the editor. */
        g_scroll -= pointer->wheel * 3 * STARTMENU_ROW_HEIGHT;
        if (g_scroll < 0)
        {
            g_scroll = 0;
        }
        if (g_scroll > startmenu_max_scroll(g_list_height))
        {
            g_scroll = startmenu_max_scroll(g_list_height);
        }
        row = startmenu_hit(pointer->x, pointer->y, g_scroll, g_view_x, g_list_y, g_list_height);
        if (row >= 0 && row < g_row_count && g_rows[row].kind != STARTMENU_ROW_ITEM)
        {
            row = -1;
        }
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if (row != g_hot_row || footer != g_hot_footer)
    {
        g_hot_row = row;
        g_hot_footer = footer;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        g_pressed_row = row;
        g_pressed_footer = footer;
    }
    else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        if (footer >= 0 && footer == g_pressed_footer)
        {
            /* The question is the Shut Down dialog's, with this action
             * already chosen. */
            g_pending_action = footer;
            actions |= DESKTOP_ACTION_CLOSE;
        }
        else if (row >= 0 && row == g_pressed_row)
        {
            const struct progman_item *item = progman_group_item_at(g_rows[row].group, g_rows[row].item);

            if (item != 0 && desktop_launch(item->path, item->launch_flags) >= 0)
            {
                actions |= DESKTOP_ACTION_CLOSE;
            }
        }
        g_pressed_row = -1;
        g_pressed_footer = -1;
    }
    return actions;
}

/* Resting on an arrow strip scrolls without any pointer event arriving, so the
 * step is driven from the desktop loop. It stops at the end of the list. */
static uint32_t startmenu_tick(void)
{
    unsigned long now;
    int target;
    int limit;

    if (g_hot_arrow == STARTMENU_ARROW_NONE)
    {
        return DESKTOP_ACTION_NONE;
    }
    now = uptime_ms();
    if (now < g_next_scroll_ms)
    {
        return DESKTOP_ACTION_NONE;
    }
    limit = startmenu_max_scroll(g_list_height);
    target = g_scroll + (g_hot_arrow == STARTMENU_ARROW_UP ? -STARTMENU_SCROLL_STEP : STARTMENU_SCROLL_STEP);
    if (target < 0)
    {
        target = 0;
    }
    if (target > limit)
    {
        target = limit;
    }
    if (target == g_scroll)
    {
        return DESKTOP_ACTION_NONE;
    }
    g_scroll = target;
    g_next_scroll_ms = now + STARTMENU_SCROLL_INTERVAL_MS;
    return DESKTOP_ACTION_REPAINT;
}

/* The pointer went to the background or the taskbar: stop scrolling. */
static uint32_t startmenu_pointer_left(void)
{
    g_hot_arrow = STARTMENU_ARROW_NONE;
    return DESKTOP_ACTION_NONE;
}

static void startmenu_paint_window(struct savanxp_gfx_context *gfx)
{
    startmenu_paint(gfx, g_hot_row, g_hot_footer, g_scroll);
}

const struct desktop_popup_ops desktop_startmenu_ops = {
    STARTMENU_WIDTH,
    STARTMENU_HEIGHT,
    0,
    startmenu_open,
    startmenu_close,
    startmenu_pointer,
    startmenu_tick,
    startmenu_paint_window,
    0,
    startmenu_pointer_left,
    0,
};
