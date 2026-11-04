/*
 * Display Properties.
 *
 * The desktop's look, in the shape of the period's Display control panel: a
 * Desktop tab that picks the wallpaper, its position and the colour behind it,
 * and a Settings tab with the screen's resolution and colour quality. Both
 * tabs show the result on a small monitor before anything is applied.
 *
 * This program never draws the real background. It writes the configuration
 * (desktop_wallpaper_write_config) and the desktop, which re-reads it, repaints
 * on its own. The resolution it asks windowd for (gfx_set_display_mode), and
 * saves only once the user keeps it. How the pieces fit is in
 * docs/desktop/DISPLAY_PROPERTIES.md.
 */

#include "libc.h"
#include "savanxp/sxgui.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "desktop_wallpaper.h"
#include "display_settings.h"

static struct sxgui_app g_app;

/* ---- layout ---------------------------------------------------------------
 *
 * A fixed layout, as a property sheet has. Everything on a page is placed
 * from the page's rect (sxgui_tabs_page), so the numbers here are the sheet's
 * outer size and the spacing inside it. */
#define DISPLAY_MARGIN SXGUI_CONTENT_MARGIN
#define DISPLAY_SHEET_WIDTH 380
#define DISPLAY_PAGE_HEIGHT 326
#define DISPLAY_PAGE_PAD 10

/* The monitor drawn at the top of both pages. Its screen keeps the real
 * screen's proportions, so the preview is what the desktop will look like. */
#define DISPLAY_PREVIEW_WIDTH 152
#define DISPLAY_BEZEL 10
#define DISPLAY_STAND_HEIGHT 22
#define DISPLAY_MONITOR_TOP 12

#define DISPLAY_LIST_WIDTH 228
#define DISPLAY_LIST_HEIGHT 132
#define DISPLAY_ROW 18
#define DISPLAY_GROUP_HEIGHT 86

/* An image no larger than this on both sides is a pattern: picking it from
 * the list tiles it, as picking a large one stretches it. */
#define DISPLAY_TILE_THRESHOLD 256

#define DISPLAY_TAB_DESKTOP 0
#define DISPLAY_TAB_SETTINGS 1

static const char *const k_tab_labels[] = {"Desktop", "Settings"};
#define DISPLAY_TAB_COUNT ((int)(sizeof(k_tab_labels) / sizeof(k_tab_labels[0])))

enum display_widget
{
    W_TABS = 0,
    /* Desktop */
    W_BACKGROUND_LABEL,
    W_BACKGROUND_LIST,
    W_BROWSE,
    W_POSITION_LABEL,
    W_POSITION,
    W_COLOR_LABEL,
    W_COLOR,
    /* Settings */
    W_DISPLAY_LABEL,
    W_DISPLAY_NAME,
    W_RESOLUTION_GROUP,
    W_LESS,
    W_RESOLUTION,
    W_MORE,
    W_RESOLUTION_TEXT,
    W_QUALITY_GROUP,
    W_QUALITY,
    W_RESOLUTION_NOTE,
    /* Under the sheet */
    W_OK,
    W_CANCEL,
    W_APPLY,
    W_COUNT
};

static struct sxgui_widget g_widgets[W_COUNT];

#define DISPLAY_FIRST_DESKTOP_WIDGET W_BACKGROUND_LABEL
#define DISPLAY_LAST_DESKTOP_WIDGET W_COLOR
#define DISPLAY_FIRST_SETTINGS_WIDGET W_DISPLAY_LABEL
#define DISPLAY_LAST_SETTINGS_WIDGET W_RESOLUTION_NOTE

/* ---- state ---------------------------------------------------------------- */

/* What the desktop shows now, and what the dialog would apply. Apply is
 * enabled while they differ. */
static struct desktop_wallpaper_config g_saved;
static struct desktop_wallpaper_config g_pending;
/* The pending wallpaper, decoded for the previews. */
static struct desktop_wallpaper_image g_preview;

static int g_screen_width = 1280;
static int g_screen_height = 800;
/* The preview's height, fixed at start-up from the screen's proportions: the
 * controls under it are placed once, so it does not follow a new mode. */
static int g_preview_height = 0;

/* ---- the background list -------------------------------------------------
 *
 * "(None)" first, then every BMP in /wallpapers by name, then whatever the
 * user picked with Browse. The list shows names; the paths are kept beside
 * them. */
#define DISPLAY_MAX_BACKGROUNDS 64
#define DISPLAY_NAME_CAPACITY 64

struct background_entry
{
    char name[DISPLAY_NAME_CAPACITY];
    char path[DESKTOP_WALLPAPER_PATH_MAX];
};

static struct background_entry g_backgrounds[DISPLAY_MAX_BACKGROUNDS];
static const char *g_background_names[DISPLAY_MAX_BACKGROUNDS];
static int g_background_count = 0;

