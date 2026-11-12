#include "libc.h"
#include "savanxp/sxgui.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

#include "shared/version.h"

#include "file_assoc.h"
#include "sxgfx/mime_icon.h"
#include "shortcut.h"

#include <string.h>
#include <unistd.h>

/*
 * File browser, shaped like the Windows 98 Explorer: menu bar, a toolbar
 * (Back, Forward, Up, Delete, Properties, Views), an editable address bar,
 * the folder as Large Icons or as a details list (Name / Size / Type) with
 * column headers -- View picks one and filesapp.cfg in the profile remembers
 * it -- and a two-pane status bar (docs/desktop/FILES.md).
 *
 * It does NOT show what files contain: that is an editor's job. Opening a file
 * goes through shortcut_open_path, as on the desktop: a program starts, a
 * document goes to the program ASSOCIATED with its extension (file_assoc,
 * docs/userland/SXE_FORMAT.md phase 5) or to /bin/notepad, and a shortcut opens
 * its target. A right click on an item opens its context menu: Open, Send To
 * Desktop (a shortcut in the Desktop folder), Rename, Delete and Properties
 * (docs/desktop/DESKTOP_ICONS.md#phase-4-context-menus-in-applications).
 */

/* ---- toolbar glyphs ----------------------------------------------------------
 * Drawn by system/gfx/toolbar_glyphs/gen_icons.py, embedded (see that file
 * for why). */
#include "../../../system/gfx/toolbar_glyphs/icons.inc"

static struct sx_bitmap g_glyph_bitmaps[6];
/* One-element arrays: a toolbar button reads its glyph from item_icons[0]. */
static const struct sx_bitmap *k_glyph_back[1];
static const struct sx_bitmap *k_glyph_forward[1];
static const struct sx_bitmap *k_glyph_up[1];
static const struct sx_bitmap *k_glyph_delete[1];
static const struct sx_bitmap *k_glyph_properties[1];
static const struct sx_bitmap *k_glyph_views[1];

