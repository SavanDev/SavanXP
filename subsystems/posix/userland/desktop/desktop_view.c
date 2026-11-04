#include "desktop.h"

#include "desktop_icons.h"
#include "mime_icon.h"
#include "shortcut.h"
#include "savanxp/sxe.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The icons on the background (docs/desktop/DESKTOP_ICONS.md, phases 2 and 3).
 *
 * What the desktop shows is the user's Desktop folder: one icon per entry,
 * in a grid of cells, with the positions the user left them in kept in the
 * profile. The folder is re-read every few seconds, since the filesystem has
 * no change notification; what this process changes itself it re-reads at
 * once.
 *
 * Mouse: click selects, double click opens, dragging moves an icon to
 * another cell, a right click opens the icon's or the background's menu.
 * Keyboard, once the background has the focus: the arrows, Enter, Delete,
 * F2 (rename in place), F5, Shift+F10 or the Menu key, and a letter to jump.
 *
 * No malloc for the items: a fixed pool with each icon's pixels inside.
 */

#define VIEW_MAX_ITEMS 40
#define VIEW_NAME_CAPACITY 128
#define VIEW_ICON_SIZE 32
#define VIEW_CELL_WIDTH 76
#define VIEW_CELL_HEIGHT 74
#define VIEW_ORIGIN_X 6
#define VIEW_ORIGIN_Y 6
#define VIEW_LABEL_WIDTH 72
#define VIEW_LABEL_TOP 36
#define VIEW_EDIT_WIDTH 140
#define VIEW_DRAG_THRESHOLD 4
#define VIEW_DOUBLE_CLICK_MS 450ul
#define VIEW_RESCAN_MS 2000ul
#define VIEW_POSITIONS_NAME "desktopicons.ini" /* in the profile: savanxp_settings_path */
#define VIEW_POSITIONS_MAX_BYTES 4096

struct view_item
{
    int used;
    char name[VIEW_NAME_CAPACITY];  /* the file's name in the folder */
    char label[VIEW_NAME_CAPACITY]; /* what the icon says: a shortcut drops .sxlnk */
    char path[SHORTCUT_PATH_CAPACITY];
    int is_dir;
    int is_shortcut;
    int column;
    int row;
    int has_icon;
    uint32_t pixels[VIEW_ICON_SIZE * VIEW_ICON_SIZE]; /* BGRA */
};

static struct view_item g_items[VIEW_MAX_ITEMS];
static char g_folder[SHORTCUT_PATH_CAPACITY];
static int g_folder_ready = 0;
static int g_screen_width = 0;
static int g_screen_height = 0;
static unsigned long g_next_scan_ms = 0;

static int g_selected = -1;
/* Left button tracking: the item pressed, where, and whether it became a drag. */
static uint32_t g_last_buttons = 0;
static int g_press_item = -1;
static int g_press_x = 0;
static int g_press_y = 0;
static int g_dragging = 0;
static int g_drag_x = 0;
static int g_drag_y = 0;
static int g_last_click_item = -1;
static unsigned long g_last_click_ms = 0;

/* Rename in place: the item and the text being typed. */
static int g_editing = -1;
static char g_edit[VIEW_NAME_CAPACITY];

/* The item a menu or a question is about. */
static int g_subject = -1;

/* Saved cell of each name, read once from the profile. */
struct view_position
{
    char name[VIEW_NAME_CAPACITY];
    int column;
    int row;
};
static struct view_position g_positions[VIEW_MAX_ITEMS];
static int g_position_count = 0;

/* ---- the folder ----------------------------------------------------------- */

