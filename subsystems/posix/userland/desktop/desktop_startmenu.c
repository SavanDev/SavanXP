#include "desktop.h"

#include "shared/version.h"
#include "shortcut.h"
#include "start_logo.h"
#include "start_shortcuts.h"

/*
 * The Start menu: a popup of the desktop, anchored at the bottom left above
 * the taskbar (docs/desktop/START_MENU.md). At the top, above an etched
 * separator, the Start menu's own shortcuts when there are any
 * (start_shortcuts.h); then two cascading entries, Programs and Settings, and
 * below an etched separator Shut Down... and Restart..., which open the Shut
 * Down dialog (desktop_shutdown.c). Mouse only: it has no keyboard. It closes
 * when a shortcut is opened, when a program is launched from its cascade,
 * when the Start button is clicked again, or on a press outside it (windowd
 * asks, the desktop closes it).
 *
 * The cascade (desktop_submenu.c) opens to the right of an entry when the
 * pointer rests on it for STARTMENU_CASCADE_DELAY_MS, or at once when it is
 * clicked; resting on the other entry, or on the footer, switches or closes it
 * after the same delay, so a pointer crossing the menu on its way into the
 * cascade does not swap it. While the pointer is in the cascade its entry
 * stays lit. Settings is shown only when progman's catalog has a Settings
 * group.
 *
 * The look is Windows NT 4.0's Start menu: a raised 2px frame, the vertical
 * banner black at the top and blue at the bottom with the system name and its
 * edition rotated along it (read bottom to top), 16x16 icons on 28px rows,
 * and an etched separator above Shut Down. The edition is a switch
 * (desktop_startmenu_set_show_edition): on by default, and shown only while
 * the menu is tall enough for both; with two entries it is not, and the
 * banner shows the name alone, as Windows 95 shows "Windows95".
 */

#define STARTMENU_WIDTH 176
#define STARTMENU_MARGIN 2
#define STARTMENU_ROW_HEIGHT 28
#define STARTMENU_ICON_SIZE 16
#define STARTMENU_ICON_GAP 6
#define STARTMENU_STRIP_WIDTH 24
#define STARTMENU_FOOTER_ROWS 2
/* Between the entries and the footer: the etched separator (2px) and the gap
 * under it, before the first footer row. */
#define STARTMENU_FOOTER_GAP 5
#define STARTMENU_FOOTER_HEIGHT (2 + STARTMENU_FOOTER_GAP + STARTMENU_FOOTER_ROWS * STARTMENU_ROW_HEIGHT)
/* Between the shortcuts and the entries: the same etched line and gap. */
#define STARTMENU_SHORTCUTS_GAP (2 + STARTMENU_FOOTER_GAP)
/* The banner caption is the system name in the caption face followed by the
 * edition in the UI face, rotated bottom to top, as in NT 4. */
#define STARTMENU_CAPTION_GAP 4
/* Space under the caption, above the banner's bottom edge. */
#define STARTMENU_CAPTION_BOTTOM_GAP 10
/* Off-screen strike the caption is drawn into before it is turned 90 degrees.
 * The caption face is 17px tall, the UI one 13; the strike leaves room for
 * both. */
#define STARTMENU_CAPTION_MAX_WIDTH 200
#define STARTMENU_CAPTION_HEIGHT 20

#define STARTMENU_MAX_ENTRIES 2
/* How long the pointer rests on an entry before its cascade opens, or before
 * another one replaces it: Windows' default menu show delay. */
#define STARTMENU_CASCADE_DELAY_MS 400ul
/* The cascade's frame overlaps the menu's by this much, as in Windows, and
 * its first row lines up with the entry that opened it. */
#define STARTMENU_CASCADE_OVERLAP 3

#define STARTMENU_ACTION_SHUTDOWN 0
#define STARTMENU_ACTION_REBOOT 1

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
/* Where the system name ends in the strike: a menu too short for the name
 * and the edition shows the name alone, as Windows 95 shows "Windows95". */
static int g_caption_title_width = 0;
/* Whether the edition follows the name, when it fits. */
static int g_show_edition = 1;