static const struct sx_bitmap *filesapp_glyph(int slot, const uint32_t *pixels)
{
    struct savanxp_fb_info info;

    memset(&info, 0, sizeof(info));
    info.width = TOOLBAR_GLYPH_SIZE;
    info.height = TOOLBAR_GLYPH_SIZE;
    info.pitch = TOOLBAR_GLYPH_SIZE * (uint32_t)sizeof(uint32_t);
    info.bpp = 32u;
    info.buffer_size = info.pitch * TOOLBAR_GLYPH_SIZE;
    sx_bitmap_wrap(&g_glyph_bitmaps[slot], (uint32_t *)pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    return &g_glyph_bitmaps[slot];
}

static void filesapp_build_toolbar_glyphs(void)
{
    k_glyph_back[0] = filesapp_glyph(0, toolbar_glyph_go_previous);
    k_glyph_forward[0] = filesapp_glyph(1, toolbar_glyph_go_next);
    k_glyph_up[0] = filesapp_glyph(2, toolbar_glyph_go_up);
    k_glyph_delete[0] = filesapp_glyph(3, toolbar_glyph_edit_delete);
    k_glyph_properties[0] = filesapp_glyph(4, toolbar_glyph_document_properties);
    k_glyph_views[0] = filesapp_glyph(5, toolbar_glyph_view_choose);
}

#define FILESAPP_PATH_CAPACITY 256
#define FILESAPP_MAX_ENTRIES 128
#define FILESAPP_LABEL_LENGTH 96

#define FILESAPP_MENU_OPEN 1
#define FILESAPP_MENU_UP 2
#define FILESAPP_MENU_REFRESH 3
#define FILESAPP_MENU_VIEW_ICONS 4
#define FILESAPP_MENU_VIEW_DETAILS 5
#define FILESAPP_MENU_ABOUT 8
#define FILESAPP_MENU_CLOSE 9
#define FILESAPP_MENU_BACK 10
#define FILESAPP_MENU_FORWARD 11
#define FILESAPP_MENU_DELETE 12
#define FILESAPP_MENU_RENAME 13
#define FILESAPP_MENU_PROPERTIES 14

/* Back and Forward each remember this many folders; the oldest goes first. */
#define FILESAPP_HISTORY_DEPTH 16

struct filesapp_entry
{
    char name[256];
    unsigned int size;
    int is_dir;
};

/* The window's widgets, in painting and Tab order: the toolbar band, the
 * address band, the list and the status bar. */
enum filesapp_widget
{
    W_RULE_MENU,
    W_BACK,
    W_FORWARD,
    W_SEP_HISTORY,
    W_UP,
    W_SEP_UP,
    W_DELETE,
    W_PROPERTIES,
    W_SEP_EDIT,
    W_VIEWS,
    W_RULE_TOOLBAR,
    W_ADDRESS_LABEL,
    W_ADDRESS,
    W_LIST,
    W_STATUS_COUNT,
    W_STATUS_SIZE,
    W_COUNT
};

static struct sxgui_app g_app;
static struct sxgui_widget g_widgets[W_COUNT];

static struct filesapp_entry g_entries[FILESAPP_MAX_ENTRIES];
static char g_item_labels[FILESAPP_MAX_ENTRIES][FILESAPP_LABEL_LENGTH];
static const char *g_item_ptrs[FILESAPP_MAX_ENTRIES];
/* Parallel to g_item_ptrs: each row's icon, or 0. One array per size: the
 * details list draws the 16x16 ones, Large Icons the 32x32 ones. */
static const struct sx_bitmap *g_item_icons[FILESAPP_MAX_ENTRIES];
static const struct sx_bitmap *g_item_large_icons[FILESAPP_MAX_ENTRIES];
static char g_current_path[FILESAPP_PATH_CAPACITY] = "/";
/* The address bar's own text: what the user types, until Enter goes there.
 * Every folder change writes the current path back into it. */
static char g_address[FILESAPP_PATH_CAPACITY] = "/";
static char g_back[FILESAPP_HISTORY_DEPTH][FILESAPP_PATH_CAPACITY];
static int g_back_count = 0;
/* The entry a menu or a dialog is about, by name: the list may be reloaded
 * in between. */
static char g_subject[256];
static char g_forward[FILESAPP_HISTORY_DEPTH][FILESAPP_PATH_CAPACITY];
static int g_forward_count = 0;
static char g_count_pane[64] = "0 object(s)";
static char g_size_pane[64] = "";
static int g_entry_count = 0;
static int g_mime_icons_loaded = 0;

static struct sxgui_dialog g_about_dialog;
static struct sxgui_widget g_about_widgets[4];

#define FILESAPP_ADDRESS (&g_widgets[W_ADDRESS])
#define FILESAPP_LIST_INDEX W_LIST
#define FILESAPP_LIST (&g_widgets[FILESAPP_LIST_INDEX])

/* Defined further down, used before: the commands reach across sections. */
static void filesapp_update_commands(void);
static void filesapp_ask_delete(void);
static void filesapp_ask_rename(void);
static void filesapp_show_properties(void);

/* Anchos fijos; la columna Name se lleva el resto y se recalcula en el layout,
 * que es lo que hace que la lista se estire con la ventana. */
static struct sxgui_column g_columns[] = {
    {"Name", 240, 0},
    {"Size", 80, SXGUI_COLUMN_RIGHT},
    {"Type", 130, 0},
};

#define FILESAPP_COLUMN_COUNT ((int)(sizeof(g_columns) / sizeof(g_columns[0])))

#define FILESAPP_COLUMN_SIZE_WIDTH 80
#define FILESAPP_COLUMN_TYPE_WIDTH 130

/* Grilla del Acerca de: la misma que usan los dialogos de notepad. */
#define FILESAPP_ABOUT_WIDTH 320
#define FILESAPP_ABOUT_HEIGHT 128
#define FILESAPP_DLG_ROW (18 + 4)

/* ---- helpers de path ------------------------------------------------------ */

static int filesapp_is_root_path(const char *path)
{
    return path != 0 && path[0] == '/' && path[1] == '\0';
}

static int filesapp_path_is_launchable(const char *path)
{
    return path != 0 &&
        (strncmp(path, "/bin/", 5) == 0 || strncmp(path, "/disk/bin/", 10) == 0);
}

static int filesapp_join_path(const char *base, const char *name, char *buffer, size_t capacity)
{
    if (buffer == 0 || capacity == 0 || name == 0 || name[0] == '\0')
    {
        return 0;
    }
    if (base == 0 || base[0] == '\0' || strcmp(base, "/") == 0)
    {
        return snprintf(buffer, capacity, "/%s", name) < (int)capacity;
    }
    return snprintf(buffer, capacity, "%s/%s", base, name) < (int)capacity;
}

static void filesapp_parent_path(const char *path, char *buffer, size_t capacity)
{
    size_t length;

    if (buffer == 0 || capacity == 0)
    {
        return;
    }
    if (path == 0 || filesapp_is_root_path(path))
    {
        snprintf(buffer, capacity, "/");
        return;
    }

    snprintf(buffer, capacity, "%s", path);
    length = strlen(buffer);
    while (length > 1 && buffer[length - 1] == '/')
    {
        buffer[length - 1] = '\0';
        length -= 1;
    }
    while (length > 1 && buffer[length - 1] != '/')
    {
        buffer[length - 1] = '\0';
        length -= 1;
    }
    if (length > 1)
    {
        buffer[length - 1] = '\0';
    }
    if (buffer[0] == '\0')
    {
        snprintf(buffer, capacity, "/");
    }
}

/* ---- formato de las celdas ------------------------------------------------ */

static void filesapp_set_status(const char *text)
{
    snprintf(g_count_pane, sizeof(g_count_pane), "%s", text != 0 ? text : "");
}

/* Como el explorador de la epoca: el tamano se muestra en KB redondeados para
 * arriba, y los directorios no muestran tamano. */
static void filesapp_format_size(unsigned int bytes, char *buffer, size_t capacity)
{
    snprintf(buffer, capacity, "%u KB", (bytes + 1023u) / 1024u);
}

/* "File Folder" / "Application" / "TXT File" / "File", el vocabulario del
 * explorador clasico para la columna Type. */
static void filesapp_format_type(
    const char *name,
    int is_dir,
    int launchable,
    char *buffer,
    size_t capacity)
{
    const char *dot = 0;
    size_t index;

    if (is_dir)
    {
        snprintf(buffer, capacity, "File Folder");
        return;
    }
    if (launchable)
    {
        snprintf(buffer, capacity, "Application");
        return;
    }

    for (index = 0; name != 0 && name[index] != '\0'; ++index)
    {
        if (name[index] == '.' && index > 0 && name[index + 1] != '\0')
        {
            dot = &name[index + 1];
        }
    }
    if (dot == 0)
    {
        snprintf(buffer, capacity, "File");
        return;
    }

    /* La extension va en mayusculas, como la mostraba el explorador. */
    {
        char extension[16];
        size_t length = 0;

        while (dot[length] != '\0' && length + 1 < sizeof(extension))
        {
            char value = dot[length];
            if (value >= 'a' && value <= 'z')
            {
                value = (char)(value - 'a' + 'A');
            }
            extension[length] = value;
            length += 1;
        }
        extension[length] = '\0';
        snprintf(buffer, capacity, "%s File", extension);
    }
}

static void filesapp_format_total(unsigned int bytes, char *buffer, size_t capacity)
{
    if (bytes < 1024u)
    {
        snprintf(buffer, capacity, "%u bytes", bytes);
        return;
    }
    if (bytes < 1024u * 1024u)
    {
        snprintf(buffer, capacity, "%u KB", bytes / 1024u);
        return;
    }
    /* Un decimal por aritmetica entera: el printf de userland no lleva float. */
    snprintf(
        buffer,
        capacity,
        "%u.%u MB",
        bytes / (1024u * 1024u),
        (bytes % (1024u * 1024u)) * 10u / (1024u * 1024u));
}

/* ---- listado -------------------------------------------------------------- */

static void filesapp_sort_entries(int start_index)
{
    int left;

    for (left = start_index; left < g_entry_count; ++left)
    {
        int right;
        for (right = left + 1; right < g_entry_count; ++right)
        {
            int swap = 0;
            if (g_entries[left].is_dir != g_entries[right].is_dir)
            {
                swap = g_entries[right].is_dir > g_entries[left].is_dir;
            }
            else if (strcmp(g_entries[right].name, g_entries[left].name) < 0)
            {
                swap = 1;
            }

            if (swap)
            {
                struct filesapp_entry temp = g_entries[left];
                g_entries[left] = g_entries[right];
                g_entries[right] = temp;
            }
        }
    }
}

/* ---- list items and entries ------------------------------------------------
 *
 * The widget's items are g_entries, except that Large Icons leaves out "..":
 * Explorer's icon view has no parent entry (Up One Level and Backspace go
 * up). Since ".." is always entry 0 when present, the icon view's item i is
 * entry i + 1, and the widget is pointed one past the start of the arrays.
 */

static int filesapp_item_offset(void)
{
    return FILESAPP_LIST->kind == SXGUI_ICONVIEW && g_entry_count > 0 &&
        strcmp(g_entries[0].name, "..") == 0 ? 1 : 0;
}

/* The entry behind widget item `item`, or -1 for none. */
static int filesapp_entry_of(int item)
{
    int entry = item < 0 ? -1 : item + filesapp_item_offset();
    return entry < g_entry_count ? entry : -1;
}

/* The widget item showing entry `entry`, or -1 when this view hides it. */
static int filesapp_item_of(int entry)
{
    int item = entry < 0 ? -1 : entry - filesapp_item_offset();
    return item >= 0 ? item : -1;
}

/* Points the widget at the arrays of the current view, keeping the
 * selected entry selected if the view shows it. */
static void filesapp_bind_list(int selected_entry)
{
    int offset = filesapp_item_offset();

    FILESAPP_LIST->items = g_item_ptrs + offset;
    FILESAPP_LIST->item_icons = (FILESAPP_LIST->kind == SXGUI_ICONVIEW ? g_item_large_icons : g_item_icons) + offset;
    FILESAPP_LIST->item_count = g_entry_count - offset;
    FILESAPP_LIST->value = filesapp_item_of(selected_entry);
    FILESAPP_LIST->scroll = 0;
}

/* Builds the "name TAB size TAB type" rows the listbox splits. */
static void filesapp_rebuild_labels(void)
{
    char full_path[FILESAPP_PATH_CAPACITY];
    char size_cell[32];
    char type_cell[32];
    int index;

    /* One read of /mimeicon.ini per process. It is cheap -- a small ini,
     * no binaries scanned as in file_assoc -- but repeating it on every
     * directory change adds nothing. */
    if (!g_mime_icons_loaded)
    {
        (void)mime_icon_load();
        g_mime_icons_loaded = 1;
    }

    for (index = 0; index < g_entry_count; ++index)
    {
        const struct filesapp_entry *entry = &g_entries[index];
        int launchable = 0;

        if (!entry->is_dir &&
            filesapp_join_path(g_current_path, entry->name, full_path, sizeof(full_path)))
        {
            launchable = filesapp_path_is_launchable(full_path);
        }

        size_cell[0] = '\0';
        if (!entry->is_dir)
        {
            filesapp_format_size(entry->size, size_cell, sizeof(size_cell));
        }
        if (strcmp(entry->name, "..") == 0)
        {
            snprintf(type_cell, sizeof(type_cell), "File Folder");
        }
        else
        {
            filesapp_format_type(entry->name, entry->is_dir, launchable, type_cell, sizeof(type_cell));
        }

        snprintf(
            g_item_labels[index],
            sizeof(g_item_labels[index]),
            "%s%c%s%c%s",
            entry->name,
            SXGUI_COLUMN_SEPARATOR,
            size_cell,
            SXGUI_COLUMN_SEPARATOR,
            type_cell);
        g_item_ptrs[index] = g_item_labels[index];
        g_item_icons[index] = mime_icon_for_file(entry->name, entry->is_dir, launchable);
        g_item_large_icons[index] = mime_icon_large_for_file(entry->name, entry->is_dir, launchable);
    }
    /* A folder opens with nothing selected, as Explorer opens one. */
    filesapp_bind_list(-1);
}

static void filesapp_update_status(void)
{
    unsigned int total = 0;
    int index;
    int objects = 0;

    for (index = 0; index < g_entry_count; ++index)
    {
        if (strcmp(g_entries[index].name, "..") == 0)
        {
            continue;
        }
        objects += 1;
        if (!g_entries[index].is_dir)
        {
            total += g_entries[index].size;
        }
    }

    snprintf(g_count_pane, sizeof(g_count_pane), "%d object(s)", objects);
    filesapp_format_total(total, g_size_pane, sizeof(g_size_pane));
}

static int filesapp_load_directory(const char *path)
{
    DIR *directory = 0;
    struct dirent *entry = 0;
    char full_path[FILESAPP_PATH_CAPACITY];
    int insert_parent = 0;

    directory = opendir(path);
    if (directory == 0)
    {
        filesapp_set_status("Unable to open directory.");
        return -1;
    }

    g_entry_count = 0;
    insert_parent = !filesapp_is_root_path(path);
    if (insert_parent && g_entry_count < FILESAPP_MAX_ENTRIES)
    {
        memset(&g_entries[g_entry_count], 0, sizeof(g_entries[g_entry_count]));
        strcpy(g_entries[g_entry_count].name, "..");
        g_entries[g_entry_count].is_dir = 1;
        g_entry_count += 1;
    }

    while ((entry = readdir(directory)) != 0 && g_entry_count < FILESAPP_MAX_ENTRIES)
    {
        struct stat info = {0};
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        memset(&g_entries[g_entry_count], 0, sizeof(g_entries[g_entry_count]));
        snprintf(g_entries[g_entry_count].name, sizeof(g_entries[g_entry_count].name), "%s", entry->d_name);
        if (filesapp_join_path(path, entry->d_name, full_path, sizeof(full_path)) && stat(full_path, &info) == 0)
        {
            g_entries[g_entry_count].is_dir = S_ISDIR(info.st_mode);
            g_entries[g_entry_count].size = info.st_size;
        }
        else
        {
            g_entries[g_entry_count].is_dir = entry->d_type == DT_DIR;
            g_entries[g_entry_count].size = 0;
        }
        g_entry_count += 1;
    }
    closedir(directory);

    snprintf(g_current_path, sizeof(g_current_path), "%s", path);
    filesapp_sort_entries(insert_parent ? 1 : 0);
    filesapp_rebuild_labels();
    filesapp_update_status();

    snprintf(g_address, sizeof(g_address), "%s", g_current_path);
    FILESAPP_ADDRESS->caret = (int)strlen(g_address);
    FILESAPP_ADDRESS->sel_anchor = -1;
    FILESAPP_ADDRESS->scroll = 0;
    filesapp_update_commands();
    return 0;
}

/* ---- navigation ------------------------------------------------------------
 *
 * Going to a folder records the one left in Back and forgets Forward, as a
 * browser does; Back and Forward move one folder between the two. Refresh,
 * Rename and Delete reload in place and record nothing.
 */

static void filesapp_history_push(char stack[][FILESAPP_PATH_CAPACITY], int *count, const char *path)
{
    if (*count == FILESAPP_HISTORY_DEPTH)
    {
        memmove(stack[0], stack[1], (size_t)(FILESAPP_HISTORY_DEPTH - 1) * FILESAPP_PATH_CAPACITY);
        *count -= 1;
    }
    snprintf(stack[*count], FILESAPP_PATH_CAPACITY, "%s", path);
    *count += 1;
}

static int filesapp_navigate(const char *path)
{
    char previous[FILESAPP_PATH_CAPACITY];
    char target[FILESAPP_PATH_CAPACITY];

    /* Copies: `path` may be one of the strings a load overwrites. */
    snprintf(previous, sizeof(previous), "%s", g_current_path);
    snprintf(target, sizeof(target), "%s", path);
    if (filesapp_load_directory(target) < 0)
    {
        return -1;
    }
    if (strcmp(previous, g_current_path) != 0)
    {
        filesapp_history_push(g_back, &g_back_count, previous);
        g_forward_count = 0;
        filesapp_update_commands();
    }
    return 0;
}

/* One step through the history: from `from` to the folder on its top, with
 * the folder left pushed on `to`. A folder that is gone is dropped and
 * reported, and the history stays where it was. */
static void filesapp_history_step(
    char from[][FILESAPP_PATH_CAPACITY], int *from_count, char to[][FILESAPP_PATH_CAPACITY], int *to_count)
{
    char previous[FILESAPP_PATH_CAPACITY];
    char target[FILESAPP_PATH_CAPACITY];

    if (*from_count <= 0)
    {
        return;
    }
    *from_count -= 1;
    snprintf(target, sizeof(target), "%s", from[*from_count]);
    snprintf(previous, sizeof(previous), "%s", g_current_path);
    if (filesapp_load_directory(target) < 0)
    {
        filesapp_update_commands();
        return;
    }
    filesapp_history_push(to, to_count, previous);
    filesapp_update_commands();
}

static void filesapp_go_back(void)
{
    filesapp_history_step(g_back, &g_back_count, g_forward, &g_forward_count);
}

static void filesapp_go_forward(void)
{
    filesapp_history_step(g_forward, &g_forward_count, g_back, &g_back_count);
}

static void filesapp_go_up(void)
{
    char parent_path[FILESAPP_PATH_CAPACITY];

    if (filesapp_is_root_path(g_current_path))
    {
        return;
    }
    filesapp_parent_path(g_current_path, parent_path, sizeof(parent_path));
    (void)filesapp_navigate(parent_path);
}

/* What the user typed in the address bar: a path, absolute or relative to
 * the folder shown, with no trailing slash needed. */
static void filesapp_go_to_address(void)
{
    char target[FILESAPP_PATH_CAPACITY];
    size_t length;

    if (g_address[0] == '\0')
    {
        return;
    }
    if (g_address[0] == '/')
    {
        snprintf(target, sizeof(target), "%s", g_address);
    }
    else if (!filesapp_join_path(g_current_path, g_address, target, sizeof(target)))
    {
        filesapp_set_status("Path too long.");
        return;
    }
    length = strlen(target);
    while (length > 1 && target[length - 1] == '/')
    {
        target[--length] = '\0';
    }
    if (filesapp_navigate(target) < 0)
    {
        char message[FILESAPP_PATH_CAPACITY + 32];
        snprintf(message, sizeof(message), "Cannot find '%s'.", target);
        filesapp_set_status(message);
        return;
    }
    sxgui_focus(&g_app.ui, FILESAPP_LIST_INDEX);
}

static void filesapp_activate_selected(void)
{
    char full_path[FILESAPP_PATH_CAPACITY];
    int selected = filesapp_entry_of(FILESAPP_LIST->value);

    if (g_entry_count <= 0 || selected < 0 || selected >= g_entry_count)
    {
        return;
    }
    if (strcmp(g_entries[selected].name, "..") == 0)
    {
        filesapp_go_up();
        return;
    }
    if (!filesapp_join_path(g_current_path, g_entries[selected].name, full_path, sizeof(full_path)))
    {
        filesapp_set_status("Path too long.");
        return;
    }
    if (g_entries[selected].is_dir)
    {
        (void)filesapp_navigate(full_path);
        return;
    }
    switch (shortcut_open_path(&g_app.gfx, full_path))
    {
    case SHORTCUT_OPEN_PROGRAM:
    case SHORTCUT_OPEN_FOLDER:
        filesapp_set_status("Launch requested.");
        break;
    case SHORTCUT_OPEN_ASSOCIATED:
        filesapp_set_status("Opening with associated program...");
        break;
    case SHORTCUT_OPEN_EDITOR:
        filesapp_set_status("Opening in Notepad...");
        break;
    default:
        filesapp_set_status("Cannot open it.");
        break;
    }
}

/* ---- callbacks ------------------------------------------------------------ */

static void on_list(struct sxgui_widget *widget, void *user)
{
    (void)user;
    if (widget->action == SXGUI_ACTION_CHANGE)
    {
        filesapp_update_commands();
    }
    if (widget->action == SXGUI_ACTION_ACTIVATE)
    {
        filesapp_activate_selected();
    }
}

static void on_up_clicked(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    filesapp_go_up();
}

static void on_dialog_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
}