static int path_is_directory(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static int path_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

static int join(char *out, size_t capacity, const char *name)
{
    return snprintf(out, capacity, "%s/%s", g_folder, name) < (int)capacity;
}

/* The first session finds no folder: it gets one with three shortcuts. */
static void create_default_shortcuts(void)
{
    static const char *const k_targets[] = {"/bin/console.sxe", "/bin/filesapp", "/bin/notepad"};
    size_t index;

    for (index = 0; index < sizeof(k_targets) / sizeof(k_targets[0]); ++index)
    {
        struct shortcut shortcut;
        char name[VIEW_NAME_CAPACITY];
        char path[SHORTCUT_PATH_CAPACITY];

        shortcut_for_target(k_targets[index], &shortcut, name, sizeof(name));
        if (join(path, sizeof(path), name) && !path_exists(path))
        {
            (void)shortcut_save(path, &shortcut);
        }
    }
}

static void ensure_folder(void)
{
    snprintf(g_folder, sizeof(g_folder), "%s/Desktop", savanxp_profile_dir());
    if (!path_is_directory(g_folder))
    {
        if (savanxp_mkdir(g_folder) < 0)
        {
            eprintf("desktop: cannot create %s\n", g_folder);
            g_folder_ready = 0;
            return;
        }
        create_default_shortcuts();
    }
    g_folder_ready = 1;
}

/* ---- positions -------------------------------------------------------------- */

static void load_positions(void)
{
    static char text[VIEW_POSITIONS_MAX_BYTES];
    char path[256];
    long fd;
    long length = 0;
    char *line;

    g_position_count = 0;
    if (savanxp_settings_path(VIEW_POSITIONS_NAME, 0, path, sizeof(path)) != 0)
    {
        return;
    }
    fd = savanxp_open_mode(path, SAVANXP_OPEN_READ);
    if (fd < 0)
    {
        return;
    }
    while (length < (long)sizeof(text) - 1)
    {
        long count = savanxp_read((int)fd, text + length, sizeof(text) - 1u - (size_t)length);
        if (count <= 0)
        {
            break;
        }
        length += count;
    }
    savanxp_close((int)fd);
    text[length] = '\0';

    line = text;
    while (*line != '\0' && g_position_count < VIEW_MAX_ITEMS)
    {
        char *end = strchr(line, '\n');
        char *equals;
        char *comma;

        if (end != 0)
        {
            *end = '\0';
        }
        equals = strrchr(line, '=');
        comma = equals != 0 ? strchr(equals, ',') : 0;
        if (equals != 0 && comma != 0 && equals != line)
        {
            struct view_position *position = &g_positions[g_position_count];

            *equals = '\0';
            snprintf(position->name, sizeof(position->name), "%s", line);
            position->column = atoi(equals + 1);
            position->row = atoi(comma + 1);
            g_position_count += 1;
        }
        if (end == 0)
        {
            break;
        }
        line = end + 1;
    }
}

static void save_positions(void)
{
    static char text[VIEW_POSITIONS_MAX_BYTES];
    char path[256];
    size_t used = 0;
    long fd;
    int index;

    for (index = 0; index < VIEW_MAX_ITEMS; ++index)
    {
        const struct view_item *item = &g_items[index];
        int written;

        if (!item->used)
        {
            continue;
        }
        written = snprintf(text + used, sizeof(text) - used, "%s=%d,%d\n", item->name, item->column, item->row);
        if (written < 0 || (size_t)written >= sizeof(text) - used)
        {
            break;
        }
        used += (size_t)written;
    }
    if (savanxp_settings_path(VIEW_POSITIONS_NAME, 1, path, sizeof(path)) != 0)
    {
        return;
    }
    fd = savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (fd < 0)
    {
        return;
    }
    (void)savanxp_write((int)fd, text, used);
    savanxp_close((int)fd);

    /* What is on disk is now the table. */
    g_position_count = 0;
    for (index = 0; index < VIEW_MAX_ITEMS && g_position_count < VIEW_MAX_ITEMS; ++index)
    {
        if (g_items[index].used)
        {
            struct view_position *position = &g_positions[g_position_count++];
            snprintf(position->name, sizeof(position->name), "%s", g_items[index].name);
            position->column = g_items[index].column;
            position->row = g_items[index].row;
        }
    }
}

/* ---- the grid --------------------------------------------------------------- */

static int grid_rows(void)
{
    int work_height = g_screen_height - DESKTOP_TASKBAR_HEIGHT - VIEW_ORIGIN_Y;
    int rows = work_height / VIEW_CELL_HEIGHT;
    return rows > 0 ? rows : 1;
}

static int grid_columns(void)
{
    int columns = (g_screen_width - VIEW_ORIGIN_X) / VIEW_CELL_WIDTH;
    return columns > 0 ? columns : 1;
}

static struct sx_rect cell_rect(int column, int row)
{
    return sx_rect_make(
        VIEW_ORIGIN_X + column * VIEW_CELL_WIDTH,
        VIEW_ORIGIN_Y + row * VIEW_CELL_HEIGHT,
        VIEW_CELL_WIDTH,
        VIEW_CELL_HEIGHT);
}

static int cell_taken(int column, int row, int except)
{
    int index;

    for (index = 0; index < VIEW_MAX_ITEMS; ++index)
    {
        if (index != except && g_items[index].used && g_items[index].column == column && g_items[index].row == row)
        {
            return index;
        }
    }
    return -1;
}

/* The first free cell, top to bottom and then left to right, as Windows
 * arranges a desktop. */
static void place_free(struct view_item *item, int self)
{
    int columns = grid_columns();
    int rows = grid_rows();
    int column;
    int row;

    for (column = 0; column < columns; ++column)
    {
        for (row = 0; row < rows; ++row)
        {
            if (cell_taken(column, row, self) < 0)
            {
                item->column = column;
                item->row = row;
                return;
            }
        }
    }
    item->column = 0;
    item->row = 0;
}

/* An icon left off the screen by a smaller mode comes back inside. */
static void clamp_into_screen(void)
{
    int index;

    for (index = 0; index < VIEW_MAX_ITEMS; ++index)
    {
        struct view_item *item = &g_items[index];

        if (item->used && (item->column >= grid_columns() || item->row >= grid_rows() ||
                           item->column < 0 || item->row < 0))
        {
            place_free(item, index);
        }
    }
}

/* ---- icons ------------------------------------------------------------------ */

static void copy_scaled(uint32_t *out, const uint32_t *pixels, uint32_t width, uint32_t height)
{
    int y;

    for (y = 0; y < VIEW_ICON_SIZE; ++y)
    {
        int x;
        for (x = 0; x < VIEW_ICON_SIZE; ++x)
        {
            uint32_t source_x = (uint32_t)x * width / VIEW_ICON_SIZE;
            uint32_t source_y = (uint32_t)y * height / VIEW_ICON_SIZE;
            out[y * VIEW_ICON_SIZE + x] = pixels[source_y * width + source_x];
        }
    }
}

static int take_icons(struct view_item *item, const struct sxe_icons *icons)
{
    const struct sxe_icon_entry *entry = sxe_icons_best(icons, VIEW_ICON_SIZE);
    const uint32_t *pixels;

    if (entry == 0 || (pixels = sxe_icons_pixels(icons, entry)) == 0 || entry->width == 0 || entry->height == 0)
    {
        return 0;
    }
    copy_scaled(item->pixels, pixels, entry->width, entry->height);
    item->has_icon = 1;
    return 1;
}

static _Alignas(8) uint8_t g_icon_scratch[SXE_ICON_MAX_BYTES];

/* A program's own icon, from its .sxicon section. */
static int load_program_icon(struct view_item *item, const char *program)
{
    struct sxe_icons icons;

    return sxe_load_icons(program, g_icon_scratch, sizeof(g_icon_scratch), &icons) == SXE_OK &&
        take_icons(item, &icons);
}

/* An icon of the file-type catalog in /icons, by name. */
static int load_named_icon(struct view_item *item, const char *name)
{
    struct sxe_icons icons;
    char path[SHORTCUT_PATH_CAPACITY];

    if (name == 0 || name[0] == '\0')
    {
        return 0;
    }
    snprintf(path, sizeof(path), "%s/%s.sxicon", MIME_ICON_DIRECTORY, name);
    return sxe_load_icon_file(path, g_icon_scratch, sizeof(g_icon_scratch), &icons) == SXE_OK &&
        take_icons(item, &icons);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != 0 ? slash + 1 : path;
}

/* What a path looks like: a program's own icon, a folder, or its type's. */
static int load_icon_for_path(struct view_item *item, const char *path, int is_dir)
{
    if (is_dir)
    {
        return load_named_icon(item, mime_icon_name_for(base_name(path), 1, 0));
    }
    if (shortcut_is_program_path(path) && load_program_icon(item, path))
    {
        return 1;
    }
    return load_named_icon(item, mime_icon_name_for(base_name(path), 0, shortcut_is_program_path(path)));
}

static void load_icon(struct view_item *item)
{
    item->has_icon = 0;
    if (item->is_shortcut)
    {
        struct shortcut shortcut;

        if (shortcut_load(item->path, &shortcut) == 0)
        {
            if (shortcut.icon[0] == '/')
            {
                (void)load_program_icon(item, shortcut.icon);
            }
            else if (shortcut.icon[0] != '\0')
            {
                (void)load_named_icon(item, shortcut.icon);
            }
            if (!item->has_icon)
            {
                (void)load_icon_for_path(item, shortcut.target, path_is_directory(shortcut.target));
            }
        }
    }
    else
    {
        (void)load_icon_for_path(item, item->path, item->is_dir);
    }
    /* The universal fallback: the generic program icon. */
    if (!item->has_icon)
    {
        const struct desktop_embedded_bitmap *generic = desktop_icon_large(DESKTOP_ICON_DESKTOP);

        if (generic != 0 && generic->pixels != 0)
        {
            copy_scaled(item->pixels, generic->pixels, generic->width, generic->height);
            item->has_icon = 1;
        }
    }
}

/* ---- scanning --------------------------------------------------------------- */

static int find_item(const char *name)
{
    int index;

    for (index = 0; index < VIEW_MAX_ITEMS; ++index)
    {
        if (g_items[index].used && strcmp(g_items[index].name, name) == 0)
        {
            return index;
        }
    }
    return -1;
}

static void fill_item(struct view_item *item, const char *name)
{
    size_t length;

    memset(item, 0, sizeof(*item));
    item->used = 1;
    snprintf(item->name, sizeof(item->name), "%s", name);
    (void)join(item->path, sizeof(item->path), name);
    item->is_dir = path_is_directory(item->path);
    item->is_shortcut = !item->is_dir && shortcut_is_shortcut_name(name);
    snprintf(item->label, sizeof(item->label), "%s", name);
    length = strlen(item->label);
    if (item->is_shortcut && length > sizeof(SHORTCUT_EXTENSION) - 1u)
    {
        item->label[length - (sizeof(SHORTCUT_EXTENSION) - 1u)] = '\0';
    }
    load_icon(item);
}

/* Re-reads the folder. Items that are still there keep their icon and cell;
 * new ones take their saved cell, or the first free one. Returns 1 if
 * anything changed. */
static int rescan(void)
{
    static char names[VIEW_MAX_ITEMS][VIEW_NAME_CAPACITY];
    int name_count = 0;
    int changed = 0;
    int index;
    DIR *directory;
    struct dirent *entry;

    if (!g_folder_ready)
    {
        return 0;
    }
    directory = opendir(g_folder);
    if (directory == 0)
    {
        return 0;
    }
    while ((entry = readdir(directory)) != 0 && name_count < VIEW_MAX_ITEMS)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }
        snprintf(names[name_count], VIEW_NAME_CAPACITY, "%s", entry->d_name);
        name_count += 1;
    }
    closedir(directory);

    /* Gone from the folder: gone from the screen. */
    for (index = 0; index < VIEW_MAX_ITEMS; ++index)
    {
        int name;
        int present = 0;

        if (!g_items[index].used)
        {
            continue;
        }
        for (name = 0; name < name_count; ++name)
        {
            if (strcmp(names[name], g_items[index].name) == 0)
            {
                present = 1;
                break;
            }
        }
        if (!present)
        {
            g_items[index].used = 0;
            if (g_selected == index)
            {
                g_selected = -1;
            }
            if (g_editing == index)
            {
                g_editing = -1;
            }
            changed = 1;
        }
    }

    for (index = 0; index < name_count; ++index)
    {
        int slot;
        int saved;

        if (find_item(names[index]) >= 0)
        {
            continue;
        }
        for (slot = 0; slot < VIEW_MAX_ITEMS && g_items[slot].used; ++slot)
        {
        }
        if (slot >= VIEW_MAX_ITEMS)
        {
            break;
        }
        fill_item(&g_items[slot], names[index]);
        g_items[slot].column = -1;
        g_items[slot].row = -1;
        for (saved = 0; saved < g_position_count; ++saved)
        {
            if (strcmp(g_positions[saved].name, names[index]) == 0 &&
                cell_taken(g_positions[saved].column, g_positions[saved].row, slot) < 0)
            {
                g_items[slot].column = g_positions[saved].column;
                g_items[slot].row = g_positions[saved].row;
                break;
            }
        }
        if (g_items[slot].column < 0)
        {
            place_free(&g_items[slot], slot);
        }
        changed = 1;
    }
    if (changed)
    {
        clamp_into_screen();
    }
    return changed;
}