void desktop_startmenu_set_show_edition(int show)
{
    g_show_edition = show != 0;
}
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

    g_caption_title_width = title_width < STARTMENU_CAPTION_MAX_WIDTH ? title_width : STARTMENU_CAPTION_MAX_WIDTH;
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
    int width;

    startmenu_build_caption();
    if (g_caption_width == 0 || g_caption_bottom < g_caption_top)
    {
        return;
    }
    width = g_caption_width;
    if (!g_show_edition || width + STARTMENU_CAPTION_BOTTOM_GAP > strip_height)
    {
        width = g_caption_title_width;
    }
    band_width = g_caption_bottom - g_caption_top + 1;
    block_x = strip_x + (STARTMENU_STRIP_WIDTH - band_width) / 2 - g_caption_top;
    if (block_x < strip_x)
    {
        block_x = strip_x;
    }
    block_y = strip_y + strip_height - STARTMENU_CAPTION_BOTTOM_GAP - width;
    if (block_y < strip_y)
    {
        block_y = strip_y;
    }
    for (row = 0; row < width; ++row)
    {
        int y = block_y + width - 1 - row;

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

/* A bitmap from start_logo.h: the entries' and the footer's icons. */
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

/* The cascading entries shown this time, each a desktop_submenu source. */
static int g_entries[STARTMENU_MAX_ENTRIES];
static int g_entry_count = 0;
static int g_height = 0;

static int g_hot_entry = -1;
static int g_hot_footer = -1;
static int g_pressed_footer = -1;
static uint32_t g_last_buttons = 0;
/* The entry whose cascade is open, and the one it becomes once
 * g_cascade_due_ms passes (-1 = closed). They differ only while the pointer
 * rests somewhere new. */
static int g_open_cascade = -1;
static int g_pending_cascade = -1;
static unsigned long g_cascade_due_ms = 0;
/* The footer action chosen: its dialog opens once the menu has closed. */
static int g_pending_action = -1;
/* The shortcuts shown this time, the one under the pointer, the one pressed,
 * and the one chosen: it opens once the menu has closed. */
static int g_shortcut_count = 0;
static int g_hot_shortcut = -1;
static int g_pressed_shortcut = -1;
static int g_pending_shortcut = -1;
static char g_pending_shortcut_path[SHORTCUT_PATH_CAPACITY];

static const char *entry_label(int entry)
{
    return g_entries[entry] == DESKTOP_SUBMENU_SETTINGS ? "Settings" : "Programs";
}

static const struct savanxp_embedded_bitmap_asset *entry_icon(int entry)
{
    return g_entries[entry] == DESKTOP_SUBMENU_SETTINGS ? &k_settings_icon : &k_programs_icon;
}

/* Shortcuts start under the top margin; the entries follow them and their
 * separator, and the footer follows the last entry. */
static struct sx_rect shortcut_rect(int index)
{
    return sx_rect_make(
        STARTMENU_MARGIN + STARTMENU_STRIP_WIDTH,
        STARTMENU_MARGIN + index * STARTMENU_ROW_HEIGHT,
        STARTMENU_WIDTH - STARTMENU_MARGIN * 2 - STARTMENU_STRIP_WIDTH,
        STARTMENU_ROW_HEIGHT);
}

static int entries_top(void)
{
    return STARTMENU_MARGIN + g_shortcut_count * STARTMENU_ROW_HEIGHT +
           (g_shortcut_count > 0 ? STARTMENU_SHORTCUTS_GAP : 0);
}

static struct sx_rect entry_rect(int entry)
{
    return sx_rect_make(
        STARTMENU_MARGIN + STARTMENU_STRIP_WIDTH,
        entries_top() + entry * STARTMENU_ROW_HEIGHT,
        STARTMENU_WIDTH - STARTMENU_MARGIN * 2 - STARTMENU_STRIP_WIDTH,
        STARTMENU_ROW_HEIGHT);
}

static int entries_bottom(void)
{
    return entries_top() + g_entry_count * STARTMENU_ROW_HEIGHT;
}

static int shortcut_hit(int x, int y)
{
    for (int index = 0; index < g_shortcut_count; ++index)
    {
        if (sx_rect_contains_point(shortcut_rect(index), x, y))
        {
            return index;
        }
    }
    return -1;
}

/* The rect of footer row `action` (0 = Shut Down, 1 = Restart): first the
 * etched line, then the gap, then the rows. */
static struct sx_rect footer_rect(int action)
{
    return sx_rect_make(
        STARTMENU_MARGIN + STARTMENU_STRIP_WIDTH,
        entries_bottom() + 2 + STARTMENU_FOOTER_GAP + action * STARTMENU_ROW_HEIGHT,
        STARTMENU_WIDTH - STARTMENU_MARGIN * 2 - STARTMENU_STRIP_WIDTH,
        STARTMENU_ROW_HEIGHT);
}

static int entry_hit(int x, int y)
{
    int entry;

    for (entry = 0; entry < g_entry_count; ++entry)
    {
        if (sx_rect_contains_point(entry_rect(entry), x, y))
        {
            return entry;
        }
    }
    return -1;
}

static int footer_hit(int x, int y)
{
    int action;

    for (action = 0; action < STARTMENU_FOOTER_ROWS; ++action)
    {
        if (sx_rect_contains_point(footer_rect(action), x, y))
        {
            return action;
        }
    }
    return -1;
}

/* Which rows there are: the shortcuts whose targets exist, Programs always,
 * Settings when the catalog has a Settings group. Sized to them, so the
 * measure decides the height. */
static void startmenu_measure(int *width, int *height)
{
    desktop_submenu_refresh_catalog();
    g_shortcut_count = start_shortcuts_load();
    g_entry_count = 0;
    g_entries[g_entry_count++] = DESKTOP_SUBMENU_PROGRAMS;
    if (!desktop_submenu_is_empty(DESKTOP_SUBMENU_SETTINGS))
    {
        g_entries[g_entry_count++] = DESKTOP_SUBMENU_SETTINGS;
    }
    g_height = STARTMENU_MARGIN + entries_bottom() + STARTMENU_FOOTER_HEIGHT;
    *width = STARTMENU_WIDTH;
    *height = g_height;
}

static void paint_row(
    struct sx_painter *painter,
    struct sx_rect rect,
    const struct savanxp_embedded_bitmap_asset *icon,
    const char *label,
    int selected,
    int cascade)
{
    uint32_t text_color = selected ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT;
    int text_x = rect.x + STARTMENU_ICON_GAP + STARTMENU_ICON_SIZE + STARTMENU_ICON_GAP;
    int text_y = rect.y + (rect.height - gfx_text_height()) / 2;

    sx_painter_fill_rect(painter, rect, selected ? SXGUI_COLOR_SELECT : SXGUI_COLOR_FACE);
    startmenu_draw_asset(painter, icon, rect.x + STARTMENU_ICON_GAP, rect.y + (rect.height - (int)icon->height) / 2);
    sx_painter_draw_text(painter, text_x, text_y, label, text_color);
    if (cascade)
    {
        /* The small solid triangle every cascading menu item carries. */
        sxchrome_draw_arrow(painter, sx_rect_make(rect.x + rect.width - 14, rect.y, 8, rect.height), SXCHROME_ARROW_RIGHT, text_color);
    }
}

static void startmenu_paint(struct savanxp_gfx_context *gfx)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame;
    int entry;
    int action;
    int separator_y = entries_bottom();
    int separator_x = STARTMENU_MARGIN + STARTMENU_STRIP_WIDTH;
    int separator_width = STARTMENU_WIDTH - STARTMENU_MARGIN * 2 - STARTMENU_STRIP_WIDTH;

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

    for (int index = 0; index < g_shortcut_count; ++index)
    {
        const struct start_shortcut *shortcut = start_shortcuts_at(index);
        struct savanxp_embedded_bitmap_asset icon = {
            START_SHORTCUTS_ICON_EXTENT, START_SHORTCUTS_ICON_EXTENT, shortcut->icon};

        paint_row(&painter, shortcut_rect(index), shortcut->has_icon ? &icon : &k_programs_icon, shortcut->name,
                  index == g_hot_shortcut, 0);
    }
    if (g_shortcut_count > 0)
    {
        int line_y = STARTMENU_MARGIN + g_shortcut_count * STARTMENU_ROW_HEIGHT + STARTMENU_FOOTER_GAP / 2;

        sx_painter_hline(&painter, separator_x, line_y, separator_width, SXGUI_COLOR_SHADOW);
        sx_painter_hline(&painter, separator_x, line_y + 1, separator_width, SXGUI_COLOR_LIGHT);
    }

    for (entry = 0; entry < g_entry_count; ++entry)
    {
        /* Lit under the pointer, and while its cascade is open and the
         * pointer is not on another row of this menu. */
        int selected = entry == g_hot_entry ||
                       (g_hot_entry < 0 && g_hot_footer < 0 && g_hot_shortcut < 0 && entry == g_open_cascade);

        paint_row(&painter, entry_rect(entry), entry_icon(entry), entry_label(entry), selected, 1);
    }

    sx_painter_hline(&painter, separator_x, separator_y, separator_width, SXGUI_COLOR_SHADOW);
    sx_painter_hline(&painter, separator_x, separator_y + 1, separator_width, SXGUI_COLOR_LIGHT);
    for (action = 0; action < STARTMENU_FOOTER_ROWS; ++action)
    {
        paint_row(&painter, footer_rect(action), k_footer_icons[action], k_footer_labels[action], action == g_hot_footer, 0);
    }
}