/* ---- commands ----------------------------------------------------------------
 *
 * Each command is in the menus and most are on the toolbar too; both are
 * enabled from the same state here, so a greyed button and a greyed menu item
 * never disagree.
 */

static struct sxgui_menu_item k_file_items[] = {
    {"Open", FILESAPP_MENU_OPEN, 0},
    {0, 0, 0},
    {"Delete", FILESAPP_MENU_DELETE, 0},
    {"Rename...", FILESAPP_MENU_RENAME, 0},
    {"Properties", FILESAPP_MENU_PROPERTIES, 0},
    {0, 0, 0},
    {"Close", FILESAPP_MENU_CLOSE, 0},
};

static struct sxgui_menu_item k_go_items[] = {
    {"Back", FILESAPP_MENU_BACK, 0},
    {"Forward", FILESAPP_MENU_FORWARD, 0},
    {"Up One Level", FILESAPP_MENU_UP, 0},
};

/* The selected entry, or -1 when there is none or it is "..": the entries a
 * command can act on. */
static int filesapp_selected_entry(void)
{
    int row = filesapp_entry_of(FILESAPP_LIST->value);

    if (row < 0 || row >= g_entry_count || strcmp(g_entries[row].name, "..") == 0)
    {
        return -1;
    }
    return row;
}