/* ---- geometry of an icon ---------------------------------------------------- */

static struct sx_rect icon_rect(const struct view_item *item)
{
    struct sx_rect cell = cell_rect(item->column, item->row);
    return sx_rect_make(cell.x + (VIEW_CELL_WIDTH - VIEW_ICON_SIZE) / 2, cell.y + 2, VIEW_ICON_SIZE, VIEW_ICON_SIZE);
}

/* The label split to fit: at most two lines, by words, a word too long cut
 * by characters, and "..." when the rest does not fit. */
static int wrap_label(const char *text, int width, char lines[2][VIEW_NAME_CAPACITY])
{
    size_t length = strlen(text);
    size_t start = 0;
    int line;

    lines[0][0] = '\0';
    lines[1][0] = '\0';
    for (line = 0; line < 2 && start < length; ++line)
    {
        size_t end = start;
        size_t best = start;
        char candidate[VIEW_NAME_CAPACITY];

        /* The longest piece that fits, preferring a cut at a space. */
        while (end < length)
        {
            size_t next = end + 1;
            memcpy(candidate, text + start, next - start);
            candidate[next - start] = '\0';
            if (gfx_text_width(candidate) > width)
            {
                break;
            }
            end = next;
            if (end == length || text[end] == ' ')
            {
                best = end;
            }
        }
        if (best == start)
        {
            best = end > start ? end : start + 1;
        }
        if (line == 1 && best < length)
        {
            /* The last line cannot hold the rest: it ends in "...". */
            size_t cut = best;
            for (;;)
            {
                snprintf(candidate, sizeof(candidate), "%.*s...", (int)(cut - start), text + start);
                if (gfx_text_width(candidate) <= width || cut <= start)
                {
                    break;
                }
                --cut;
            }
            snprintf(lines[line], VIEW_NAME_CAPACITY, "%s", candidate);
            return 2;
        }
        snprintf(lines[line], VIEW_NAME_CAPACITY, "%.*s", (int)(best - start), text + start);
        start = best;
        while (start < length && text[start] == ' ')
        {
            ++start;
        }
    }
    return lines[1][0] != '\0' ? 2 : 1;
}