/* Opens the cascade of `entry` beside it, or closes the open one (-1). */
static void startmenu_apply_cascade(int entry)
{
    if (entry < 0)
    {
        desktop_close_submenu();
    }
    else
    {
        const struct savanxp_gfx_context *main_window = desktop_main_window();
        int work_height = (int)main_window->info.height - DESKTOP_TASKBAR_HEIGHT;
        /* Where the menu is: open_slot_at anchors it at x 2, on the taskbar. */
        int menu_x = 2;
        int menu_y = work_height - g_height;
        struct sx_rect rect = entry_rect(entry);

        desktop_submenu_select(g_entries[entry]);
        desktop_open_submenu(
            menu_x + STARTMENU_WIDTH - STARTMENU_CASCADE_OVERLAP,
            menu_y + rect.y - STARTMENU_MARGIN);
        g_open_cascade = desktop_popup_is_open(DESKTOP_POPUP_SUBMENU) ? entry : -1;
    }
    g_pending_cascade = g_open_cascade;
}

/* The pointer rests somewhere that wants `entry`'s cascade (-1: none): it
 * happens once the delay passes, unless the pointer moves on first. */
static void startmenu_want_cascade(int entry)
{
    if (entry == g_pending_cascade)
    {
        return;
    }
    g_pending_cascade = entry;
    g_cascade_due_ms = uptime_ms() + STARTMENU_CASCADE_DELAY_MS;
}