static void filesapp_enable(struct sxgui_widget *widget, int enabled)
{
    if (enabled)
    {
        widget->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
    }
    else
    {
        widget->flags |= SXGUI_FLAG_DISABLED;
        /* A button greyed under the pointer would stay raised. */
        widget->hover = 0;
        widget->pressed = 0;
    }
}

static void filesapp_enable_item(struct sxgui_menu_item *item, int enabled)
{
    item->flags = enabled ? (item->flags & ~(uint32_t)SXGUI_MENU_DISABLED) : (item->flags | SXGUI_MENU_DISABLED);
}

static void filesapp_update_commands(void)
{
    int has_selection = filesapp_selected_entry() >= 0;
    int can_go_up = !filesapp_is_root_path(g_current_path);

    filesapp_enable(&g_widgets[W_BACK], g_back_count > 0);
    filesapp_enable(&g_widgets[W_FORWARD], g_forward_count > 0);
    filesapp_enable(&g_widgets[W_UP], can_go_up);
    filesapp_enable(&g_widgets[W_DELETE], has_selection);
    filesapp_enable_item(&k_file_items[0], FILESAPP_LIST->value >= 0);
    filesapp_enable_item(&k_file_items[2], has_selection);
    filesapp_enable_item(&k_file_items[3], has_selection);
    filesapp_enable_item(&k_go_items[0], g_back_count > 0);
    filesapp_enable_item(&k_go_items[1], g_forward_count > 0);
    filesapp_enable_item(&k_go_items[2], can_go_up);
}

/* Delete and Rename act on the selected entry, by name (g_subject). */
static int filesapp_take_subject(void)
{
    int row = filesapp_selected_entry();

    if (row < 0)
    {
        return 0;
    }
    snprintf(g_subject, sizeof(g_subject), "%s", g_entries[row].name);
    return 1;
}

static void filesapp_delete_selected(void)
{
    if (filesapp_take_subject())
    {
        filesapp_ask_delete();
    }
}

static void filesapp_rename_selected(void)
{
    if (filesapp_take_subject())
    {
        filesapp_ask_rename();
    }
}

static void on_back_clicked(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    filesapp_go_back();
}

static void on_forward_clicked(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    filesapp_go_forward();
}

static void on_delete_clicked(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    filesapp_delete_selected();
}

static void on_properties_clicked(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    filesapp_show_properties();
}

/* ---- view ------------------------------------------------------------------ */

/* The user's last choice, in the profile (savanxp_settings_path). One line,
 * "view=icons" or "view=details"; anything else is the default, Large Icons,
 * as Windows 98's Explorer opens a folder. */
#define FILESAPP_SETTINGS_NAME "filesapp.cfg"

static struct sxgui_menu_item k_view_items[] = {
    {"Large Icons", FILESAPP_MENU_VIEW_ICONS, 0},
    {"Details", FILESAPP_MENU_VIEW_DETAILS, 0},
    {0, 0, 0},
    {"Refresh", FILESAPP_MENU_REFRESH, 0},
};

/* The toolbar's Views button drops down the same two choices. */
static struct sxgui_menu_item k_views_items[] = {
    {"Large Icons", FILESAPP_MENU_VIEW_ICONS, 0},
    {"Details", FILESAPP_MENU_VIEW_DETAILS, 0},
};

static const struct sxgui_menu k_views_menu = {
    "Views",
    k_views_items,
    (int)(sizeof(k_views_items) / sizeof(k_views_items[0])),
};

static int filesapp_load_view(void)
{
    char path[FILESAPP_PATH_CAPACITY];
    char line[32];
    FILE *file;
    int kind = SXGUI_ICONVIEW;

    if (savanxp_settings_path(FILESAPP_SETTINGS_NAME, 0, path, sizeof(path)) < 0)
    {
        return kind;
    }
    file = fopen(path, "r");
    if (file == 0)
    {
        return kind;
    }
    if (fgets(line, sizeof(line), file) != 0 && strncmp(line, "view=details", 12) == 0)
    {
        kind = SXGUI_LISTBOX;
    }
    fclose(file);
    return kind;
}

/* A disk that does not take the write only loses the preference: the view
 * already changed. */
static void filesapp_save_view(int kind)
{
    char path[FILESAPP_PATH_CAPACITY];
    FILE *file;

    if (savanxp_settings_path(FILESAPP_SETTINGS_NAME, 1, path, sizeof(path)) < 0)
    {
        return;
    }
    file = fopen(path, "w");
    if (file == 0)
    {
        return;
    }
    fputs(kind == SXGUI_LISTBOX ? "view=details\n" : "view=icons\n", file);
    fclose(file);
}

/* The same widget either way: the entries, the selection and the context
 * menu do not change, only how they are drawn -- and whether ".." shows. */
static void filesapp_set_view(int kind)
{
    int selected = filesapp_entry_of(FILESAPP_LIST->value);

    FILESAPP_LIST->kind = kind;
    /* `scroll` counts rows in one view and grid rows in the other, and the
     * icon view hides "..": rebinding converts the selection and resets the
     * scroll. A selected ".." ends with nothing selected in Large Icons. */
    filesapp_bind_list(selected);
    k_view_items[0].flags = kind == SXGUI_ICONVIEW ? SXGUI_MENU_CHECKED : 0;
    k_view_items[1].flags = kind == SXGUI_LISTBOX ? SXGUI_MENU_CHECKED : 0;
    k_views_items[0].flags = k_view_items[0].flags;
    k_views_items[1].flags = k_view_items[1].flags;
    filesapp_update_commands();
}

static void filesapp_choose_view(int kind)
{
    if (FILESAPP_LIST->kind == kind)
    {
        return;
    }
    filesapp_set_view(kind);
    filesapp_save_view(kind);
}

static void on_views_command(int id, void *user)
{
    (void)user;
    filesapp_choose_view(id == FILESAPP_MENU_VIEW_DETAILS ? SXGUI_LISTBOX : SXGUI_ICONVIEW);
}

static void on_views_clicked(struct sxgui_widget *widget, void *user)
{
    (void)user;
    sxgui_context_menu_open(
        &g_app.ui, &k_views_menu, widget->rect.x, widget->rect.y + widget->rect.height, on_views_command, 0);
}

static void on_menu_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case FILESAPP_MENU_OPEN:
        filesapp_activate_selected();
        break;
    case FILESAPP_MENU_UP:
        filesapp_go_up();
        break;
    case FILESAPP_MENU_BACK:
        filesapp_go_back();
        break;
    case FILESAPP_MENU_FORWARD:
        filesapp_go_forward();
        break;
    case FILESAPP_MENU_DELETE:
        filesapp_delete_selected();
        break;
    case FILESAPP_MENU_RENAME:
        filesapp_rename_selected();
        break;
    case FILESAPP_MENU_PROPERTIES:
        filesapp_show_properties();
        break;
    case FILESAPP_MENU_REFRESH:
        (void)filesapp_load_directory(g_current_path);
        break;
    case FILESAPP_MENU_VIEW_ICONS:
        filesapp_choose_view(SXGUI_ICONVIEW);
        break;
    case FILESAPP_MENU_VIEW_DETAILS:
        filesapp_choose_view(SXGUI_LISTBOX);
        break;
    case FILESAPP_MENU_ABOUT:
        sxgui_dialog_begin(&g_app.ui, &g_about_dialog, FILESAPP_ABOUT_WIDTH, FILESAPP_ABOUT_HEIGHT);
        break;
    case FILESAPP_MENU_CLOSE:
        sxgui_app_quit(&g_app, 0);
        break;
    default:
        break;
    }
}

/* ---- context menu --------------------------------------------------------
 * (docs/desktop/DESKTOP_ICONS.md#phase-4-context-menus-in-applications) */

#define FILESAPP_CMD_OPEN 20
#define FILESAPP_CMD_SEND_TO_DESKTOP 21
#define FILESAPP_CMD_RENAME 22
#define FILESAPP_CMD_DELETE 23
#define FILESAPP_CMD_UP 24
#define FILESAPP_CMD_REFRESH 25
#define FILESAPP_CMD_PROPERTIES 26

#define FILESAPP_QUESTION_WIDTH 340
#define FILESAPP_QUESTION_HEIGHT 96
#define FILESAPP_RENAME_HEIGHT 112

static const struct sxgui_menu_item k_entry_items[] = {
    {"Open", FILESAPP_CMD_OPEN, 0},
    {0, 0, 0},
    {"Send To Desktop", FILESAPP_CMD_SEND_TO_DESKTOP, 0},
    {0, 0, 0},
    {"Rename...", FILESAPP_CMD_RENAME, 0},
    {"Delete", FILESAPP_CMD_DELETE, 0},
    {0, 0, 0},
    {"Properties", FILESAPP_CMD_PROPERTIES, 0},
};