/* The label's box under the icon, for painting and hit testing. */
static struct sx_rect label_rect(const struct view_item *item, char lines[2][VIEW_NAME_CAPACITY], int *line_count)
{
    struct sx_rect cell = cell_rect(item->column, item->row);
    int count = wrap_label(item->label, VIEW_LABEL_WIDTH, lines);
    int widest = gfx_text_width(lines[0]);
    int line_height = gfx_text_height() + 1;

    if (count > 1 && gfx_text_width(lines[1]) > widest)
    {
        widest = gfx_text_width(lines[1]);
    }
    *line_count = count;
    return sx_rect_make(
        cell.x + (VIEW_CELL_WIDTH - widest) / 2 - 2,
        cell.y + VIEW_LABEL_TOP,
        widest + 4,
        count * line_height + 2);
}

static int item_at(int x, int y)
{
    int index;

    for (index = 0; index < VIEW_MAX_ITEMS; ++index)
    {
        char lines[2][VIEW_NAME_CAPACITY];
        int count;
        const struct view_item *item = &g_items[index];

        if (!item->used)
        {
            continue;
        }
        if (sx_rect_contains_point(icon_rect(item), x, y) ||
            sx_rect_contains_point(label_rect(item, lines, &count), x, y))
        {
            return index;
        }
    }
    return -1;
}

/* ---- painting --------------------------------------------------------------- */