static char lower_char(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int compare_names(const char *left, const char *right)
{
    while (*left != '\0' && lower_char(*left) == lower_char(*right))
    {
        ++left;
        ++right;
    }
    return (int)(unsigned char)lower_char(*left) - (int)(unsigned char)lower_char(*right);
}

static int has_bmp_extension(const char *name)
{
    size_t length = strlen(name);

    return length > 4 && compare_names(name + length - 4, ".bmp") == 0;
}

/* The name a path shows in the list: its file name without ".bmp". */
static void name_from_path(const char *path, char *name, size_t capacity)
{
    const char *start = strrchr(path, '/');
    size_t length;

    start = start != 0 ? start + 1 : path;
    length = strlen(start);
    if (has_bmp_extension(start))
    {
        length -= 4;
    }
    if (length >= capacity)
    {
        length = capacity - 1;
    }
    memcpy(name, start, length);
    name[length] = '\0';
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    size_t length = strlen(source);

    if (length >= capacity)
    {
        length = capacity - 1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static int find_background(const char *path)
{
    int index;

    for (index = 0; index < g_background_count; ++index)
    {
        if (strcmp(g_backgrounds[index].path, path) == 0)
        {
            return index;
        }
    }
    return -1;
}

/* Adds a path to the list if it is not there. Returns its row, or -1 when
 * the list is full. */
static int add_background(const char *path)
{
    struct background_entry *entry;
    int existing = find_background(path);

    if (existing >= 0)
    {
        return existing;
    }
    if (g_background_count >= DISPLAY_MAX_BACKGROUNDS)
    {
        return -1;
    }
    entry = &g_backgrounds[g_background_count];
    copy_text(entry->path, sizeof(entry->path), path);
    if (path[0] == '\0')
    {
        copy_text(entry->name, sizeof(entry->name), "(None)");
    }
    else
    {
        name_from_path(path, entry->name, sizeof(entry->name));
    }
    g_background_names[g_background_count] = entry->name;
    return g_background_count++;
}

static void scan_backgrounds(void)
{
    DIR *directory;
    struct dirent *item;
    int first;
    int index;

    g_background_count = 0;
    (void)add_background("");
    first = g_background_count;

    directory = opendir(DESKTOP_WALLPAPER_DIRECTORY);
    if (directory != 0)
    {
        while ((item = readdir(directory)) != 0)
        {
            char path[DESKTOP_WALLPAPER_PATH_MAX];

            if (item->d_type == DT_DIR || !has_bmp_extension(item->d_name))
            {
                continue;
            }
            if (snprintf(path, sizeof(path), "%s/%s", DESKTOP_WALLPAPER_DIRECTORY, item->d_name) >= (int)sizeof(path))
            {
                continue;
            }
            (void)add_background(path);
        }
        closedir(directory);
    }

    /* Insertion sort by name; a few dozen entries at most. */
    for (index = first + 1; index < g_background_count; ++index)
    {
        struct background_entry moving = g_backgrounds[index];
        int slot = index;

        while (slot > first && compare_names(g_backgrounds[slot - 1].name, moving.name) > 0)
        {
            g_backgrounds[slot] = g_backgrounds[slot - 1];
            --slot;
        }
        g_backgrounds[slot] = moving;
    }
    for (index = 0; index < g_background_count; ++index)
    {
        g_background_names[index] = g_backgrounds[index].name;
    }
}

static void refresh_list_widget(void)
{
    struct sxgui_widget *list = &g_widgets[W_BACKGROUND_LIST];

    list->items = g_background_names;
    list->item_count = g_background_count;
}

/* ---- colours -------------------------------------------------------------- */

struct named_color
{
    const char *name;
    uint32_t rgb;
};

/* The classic 16, plus the system's own teal and the period's desktop blue. */
static const struct named_color k_colors[] = {
    {"Teal", 0x008080u},
    {"Desktop Blue", 0x3a6ea5u},
    {"Black", 0x000000u},
    {"White", 0xffffffu},
    {"Gray", 0x808080u},
    {"Silver", 0xc0c0c0u},
    {"Maroon", 0x800000u},
    {"Red", 0xff0000u},
    {"Olive", 0x808000u},
    {"Yellow", 0xffff00u},
    {"Green", 0x008000u},
    {"Lime", 0x00ff00u},
    {"Aqua", 0x00ffffu},
    {"Navy", 0x000080u},
    {"Blue", 0x0000ffu},
    {"Purple", 0x800080u},
    {"Fuchsia", 0xff00ffu},
};
#define DISPLAY_NAMED_COLORS ((int)(sizeof(k_colors) / sizeof(k_colors[0])))
/* The last row of the colour list opens the dialog for any other colour. */
#define DISPLAY_COLOR_OTHER DISPLAY_NAMED_COLORS

static const char *g_color_names[DISPLAY_NAMED_COLORS + 1];

static int color_index(uint32_t rgb)
{
    int index;

    for (index = 0; index < DISPLAY_NAMED_COLORS; ++index)
    {
        if (k_colors[index].rgb == rgb)
        {
            return index;
        }
    }
    return DISPLAY_COLOR_OTHER;
}

static uint32_t painter_color(uint32_t rgb)
{
    return gfx_rgb((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

/* ---- resolution ----------------------------------------------------------
 *
 * The slider's stops are the usual modes the adapter and the session can take,
 * plus the current mode when it is not one of them. The adapter says what it
 * takes (GPU_IOC_GET_CONNECTOR_PROPERTIES, a query any program may make); the
 * session never goes past SAVANXP_DISPLAY_SESSION_MAX_*, what its full-screen
 * memory was allocated for, unless it booted in a larger mode. */
struct display_mode
{
    uint32_t width;
    uint32_t height;
};

static const struct display_mode k_usual_modes[] = {
    {640, 480},
    {800, 600},
    {1024, 768},
    {1152, 864},
    {1280, 720},
    {1280, 800},
    {1280, 1024},
    {1440, 900},
    {1600, 900},
    {1600, 1200},
    {1920, 1080},
    {1920, 1200},
};
#define DISPLAY_USUAL_MODES ((int)(sizeof(k_usual_modes) / sizeof(k_usual_modes[0])))

static struct display_mode g_modes[DISPLAY_USUAL_MODES + 1];
static int g_mode_count = 0;
static int g_current_mode = 0;  /* the mode on the screen */
static int g_pending_mode = 0;  /* the mode the slider shows */
static int g_mode_mutable = 0;  /* the adapter can change it at all */
static char g_resolution_text[48];
static char g_adapter_text[64];

/* Sorted by size, so the slider goes from less to more. */
static void add_mode(uint32_t width, uint32_t height)
{
    unsigned long area = (unsigned long)width * height;
    int slot = g_mode_count;
    int index;

    for (index = 0; index < g_mode_count; ++index)
    {
        if (g_modes[index].width == width && g_modes[index].height == height)
        {
            return;
        }
    }
    if (g_mode_count >= (int)(sizeof(g_modes) / sizeof(g_modes[0])))
    {
        return;
    }
    while (slot > 0)
    {
        const struct display_mode *before = &g_modes[slot - 1];
        unsigned long before_area = (unsigned long)before->width * before->height;

        if (before_area < area || (before_area == area && before->width < width))
        {
            break;
        }
        g_modes[slot] = g_modes[slot - 1];
        --slot;
    }
    g_modes[slot].width = width;
    g_modes[slot].height = height;
    ++g_mode_count;
}

static int find_mode(uint32_t width, uint32_t height)
{
    int index;

    for (index = 0; index < g_mode_count; ++index)
    {
        if (g_modes[index].width == width && g_modes[index].height == height)
        {
            return index;
        }
    }
    return 0;
}

static void build_modes(void)
{
    struct savanxp_gpu_connector_properties properties;
    struct savanxp_gpu_info info;
    const char *adapter = "the display adapter";
    long fd = savanxp_open_mode("/dev/gpu0", SAVANXP_OPEN_READ);
    int have_properties = 0;
    int index;

    memset(&properties, 0, sizeof(properties));
    memset(&info, 0, sizeof(info));
    if (fd >= 0)
    {
        if (gpu_get_info((int)fd, &info) == 0 && info.width != 0 && info.height != 0)
        {
            g_screen_width = (int)info.width;
            g_screen_height = (int)info.height;
        }
        have_properties = gpu_get_connector_properties((int)fd, &properties) == 0;
        savanxp_close((int)fd);
    }
    if (info.backend == SAVANXP_GPU_BACKEND_VIRTIO)
    {
        adapter = "VirtIO GPU";
    }
    else if (info.backend == SAVANXP_GPU_BACKEND_FRAMEBUFFER)
    {
        adapter = "the boot framebuffer";
    }
    snprintf(g_adapter_text, sizeof(g_adapter_text), "Default Monitor on %s", adapter);

    g_mode_mutable = have_properties &&
        (properties.flags & SAVANXP_GPU_CONNECTOR_FLAG_MUTABLE_MODE_SETTING) != 0u;
    g_mode_count = 0;
    if (g_mode_mutable)
    {
        uint32_t max_width = properties.max_width < SAVANXP_DISPLAY_SESSION_MAX_WIDTH
            ? properties.max_width : SAVANXP_DISPLAY_SESSION_MAX_WIDTH;
        uint32_t max_height = properties.max_height < SAVANXP_DISPLAY_SESSION_MAX_HEIGHT
            ? properties.max_height : SAVANXP_DISPLAY_SESSION_MAX_HEIGHT;

        for (index = 0; index < DISPLAY_USUAL_MODES; ++index)
        {
            const struct display_mode *mode = &k_usual_modes[index];

            if (mode->width <= max_width && mode->height <= max_height &&
                (unsigned long)mode->width * mode->height * 4ul <= (unsigned long)properties.max_bytes)
            {
                add_mode(mode->width, mode->height);
            }
        }
    }
    add_mode((uint32_t)g_screen_width, (uint32_t)g_screen_height);
    g_current_mode = find_mode((uint32_t)g_screen_width, (uint32_t)g_screen_height);
    g_pending_mode = g_current_mode;
}

/* The text under the slider, centred in its group. */
static void update_resolution_text(void)
{
    struct sxgui_widget *label = &g_widgets[W_RESOLUTION_TEXT];
    const struct sxgui_widget *group = &g_widgets[W_RESOLUTION_GROUP];
    int width;

    snprintf(g_resolution_text, sizeof(g_resolution_text), "%u by %u pixels",
        (unsigned int)g_modes[g_pending_mode].width, (unsigned int)g_modes[g_pending_mode].height);
    width = gfx_text_width(g_resolution_text) + 2;
    label->rect.x = group->rect.x + (group->rect.width - width) / 2;
    label->rect.width = width;
}

static const char *const k_quality_names[] = {"Highest (32 bit)"};

/* ---- painting ------------------------------------------------------------- */

static struct sx_rect page_rect(void)
{
    return sxgui_tabs_page(&g_widgets[W_TABS]);
}

/* Where the preview's screen goes: centred at the top of the page. */
static struct sx_rect preview_screen_rect(void)
{
    struct sx_rect page = page_rect();
    int height = g_preview_height;

    if (height <= 0)
    {
        height = (DISPLAY_PREVIEW_WIDTH * g_screen_height) / (g_screen_width > 0 ? g_screen_width : 1);
        if (height > DISPLAY_PREVIEW_WIDTH)
        {
            height = DISPLAY_PREVIEW_WIDTH;
        }
        g_preview_height = height;
    }
    return sx_rect_make(
        page.x + (page.width - DISPLAY_PREVIEW_WIDTH) / 2,
        page.y + DISPLAY_MONITOR_TOP + DISPLAY_BEZEL,
        DISPLAY_PREVIEW_WIDTH,
        height);
}

/* The bottom of the monitor, stand included: where a page's controls start. */
static int monitor_bottom(void)
{
    struct sx_rect screen = preview_screen_rect();

    return screen.y + screen.height + DISPLAY_BEZEL + DISPLAY_STAND_HEIGHT;
}

/* A period CRT: a light bezel with rounded corners, the screen sunk into it,
 * a power light, and a neck and foot under it. */
static void paint_monitor(struct sx_painter *painter, struct sx_rect screen)
{
    const uint32_t bezel = gfx_rgb(236, 233, 216);
    const uint32_t bezel_shadow = gfx_rgb(160, 158, 146);
    const uint32_t bezel_light = gfx_rgb(255, 255, 255);
    struct sx_rect outer = sx_rect_make(
        screen.x - DISPLAY_BEZEL,
        screen.y - DISPLAY_BEZEL,
        screen.width + DISPLAY_BEZEL * 2,
        screen.height + DISPLAY_BEZEL * 2);
    int foot_y = outer.y + outer.height;
    int centre = outer.x + outer.width / 2;
    struct sx_point neck[4];

    /* Neck, then the foot over it. */
    neck[0] = (struct sx_point){centre - 22, foot_y};
    neck[1] = (struct sx_point){centre + 22, foot_y};
    neck[2] = (struct sx_point){centre + 34, foot_y + 12};
    neck[3] = (struct sx_point){centre - 34, foot_y + 12};
    sx_painter_fill_polygon(painter, neck, 4, bezel_shadow);
    sx_painter_fill_round_rect(painter, sx_rect_make(centre - 50, foot_y + 10, 100, 11), 5, bezel);
    sx_painter_draw_round_rect(painter, sx_rect_make(centre - 50, foot_y + 10, 100, 11), 5, bezel_shadow);

    sx_painter_fill_round_rect(painter, outer, 7, bezel);
    sx_painter_draw_round_rect(painter, outer, 7, bezel_shadow);
    sx_painter_hline(painter, outer.x + 7, outer.y + 1, outer.width - 14, bezel_light);
    sx_painter_vline(painter, outer.x + 1, outer.y + 7, outer.height - 14, bezel_light);

    /* Power light, bottom right of the bezel. */
    sx_painter_fill_ellipse(
        painter,
        sx_rect_make(outer.x + outer.width - 14, outer.y + outer.height - 8, 5, 5),
        gfx_rgb(64, 176, 72));

    sxgui_draw_sunken_edge(painter, sx_rect_make(
        screen.x - SXGUI_BORDER_SUNKEN,
        screen.y - SXGUI_BORDER_SUNKEN,
        screen.width + SXGUI_BORDER_SUNKEN * 2,
        screen.height + SXGUI_BORDER_SUNKEN * 2));
}

/* The taskbar, at the preview's scale: a face-coloured strip with a raised
 * edge and the Start button at its left. */
static void paint_preview_taskbar(struct sx_painter *painter, struct sx_rect screen)
{
    int height = (DESKTOP_WALLPAPER_TASKBAR_HEIGHT * screen.height) / (g_screen_height > 0 ? g_screen_height : 1);
    struct sx_rect bar;

    if (height < 4)
    {
        height = 4;
    }
    bar = sx_rect_make(screen.x, screen.y + screen.height - height, screen.width, height);
    sx_painter_fill_rect(painter, bar, SXGUI_COLOR_FACE);
    sx_painter_hline(painter, bar.x, bar.y, bar.width, SXGUI_COLOR_LIGHT);
    sx_painter_fill_rect(painter, sx_rect_make(bar.x + 2, bar.y + 1, height * 2, height - 2), SXGUI_COLOR_BEVEL);
    sx_painter_fill_rect(painter, sx_rect_make(bar.x + 2, bar.y + 1, height * 2 - 1, height - 3), SXGUI_COLOR_FACE);
    sx_painter_hline(painter, bar.x + 2, bar.y + 1, height * 2 - 1, SXGUI_COLOR_LIGHT);
}

static void paint_preview(struct sx_painter *painter, int with_taskbar)
{
    struct sx_rect screen = preview_screen_rect();

    paint_monitor(painter, screen);
    desktop_wallpaper_paint(painter, screen, g_screen_width, g_screen_height, &g_pending, &g_preview);
    if (with_taskbar && sx_painter_push_clip(painter, screen))
    {
        paint_preview_taskbar(painter, screen);
        sx_painter_pop_clip(painter);
    }
}

/* The colour combobox shows the colour itself, not its name. */
static void paint_color_swatch(struct sx_painter *painter)
{
    const struct sxgui_widget *combo = &g_widgets[W_COLOR];
    struct sx_rect swatch = sx_rect_make(
        combo->rect.x + SXGUI_BORDER_SUNKEN + 3,
        combo->rect.y + SXGUI_BORDER_SUNKEN + 3,
        combo->rect.width - SXGUI_BORDER_SUNKEN * 2 - SXGUI_SCROLLBAR_THICKNESS - 6,
        combo->rect.height - SXGUI_BORDER_SUNKEN * 2 - 6);

    sx_painter_fill_rect(painter, swatch, painter_color(g_pending.color));
    sx_painter_draw_frame(painter, swatch, SXGUI_COLOR_TEXT);
}

/* The colour quality's spectrum: what 32 bits can show, as a hue band. */
static void paint_spectrum(struct sx_painter *painter)
{
    const struct sxgui_widget *quality = &g_widgets[W_QUALITY];
    struct sx_rect band = sx_rect_make(quality->rect.x, quality->rect.y + quality->rect.height + 8, quality->rect.width, 12);
    int x;

    for (x = 0; x < band.width; ++x)
    {
        /* Six segments of the hue wheel, red to red. */
        int position = (x * 6 * 255) / (band.width > 1 ? band.width - 1 : 1);
        int segment = position / 255;
        int ramp = position % 255;
        int r = 0;
        int g = 0;
        int b = 0;

        switch (segment)
        {
        case 0: r = 255; g = ramp; break;
        case 1: r = 255 - ramp; g = 255; break;
        case 2: g = 255; b = ramp; break;
        case 3: g = 255 - ramp; b = 255; break;
        case 4: r = ramp; b = 255; break;
        default: r = 255; b = 255 - ramp; break;
        }
        sx_painter_vline(painter, band.x + x, band.y, band.height, gfx_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
    }
    sxgui_draw_sunken_edge(painter, sx_rect_make(band.x - 1, band.y - 1, band.width + 2, band.height + 2));
}

static void on_paint(struct sxgui_app *app)
{
    struct sx_painter *painter = &app->ui.painter;

    if (!sx_painter_push_clip(painter, page_rect()))
    {
        return;
    }
    if (g_widgets[W_TABS].value == DISPLAY_TAB_SETTINGS)
    {
        paint_preview(painter, 1);
        paint_spectrum(painter);
    }
    else
    {
        paint_preview(painter, 0);
        paint_color_swatch(painter);
    }
    sx_painter_pop_clip(painter);
}

/* ---- pending changes ------------------------------------------------------ */

static void set_widget_enabled(struct sxgui_widget *widget, int enabled)
{
    if (enabled)
    {
        widget->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
    }
    else
    {
        widget->flags |= SXGUI_FLAG_DISABLED;
    }
}

/* Brings the controls in line with g_pending after it changed. */
static void sync_controls(void)
{
    int row = find_background(g_pending.path);

    g_widgets[W_BACKGROUND_LIST].value = row;
    g_widgets[W_POSITION].value = g_pending.position;
    /* Position means nothing without an image, as in the period's dialog. */
    set_widget_enabled(&g_widgets[W_POSITION], g_pending.path[0] != '\0');
    g_widgets[W_COLOR].value = color_index(g_pending.color);
    g_widgets[W_RESOLUTION].value = g_pending_mode;
    update_resolution_text();
    set_widget_enabled(&g_widgets[W_APPLY],
        !desktop_wallpaper_config_equal(&g_pending, &g_saved) || g_pending_mode != g_current_mode);
    sxgui_app_request_repaint(&g_app);
}

static void select_background(const char *path)
{
    copy_text(g_pending.path, sizeof(g_pending.path), path);
    if (path[0] != '\0' && desktop_wallpaper_image_load(&g_preview, path) == 0)
    {
        /* A pattern tiles, a picture fills the screen; a picture the user
         * centred stays centred. */
        if (g_preview.width <= DISPLAY_TILE_THRESHOLD && g_preview.height <= DISPLAY_TILE_THRESHOLD)
        {
            g_pending.position = DESKTOP_WALLPAPER_TILE;
        }
        else if (g_pending.position == DESKTOP_WALLPAPER_TILE)
        {
            g_pending.position = DESKTOP_WALLPAPER_STRETCH;
        }
    }
    else
    {
        desktop_wallpaper_image_unload(&g_preview);
    }
    sync_controls();
}

static void write_wallpaper(void)
{
    if (!desktop_wallpaper_config_equal(&g_pending, &g_saved) &&
        desktop_wallpaper_write_config(&g_pending) == 0)
    {
        g_saved = g_pending;
    }
}

/* ---- dialogs ---------------------------------------------------------------
 *
 * Two small ones, each a prompt with a field: the path of an image for
 * Browse, and an RGB value for "Other..." in the colour list. The OK button
 * checks the field and leaves the dialog open, with the reason in its prompt,
 * when the value cannot be used. */
#define DISPLAY_DIALOG_WIDTH 320
#define DISPLAY_DIALOG_HEIGHT 104

enum dialog_widget
{
    D_PROMPT = 0,
    D_FIELD,
    D_OK,
    D_CANCEL,
    D_COUNT
};

static struct sxgui_widget g_dialog_widgets[D_COUNT];
static struct sxgui_dialog g_dialog;
static char g_dialog_text[DESKTOP_WALLPAPER_PATH_MAX];
static void (*g_dialog_accept)(void);

static void on_dialog_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    if (g_dialog_accept != 0)
    {
        g_dialog_accept();
    }
}

static void on_dialog_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 0);
}

static void open_prompt(const char *title, const char *prompt, const char *initial, void (*accept)(void))
{
    const int inner = DISPLAY_DIALOG_WIDTH - SXGUI_DIALOG_MARGIN * 2;
    const int buttons_y = DISPLAY_DIALOG_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT;

    copy_text(g_dialog_text, sizeof(g_dialog_text), initial);
    g_dialog_accept = accept;

    g_dialog_widgets[D_PROMPT] = sxgui_label(sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN, inner, DISPLAY_ROW), prompt);
    g_dialog_widgets[D_FIELD] = sxgui_textfield(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + DISPLAY_ROW + 4, inner, SXGUI_FIELD_HEIGHT),
        g_dialog_text,
        (int)sizeof(g_dialog_text));
    g_dialog_widgets[D_FIELD].caret = (int)strlen(g_dialog_text);
    g_dialog_widgets[D_OK] = sxgui_button(
        sx_rect_make(DISPLAY_DIALOG_WIDTH - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_WIDTH * 2 - SXGUI_GAP, buttons_y,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_dialog_ok, 0);
    g_dialog_widgets[D_CANCEL] = sxgui_button(
        sx_rect_make(DISPLAY_DIALOG_WIDTH - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_WIDTH, buttons_y,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Cancel", on_dialog_cancel, 0);

    memset(&g_dialog, 0, sizeof(g_dialog));
    g_dialog.title = title;
    g_dialog.widgets = g_dialog_widgets;
    g_dialog.widget_count = D_COUNT;
    g_dialog.initial_focus = D_FIELD;
    g_dialog.default_button = D_OK;
    sxgui_dialog_begin(&g_app.ui, &g_dialog, DISPLAY_DIALOG_WIDTH, DISPLAY_DIALOG_HEIGHT);
    sxgui_app_request_repaint(&g_app);
}

static void accept_browse(void)
{
    struct desktop_wallpaper_image probe;
    int row;

    memset(&probe, 0, sizeof(probe));
    if (g_dialog_text[0] != '/')
    {
        g_dialog_widgets[D_PROMPT].text = "Type the full path, starting with /.";
        sxgui_app_request_repaint(&g_app);
        return;
    }
    if (desktop_wallpaper_image_load(&probe, g_dialog_text) < 0)
    {
        g_dialog_widgets[D_PROMPT].text = "That file is not a BMP image this system can show.";
        sxgui_app_request_repaint(&g_app);
        return;
    }
    desktop_wallpaper_image_unload(&probe);

    row = add_background(g_dialog_text);
    if (row < 0)
    {
        g_dialog_widgets[D_PROMPT].text = "The list is full.";
        sxgui_app_request_repaint(&g_app);
        return;
    }
    refresh_list_widget();
    sxgui_dialog_end(&g_app.ui, 1);
    select_background(g_backgrounds[row].path);
}

static void accept_color(void)
{
    uint32_t value = 0;
    const char *text = g_dialog_text;
    int digits = 0;

    if (*text == '#')
    {
        ++text;
    }
    for (; *text != '\0'; ++text, ++digits)
    {
        char c = lower_char(*text);

        if (c >= '0' && c <= '9')
        {
            value = (value << 4) | (uint32_t)(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            value = (value << 4) | (uint32_t)(c - 'a' + 10);
        }
        else
        {
            break;
        }
    }
    if (*text != '\0' || digits != 6)
    {
        g_dialog_widgets[D_PROMPT].text = "Six hexadecimal digits, as in 3A6EA5.";
        sxgui_app_request_repaint(&g_app);
        return;
    }
    sxgui_dialog_end(&g_app.ui, 1);
    g_pending.color = value;
    sync_controls();
}

/* ---- keeping a new resolution ------------------------------------------------
 *
 * A mode the monitor cannot show must not stick, so a new one is a trial: the
 * dialog asks whether to keep it and goes back to the previous mode unless
 * the answer is Yes within DISPLAY_CONFIRM_SECONDS. Closing the question with
 * Esc is a No. Only a kept mode is saved (display_settings_write_mode). */
#define DISPLAY_CONFIRM_SECONDS 15
#define DISPLAY_CONFIRM_WIDTH 340
#define DISPLAY_CONFIRM_HEIGHT 96

enum confirm_widget
{
    C_QUESTION = 0,
    C_COUNTDOWN,
    C_YES,
    C_NO,
    C_COUNT
};

static struct sxgui_widget g_confirm_widgets[C_COUNT];
static char g_countdown_text[64];
static int g_confirm_open = 0;
static unsigned long g_confirm_deadline_ms = 0;
static int g_previous_mode = 0;
static int g_quit_after_confirm = 0;

/* A one-button notice. */
#define DISPLAY_MESSAGE_HEIGHT 72
static struct sxgui_widget g_message_widgets[2];

static void show_message(const char *title, const char *text)
{
    const int inner = DISPLAY_CONFIRM_WIDTH - SXGUI_DIALOG_MARGIN * 2;

    g_message_widgets[0] = sxgui_label(sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN, inner, DISPLAY_ROW), text);
    g_message_widgets[1] = sxgui_button(
        sx_rect_make(DISPLAY_CONFIRM_WIDTH - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_WIDTH,
                     DISPLAY_MESSAGE_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_dialog_cancel, 0);
    memset(&g_dialog, 0, sizeof(g_dialog));
    g_dialog.title = title;
    g_dialog.widgets = g_message_widgets;
    g_dialog.widget_count = 2;
    g_dialog.initial_focus = 1;
    g_dialog.default_button = 1;
    sxgui_dialog_begin(&g_app.ui, &g_dialog, DISPLAY_CONFIRM_WIDTH, DISPLAY_MESSAGE_HEIGHT);
    sxgui_app_request_repaint(&g_app);
}

static void set_screen_mode(int mode)
{
    g_current_mode = mode;
    g_pending_mode = mode;
    g_screen_width = (int)g_modes[mode].width;
    g_screen_height = (int)g_modes[mode].height;
}

static void finish_confirm(int keep)
{
    g_confirm_open = 0;
    if (keep)
    {
        (void)display_settings_write_mode(g_modes[g_current_mode].width, g_modes[g_current_mode].height);
    }
    else
    {
        (void)gfx_set_display_mode(&g_app.gfx, g_modes[g_previous_mode].width, g_modes[g_previous_mode].height);
        set_screen_mode(g_previous_mode);
        g_quit_after_confirm = 0;
    }
    sync_controls();
    if (g_quit_after_confirm)
    {
        sxgui_app_quit(&g_app, 0);
    }
}

static void on_confirm_yes(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    g_confirm_open = 0;
    sxgui_dialog_end(&g_app.ui, 1);
    finish_confirm(1);
}

static void on_confirm_no(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    g_confirm_open = 0;
    sxgui_dialog_end(&g_app.ui, 0);
    finish_confirm(0);
}

static void update_countdown(void)
{
    unsigned long now = uptime_ms();
    unsigned long left = now < g_confirm_deadline_ms ? (g_confirm_deadline_ms - now + 999ul) / 1000ul : 0ul;

    snprintf(g_countdown_text, sizeof(g_countdown_text), "Reverting in %lu second%s.", left, left == 1ul ? "" : "s");
}

static void open_confirm(void)
{
    const int inner = DISPLAY_CONFIRM_WIDTH - SXGUI_DIALOG_MARGIN * 2;
    const int buttons_y = DISPLAY_CONFIRM_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT;

    g_confirm_deadline_ms = uptime_ms() + DISPLAY_CONFIRM_SECONDS * 1000ul;
    update_countdown();
    g_confirm_widgets[C_QUESTION] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN, inner, DISPLAY_ROW),
        "Your desktop has been reconfigured. Keep these settings?");
    g_confirm_widgets[C_COUNTDOWN] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + DISPLAY_ROW + 2, inner, DISPLAY_ROW),
        g_countdown_text);
    g_confirm_widgets[C_YES] = sxgui_button(
        sx_rect_make(DISPLAY_CONFIRM_WIDTH - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_WIDTH * 2 - SXGUI_GAP, buttons_y,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Yes", on_confirm_yes, 0);
    g_confirm_widgets[C_NO] = sxgui_button(
        sx_rect_make(DISPLAY_CONFIRM_WIDTH - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_WIDTH, buttons_y,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "No", on_confirm_no, 0);

    memset(&g_dialog, 0, sizeof(g_dialog));
    g_dialog.title = "Monitor Settings";
    g_dialog.widgets = g_confirm_widgets;
    g_dialog.widget_count = C_COUNT;
    g_dialog.initial_focus = C_YES;
    g_dialog.default_button = C_YES;
    g_confirm_open = 1;
    sxgui_dialog_begin(&g_app.ui, &g_dialog, DISPLAY_CONFIRM_WIDTH, DISPLAY_CONFIRM_HEIGHT);
    sxgui_app_request_repaint(&g_app);
}

/* The countdown, and the question closed by Esc (the toolkit ends a dialog
 * on Esc without a callback). */
static void on_tick(struct sxgui_app *app)
{
    if (!g_confirm_open)
    {
        return;
    }
    if (!sxgui_dialog_active(&app->ui))
    {
        finish_confirm(0);
        return;
    }
    if (uptime_ms() >= g_confirm_deadline_ms)
    {
        g_confirm_open = 0;
        sxgui_dialog_end(&app->ui, 0);
        finish_confirm(0);
        return;
    }
    update_countdown();
    sxgui_app_request_repaint(app);
}

/* Writes the wallpaper and, when the slider moved, tries the new mode. With
 * `quit` the dialog closes afterwards -- after the question, when there is
 * one, and not at all when the mode is turned down. */
static void apply_pending(int quit)
{
    char text[96];
    const struct display_mode *mode = &g_modes[g_pending_mode];
    long result;

    write_wallpaper();
    if (g_pending_mode == g_current_mode || !g_mode_mutable)
    {
        sync_controls();
        if (quit)
        {
            sxgui_app_quit(&g_app, 0);
        }
        return;
    }

    result = gfx_set_display_mode(&g_app.gfx, mode->width, mode->height);
    if (result < 0)
    {
        g_pending_mode = g_current_mode;
        sync_controls();
        snprintf(text, sizeof(text), "The display adapter did not accept %u by %u pixels.",
            (unsigned int)mode->width, (unsigned int)mode->height);
        show_message("Display Properties", text);
        return;
    }
    g_previous_mode = g_current_mode;
    set_screen_mode(g_pending_mode);
    g_quit_after_confirm = quit;
    sync_controls();
    open_confirm();
}

/* ---- interaction ---------------------------------------------------------- */

static void show_tab(int tab)
{
    int index;

    for (index = DISPLAY_FIRST_DESKTOP_WIDGET; index <= DISPLAY_LAST_SETTINGS_WIDGET; ++index)
    {
        int on_desktop = index <= DISPLAY_LAST_DESKTOP_WIDGET;
        int visible = on_desktop ? tab == DISPLAY_TAB_DESKTOP : tab == DISPLAY_TAB_SETTINGS;

        if (index == W_RESOLUTION_NOTE && g_mode_mutable)
        {
            visible = 0;
        }
        if (visible)
        {
            g_widgets[index].flags |= SXGUI_FLAG_VISIBLE;
        }
        else
        {
            g_widgets[index].flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
        }
    }
    sxgui_app_request_repaint(&g_app);
}

static void on_tab_changed(struct sxgui_widget *widget, void *user)
{
    (void)user;
    show_tab(widget->value);
}

static void on_background_changed(struct sxgui_widget *widget, void *user)
{
    (void)user;
    if (widget->value >= 0 && widget->value < g_background_count &&
        strcmp(g_backgrounds[widget->value].path, g_pending.path) != 0)
    {
        select_background(g_backgrounds[widget->value].path);
    }
}

static void on_browse(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    open_prompt("Browse", "Path of a BMP image:", g_pending.path[0] != '\0' ? g_pending.path : "/", accept_browse);
}

static void on_position_changed(struct sxgui_widget *widget, void *user)
{
    (void)user;
    if (widget->value >= 0 && widget->value < DESKTOP_WALLPAPER_POSITION_COUNT)
    {
        g_pending.position = widget->value;
        sync_controls();
    }
}

static void on_color_changed(struct sxgui_widget *widget, void *user)
{
    char current[8];

    (void)user;
    if (widget->value >= 0 && widget->value < DISPLAY_NAMED_COLORS)
    {
        g_pending.color = k_colors[widget->value].rgb;
        sync_controls();
        return;
    }
    snprintf(current, sizeof(current), "%06X", (unsigned int)(g_pending.color & 0xffffffu));
    open_prompt("Other Color", "Red, green and blue, as RRGGBB:", current, accept_color);
}

static void on_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    apply_pending(1);
}

static void on_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_app_quit(&g_app, 0);
}

static void on_apply(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    apply_pending(0);
}

static void on_resolution_changed(struct sxgui_widget *widget, void *user)
{
    (void)user;
    if (widget->value >= 0 && widget->value < g_mode_count)
    {
        g_pending_mode = widget->value;
        sync_controls();
    }
}

/* Enter is OK, as on any property sheet, unless a dialog or an open drop-down
 * list is the one taking the keys. */
static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    if (event->type == SAVANXP_INPUT_EVENT_KEY_DOWN && event->key == SAVANXP_KEY_ENTER &&
        !sxgui_dialog_active(&app->ui) && app->ui.popup_owner < 0)
    {
        on_ok(0, 0);
        return 1;
    }
    return 0;
}

/* ---- start-up ------------------------------------------------------------- */

static void read_screen_size(void)
{
    struct savanxp_system_info info;

    memset(&info, 0, sizeof(info));
    if (system_info(&info) >= 0 && info.framebuffer_ready != 0 &&
        info.framebuffer_width != 0 && info.framebuffer_height != 0)
    {
        g_screen_width = (int)info.framebuffer_width;
        g_screen_height = (int)info.framebuffer_height;
    }
}

static void build_widgets(void)
{
    const int sheet_height = sxgui_tabs_height() + DISPLAY_PAGE_HEIGHT + SXGUI_BORDER_RAISED * 2;
    struct sx_rect page;
    int top;
    int x;
    int right_x;
    int right_width;
    int group_width;
    int group_y;
    int buttons_y;
    int index;

    g_widgets[W_TABS] = sxgui_tabs(
        sx_rect_make(DISPLAY_MARGIN, DISPLAY_MARGIN, DISPLAY_SHEET_WIDTH, sheet_height),
        k_tab_labels, DISPLAY_TAB_COUNT, DISPLAY_TAB_DESKTOP);
    g_widgets[W_TABS].on_action = on_tab_changed;

    page = page_rect();
    top = monitor_bottom() + 8;
    x = page.x + DISPLAY_PAGE_PAD;

    /* Desktop: the list on the left, its options in a column on the right. */
    right_x = x + DISPLAY_LIST_WIDTH + DISPLAY_PAGE_PAD;
    right_width = page.x + page.width - DISPLAY_PAGE_PAD - right_x;
    g_widgets[W_BACKGROUND_LABEL] = sxgui_label(sx_rect_make(x, top, DISPLAY_LIST_WIDTH, DISPLAY_ROW), "Background:");
    g_widgets[W_BACKGROUND_LIST] = sxgui_listbox(
        sx_rect_make(x, top + DISPLAY_ROW, DISPLAY_LIST_WIDTH, DISPLAY_LIST_HEIGHT),
        g_background_names, g_background_count);
    g_widgets[W_BACKGROUND_LIST].on_action = on_background_changed;
    g_widgets[W_BROWSE] = sxgui_button(
        sx_rect_make(right_x, top + DISPLAY_ROW, right_width, SXGUI_BUTTON_HEIGHT),
        "Browse...", on_browse, 0);
    g_widgets[W_POSITION_LABEL] = sxgui_label(
        sx_rect_make(right_x, top + DISPLAY_ROW + SXGUI_BUTTON_HEIGHT + 10, right_width, DISPLAY_ROW), "Position:");
    {
        static const char *position_names[DESKTOP_WALLPAPER_POSITION_COUNT];

        for (index = 0; index < DESKTOP_WALLPAPER_POSITION_COUNT; ++index)
        {
            position_names[index] = desktop_wallpaper_position_name(index);
        }
        g_widgets[W_POSITION] = sxgui_combobox(
            sx_rect_make(right_x, g_widgets[W_POSITION_LABEL].rect.y + DISPLAY_ROW, right_width, SXGUI_FIELD_HEIGHT),
            position_names, DESKTOP_WALLPAPER_POSITION_COUNT, g_pending.position);
        g_widgets[W_POSITION].on_action = on_position_changed;
    }
    g_widgets[W_COLOR_LABEL] = sxgui_label(
        sx_rect_make(right_x, g_widgets[W_POSITION].rect.y + SXGUI_FIELD_HEIGHT + 8, right_width, DISPLAY_ROW), "Color:");
    for (index = 0; index < DISPLAY_NAMED_COLORS; ++index)
    {
        g_color_names[index] = k_colors[index].name;
    }
    g_color_names[DISPLAY_COLOR_OTHER] = "Other...";
    g_widgets[W_COLOR] = sxgui_combobox(
        sx_rect_make(right_x, g_widgets[W_COLOR_LABEL].rect.y + DISPLAY_ROW, right_width, SXGUI_FIELD_HEIGHT),
        g_color_names, DISPLAY_NAMED_COLORS + 1, color_index(g_pending.color));
    g_widgets[W_COLOR].on_action = on_color_changed;

    /* Settings: which display, then two groups side by side. */
    g_widgets[W_DISPLAY_LABEL] = sxgui_label(sx_rect_make(x, top, page.width - DISPLAY_PAGE_PAD * 2, DISPLAY_ROW), "Display:");
    g_widgets[W_DISPLAY_NAME] = sxgui_label(
        sx_rect_make(x, top + DISPLAY_ROW, page.width - DISPLAY_PAGE_PAD * 2, DISPLAY_ROW), g_adapter_text);
    group_y = top + DISPLAY_ROW * 2 + 8;
    group_width = (page.width - DISPLAY_PAGE_PAD * 3) / 2;
    g_widgets[W_RESOLUTION_GROUP] = sxgui_groupbox(sx_rect_make(x, group_y, group_width, DISPLAY_GROUP_HEIGHT), "Screen resolution");
    g_widgets[W_LESS] = sxgui_label(sx_rect_make(x + 8, group_y + 22, 30, DISPLAY_ROW), "Less");
    g_widgets[W_MORE] = sxgui_label(sx_rect_make(x + group_width - 8 - 32, group_y + 22, 32, DISPLAY_ROW), "More");
    {
        int slider_x = g_widgets[W_LESS].rect.x + g_widgets[W_LESS].rect.width + 2;

        g_widgets[W_RESOLUTION] = sxgui_trackbar(
            sx_rect_make(slider_x, group_y + 18, g_widgets[W_MORE].rect.x - 4 - slider_x, SXGUI_TRACKBAR_HEIGHT),
            0, g_mode_count - 1, g_current_mode);
        g_widgets[W_RESOLUTION].on_action = on_resolution_changed;
        set_widget_enabled(&g_widgets[W_RESOLUTION], g_mode_mutable && g_mode_count > 1);
    }
    g_widgets[W_RESOLUTION_TEXT] = sxgui_label(sx_rect_make(x, group_y + 58, group_width, DISPLAY_ROW), g_resolution_text);
    {
        int quality_x = x + group_width + DISPLAY_PAGE_PAD;

        g_widgets[W_QUALITY_GROUP] = sxgui_groupbox(sx_rect_make(quality_x, group_y, group_width, DISPLAY_GROUP_HEIGHT), "Color quality");
        g_widgets[W_QUALITY] = sxgui_combobox(
            sx_rect_make(quality_x + 8, group_y + 20, group_width - 16, SXGUI_FIELD_HEIGHT),
            k_quality_names, 1, 0);
    }
    g_widgets[W_RESOLUTION_NOTE] = sxgui_label(
        sx_rect_make(x, group_y + DISPLAY_GROUP_HEIGHT + 8, page.width - DISPLAY_PAGE_PAD * 2, DISPLAY_ROW),
        "This display adapter cannot change its resolution.");
    g_widgets[W_RESOLUTION_NOTE].flags |= SXGUI_FLAG_DISABLED;

    /* OK, Cancel and Apply, right-aligned under the sheet. */
    buttons_y = g_widgets[W_TABS].rect.y + g_widgets[W_TABS].rect.height + SXGUI_GAP * 2;
    g_widgets[W_APPLY] = sxgui_button(
        sx_rect_make(DISPLAY_MARGIN + DISPLAY_SHEET_WIDTH - SXGUI_BUTTON_WIDTH, buttons_y,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Apply", on_apply, 0);
    g_widgets[W_CANCEL] = sxgui_button(
        sx_rect_make(g_widgets[W_APPLY].rect.x - SXGUI_GAP - SXGUI_BUTTON_WIDTH, buttons_y,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Cancel", on_cancel, 0);
    g_widgets[W_OK] = sxgui_button(
        sx_rect_make(g_widgets[W_CANCEL].rect.x - SXGUI_GAP - SXGUI_BUTTON_WIDTH, buttons_y,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_ok, 0);
}

int main(void)
{
    read_screen_size();
    build_modes();

    (void)desktop_wallpaper_read_config(&g_saved);
    g_pending = g_saved;
    scan_backgrounds();
    /* A wallpaper from elsewhere on the disk joins the list, as one picked
     * with Browse would. */
    (void)add_background(g_pending.path);
    (void)desktop_wallpaper_image_load(&g_preview, g_pending.path);

    build_widgets();
    refresh_list_widget();

    if (sxgui_app_init(&g_app, "display", g_widgets, W_COUNT) < 0)
    {
        return 1;
    }
    (void)sxgui_app_set_content_size(
        &g_app,
        DISPLAY_MARGIN * 2 + DISPLAY_SHEET_WIDTH,
        g_widgets[W_OK].rect.y + SXGUI_BUTTON_HEIGHT + DISPLAY_MARGIN);
    g_app.on_paint = on_paint;
    g_app.on_key = on_key;
    /* The countdown of "Keep these settings?". */
    g_app.on_tick = on_tick;
    g_app.tick_interval_ms = 250;

    show_tab(DISPLAY_TAB_DESKTOP);
    sync_controls();
    sxgui_focus(&g_app.ui, W_BACKGROUND_LIST);
    return sxgui_app_run(&g_app);
}