static const struct sxgui_menu k_entry_menu = {
    "Entry",
    k_entry_items,
    (int)(sizeof(k_entry_items) / sizeof(k_entry_items[0])),
};

static const struct sxgui_menu_item k_folder_items[] = {
    {"Up One Level", FILESAPP_CMD_UP, 0},
    {"Refresh", FILESAPP_CMD_REFRESH, 0},
    {0, 0, 0},
    {"Properties", FILESAPP_CMD_PROPERTIES, 0},
};

static const struct sxgui_menu k_folder_menu = {
    "Folder",
    k_folder_items,
    (int)(sizeof(k_folder_items) / sizeof(k_folder_items[0])),
};

static char g_question_text[300];
static char g_rename_buffer[128];
static struct sxgui_dialog g_question_dialog;
static struct sxgui_widget g_question_widgets[3];
static struct sxgui_dialog g_rename_dialog;
static struct sxgui_widget g_rename_widgets[4];
static uint32_t g_pointer_buttons = 0;

static int filesapp_subject_path(char *buffer, size_t capacity)
{
    return g_subject[0] != '\0' && filesapp_join_path(g_current_path, g_subject, buffer, capacity);
}

/* A shortcut to the entry in the user's Desktop folder, as Send To > Desktop
 * (create shortcut) does. A name already taken gets " (2)" and on. */
static void filesapp_send_to_desktop(void)
{
    char target[FILESAPP_PATH_CAPACITY];
    char folder[FILESAPP_PATH_CAPACITY];
    char name[160];
    char path[FILESAPP_PATH_CAPACITY + 160];
    struct shortcut shortcut;
    struct stat info;
    int attempt;

    if (!filesapp_subject_path(target, sizeof(target)))
    {
        return;
    }
    snprintf(folder, sizeof(folder), "%s/Desktop", savanxp_profile_dir());
    if (stat(folder, &info) != 0)
    {
        (void)savanxp_mkdir(folder);
    }
    shortcut_for_target(target, &shortcut, name, sizeof(name));
    snprintf(path, sizeof(path), "%s/%s", folder, name);
    for (attempt = 2; stat(path, &info) == 0 && attempt < 100; ++attempt)
    {
        size_t stem = strlen(name) - (sizeof(SHORTCUT_EXTENSION) - 1u);
        snprintf(path, sizeof(path), "%s/%.*s (%d)%s", folder, (int)stem, name, attempt, SHORTCUT_EXTENSION);
    }
    filesapp_set_status(shortcut_save(path, &shortcut) == 0
        ? "Shortcut created on the desktop."
        : "Cannot create the shortcut.");
}

static void on_question_yes(struct sxgui_widget *widget, void *user)
{
    char path[FILESAPP_PATH_CAPACITY];
    struct stat info;
    int result = -1;

    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
    if (!filesapp_subject_path(path, sizeof(path)) || stat(path, &info) != 0)
    {
        return;
    }
    result = S_ISDIR(info.st_mode) ? rmdir(path) : unlink(path);
    (void)filesapp_load_directory(g_current_path);
    filesapp_set_status(result == 0 ? "Deleted." :
        (S_ISDIR(info.st_mode) ? "Cannot delete: is the folder empty?" : "Cannot delete it."));
}

static void on_dialog_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 0);
}

static void filesapp_ask_delete(void)
{
    snprintf(g_question_text, sizeof(g_question_text), "Delete '%s'?", g_subject);
    sxgui_dialog_begin(&g_app.ui, &g_question_dialog, FILESAPP_QUESTION_WIDTH, FILESAPP_QUESTION_HEIGHT);
}

static void on_rename_ok(struct sxgui_widget *widget, void *user)
{
    char from[FILESAPP_PATH_CAPACITY];
    char to[FILESAPP_PATH_CAPACITY];
    struct stat info;

    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
    if (strcmp(g_rename_buffer, g_subject) == 0)
    {
        return;
    }
    if (g_rename_buffer[0] == '\0' || strchr(g_rename_buffer, '/') != 0 ||
        strcmp(g_rename_buffer, ".") == 0 || strcmp(g_rename_buffer, "..") == 0)
    {
        filesapp_set_status("A name cannot be empty or contain '/'.");
        return;
    }
    if (!filesapp_subject_path(from, sizeof(from)) ||
        !filesapp_join_path(g_current_path, g_rename_buffer, to, sizeof(to)) ||
        stat(to, &info) == 0 || rename(from, to) != 0)
    {
        filesapp_set_status("Cannot rename it.");
        return;
    }
    (void)filesapp_load_directory(g_current_path);
    filesapp_set_status("Renamed.");
}

static void filesapp_ask_rename(void)
{
    snprintf(g_rename_buffer, sizeof(g_rename_buffer), "%s", g_subject);
    g_rename_widgets[1].caret = (int)strlen(g_rename_buffer);
    g_rename_widgets[1].scroll = 0;
    sxgui_dialog_begin(&g_app.ui, &g_rename_dialog, FILESAPP_QUESTION_WIDTH, FILESAPP_RENAME_HEIGHT);
}

/* ---- properties --------------------------------------------------------------
 *
 * The selected entry's, or the folder's own when nothing is selected, as
 * Explorer's File > Properties does. A folder's size and contents count only
 * what is directly in it: a recursive walk could take long on a big tree,
 * and the window would hang meanwhile.
 */

#define FILESAPP_PROPERTIES_WIDTH 360
#define FILESAPP_PROPERTIES_ROWS 5
#define FILESAPP_PROPERTIES_CAPTION_WIDTH 70
#define FILESAPP_PROPERTIES_HEIGHT \
    (SXGUI_DIALOG_MARGIN * 3 + FILESAPP_PROPERTIES_ROWS * FILESAPP_DLG_ROW + SXGUI_BUTTON_HEIGHT)

static struct sxgui_dialog g_properties_dialog;
static struct sxgui_widget g_properties_widgets[FILESAPP_PROPERTIES_ROWS * 2 + 1];
static char g_properties_title[300];
static char g_properties_values[FILESAPP_PROPERTIES_ROWS][FILESAPP_PATH_CAPACITY];
static const char *const k_properties_captions[FILESAPP_PROPERTIES_ROWS] = {
    "Name:", "Type:", "Location:", "Size:", "Contains:",
};

static void filesapp_format_bytes(unsigned int bytes, char *buffer, size_t capacity)
{
    char short_form[64];

    if (bytes < 1024u)
    {
        snprintf(buffer, capacity, "%u bytes", bytes);
        return;
    }
    filesapp_format_total(bytes, short_form, sizeof(short_form));
    snprintf(buffer, capacity, "%s (%u bytes)", short_form, bytes);
}

/* What is directly in `path`: files, folders and the files' bytes. */
static void filesapp_count_folder(const char *path, int *files, int *folders, unsigned int *bytes)
{
    DIR *directory = opendir(path);
    struct dirent *entry;
    char full_path[FILESAPP_PATH_CAPACITY];

    *files = 0;
    *folders = 0;
    *bytes = 0;
    if (directory == 0)
    {
        return;
    }
    while ((entry = readdir(directory)) != 0)
    {
        struct stat info;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            !filesapp_join_path(path, entry->d_name, full_path, sizeof(full_path)) || stat(full_path, &info) != 0)
        {
            continue;
        }
        if (S_ISDIR(info.st_mode))
        {
            *folders += 1;
        }
        else
        {
            *files += 1;
            *bytes += (unsigned int)info.st_size;
        }
    }
    closedir(directory);
}

static void on_properties_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
}