static void draw_icon_pixels(struct sx_painter *painter, const uint32_t *pixels, int x, int y)
{
    struct sx_bitmap bitmap;
    struct savanxp_fb_info info;

    info.width = VIEW_ICON_SIZE;
    info.height = VIEW_ICON_SIZE;
    info.pitch = VIEW_ICON_SIZE * (uint32_t)sizeof(uint32_t);
    info.bpp = 32u;
    info.buffer_size = info.pitch * VIEW_ICON_SIZE;
    sx_bitmap_wrap(&bitmap, (uint32_t *)pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    sx_painter_blit_bitmap(painter, &bitmap, x, y);
}

/* A selected icon is tinted with the selection colour, half and half, as
 * Windows draws it. */
static void draw_item_icon(struct sx_painter *painter, const struct view_item *item, int x, int y, int selected)
{
    static uint32_t tinted[VIEW_ICON_SIZE * VIEW_ICON_SIZE];

    if (!item->has_icon)
    {
        return;
    }
    if (selected)
    {
        uint32_t tint = SXGUI_COLOR_SELECT;
        int index;

        for (index = 0; index < VIEW_ICON_SIZE * VIEW_ICON_SIZE; ++index)
        {
            uint32_t pixel = item->pixels[index];
            uint32_t alpha = pixel & 0xFF000000u;
            uint32_t blue = ((pixel & 0xFFu) + (tint & 0xFFu)) / 2u;
            uint32_t green = (((pixel >> 8) & 0xFFu) + ((tint >> 8) & 0xFFu)) / 2u;
            uint32_t red = (((pixel >> 16) & 0xFFu) + ((tint >> 16) & 0xFFu)) / 2u;

            tinted[index] = alpha | (red << 16) | (green << 8) | blue;
        }
        draw_icon_pixels(painter, tinted, x, y);
    }
    else
    {
        draw_icon_pixels(painter, item->pixels, x, y);
    }

    /* A shortcut carries the small arrow in its lower left corner. */
    if (item->is_shortcut)
    {
        int base_x = x;
        int base_y = y + VIEW_ICON_SIZE - 11;
        int step;

        sx_painter_fill_rect(painter, sx_rect_make(base_x, base_y, 11, 11), 0x000000u);
        sx_painter_fill_rect(painter, sx_rect_make(base_x + 1, base_y + 1, 9, 9), 0xFFFFFFu);
        for (step = 0; step < 5; ++step)
        {
            sx_painter_fill_rect(painter, sx_rect_make(base_x + 3 + step, base_y + 7 - step, 1, 1), 0x000000u);
        }
        sx_painter_fill_rect(painter, sx_rect_make(base_x + 5, base_y + 3, 3, 1), 0x000000u);
        sx_painter_fill_rect(painter, sx_rect_make(base_x + 7, base_y + 3, 1, 3), 0x000000u);
    }
}

static void draw_label(struct sx_painter *painter, const struct view_item *item, int selected)
{
    char lines[2][VIEW_NAME_CAPACITY];
    int count = 0;
    struct sx_rect box = label_rect(item, lines, &count);
    struct sx_rect cell = cell_rect(item->column, item->row);
    int line_height = gfx_text_height() + 1;
    int line;

    if (selected)
    {
        sx_painter_fill_rect(painter, box, SXGUI_COLOR_SELECT);
    }
    for (line = 0; line < count; ++line)
    {
        int text_x = cell.x + (VIEW_CELL_WIDTH - gfx_text_width(lines[line])) / 2;
        int text_y = box.y + 1 + line * line_height;

        if (!selected)
        {
            /* A shadow keeps white text readable on any wallpaper. */
            sx_painter_draw_text(painter, text_x + 1, text_y + 1, lines[line], 0x000000u);
        }
        sx_painter_draw_text(painter, text_x, text_y, lines[line], 0xFFFFFFu);
    }
}

static void draw_edit_box(struct sx_painter *painter, const struct view_item *item)
{
    struct sx_rect cell = cell_rect(item->column, item->row);
    int text_width = gfx_text_width(g_edit);
    int width = text_width + 8 > VIEW_EDIT_WIDTH ? text_width + 8 : VIEW_EDIT_WIDTH;
    int height = gfx_text_height() + 4;
    struct sx_rect box = sx_rect_make(cell.x + (VIEW_CELL_WIDTH - width) / 2, cell.y + VIEW_LABEL_TOP, width, height);

    if (box.x < 0)
    {
        box.x = 0;
    }
    sx_painter_fill_rect(painter, box, 0x000000u);
    sx_painter_fill_rect(painter, sx_rect_make(box.x + 1, box.y + 1, box.width - 2, box.height - 2), 0xFFFFFFu);
    sx_painter_draw_text(painter, box.x + 3, box.y + 2, g_edit, 0x000000u);
    /* The caret, at the end: the edit only appends and deletes backwards. */
    sx_painter_fill_rect(painter, sx_rect_make(box.x + 3 + text_width, box.y + 2, 1, gfx_text_height()), 0x000000u);
}

void desktop_view_paint(struct sx_painter *painter, const struct savanxp_fb_info *info)
{
    int index;

    g_screen_width = (int)info->width;
    g_screen_height = (int)info->height;
    for (index = 0; index < VIEW_MAX_ITEMS; ++index)
    {
        const struct view_item *item = &g_items[index];
        struct sx_rect icon;
        int selected = index == g_selected;

        if (!item->used || (g_dragging && index == g_press_item))
        {
            continue;
        }
        icon = icon_rect(item);
        draw_item_icon(painter, item, icon.x, icon.y, selected);
        if (index == g_editing)
        {
            draw_edit_box(painter, item);
        }
        else
        {
            draw_label(painter, item, selected);
        }
    }
    /* The icon being dragged follows the pointer, on top of the rest. */
    if (g_dragging && g_press_item >= 0 && g_items[g_press_item].used)
    {
        const struct view_item *item = &g_items[g_press_item];
        struct sx_rect icon = icon_rect(item);

        draw_item_icon(painter, item, icon.x + g_drag_x - g_press_x, icon.y + g_drag_y - g_press_y, 1);
    }
}

/* ---- commands ----------------------------------------------------------------- */

#define VIEW_CMD_ARRANGE 1
#define VIEW_CMD_REFRESH 2
#define VIEW_CMD_NEW_FOLDER 3
#define VIEW_CMD_NEW_TEXT 4
#define VIEW_CMD_PROPERTIES 5
#define VIEW_CMD_CONSOLE 6
#define VIEW_CMD_OPEN 10
#define VIEW_CMD_DELETE 11
#define VIEW_CMD_RENAME 12

/* The Command Prompt, as Ctrl+Alt+T opens it (docs/desktop/CONSOLE.md). */
static const char *k_console_path = "/bin/console.sxe";
/* Display Properties (docs/desktop/DISPLAY_PROPERTIES.md). */
static const char *k_display_path = "/bin/display";

static const struct sxgui_menu_item k_background_items[] = {
    {"Arrange Icons", VIEW_CMD_ARRANGE, 0},
    {"Refresh", VIEW_CMD_REFRESH, 0},
    {0, 0, 0},
    {"New Folder", VIEW_CMD_NEW_FOLDER, 0},
    {"New Text Document", VIEW_CMD_NEW_TEXT, 0},
    {0, 0, 0},
    {"Open Command Prompt", VIEW_CMD_CONSOLE, 0},
    {0, 0, 0},
    {"Properties", VIEW_CMD_PROPERTIES, 0},
};

static const struct sxgui_menu k_background_menu = {
    "Desktop",
    k_background_items,
    (int)(sizeof(k_background_items) / sizeof(k_background_items[0])),
};

static const struct sxgui_menu_item k_item_items[] = {
    {"Open", VIEW_CMD_OPEN, 0},
    {0, 0, 0},
    {"Rename", VIEW_CMD_RENAME, 0},
    {"Delete", VIEW_CMD_DELETE, 0},
};

static const struct sxgui_menu k_item_menu = {
    "Icon",
    k_item_items,
    (int)(sizeof(k_item_items) / sizeof(k_item_items[0])),
};

static int subject_valid(void)
{
    return g_subject >= 0 && g_subject < VIEW_MAX_ITEMS && g_items[g_subject].used;
}

static void open_item(int index)
{
    if (index < 0 || !g_items[index].used)
    {
        return;
    }
    if (shortcut_open_path(desktop_main_window(), g_items[index].path) == SHORTCUT_OPEN_FAILED)
    {
        char text[VIEW_NAME_CAPACITY + 32];

        snprintf(text, sizeof(text), "Cannot open '%s'.", g_items[index].label);
        desktop_confirm("Desktop", text, 0, 0);
    }
}

static void begin_rename(int index)
{
    if (index < 0 || !g_items[index].used)
    {
        return;
    }
    g_editing = index;
    g_selected = index;
    snprintf(g_edit, sizeof(g_edit), "%s", g_items[index].label);
    desktop_repaint_background();
}

static void end_rename(int commit)
{
    struct view_item *item;
    char name[VIEW_NAME_CAPACITY];
    char path[SHORTCUT_PATH_CAPACITY];
    int index = g_editing;

    g_editing = -1;
    desktop_repaint_background();
    if (!commit || index < 0 || !g_items[index].used)
    {
        return;
    }
    item = &g_items[index];
    if (strcmp(g_edit, item->label) == 0)
    {
        return;
    }
    if (g_edit[0] == '\0' || strchr(g_edit, '/') != 0 || strcmp(g_edit, ".") == 0 || strcmp(g_edit, "..") == 0)
    {
        desktop_confirm("Rename", "A name cannot be empty or contain '/'.", 0, 0);
        return;
    }
    snprintf(name, sizeof(name), "%s%s", g_edit, item->is_shortcut ? SHORTCUT_EXTENSION : "");
    if (!join(path, sizeof(path), name) || path_exists(path) || rename(item->path, path) != 0)
    {
        char text[VIEW_NAME_CAPACITY + 48];

        snprintf(text, sizeof(text), "Cannot rename to '%s'.", g_edit);
        desktop_confirm("Rename", text, 0, 0);
        return;
    }
    /* The item keeps its cell under its new name. */
    snprintf(item->name, sizeof(item->name), "%s", name);
    snprintf(item->path, sizeof(item->path), "%s", path);
    snprintf(item->label, sizeof(item->label), "%s", g_edit);
    save_positions();
}

static void delete_answer(int yes)
{
    char text[VIEW_NAME_CAPACITY + 48];
    struct view_item *item;
    int result;

    if (!yes || !subject_valid())
    {
        return;
    }
    item = &g_items[g_subject];
    result = item->is_dir ? rmdir(item->path) : unlink(item->path);
    if (result != 0)
    {
        snprintf(text, sizeof(text), item->is_dir ? "Cannot delete '%s'. Is the folder empty?" : "Cannot delete '%s'.",
            item->label);
        desktop_confirm("Delete", text, 0, 0);
        return;
    }
    if (rescan())
    {
        save_positions();
    }
    desktop_repaint_background();
}

static void ask_delete(int index)
{
    char text[VIEW_NAME_CAPACITY + 48];

    if (index < 0 || !g_items[index].used)
    {
        return;
    }
    g_subject = index;
    snprintf(text, sizeof(text), "Delete '%s'?", g_items[index].label);
    desktop_confirm(g_items[index].is_dir ? "Confirm Folder Delete" : "Confirm File Delete", text, 1, delete_answer);
}

/* By name, folders first, in columns from the top left. */
static void arrange_icons(void)
{
    int order[VIEW_MAX_ITEMS];
    int count = 0;
    int index;
    int rows = grid_rows();

    for (index = 0; index < VIEW_MAX_ITEMS; ++index)
    {
        if (g_items[index].used)
        {
            order[count++] = index;
        }
    }
    for (index = 1; index < count; ++index)
    {
        int current = order[index];
        int at = index - 1;

        while (at >= 0)
        {
            const struct view_item *a = &g_items[order[at]];
            const struct view_item *b = &g_items[current];
            int before = a->is_dir != b->is_dir ? a->is_dir < b->is_dir : strcasecmp(a->label, b->label) > 0;

            if (!before)
            {
                break;
            }
            order[at + 1] = order[at];
            --at;
        }
        order[at + 1] = current;
    }
    for (index = 0; index < count; ++index)
    {
        g_items[order[index]].column = index / rows;
        g_items[order[index]].row = index % rows;
    }
    save_positions();
}

/* "New Folder", or "New Folder (2)" and on when it is taken. */
static int unique_name(const char *stem, const char *extension, char *out, size_t capacity)
{
    int attempt;

    for (attempt = 1; attempt < 100; ++attempt)
    {
        char path[SHORTCUT_PATH_CAPACITY];

        if (attempt == 1)
        {
            snprintf(out, capacity, "%s%s", stem, extension);
        }
        else
        {
            snprintf(out, capacity, "%s (%d)%s", stem, attempt, extension);
        }
        if (join(path, sizeof(path), out) && !path_exists(path))
        {
            return 1;
        }
    }
    return 0;
}

static void create_new(int folder)
{
    char name[VIEW_NAME_CAPACITY];
    char path[SHORTCUT_PATH_CAPACITY];
    int created = 0;
    int index;

    if (!unique_name(folder ? "New Folder" : "New Text Document", folder ? "" : ".txt", name, sizeof(name)) ||
        !join(path, sizeof(path), name))
    {
        return;
    }
    if (folder)
    {
        created = savanxp_mkdir(path) >= 0;
    }
    else
    {
        long fd = savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
        if (fd >= 0)
        {
            savanxp_close((int)fd);
            created = 1;
        }
    }
    if (!created)
    {
        desktop_confirm("Desktop", folder ? "Cannot create the folder." : "Cannot create the document.", 0, 0);
        return;
    }
    (void)rescan();
    save_positions();
    index = find_item(name);
    /* As on Windows, the new icon starts with its name being edited. */
    begin_rename(index);
}

static void run_command(int id)
{
    switch (id)
    {
    case VIEW_CMD_ARRANGE:
        arrange_icons();
        break;
    case VIEW_CMD_REFRESH:
        (void)rescan();
        break;
    case VIEW_CMD_NEW_FOLDER:
        create_new(1);
        break;
    case VIEW_CMD_NEW_TEXT:
        create_new(0);
        break;
    case VIEW_CMD_PROPERTIES:
        (void)desktop_launch(k_display_path, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE);
        break;
    case VIEW_CMD_CONSOLE:
        (void)desktop_launch(k_console_path, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE);
        break;
    case VIEW_CMD_OPEN:
        if (subject_valid())
        {
            open_item(g_subject);
        }
        break;
    case VIEW_CMD_RENAME:
        if (subject_valid())
        {
            begin_rename(g_subject);
        }
        break;
    case VIEW_CMD_DELETE:
        if (subject_valid())
        {
            ask_delete(g_subject);
        }
        break;
    default:
        break;
    }
    desktop_repaint_background();
}

/* The menu for the item under (x, y), or the background's. */
static void show_menu(int index, int x, int y)
{
    g_subject = index;
    if (index >= 0)
    {
        g_selected = index;
        desktop_repaint_background();
        desktop_context_menu(&k_item_menu, run_command, x, y);
    }
    else
    {
        desktop_context_menu(&k_background_menu, run_command, x, y);
    }
}

/* ---- input ---------------------------------------------------------------------- */

uint32_t desktop_view_pointer(const struct savanxp_gui_pointer_event *pointer)
{
    uint32_t down = pointer->buttons & ~g_last_buttons;
    uint32_t up = g_last_buttons & ~pointer->buttons;
    uint32_t actions = DESKTOP_ACTION_NONE;
    int hit = item_at(pointer->x, pointer->y);

    g_last_buttons = pointer->buttons;

    if ((down & (SAVANXP_MOUSE_BUTTON_LEFT | SAVANXP_MOUSE_BUTTON_RIGHT)) != 0 && g_editing >= 0)
    {
        /* A click anywhere else keeps the name typed so far. */
        end_rename(1);
        actions |= DESKTOP_ACTION_REPAINT;
    }

    if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        unsigned long now = uptime_ms();

        if (hit >= 0 && hit == g_last_click_item && now - g_last_click_ms <= VIEW_DOUBLE_CLICK_MS)
        {
            g_last_click_item = -1;
            g_press_item = -1;
            open_item(hit);
            return actions | DESKTOP_ACTION_REPAINT;
        }
        g_last_click_item = hit;
        g_last_click_ms = now;
        g_press_item = hit;
        g_press_x = pointer->x;
        g_press_y = pointer->y;
        g_dragging = 0;
        if (g_selected != hit)
        {
            g_selected = hit;
            actions |= DESKTOP_ACTION_REPAINT;
        }
    }
    else if ((pointer->buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0 && g_press_item >= 0)
    {
        int dx = pointer->x - g_press_x;
        int dy = pointer->y - g_press_y;

        if (!g_dragging && (dx * dx + dy * dy) > VIEW_DRAG_THRESHOLD * VIEW_DRAG_THRESHOLD)
        {
            g_dragging = 1;
        }
        if (g_dragging)
        {
            g_drag_x = pointer->x;
            g_drag_y = pointer->y;
            actions |= DESKTOP_ACTION_REPAINT;
        }
    }
    else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        if (g_dragging && g_press_item >= 0 && g_items[g_press_item].used)
        {
            struct view_item *item = &g_items[g_press_item];
            struct sx_rect cell = cell_rect(item->column, item->row);
            int center_x = cell.x + VIEW_CELL_WIDTH / 2 + (pointer->x - g_press_x);
            int center_y = cell.y + VIEW_CELL_HEIGHT / 2 + (pointer->y - g_press_y);
            int column = (center_x - VIEW_ORIGIN_X) / VIEW_CELL_WIDTH;
            int row = (center_y - VIEW_ORIGIN_Y) / VIEW_CELL_HEIGHT;
            int other;

            column = column < 0 ? 0 : (column >= grid_columns() ? grid_columns() - 1 : column);
            row = row < 0 ? 0 : (row >= grid_rows() ? grid_rows() - 1 : row);
            /* Dropped on another icon: the two trade cells. */
            other = cell_taken(column, row, g_press_item);
            if (other >= 0)
            {
                g_items[other].column = item->column;
                g_items[other].row = item->row;
            }
            item->column = column;
            item->row = row;
            save_positions();
            g_last_click_item = -1;
        }
        g_dragging = 0;
        g_press_item = -1;
        actions |= DESKTOP_ACTION_REPAINT;
    }

    if ((down & SAVANXP_MOUSE_BUTTON_RIGHT) != 0 && g_selected != hit)
    {
        g_selected = hit;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    /* On the release, as Windows sends WM_CONTEXTMENU: the press is over by
     * the time the menu is under the pointer. */
    if ((up & SAVANXP_MOUSE_BUTTON_RIGHT) != 0)
    {
        show_menu(hit, pointer->x, pointer->y);
        actions |= DESKTOP_ACTION_REPAINT;
    }
    return actions;
}

/* The nearest item from the selected one in a direction, by cells. */
static int item_toward(int dx, int dy)
{
    int best = -1;
    int best_distance = 0x7FFFFFFF;
    int index;
    const struct view_item *from;

    if (g_selected < 0 || !g_items[g_selected].used)
    {
        for (index = 0; index < VIEW_MAX_ITEMS; ++index)
        {
            if (g_items[index].used &&
                (best < 0 || g_items[index].column < g_items[best].column ||
                 (g_items[index].column == g_items[best].column && g_items[index].row < g_items[best].row)))
            {
                best = index;
            }
        }
        return best;
    }
    from = &g_items[g_selected];
    for (index = 0; index < VIEW_MAX_ITEMS; ++index)
    {
        const struct view_item *item = &g_items[index];
        int along;
        int across;
        int distance;

        if (!item->used || index == g_selected)
        {
            continue;
        }
        along = dx != 0 ? (item->column - from->column) * dx : (item->row - from->row) * dy;
        across = dx != 0 ? item->row - from->row : item->column - from->column;
        if (along <= 0)
        {
            continue;
        }
        across = across < 0 ? -across : across;
        distance = along * 4 + across * 5;
        if (distance < best_distance)
        {
            best_distance = distance;
            best = index;
        }
    }
    return best >= 0 ? best : g_selected;
}

/* The next item whose label starts with the letter, after the selected one. */
static int item_by_letter(int letter)
{
    int index;
    int start = g_selected >= 0 ? g_selected + 1 : 0;

    if (letter >= 'A' && letter <= 'Z')
    {
        letter = letter - 'A' + 'a';
    }
    for (index = 0; index < VIEW_MAX_ITEMS; ++index)
    {
        int candidate = (start + index) % VIEW_MAX_ITEMS;
        int first;

        if (!g_items[candidate].used)
        {
            continue;
        }
        first = g_items[candidate].label[0];
        if (first >= 'A' && first <= 'Z')
        {
            first = first - 'A' + 'a';
        }
        if (first == letter)
        {
            return candidate;
        }
    }
    return g_selected;
}

static uint32_t rename_key(const struct savanxp_input_event *event)
{
    size_t length = strlen(g_edit);

    switch (event->key)
    {
    case SAVANXP_KEY_ENTER:
        end_rename(1);
        break;
    case SAVANXP_KEY_ESC:
        end_rename(0);
        break;
    case SAVANXP_KEY_BACKSPACE:
        if (length > 0)
        {
            g_edit[length - 1] = '\0';
        }
        break;
    default:
        if (event->ascii >= 32 && event->ascii < 127 && length + 1 < sizeof(g_edit) &&
            (event->modifiers & (SAVANXP_KEY_MOD_CTRL | SAVANXP_KEY_MOD_ALT)) == 0)
        {
            g_edit[length] = (char)event->ascii;
            g_edit[length + 1] = '\0';
        }
        break;
    }
    return DESKTOP_ACTION_REPAINT;
}

uint32_t desktop_view_key(const struct savanxp_input_event *event)
{
    int shift = (event->modifiers & SAVANXP_KEY_MOD_SHIFT) != 0;
    int target = g_selected;

    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return DESKTOP_ACTION_NONE;
    }
    if (g_editing >= 0)
    {
        return rename_key(event);
    }
    switch (event->key)
    {
    case SAVANXP_KEY_LEFT:
        target = item_toward(-1, 0);
        break;
    case SAVANXP_KEY_RIGHT:
        target = item_toward(1, 0);
        break;
    case SAVANXP_KEY_UP:
        target = item_toward(0, -1);
        break;
    case SAVANXP_KEY_DOWN:
        target = item_toward(0, 1);
        break;
    case SAVANXP_KEY_ENTER:
        open_item(g_selected);
        return DESKTOP_ACTION_REPAINT;
    case SAVANXP_KEY_DELETE:
        ask_delete(g_selected);
        return DESKTOP_ACTION_REPAINT;
    case SAVANXP_KEY_F2:
        begin_rename(g_selected);
        return DESKTOP_ACTION_REPAINT;
    case SAVANXP_KEY_F5:
        (void)rescan();
        return DESKTOP_ACTION_REPAINT;
    case SAVANXP_KEY_ESC:
        target = -1;
        break;
    case SAVANXP_KEY_MENU:
    case SAVANXP_KEY_F10:
        if (event->key == SAVANXP_KEY_F10 && !shift)
        {
            return DESKTOP_ACTION_NONE;
        }
        /* The keyboard's right click: at the selected icon, or at the corner. */
        if (g_selected >= 0 && g_items[g_selected].used)
        {
            struct sx_rect icon = icon_rect(&g_items[g_selected]);
            show_menu(g_selected, icon.x + icon.width / 2, icon.y + icon.height / 2);
        }
        else
        {
            show_menu(-1, VIEW_ORIGIN_X, VIEW_ORIGIN_Y);
        }
        return DESKTOP_ACTION_REPAINT;
    default:
        if (event->ascii > 32 && event->ascii < 127 && (event->modifiers & (SAVANXP_KEY_MOD_CTRL | SAVANXP_KEY_MOD_ALT)) == 0)
        {
            target = item_by_letter(event->ascii);
            break;
        }
        return DESKTOP_ACTION_NONE;
    }
    if (target != g_selected)
    {
        g_selected = target;
        return DESKTOP_ACTION_REPAINT;
    }
    return DESKTOP_ACTION_NONE;
}

/* ---- lifetime ------------------------------------------------------------------- */

void desktop_view_init(const struct savanxp_fb_info *info)
{
    g_screen_width = (int)info->width;
    g_screen_height = (int)info->height;
    (void)mime_icon_load();
    ensure_folder();
    load_positions();
    (void)rescan();
    g_next_scan_ms = uptime_ms() + VIEW_RESCAN_MS;
}

void desktop_view_resize(const struct savanxp_fb_info *info)
{
    g_screen_width = (int)info->width;
    g_screen_height = (int)info->height;
    clamp_into_screen();
}

int desktop_view_tick(void)
{
    unsigned long now = uptime_ms();

    if (now < g_next_scan_ms || g_dragging || g_editing >= 0)
    {
        return 0;
    }
    g_next_scan_ms = now + VIEW_RESCAN_MS;
    if (!g_folder_ready)
    {
        ensure_folder();
    }
    return rescan();
}