void desktop_startmenu_submenu_entered(void)
{
    g_pending_cascade = g_open_cascade;
    if (g_hot_entry != -1 || g_hot_footer != -1 || g_hot_shortcut != -1)
    {
        g_hot_entry = -1;
        g_hot_footer = -1;
        g_hot_shortcut = -1;
        desktop_repaint_popup(DESKTOP_POPUP_START_MENU);
    }
}

void desktop_startmenu_submenu_closed(void)
{
    if (g_open_cascade != -1)
    {
        g_open_cascade = -1;
        desktop_repaint_popup(DESKTOP_POPUP_START_MENU);
    }
    g_pending_cascade = -1;
}

static int startmenu_open(void)
{
    g_hot_entry = -1;
    g_hot_footer = -1;
    g_pressed_footer = -1;
    g_last_buttons = 0;
    g_open_cascade = -1;
    g_pending_cascade = -1;
    g_cascade_due_ms = 0;
    g_pending_action = -1;
    g_hot_shortcut = -1;
    g_pressed_shortcut = -1;
    g_pending_shortcut = -1;
    return 0;
}

static void startmenu_close(void)
{
    int action = g_pending_action;

    g_pending_action = -1;
    /* The cascade goes with its menu. */
    desktop_close_submenu();
    /* The menu's window is free by now, and the dialog takes it. */
    if (action == STARTMENU_ACTION_SHUTDOWN)
    {
        desktop_shutdown_dialog(DESKTOP_SHUTDOWN_POWER_OFF);
    }
    else if (action == STARTMENU_ACTION_REBOOT)
    {
        desktop_shutdown_dialog(DESKTOP_SHUTDOWN_RESTART);
    }
    /* A shortcut opens the way the desktop opens one: a .exe through its
     * association, which is winhost. */
    if (g_pending_shortcut >= 0)
    {
        g_pending_shortcut = -1;
        (void)shortcut_open_path(desktop_main_window(), g_pending_shortcut_path);
    }
}