static void filesapp_show_properties(void)
{
    char path[FILESAPP_PATH_CAPACITY];
    char name[256];
    struct stat info;
    int row = filesapp_selected_entry();
    int is_folder;
    int launchable = 0;

    if (row >= 0)
    {
        snprintf(name, sizeof(name), "%s", g_entries[row].name);
        if (!filesapp_join_path(g_current_path, name, path, sizeof(path)))
        {
            filesapp_set_status("Path too long.");
            return;
        }
        snprintf(g_properties_values[2], FILESAPP_PATH_CAPACITY, "%s", g_current_path);
    }
    else
    {
        const char *slash = strrchr(g_current_path, '/');

        snprintf(path, sizeof(path), "%s", g_current_path);
        snprintf(name, sizeof(name), "%s", filesapp_is_root_path(path) || slash == 0 ? "/" : slash + 1);
        if (filesapp_is_root_path(path))
        {
            g_properties_values[2][0] = '\0';
        }
        else
        {
            filesapp_parent_path(path, g_properties_values[2], FILESAPP_PATH_CAPACITY);
        }
    }
    if (stat(path, &info) != 0)
    {
        filesapp_set_status("Cannot read its properties.");
        return;
    }
    is_folder = S_ISDIR(info.st_mode);
    if (!is_folder)
    {
        launchable = filesapp_path_is_launchable(path);
    }

    snprintf(g_properties_title, sizeof(g_properties_title), "%s Properties", name);
    snprintf(g_properties_values[0], FILESAPP_PATH_CAPACITY, "%s", name);
    filesapp_format_type(name, is_folder, launchable, g_properties_values[1], FILESAPP_PATH_CAPACITY);
    if (is_folder)
    {
        int files;
        int folders;
        unsigned int bytes;

        filesapp_count_folder(path, &files, &folders, &bytes);
        filesapp_format_bytes(bytes, g_properties_values[3], FILESAPP_PATH_CAPACITY);
        snprintf(g_properties_values[4], FILESAPP_PATH_CAPACITY, "%d File(s), %d Folder(s)", files, folders);
    }
    else
    {
        filesapp_format_bytes((unsigned int)info.st_size, g_properties_values[3], FILESAPP_PATH_CAPACITY);
    }
    /* A file has nothing to contain: the row goes away. */
    if (is_folder)
    {
        g_properties_widgets[8].flags |= SXGUI_FLAG_VISIBLE;
        g_properties_widgets[9].flags |= SXGUI_FLAG_VISIBLE;
    }
    else
    {
        g_properties_widgets[8].flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
        g_properties_widgets[9].flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
    }
    sxgui_dialog_begin(&g_app.ui, &g_properties_dialog, FILESAPP_PROPERTIES_WIDTH, FILESAPP_PROPERTIES_HEIGHT);
}

static void filesapp_build_properties_dialog(void)
{
    int index;
    int value_x = SXGUI_DIALOG_MARGIN + FILESAPP_PROPERTIES_CAPTION_WIDTH;

    for (index = 0; index < FILESAPP_PROPERTIES_ROWS; ++index)
    {
        int y = SXGUI_DIALOG_MARGIN + index * FILESAPP_DLG_ROW;

        g_properties_widgets[index * 2] = sxgui_label(
            sx_rect_make(SXGUI_DIALOG_MARGIN, y, FILESAPP_PROPERTIES_CAPTION_WIDTH, 18),
            k_properties_captions[index]);
        g_properties_widgets[index * 2 + 1] = sxgui_label(
            sx_rect_make(value_x, y, FILESAPP_PROPERTIES_WIDTH - value_x - SXGUI_DIALOG_MARGIN, 18),
            g_properties_values[index]);
    }
    g_properties_widgets[FILESAPP_PROPERTIES_ROWS * 2] = sxgui_button(
        sx_rect_make(FILESAPP_PROPERTIES_WIDTH - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_WIDTH,
                     FILESAPP_PROPERTIES_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_properties_ok, 0);
    g_properties_dialog.title = g_properties_title;
    g_properties_dialog.widgets = g_properties_widgets;
    g_properties_dialog.widget_count = FILESAPP_PROPERTIES_ROWS * 2 + 1;
    g_properties_dialog.initial_focus = FILESAPP_PROPERTIES_ROWS * 2;
    g_properties_dialog.default_button = FILESAPP_PROPERTIES_ROWS * 2;
}

static void on_context_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case FILESAPP_CMD_OPEN:
        filesapp_activate_selected();
        break;
    case FILESAPP_CMD_SEND_TO_DESKTOP:
        filesapp_send_to_desktop();
        break;
    case FILESAPP_CMD_RENAME:
        filesapp_ask_rename();
        break;
    case FILESAPP_CMD_DELETE:
        filesapp_ask_delete();
        break;
    case FILESAPP_CMD_UP:
        filesapp_go_up();
        break;
    case FILESAPP_CMD_REFRESH:
        (void)filesapp_load_directory(g_current_path);
        break;
    case FILESAPP_CMD_PROPERTIES:
        filesapp_show_properties();
        break;
    default:
        break;
    }
}

/* The menu for widget item `item` (selecting it), or the folder's for -1
 * and "..". */
static void filesapp_show_menu(int item, int x, int y)
{
    int row = filesapp_entry_of(item);

    g_subject[0] = '\0';
    if (row >= 0 && row < g_entry_count && strcmp(g_entries[row].name, "..") != 0)
    {
        FILESAPP_LIST->value = item;
        snprintf(g_subject, sizeof(g_subject), "%s", g_entries[row].name);
        filesapp_update_commands();
        sxgui_context_menu_open(&g_app.ui, &k_entry_menu, x, y, on_context_command, 0);
        return;
    }
    /* The folder's menu is about the folder: its Properties are the
     * folder's, so nothing stays selected. */
    if (item < 0)
    {
        FILESAPP_LIST->value = -1;
        filesapp_update_commands();
    }
    sxgui_context_menu_open(&g_app.ui, &k_folder_menu, x, y, on_context_command, 0);
}

/* A right click on the list: on the release, as Windows sends
 * WM_CONTEXTMENU. Everything else goes on to the toolkit. */
static int on_pointer(struct sxgui_app *app, const struct savanxp_gui_pointer_event *event)
{
    uint32_t released = g_pointer_buttons & ~event->buttons;

    (void)app;
    g_pointer_buttons = event->buttons;
    if ((released & SAVANXP_MOUSE_BUTTON_RIGHT) == 0 || sxgui_dialog_active(&g_app.ui) ||
        !sx_rect_contains_point(FILESAPP_LIST->rect, event->x, event->y))
    {
        return 0;
    }
    filesapp_show_menu(sxgui_listbox_row_at(FILESAPP_LIST, event->x, event->y), event->x, event->y);
    return 1;
}

static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    int alt = (event->modifiers & SAVANXP_KEY_MOD_ALT) != 0;
    int typing = g_app.ui.focus_index == W_ADDRESS;

    (void)app;
    /* A dialog's keys come through here too: they are the dialog's (the
     * Rename field's Backspace erases, it does not go up a folder). */
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || sxgui_dialog_active(&g_app.ui))
    {
        return 0;
    }
    if (alt && event->key == SAVANXP_KEY_LEFT)
    {
        filesapp_go_back();
        return 1;
    }
    if (alt && event->key == SAVANXP_KEY_RIGHT)
    {
        filesapp_go_forward();
        return 1;
    }
    if (alt && event->key == SAVANXP_KEY_ENTER)
    {
        filesapp_show_properties();
        return 1;
    }
    if (typing && event->key == SAVANXP_KEY_ENTER)
    {
        filesapp_go_to_address();
        return 1;
    }
    /* Esc in the address bar puts the folder's path back instead of
     * closing the window. */
    if (typing && event->key == SAVANXP_KEY_ESC)
    {
        snprintf(g_address, sizeof(g_address), "%s", g_current_path);
        FILESAPP_ADDRESS->caret = (int)strlen(g_address);
        FILESAPP_ADDRESS->sel_anchor = -1;
        FILESAPP_ADDRESS->scroll = 0;
        sxgui_focus(&g_app.ui, FILESAPP_LIST_INDEX);
        return 1;
    }
    /* In the address bar, Backspace and Delete edit the text. */
    if (typing)
    {
        return 0;
    }
    /* The keyboard's right click opens the selected row's menu next to it. */
    if (event->key == SAVANXP_KEY_MENU ||
        (event->key == SAVANXP_KEY_F10 && (event->modifiers & SAVANXP_KEY_MOD_SHIFT) != 0))
    {
        int row_height = gfx_text_height() + 4;
        int visible = FILESAPP_LIST->value - FILESAPP_LIST->scroll;
        int x = FILESAPP_LIST->rect.x + 24;
        int y = FILESAPP_LIST->rect.y + 2 + row_height + (visible + 1) * row_height;

        if (FILESAPP_LIST->kind == SXGUI_ICONVIEW)
        {
            /* Under the selected icon, over its name. */
            struct sx_rect cell = sxgui_iconview_item_rect(FILESAPP_LIST, FILESAPP_LIST->value);

            x = cell.x + cell.width / 2;
            y = cell.y + SXGUI_ICONVIEW_ICON_SIZE + 8;
            visible = cell.width > 0 && y >= FILESAPP_LIST->rect.y ? 0 : -1;
        }
        if (visible < 0 || y > FILESAPP_LIST->rect.y + FILESAPP_LIST->rect.height)
        {
            x = FILESAPP_LIST->rect.x + 24;
            y = FILESAPP_LIST->rect.y + row_height * 2;
        }
        filesapp_show_menu(FILESAPP_LIST->value, x, y);
        return 1;
    }
    if (event->key == SAVANXP_KEY_DELETE)
    {
        filesapp_delete_selected();
        return 1;
    }
    if (event->key == SAVANXP_KEY_BACKSPACE)
    {
        filesapp_go_up();
        return 1;
    }
    if (event->key == SAVANXP_KEY_F5)
    {
        (void)filesapp_load_directory(g_current_path);
        return 1;
    }
    return 0;
}

/* ---- layout ------------------------------------------------------------------
 *
 * Three bands under the menu bar, each ruled off by an etched line as in the
 * Explorer: the toolbar, the address bar, then the list down to the status
 * bar. The list stretches with the window, so there is no bounding box to
 * size the window from; this is a comfortable browser that does not eat the
 * screen.
 */
#define FILESAPP_CONTENT_WIDTH 560
#define FILESAPP_CONTENT_HEIGHT 440

/* An etched rule is two pixels; it gets one more above and below. */
#define FILESAPP_RULE_HEIGHT 4
#define FILESAPP_SEPARATOR_WIDTH 8
/* Gap between the status bar's two panes: smaller than the general GAP on
 * purpose, they are two halves of one bar, not two controls. */
#define FILESAPP_STATUS_GAP 2

static const int k_toolbar_order[] = {
    W_BACK, W_FORWARD, W_SEP_HISTORY, W_UP, W_SEP_UP, W_DELETE, W_PROPERTIES, W_SEP_EDIT, W_VIEWS,
};

static int filesapp_toolbar_button_width(void)
{
    int widest = 0;
    int index;

    for (index = 0; index < (int)(sizeof(k_toolbar_order) / sizeof(k_toolbar_order[0])); ++index)
    {
        const struct sxgui_widget *widget = &g_widgets[k_toolbar_order[index]];

        if (widget->kind == SXGUI_BUTTON)
        {
            int width = sxgui_toolbar_button_width(widget->text);
            widest = width > widest ? width : widest;
        }
    }
    /* Room for the Views button's arrow. */
    return widest + 8;
}

static void filesapp_layout(struct sxgui_app *app)
{
    int width = (int)app->gfx.info.width;
    int height = (int)app->gfx.info.height;
    int toolbar_height = sxgui_toolbar_height();
    int button_width = filesapp_toolbar_button_width();
    int y = sxgui_menubar_height();
    int x = SXGUI_MARGIN;
    int list_y;
    int status_y = height - SXGUI_MARGIN - SXGUI_STATUS_HEIGHT;
    int list_height;
    int list_width = width - SXGUI_MARGIN * 2;
    int label_width = gfx_text_width("Address") + SXGUI_GAP;
    int address_width;
    int name_width;
    int count_pane_width;
    int index;

    if (list_width < 120)
    {
        list_width = 120;
    }

    g_widgets[W_RULE_MENU].rect = sx_rect_make(0, y, width, FILESAPP_RULE_HEIGHT);
    y += FILESAPP_RULE_HEIGHT;
    /* Buttons that do not fit are hidden rather than squeezed, as a toolbar
     * cut by a narrow window. */
    for (index = 0; index < (int)(sizeof(k_toolbar_order) / sizeof(k_toolbar_order[0])); ++index)
    {
        struct sxgui_widget *widget = &g_widgets[k_toolbar_order[index]];
        int item_width = widget->kind == SXGUI_BUTTON ? button_width : FILESAPP_SEPARATOR_WIDTH;

        widget->rect = widget->kind == SXGUI_BUTTON
            ? sx_rect_make(x, y, item_width, toolbar_height)
            : sx_rect_make(x, y + 3, item_width, toolbar_height - 6);
        if (x + item_width <= width - SXGUI_MARGIN)
        {
            widget->flags |= SXGUI_FLAG_VISIBLE;
        }
        else
        {
            widget->flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
        }
        x += item_width;
    }
    y += toolbar_height;
    g_widgets[W_RULE_TOOLBAR].rect = sx_rect_make(0, y, width, FILESAPP_RULE_HEIGHT);
    y += FILESAPP_RULE_HEIGHT;

    address_width = list_width - label_width;
    if (address_width < 80)
    {
        address_width = 80;
    }
    g_widgets[W_ADDRESS_LABEL].rect = sx_rect_make(SXGUI_MARGIN, y, label_width, SXGUI_FIELD_HEIGHT);
    FILESAPP_ADDRESS->rect = sx_rect_make(SXGUI_MARGIN + label_width, y, address_width, SXGUI_FIELD_HEIGHT);
    list_y = y + SXGUI_FIELD_HEIGHT + SXGUI_GAP;
    list_height = status_y - SXGUI_GAP - list_y;
    if (list_height < 60)
    {
        list_height = 60;
    }
    FILESAPP_LIST->rect = sx_rect_make(SXGUI_MARGIN, list_y, list_width, list_height);

    /* Name takes what is left: it is the column that grows with the window,
     * as in the Explorer. What is subtracted is the list's REAL usable area --
     * both sunken bevels and the scrollbar column, which is always reserved:
     * if it were subtracted only while the bar shows, the columns would jump
     * in width when a file is added. */
    name_width = list_width - SXGUI_BORDER_SUNKEN * 2 - SXGUI_SCROLLBAR_THICKNESS -
        FILESAPP_COLUMN_SIZE_WIDTH - FILESAPP_COLUMN_TYPE_WIDTH;
    if (name_width < 90)
    {
        name_width = 90;
    }
    g_columns[0].width = name_width;

    count_pane_width = (list_width - FILESAPP_STATUS_GAP) / 2;
    if (count_pane_width < 80)
    {
        count_pane_width = 80;
    }
    g_widgets[W_STATUS_COUNT].rect = sx_rect_make(SXGUI_MARGIN, status_y, count_pane_width, SXGUI_STATUS_HEIGHT);
    g_widgets[W_STATUS_SIZE].rect = sx_rect_make(
        SXGUI_MARGIN + count_pane_width + FILESAPP_STATUS_GAP, status_y,
        list_width - count_pane_width - FILESAPP_STATUS_GAP, SXGUI_STATUS_HEIGHT);
}

static void on_resize(struct sxgui_app *app)
{
    filesapp_layout(app);
}

/* ---- menus ----------------------------------------------------------------- */

static const struct sxgui_menu_item k_help_items[] = {
    {"About Files", FILESAPP_MENU_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"View", k_view_items, (int)(sizeof(k_view_items) / sizeof(k_view_items[0]))},
    {"Go", k_go_items, (int)(sizeof(k_go_items) / sizeof(k_go_items[0]))},
    {"Help", k_help_items, (int)(sizeof(k_help_items) / sizeof(k_help_items[0]))},
};

static struct sxgui_menubar g_menubar = {
    k_menus,
    (int)(sizeof(k_menus) / sizeof(k_menus[0])),
    -1,
    -1,
    on_menu_command,
    0
};

/* ---- selftest ------------------------------------------------------------- */

/* mime_icon's failed checks, in the line the smoke harness stops on. */
static void filesapp_report_mime_failure(const char *label)
{
    printf("FILESAPP SMOKE FAIL %s\n", label);
}