static uint32_t startmenu_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    uint32_t down = pointer->buttons & ~g_last_buttons;
    uint32_t up = g_last_buttons & ~pointer->buttons;
    int entry = entry_hit(pointer->x, pointer->y);
    int footer = footer_hit(pointer->x, pointer->y);
    int shortcut = shortcut_hit(pointer->x, pointer->y);
    uint32_t actions = DESKTOP_ACTION_NONE;

    (void)gfx;
    g_last_buttons = pointer->buttons;
    if (entry != g_hot_entry || footer != g_hot_footer || shortcut != g_hot_shortcut)
    {
        g_hot_entry = entry;
        g_hot_footer = footer;
        g_hot_shortcut = shortcut;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    /* Resting on an entry asks for its cascade, on a shortcut or the footer
     * for none. The banner and the separators ask for nothing. */
    if (entry >= 0)
    {
        startmenu_want_cascade(entry);
    }
    else if (footer >= 0 || shortcut >= 0)
    {
        startmenu_want_cascade(-1);
    }
    if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        g_pressed_footer = footer;
        g_pressed_shortcut = shortcut;
        /* A click does not wait for the delay. */
        if (entry >= 0 && entry != g_open_cascade)
        {
            startmenu_apply_cascade(entry);
            actions |= DESKTOP_ACTION_REPAINT;
        }
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
        else if (shortcut >= 0 && shortcut == g_pressed_shortcut)
        {
            const struct start_shortcut *chosen = start_shortcuts_at(shortcut);

            memcpy(g_pending_shortcut_path, chosen->path, sizeof(g_pending_shortcut_path));
            g_pending_shortcut = shortcut;
            actions |= DESKTOP_ACTION_CLOSE;
        }
        g_pressed_footer = -1;
        g_pressed_shortcut = -1;
    }
    return actions;
}

/* The cascade switches without any pointer event arriving, once the pointer
 * has rested long enough, so it is driven from the desktop loop. */
static uint32_t startmenu_tick(void)
{
    if (g_pending_cascade == g_open_cascade || uptime_ms() < g_cascade_due_ms)
    {
        return DESKTOP_ACTION_NONE;
    }
    startmenu_apply_cascade(g_pending_cascade);
    return DESKTOP_ACTION_REPAINT;
}

/* The pointer went to the background or the taskbar: whatever it rested on
 * last no longer asks for anything, and the open cascade stays, as in
 * Windows. */
static uint32_t startmenu_pointer_left(void)
{
    g_pending_cascade = g_open_cascade;
    if (g_hot_entry != -1 || g_hot_footer != -1 || g_hot_shortcut != -1)
    {
        g_hot_entry = -1;
        g_hot_footer = -1;
        g_hot_shortcut = -1;
        return DESKTOP_ACTION_REPAINT;
    }
    return DESKTOP_ACTION_NONE;
}

const struct desktop_popup_ops desktop_startmenu_ops = {
    STARTMENU_WIDTH,
    STARTMENU_MARGIN * 2 + STARTMENU_ROW_HEIGHT + STARTMENU_FOOTER_HEIGHT,
    0,
    startmenu_open,
    startmenu_close,
    startmenu_pointer,
    startmenu_tick,
    startmenu_paint,
    0,
    startmenu_pointer_left,
    startmenu_measure,
};