static int filesapp_selftest(void)
{
    int failures = 0;
    int index;

    /* La tabla se vuelca SIEMPRE, tambien al fallar: un assert que dice "no
     * resolvio a notepad" sin mostrar a que resolvio no alcanza para
     * diagnosticar nada. */
    {
        /*
         * El costo del escaneo se MIDE, no se estima: la decision del
         * documento fue no poner cache hasta saber cuanto duele. Estos dos
         * numeros -- ejecutables abiertos y milisegundos -- son la evidencia
         * para revisarla.
         */
        unsigned long started_ms = uptime_ms();

        (void)file_assoc_load();
        printf("FILESAPP SMOKE assoc entries=%d examined=%d ms=%lu\n",
            file_assoc_count(),
            file_assoc_scan_examined(),
            uptime_ms() - started_ms);
    }
    for (index = 0; index < file_assoc_count(); ++index)
    {
        const struct file_assoc_entry *entry = file_assoc_at(index);
        if (entry != 0)
        {
            printf("FILESAPP SMOKE assoc %s -> %s (%s)\n",
                entry->extension,
                entry->program,
                entry->from_policy ? "politica" : "declarada");
        }
    }

    /*
     * The per-type icon layer. It reports the count of the image's real
     * mapping and whether the /icons catalog answers: a loaded mapping with
     * zero icons that open means the build did not emit the blobs, and from
     * outside that looks the same as "the list has no icons".
     */
    {
        int mapped = mime_icon_load();
        int resolved = 0;
        int resolved_large = 0;
        static const char *const k_probe[] = {"notas.txt", "captura.png", "tema.wav", "doom.sxe"};
        int probe;

        for (probe = 0; probe < (int)(sizeof(k_probe) / sizeof(k_probe[0])); ++probe)
        {
            if (mime_icon_for_file(k_probe[probe], 0, 0) != 0)
            {
                resolved += 1;
            }
            if (mime_icon_large_for_file(k_probe[probe], 0, 0) != 0)
            {
                resolved_large += 1;
            }
        }
        printf("FILESAPP SMOKE mimeicon mapped=%d probes=%d resolved=%d large=%d\n",
            mapped,
            (int)(sizeof(k_probe) / sizeof(k_probe[0])),
            resolved,
            resolved_large);
    }

    /* El harness corta la corrida en la PRIMERA linea con el token de fallo,
     * asi que el volcado tiene que salir antes del selftest o no se ve nunca
     * cuando algo falla. */
    failures = file_assoc_selftest();
    failures += mime_icon_selftest(filesapp_report_mime_failure);
    failures += shortcut_selftest();

    if (failures != 0)
    {
        printf("FILESAPP SMOKE FAIL %d checks\n", failures);
        return 1;
    }

    /*
     * Lo que los fixtures no pueden fingir: la resolucion contra los binarios
     * REALES de esta imagen, y cuanto cuesta. El numero de ejecutables abiertos
     * es la magnitud a mirar antes de decidir si hace falta una cache -- la
     * decision del documento es medir primero.
     */
    if (file_assoc_count() <= 0)
    {
        printf("FILESAPP SMOKE FAIL ningun binario declara extensiones\n");
        return 1;
    }

    printf("FILESAPP SMOKE PASS entries=%d\n", file_assoc_count());
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        return filesapp_selftest();
    }

    filesapp_build_toolbar_glyphs();
    {
        struct sx_rect none = sx_rect_make(0, 0, 0, 0);

        g_widgets[W_RULE_MENU] = sxgui_separator(none);
        g_widgets[W_BACK] = sxgui_toolbar_button(none, "Back", k_glyph_back, on_back_clicked, 0);
        g_widgets[W_FORWARD] = sxgui_toolbar_button(none, "Forward", k_glyph_forward, on_forward_clicked, 0);
        g_widgets[W_SEP_HISTORY] = sxgui_separator(none);
        g_widgets[W_UP] = sxgui_toolbar_button(none, "Up", k_glyph_up, on_up_clicked, 0);
        g_widgets[W_SEP_UP] = sxgui_separator(none);
        g_widgets[W_DELETE] = sxgui_toolbar_button(none, "Delete", k_glyph_delete, on_delete_clicked, 0);
        g_widgets[W_PROPERTIES] = sxgui_toolbar_button(
            none, "Properties", k_glyph_properties, on_properties_clicked, 0);
        g_widgets[W_SEP_EDIT] = sxgui_separator(none);
        g_widgets[W_VIEWS] = sxgui_toolbar_button(none, "Views", k_glyph_views, on_views_clicked, 0);
        g_widgets[W_VIEWS].flags |= SXGUI_FLAG_DROPDOWN;
        g_widgets[W_RULE_TOOLBAR] = sxgui_separator(none);
        g_widgets[W_ADDRESS_LABEL] = sxgui_label(none, "Address");
        g_widgets[W_ADDRESS] = sxgui_textfield(none, g_address, (int)sizeof(g_address));
        g_widgets[W_LIST] = sxgui_listbox(none, g_item_ptrs, 0);
        g_widgets[W_LIST].columns = g_columns;
        g_widgets[W_LIST].column_count = FILESAPP_COLUMN_COUNT;
        g_widgets[W_LIST].on_action = on_list;
        g_widgets[W_LIST].flags |= SXGUI_FLAG_EMPTY_SELECTION;
        g_widgets[W_STATUS_COUNT] = sxgui_label(none, g_count_pane);
        g_widgets[W_STATUS_COUNT].flags |= SXGUI_FLAG_SUNKEN;
        g_widgets[W_STATUS_SIZE] = sxgui_label(none, g_size_pane);
        g_widgets[W_STATUS_SIZE].flags |= SXGUI_FLAG_SUNKEN;
    }
    filesapp_build_properties_dialog();

    g_about_widgets[0] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN,
                     FILESAPP_ABOUT_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
        "SavanXP Files");
    g_about_widgets[1] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + FILESAPP_DLG_ROW,
                     FILESAPP_ABOUT_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
        "Version: " SAVANXP_VERSION_STRING);
    g_about_widgets[2] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + FILESAPP_DLG_ROW * 2,
                     FILESAPP_ABOUT_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
        "Browse directories and launch programs");
    g_about_widgets[3] = sxgui_button(
        sx_rect_make((FILESAPP_ABOUT_WIDTH - SXGUI_BUTTON_WIDTH) / 2,
                     FILESAPP_ABOUT_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_dialog_ok, 0);
    g_question_widgets[0] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN,
                     FILESAPP_QUESTION_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
        g_question_text);
    g_question_widgets[1] = sxgui_button(
        sx_rect_make(FILESAPP_QUESTION_WIDTH / 2 - SXGUI_BUTTON_WIDTH - 4,
                     FILESAPP_QUESTION_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Yes", on_question_yes, 0);
    g_question_widgets[2] = sxgui_button(
        sx_rect_make(FILESAPP_QUESTION_WIDTH / 2 + 4,
                     FILESAPP_QUESTION_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "No", on_dialog_cancel, 0);
    g_question_dialog.title = "Confirm Delete";
    g_question_dialog.widgets = g_question_widgets;
    g_question_dialog.widget_count = 3;
    g_question_dialog.initial_focus = 1;
    g_question_dialog.default_button = 1;

    g_rename_widgets[0] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN,
                     FILESAPP_QUESTION_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
        "New name:");
    g_rename_widgets[1] = sxgui_textfield(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + FILESAPP_DLG_ROW,
                     FILESAPP_QUESTION_WIDTH - SXGUI_DIALOG_MARGIN * 2, SXGUI_FIELD_HEIGHT),
        g_rename_buffer, (int)sizeof(g_rename_buffer));
    g_rename_widgets[2] = sxgui_button(
        sx_rect_make(FILESAPP_QUESTION_WIDTH / 2 - SXGUI_BUTTON_WIDTH - 4,
                     FILESAPP_RENAME_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_rename_ok, 0);
    g_rename_widgets[3] = sxgui_button(
        sx_rect_make(FILESAPP_QUESTION_WIDTH / 2 + 4,
                     FILESAPP_RENAME_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Cancel", on_dialog_cancel, 0);
    g_rename_dialog.title = "Rename";
    g_rename_dialog.widgets = g_rename_widgets;
    g_rename_dialog.widget_count = 4;
    g_rename_dialog.initial_focus = 1;
    g_rename_dialog.default_button = 2;

    g_about_dialog.title = "About Files";
    g_about_dialog.widgets = g_about_widgets;
    g_about_dialog.widget_count = 4;
    g_about_dialog.default_button = 3;

    if (sxgui_app_init(&g_app, "filesapp", g_widgets, W_COUNT) < 0)
    {
        return 1;
    }
    g_app.on_key = on_key;
    g_app.on_pointer = on_pointer;
    g_app.on_resize = on_resize;
    sxgui_set_menubar(&g_app.ui, &g_menubar);
    filesapp_set_view(filesapp_load_view());
    (void)sxgui_app_set_content_size(&g_app, FILESAPP_CONTENT_WIDTH, FILESAPP_CONTENT_HEIGHT);
    filesapp_layout(&g_app);

    /* The folder it was asked to show (a folder opened from the desktop);
     * else the user's profile, as NT's Explorer opens on My Documents; the
     * root for the system, and if the profile is not there. */
    if ((argc < 2 || argv == 0 || argv[1] == 0 || filesapp_load_directory(argv[1]) < 0) &&
        filesapp_load_directory(savanxp_profile_dir()) < 0 && filesapp_load_directory("/") < 0)
    {
        sxgui_app_quit(&g_app, 1);
    }
    /* El contenido de Files ES la lista: sin esto el foco arranca en ninguna
     * parte y las flechas no mueven la seleccion hasta que alguien tabula. */
    sxgui_focus(&g_app.ui, FILESAPP_LIST_INDEX);
    return sxgui_app_run(&g_app);
}
