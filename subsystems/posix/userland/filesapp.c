#include "libc.h"
#include "savanxp/sxgui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

#include "shared/version.h"

#include "file_assoc.h"
#include "file_copy.h"
#include "sxgfx/mime_icon.h"
#include "shortcut.h"
#include "start_pins.h"

#include <string.h>
#include <strings.h>
#include <unistd.h>

/*
 * File browser, shaped like the Windows 98 Explorer: menu bar, a toolbar
 * (Back, Forward, Up, Cut, Copy, Paste, Delete, Properties, Views), an
 * editable address bar,
 * the folder as Large Icons or as a details list (Name / Size / Type) with
 * column headers -- View picks one and filesapp.cfg in the profile remembers
 * it -- and a two-pane status bar (docs/desktop/FILES.md).
 *
 * It does NOT show what files contain: that is an editor's job. Opening a file
 * goes through shortcut_open_path, as on the desktop: a program starts, a
 * document goes to the program ASSOCIATED with its extension (file_assoc,
 * docs/userland/SXE_FORMAT.md phase 5) or to /bin/notepad, and a shortcut opens
 * its target. A right click on an item opens its context menu: Open, Send To
 * Desktop (a shortcut in the Desktop folder), Cut, Copy, Rename, Delete and
 * Properties (docs/desktop/DESKTOP_ICONS.md#phase-4-context-menus-in-applications).
 * New Folder, in the File menu and the folder's context menu, asks for a name
 * and creates the folder in the one shown.
 * Cut, Copy and Paste go through the system clipboard, and a paste runs a
 * piece at a time behind a progress dialog (docs/desktop/FILES.md).
 * Opened with no folder it shows My Computer: the mounted volumes, as drives.
 */

/* ---- toolbar glyphs ----------------------------------------------------------
 * Drawn by system/gfx/toolbar_glyphs/gen_icons.py, embedded (see that file
 * for why). */
#include "../../../system/gfx/toolbar_glyphs/icons.inc"

static struct sx_bitmap g_glyph_bitmaps[9];
/* One-element arrays: a toolbar button reads its glyph from item_icons[0]. */
static const struct sx_bitmap *k_glyph_back[1];
static const struct sx_bitmap *k_glyph_forward[1];
static const struct sx_bitmap *k_glyph_up[1];
static const struct sx_bitmap *k_glyph_cut[1];
static const struct sx_bitmap *k_glyph_copy[1];
static const struct sx_bitmap *k_glyph_paste[1];
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
    k_glyph_cut[0] = filesapp_glyph(6, toolbar_glyph_edit_cut);
    k_glyph_copy[0] = filesapp_glyph(7, toolbar_glyph_edit_copy);
    k_glyph_paste[0] = filesapp_glyph(8, toolbar_glyph_edit_paste);
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
#define FILESAPP_MENU_CUT 15
#define FILESAPP_MENU_COPY 16
#define FILESAPP_MENU_PASTE 17
#define FILESAPP_MENU_NEW_FOLDER 18

/* Back and Forward each remember this many folders; the oldest goes first. */
#define FILESAPP_HISTORY_DEPTH 16

/* My Computer is not a folder but the list of mounted volumes
 * (docs/desktop/FILES.md#my-computer). It stands where a path would, in the
 * address bar and the history; a real path starts with '/', so none is it. */
#define FILESAPP_COMPUTER "My Computer"
#define FILESAPP_DRIVE_TYPE "Local Disk"
#define FILESAPP_DRIVE_ICON "drive-harddisk"

struct filesapp_entry
{
    char name[256];
    unsigned int size;
    int is_dir;
    /* My Computer's entries: the user's folder (`documents`), and the
     * volumes with where each is mounted and its space. */
    int documents;
    int volume;
    char mount_point[SAVANXP_STORAGE_MOUNT_CAPACITY];
    unsigned long long total_bytes;
    unsigned long long free_bytes;
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
    W_CUT,
    W_COPY,
    W_PASTE,
    W_SEP_CLIPBOARD,
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
static void filesapp_ask_new_folder(void);
static void filesapp_show_properties(void);
static void filesapp_cut_selected(void);
static void filesapp_copy_selected(void);
static void filesapp_paste(void);
static int filesapp_can_paste(void);
static void filesapp_apply_cut_fade(void);

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

static int filesapp_is_computer_path(const char *path)
{
    return path != 0 && strcmp(path, FILESAPP_COMPUTER) == 0;
}

static int filesapp_in_computer(void)
{
    return filesapp_is_computer_path(g_current_path);
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

/* The small and large icons of the entry at `index`: the type's, except a
 * Linux ELF, which has no icon of its own and gets the Linux program icon. */
static void filesapp_set_entry_icons(int index, const struct filesapp_entry *entry, int launchable)
{
    char full_path[FILESAPP_PATH_CAPACITY];

    g_item_icons[index] = 0;
    /* A volume is a drive; a folder until the drive's icon is installed. */
    if (entry->volume)
    {
        g_item_icons[index] = mime_icon_small(FILESAPP_DRIVE_ICON);
        g_item_large_icons[index] = mime_icon_large(FILESAPP_DRIVE_ICON);
        if (g_item_icons[index] == 0 || g_item_large_icons[index] == 0)
        {
            g_item_icons[index] = mime_icon_for_file(entry->name, 1, 0);
            g_item_large_icons[index] = mime_icon_large_for_file(entry->name, 1, 0);
        }
        return;
    }
    if (!entry->is_dir && strncmp(g_current_path, "/system/linux/", 14) == 0 &&
        filesapp_join_path(g_current_path, entry->name, full_path, sizeof(full_path)) &&
        shortcut_is_linux_program(full_path))
    {
        g_item_icons[index] = mime_icon_small(SHORTCUT_LINUX_ICON);
        g_item_large_icons[index] = mime_icon_large(SHORTCUT_LINUX_ICON);
    }
    if (g_item_icons[index] == 0)
    {
        g_item_icons[index] = mime_icon_for_file(entry->name, entry->is_dir, launchable);
        g_item_large_icons[index] = mime_icon_large_for_file(entry->name, entry->is_dir, launchable);
    }
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

/* A volume's space: one decimal from KB up, by integer arithmetic, since a
 * disk does not fit filesapp_format_total's 32 bits. */
static void filesapp_format_capacity(unsigned long long bytes, char *buffer, size_t capacity)
{
    static const char *const k_units[] = {"KB", "MB", "GB", "TB"};
    unsigned long long scale = 1024ull;
    int unit = 0;

    if (bytes < 1024ull)
    {
        snprintf(buffer, capacity, "%llu bytes", bytes);
        return;
    }
    while (unit < 3 && bytes >= scale * 1024ull)
    {
        scale *= 1024ull;
        unit += 1;
    }
    snprintf(buffer, capacity, "%llu.%llu %s", bytes / scale, (bytes % scale) * 10ull / scale, k_units[unit]);
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
 * The widget's items are built from g_entries for the view shown, and
 * g_item_entry says which entry each item is (-1 for a group header). Large
 * Icons leaves out "..": Explorer's icon view has no parent entry (Up One
 * Level and Backspace go up). In My Computer, Large Icons groups the entries
 * under headers, as Windows XP's My Computer does
 * (docs/desktop/FILES.md#my-computer); Details lists them plainly.
 */

#define FILESAPP_MAX_ITEMS (FILESAPP_MAX_ENTRIES + 2)

static const char k_group_files[] = SXGUI_GROUP_HEADER_TEXT "Files Stored on This Computer";
static const char k_group_disks[] = SXGUI_GROUP_HEADER_TEXT "Hard Disk Drives";

static const char *g_view_items[FILESAPP_MAX_ITEMS];
static const struct sx_bitmap *g_view_icons[FILESAPP_MAX_ITEMS];
static int g_item_entry[FILESAPP_MAX_ITEMS];
static int g_view_count = 0;

/* The entry behind widget item `item`, or -1 for none or a header. */
static int filesapp_entry_of(int item)
{
    return item >= 0 && item < g_view_count ? g_item_entry[item] : -1;
}

/* The widget item showing entry `entry`, or -1 when this view hides it. */
static int filesapp_item_of(int entry)
{
    int item;

    for (item = 0; entry >= 0 && item < g_view_count; ++item)
    {
        if (g_item_entry[item] == entry)
        {
            return item;
        }
    }
    return -1;
}

static void filesapp_view_add(const char *text, int entry)
{
    if (g_view_count < FILESAPP_MAX_ITEMS)
    {
        g_view_items[g_view_count] = text;
        g_item_entry[g_view_count] = entry;
        g_view_count += 1;
    }
}

/* My Computer's entries of one kind under their header; nothing, header
 * included, when there are none. */
static void filesapp_view_add_group(const char *header, int volumes)
{
    int entry;
    int first = 1;

    for (entry = 0; entry < g_entry_count; ++entry)
    {
        if ((g_entries[entry].volume != 0) == (volumes != 0))
        {
            if (first)
            {
                filesapp_view_add(header, -1);
                first = 0;
            }
            filesapp_view_add(g_item_ptrs[entry], entry);
        }
    }
}

/* Each item's icon, in the size of the view: again whenever icons change. */
static void filesapp_sync_view_icons(void)
{
    int item;

    for (item = 0; item < g_view_count; ++item)
    {
        int entry = g_item_entry[item];

        g_view_icons[item] =
            entry < 0 ? 0 : (FILESAPP_LIST->kind == SXGUI_ICONVIEW ? g_item_large_icons : g_item_icons)[entry];
    }
}

/* Points the widget at the items of the current view, keeping the selected
 * entry selected if the view shows it. */
static void filesapp_bind_list(int selected_entry)
{
    int icons = FILESAPP_LIST->kind == SXGUI_ICONVIEW;
    int entry;

    g_view_count = 0;
    if (icons && filesapp_in_computer())
    {
        filesapp_view_add_group(k_group_files, 0);
        filesapp_view_add_group(k_group_disks, 1);
    }
    else
    {
        for (entry = 0; entry < g_entry_count; ++entry)
        {
            if (!icons || strcmp(g_entries[entry].name, "..") != 0)
            {
                filesapp_view_add(g_item_ptrs[entry], entry);
            }
        }
    }
    filesapp_sync_view_icons();
    FILESAPP_LIST->items = g_view_items;
    FILESAPP_LIST->item_icons = g_view_icons;
    FILESAPP_LIST->item_count = g_view_count;
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

    /* One read of mimeicon.ini per process. It is cheap -- a small ini,
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
        filesapp_set_entry_icons(index, entry, launchable);
    }
    filesapp_apply_cut_fade();
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

/* ---- My Computer ---------------------------------------------------------------
 *
 * One entry per mounted volume, from the storage list (storage_snapshot):
 * named by its label, else "Local Disk", with where it is mounted in
 * brackets, as Windows writes "Local Disk (C:)". The EFI System Partition
 * mounted at /boot is the firmware's and is left out, as Windows hides it.
 */

static void filesapp_volume_name(const char *label, const char *mount_point, char *buffer, size_t capacity)
{
    snprintf(buffer, capacity, "%s (%s)", label != 0 && label[0] != '\0' ? label : FILESAPP_DRIVE_TYPE,
             mount_point);
}

static int filesapp_volume_listed(const struct savanxp_storage_device *device)
{
    return (device->flags & SAVANXP_STORAGE_MOUNTED) != 0 && device->mount_point[0] == '/' &&
           (device->flags & SAVANXP_STORAGE_ESP) == 0 && strcmp(device->mount_point, "/boot") != 0;
}

/* Fills g_entries with the volumes, sorted by where they are mounted, so
 * the root comes first. */
static void filesapp_collect_volumes(void)
{
    struct savanxp_storage_device device;
    unsigned long index;
    int left;

    g_entry_count = 0;
    for (index = 0; g_entry_count < FILESAPP_MAX_ENTRIES && storage_snapshot(index, &device) == 1; ++index)
    {
        struct filesapp_entry *entry = &g_entries[g_entry_count];

        if (!filesapp_volume_listed(&device))
        {
            continue;
        }
        memset(entry, 0, sizeof(*entry));
        device.mount_point[sizeof(device.mount_point) - 1] = '\0';
        device.label[sizeof(device.label) - 1] = '\0';
        filesapp_volume_name(device.label, device.mount_point, entry->name, sizeof(entry->name));
        snprintf(entry->mount_point, sizeof(entry->mount_point), "%s", device.mount_point);
        entry->is_dir = 1;
        entry->volume = 1;
        entry->total_bytes = device.total_bytes;
        entry->free_bytes = device.free_bytes;
        g_entry_count += 1;
    }
    for (left = 0; left < g_entry_count; ++left)
    {
        int right;

        for (right = left + 1; right < g_entry_count; ++right)
        {
            if (strcmp(g_entries[right].mount_point, g_entries[left].mount_point) < 0)
            {
                struct filesapp_entry temp = g_entries[left];
                g_entries[left] = g_entries[right];
                g_entries[right] = temp;
            }
        }
    }
}

/* The user's own folder, first, as XP lists "<user>'s Documents" under
 * Files Stored on This Computer: named after the profile folder's name. */
static void filesapp_add_documents(void)
{
    const char *profile = savanxp_profile_dir();
    const char *slash = profile != 0 ? strrchr(profile, '/') : 0;
    struct filesapp_entry *entry = &g_entries[0];

    if (profile == 0 || profile[0] != '/' || slash == 0 || slash[1] == '\0' || g_entry_count >= FILESAPP_MAX_ENTRIES)
    {
        return;
    }
    memmove(&g_entries[1], &g_entries[0], (size_t)g_entry_count * sizeof(g_entries[0]));
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->name, sizeof(entry->name), "%s's Documents", slash + 1);
    entry->is_dir = 1;
    entry->documents = 1;
    g_entry_count += 1;
}

static int filesapp_load_computer(void)
{
    char size_cell[32];
    int index;

    if (!g_mime_icons_loaded)
    {
        (void)mime_icon_load();
        g_mime_icons_loaded = 1;
    }
    filesapp_collect_volumes();
    filesapp_add_documents();
    snprintf(g_current_path, sizeof(g_current_path), "%s", FILESAPP_COMPUTER);
    for (index = 0; index < g_entry_count; ++index)
    {
        size_cell[0] = '\0';
        if (g_entries[index].volume)
        {
            filesapp_format_capacity(g_entries[index].total_bytes, size_cell, sizeof(size_cell));
        }
        snprintf(g_item_labels[index], sizeof(g_item_labels[index]), "%s%c%s%c%s", g_entries[index].name,
                 SXGUI_COLUMN_SEPARATOR, size_cell, SXGUI_COLUMN_SEPARATOR,
                 g_entries[index].volume ? FILESAPP_DRIVE_TYPE : "File Folder");
        g_item_ptrs[index] = g_item_labels[index];
        filesapp_set_entry_icons(index, &g_entries[index], 0);
    }
    filesapp_bind_list(-1);
    snprintf(g_count_pane, sizeof(g_count_pane), "%d object(s)", g_entry_count);
    snprintf(g_size_pane, sizeof(g_size_pane), "%s", FILESAPP_COMPUTER);

    snprintf(g_address, sizeof(g_address), "%s", g_current_path);
    FILESAPP_ADDRESS->caret = (int)strlen(g_address);
    FILESAPP_ADDRESS->sel_anchor = -1;
    FILESAPP_ADDRESS->scroll = 0;
    filesapp_update_commands();
    return 0;
}

static int filesapp_load_directory(const char *path)
{
    DIR *directory = 0;
    struct dirent *entry = 0;
    char full_path[FILESAPP_PATH_CAPACITY];
    int insert_parent = 0;

    if (filesapp_is_computer_path(path))
    {
        return filesapp_load_computer();
    }
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

    /* Above the root is My Computer, as above C:\ in the Explorer. */
    if (filesapp_in_computer())
    {
        return;
    }
    if (filesapp_is_root_path(g_current_path))
    {
        (void)filesapp_navigate(FILESAPP_COMPUTER);
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
    if (strcasecmp(g_address, FILESAPP_COMPUTER) == 0)
    {
        snprintf(target, sizeof(target), "%s", FILESAPP_COMPUTER);
    }
    else if (g_address[0] == '/')
    {
        snprintf(target, sizeof(target), "%s", g_address);
    }
    else if (!filesapp_join_path(g_current_path, g_address, target, sizeof(target)))
    {
        filesapp_set_status("Path too long.");
        return;
    }
    length = strlen(target);
    while (length > 1 && target[0] == '/' && target[length - 1] == '/')
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
    if (g_entries[selected].volume)
    {
        (void)filesapp_navigate(g_entries[selected].mount_point);
        return;
    }
    if (g_entries[selected].documents)
    {
        (void)filesapp_navigate(savanxp_profile_dir());
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
    {"New Folder...", FILESAPP_MENU_NEW_FOLDER, 0},
    {0, 0, 0},
    {"Open", FILESAPP_MENU_OPEN, 0},
    {0, 0, 0},
    {"Delete", FILESAPP_MENU_DELETE, 0},
    {"Rename...", FILESAPP_MENU_RENAME, 0},
    {"Properties", FILESAPP_MENU_PROPERTIES, 0},
    {0, 0, 0},
    {"Close", FILESAPP_MENU_CLOSE, 0},
};

static struct sxgui_menu_item k_edit_items[] = {
    {"Cut", FILESAPP_MENU_CUT, 0},
    {"Copy", FILESAPP_MENU_COPY, 0},
    {"Paste", FILESAPP_MENU_PASTE, 0},
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

/* The selected entry when it is a file or a folder: what Cut, Copy, Delete
 * and Rename take. A volume in My Computer is not one. */
static int filesapp_selected_file(void)
{
    return filesapp_in_computer() ? -1 : filesapp_selected_entry();
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
    int in_computer = filesapp_in_computer();
    int has_selection = filesapp_selected_file() >= 0;
    int can_go_up = !in_computer;
    /* My Computer has properties only for the volume selected. */
    int has_properties = !in_computer || filesapp_selected_entry() >= 0;

    filesapp_enable(&g_widgets[W_BACK], g_back_count > 0);
    filesapp_enable(&g_widgets[W_FORWARD], g_forward_count > 0);
    filesapp_enable(&g_widgets[W_UP], can_go_up);
    int can_paste = filesapp_can_paste() && !in_computer;

    filesapp_enable(&g_widgets[W_CUT], has_selection);
    filesapp_enable(&g_widgets[W_COPY], has_selection);
    filesapp_enable(&g_widgets[W_PASTE], can_paste);
    filesapp_enable(&g_widgets[W_DELETE], has_selection);
    filesapp_enable(&g_widgets[W_PROPERTIES], has_properties);
    filesapp_enable_item(&k_edit_items[0], has_selection);
    filesapp_enable_item(&k_edit_items[1], has_selection);
    filesapp_enable_item(&k_edit_items[2], can_paste);
    filesapp_enable_item(&k_file_items[2], FILESAPP_LIST->value >= 0);
    filesapp_enable_item(&k_file_items[4], has_selection);
    filesapp_enable_item(&k_file_items[5], has_selection);
    filesapp_enable_item(&k_file_items[0], !in_computer);
    filesapp_enable_item(&k_file_items[6], has_properties);
    filesapp_enable_item(&k_go_items[0], g_back_count > 0);
    filesapp_enable_item(&k_go_items[1], g_forward_count > 0);
    filesapp_enable_item(&k_go_items[2], can_go_up);
}

/* Delete and Rename act on the selected entry, by name (g_subject). */
static int filesapp_take_subject(void)
{
    int row = filesapp_selected_file();

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

static void on_cut_clicked(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    filesapp_cut_selected();
}

static void on_copy_clicked(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    filesapp_copy_selected();
}

static void on_paste_clicked(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    filesapp_paste();
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
    case FILESAPP_MENU_NEW_FOLDER:
        filesapp_ask_new_folder();
        break;
    case FILESAPP_MENU_PROPERTIES:
        filesapp_show_properties();
        break;
    case FILESAPP_MENU_CUT:
        filesapp_cut_selected();
        break;
    case FILESAPP_MENU_COPY:
        filesapp_copy_selected();
        break;
    case FILESAPP_MENU_PASTE:
        filesapp_paste();
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
#define FILESAPP_CMD_PIN_TO_START 27
#define FILESAPP_CMD_CUT 28
#define FILESAPP_CMD_COPY 29
#define FILESAPP_CMD_PASTE 30
#define FILESAPP_CMD_EDIT 31
#define FILESAPP_CMD_NEW_FOLDER 32

#define FILESAPP_QUESTION_WIDTH 340
#define FILESAPP_QUESTION_HEIGHT 96
#define FILESAPP_RENAME_HEIGHT 112

/* Edit needs an associated editor; Pin to Start needs a program or shortcut. */
static struct sxgui_menu_item k_entry_items[] = {
    {"Open", FILESAPP_CMD_OPEN, 0},
    {"Edit", FILESAPP_CMD_EDIT, 0},
    {0, 0, 0},
    {"Send To Desktop", FILESAPP_CMD_SEND_TO_DESKTOP, 0},
    {"Pin to Start Menu", FILESAPP_CMD_PIN_TO_START, 0},
    {0, 0, 0},
    {"Cut", FILESAPP_CMD_CUT, 0},
    {"Copy", FILESAPP_CMD_COPY, 0},
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

/* A volume or the user's folder in My Computer: it opens and has
 * properties; it is not cut, copied, renamed or deleted from there. */
static const struct sxgui_menu_item k_volume_items[] = {
    {"Open", FILESAPP_CMD_OPEN, 0},
    {0, 0, 0},
    {"Properties", FILESAPP_CMD_PROPERTIES, 0},
};

static const struct sxgui_menu k_volume_menu = {
    "Volume",
    k_volume_items,
    (int)(sizeof(k_volume_items) / sizeof(k_volume_items[0])),
};

/* Not const: Paste is enabled only while the clipboard holds a file. */
static struct sxgui_menu_item k_folder_items[] = {
    {"Up One Level", FILESAPP_CMD_UP, 0},
    {"Refresh", FILESAPP_CMD_REFRESH, 0},
    {0, 0, 0},
    {"Paste", FILESAPP_CMD_PASTE, 0},
    {0, 0, 0},
    {"New Folder...", FILESAPP_CMD_NEW_FOLDER, 0},
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
static char g_new_folder_buffer[128];
static struct sxgui_dialog g_new_folder_dialog;
static struct sxgui_widget g_new_folder_widgets[4];
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

/* A shortcut to the entry in the user's Start Menu folder, listed at the top
 * of the Start menu (docs/desktop/START_MENU.md, "Shortcuts"). */
static void filesapp_pin_to_start(void)
{
    static const char *const k_results[] = {
        "Pinned to the Start menu.",
        "It is already on the Start menu.",
        "The Start menu is full: unpin something first.",
        "Cannot pin it to the Start menu.",
    };
    char target[FILESAPP_PATH_CAPACITY];

    if (filesapp_subject_path(target, sizeof(target)))
    {
        filesapp_set_status(k_results[start_pins_pin(target)]);
    }
}

/* Whether the entry is something Pin to Start Menu takes: not a folder. */
static int filesapp_subject_can_pin(void)
{
    char path[FILESAPP_PATH_CAPACITY];
    struct stat info;

    return filesapp_subject_path(path, sizeof(path)) && start_pins_can_pin(path) && stat(path, &info) == 0 &&
           !S_ISDIR(info.st_mode);
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

/* ---- new folder ----------------------------------------------------------------
 *
 * Asks for the name first, offering the first free "New Folder", "New Folder
 * (2)", ... as Windows names one, and creates it in the folder shown, which
 * then shows it selected. Cancel creates nothing.
 */

static int filesapp_entry_named(const char *name)
{
    int index;

    for (index = 0; index < g_entry_count; ++index)
    {
        if (strcmp(g_entries[index].name, name) == 0)
        {
            return index;
        }
    }
    return -1;
}

static void on_new_folder_ok(struct sxgui_widget *widget, void *user)
{
    char path[FILESAPP_PATH_CAPACITY];
    struct stat info;

    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
    if (g_new_folder_buffer[0] == '\0' || strchr(g_new_folder_buffer, '/') != 0 ||
        strcmp(g_new_folder_buffer, ".") == 0 || strcmp(g_new_folder_buffer, "..") == 0)
    {
        filesapp_set_status("A name cannot be empty or contain '/'.");
        return;
    }
    if (!filesapp_join_path(g_current_path, g_new_folder_buffer, path, sizeof(path)))
    {
        filesapp_set_status("The name is too long.");
        return;
    }
    if (stat(path, &info) == 0)
    {
        filesapp_set_status("That name is already taken.");
        return;
    }
    if (mkdir(path, 0755) != 0)
    {
        filesapp_set_status("Cannot create the folder.");
        return;
    }
    (void)filesapp_load_directory(g_current_path);
    filesapp_bind_list(filesapp_entry_named(g_new_folder_buffer));
    filesapp_update_commands();
    filesapp_set_status("Folder created.");
}

static void filesapp_ask_new_folder(void)
{
    char path[FILESAPP_PATH_CAPACITY];
    struct stat info;
    int attempt;

    if (filesapp_in_computer())
    {
        return;
    }
    snprintf(g_new_folder_buffer, sizeof(g_new_folder_buffer), "New Folder");
    for (attempt = 2; attempt < 100 && filesapp_join_path(g_current_path, g_new_folder_buffer, path, sizeof(path)) &&
                      stat(path, &info) == 0;
         ++attempt)
    {
        snprintf(g_new_folder_buffer, sizeof(g_new_folder_buffer), "New Folder (%d)", attempt);
    }
    g_new_folder_widgets[1].caret = (int)strlen(g_new_folder_buffer);
    g_new_folder_widgets[1].scroll = 0;
    sxgui_dialog_begin(&g_app.ui, &g_new_folder_dialog, FILESAPP_QUESTION_WIDTH, FILESAPP_RENAME_HEIGHT);
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
/* A volume's rows: where it is mounted and its space. */
static const char *const k_volume_captions[FILESAPP_PROPERTIES_ROWS] = {
    "Name:", "Type:", "Mounted at:", "Capacity:", "Free space:",
};

static void filesapp_set_properties_captions(const char *const *captions)
{
    int index;

    for (index = 0; index < FILESAPP_PROPERTIES_ROWS; ++index)
    {
        g_properties_widgets[index * 2].text = captions[index];
        g_properties_widgets[index * 2].flags |= SXGUI_FLAG_VISIBLE;
        g_properties_widgets[index * 2 + 1].flags |= SXGUI_FLAG_VISIBLE;
    }
}

/* The selected volume's name, mount point, capacity and free space, as the
 * Explorer's drive Properties gives them. */
static void filesapp_show_volume_properties(const struct filesapp_entry *entry)
{
    char free_text[64];
    unsigned long long percent =
        entry->total_bytes == 0 ? 0 : entry->free_bytes * 100ull / entry->total_bytes;

    filesapp_set_properties_captions(k_volume_captions);
    snprintf(g_properties_title, sizeof(g_properties_title), "%s Properties", entry->name);
    snprintf(g_properties_values[0], FILESAPP_PATH_CAPACITY, "%s", entry->name);
    snprintf(g_properties_values[1], FILESAPP_PATH_CAPACITY, "%s", FILESAPP_DRIVE_TYPE);
    snprintf(g_properties_values[2], FILESAPP_PATH_CAPACITY, "%s", entry->mount_point);
    filesapp_format_capacity(entry->total_bytes, g_properties_values[3], FILESAPP_PATH_CAPACITY);
    filesapp_format_capacity(entry->free_bytes, free_text, sizeof(free_text));
    snprintf(g_properties_values[4], FILESAPP_PATH_CAPACITY, "%s (%llu%% free)", free_text, percent);
    sxgui_dialog_begin(&g_app.ui, &g_properties_dialog, FILESAPP_PROPERTIES_WIDTH, FILESAPP_PROPERTIES_HEIGHT);
}

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

    if (filesapp_in_computer() && (row < 0 || g_entries[row].volume))
    {
        if (row >= 0)
        {
            filesapp_show_volume_properties(&g_entries[row]);
        }
        return;
    }
    filesapp_set_properties_captions(k_properties_captions);
    /* The user's folder from My Computer: the folder's own properties. */
    if (filesapp_in_computer())
    {
        const char *slash;

        snprintf(path, sizeof(path), "%s", savanxp_profile_dir());
        slash = strrchr(path, '/');
        snprintf(name, sizeof(name), "%s", slash != 0 && slash[1] != '\0' ? slash + 1 : path);
        filesapp_parent_path(path, g_properties_values[2], FILESAPP_PATH_CAPACITY);
    }
    else if (row >= 0)
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

/* ---- cut, copy and paste ------------------------------------------------------
 *
 * (docs/desktop/FILES.md, "Cut, Copy and Paste".) Copy and Cut put the
 * selected entry on the system clipboard as a file:// URI (text/uri-list), as
 * its path in plain text, so a text editor pastes the path, and with a drop
 * effect saying "copy" or "move". Paste takes the one URI there and copies or
 * moves it into the folder shown, through the shared engine (file_copy.h).
 *
 * The cut entry is drawn faded until it is pasted or something else is
 * copied: the clipboard's sequence says which, read on a slow tick.
 */

#define FILESAPP_DROP_EFFECT "application/x-savanxp-drop-effect"
#define FILESAPP_URI_CAPACITY (FILESAPP_PATH_CAPACITY * 3 + 16)
/* The tick while nothing runs: how soon Paste follows another program's
 * copy. While a paste runs it is the frame's 16 ms. */
#define FILESAPP_IDLE_TICK_MS 500
#define FILESAPP_BUSY_TICK_MS 16
/* A paste runs in the click until this long has passed, so a small one
 * never flashes a dialog; then it goes on behind the progress dialog. */
#define FILESAPP_DIALOG_DELAY_MS 500
/* How long each tick may copy before it lets the window paint. */
#define FILESAPP_WORK_SLICE_MS 40

static char g_cut_path[FILESAPP_PATH_CAPACITY];
static unsigned long long g_cut_sequence;
static unsigned long long g_seen_sequence = ~0ull;
static int g_paste_available;

/* RFC 3986: the unreserved characters and '/' stay, every other byte is
 * %XX. A path with a space or a non-ASCII name survives the trip. */
static int filesapp_uri_from_path(const char *path, char *uri, size_t capacity)
{
    static const char k_hex[] = "0123456789ABCDEF";
    size_t used = 0;
    const unsigned char *cursor = (const unsigned char *)path;

    if (snprintf(uri, capacity, "file://") >= (int)capacity)
    {
        return -1;
    }
    used = 7;
    for (; *cursor != '\0'; ++cursor)
    {
        unsigned char c = *cursor;
        int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '.' || c == '_' || c == '~' || c == '/';

        if (used + (plain ? 1u : 3u) >= capacity)
        {
            return -1;
        }
        if (plain)
        {
            uri[used++] = (char)c;
        }
        else
        {
            uri[used++] = '%';
            uri[used++] = k_hex[c >> 4];
            uri[used++] = k_hex[c & 15u];
        }
    }
    uri[used] = '\0';
    return 0;
}

static int filesapp_hex_value(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

/* The path of a file:// URI of `length` bytes: an empty host or
 * "localhost", %XX decoded. Anything else, a NUL included, is refused. */
static int filesapp_path_from_uri(const char *uri, size_t length, char *path, size_t capacity)
{
    size_t index = 7;
    size_t used = 0;

    if (length < 8 || strncmp(uri, "file://", 7) != 0)
    {
        return -1;
    }
    if (uri[index] != '/')
    {
        if (length - index < 10 || strncmp(uri + index, "localhost/", 10) != 0)
        {
            return -1;
        }
        index += 9;
    }
    for (; index < length; ++index)
    {
        char c = uri[index];

        if (c == '%')
        {
            int high = index + 2 < length ? filesapp_hex_value(uri[index + 1]) : -1;
            int low = index + 2 < length ? filesapp_hex_value(uri[index + 2]) : -1;

            if (high < 0 || low < 0 || (high == 0 && low == 0))
            {
                return -1;
            }
            c = (char)(high * 16 + low);
            index += 2;
        }
        if (used + 1 >= capacity)
        {
            return -1;
        }
        path[used++] = c;
    }
    path[used] = '\0';
    return path[0] == '/' ? 0 : -1;
}

/* The one path a text/uri-list names, skipping comment lines. 1 with a
 * path, 0 when it names none, -1 when it names several or a URI this
 * cannot read. */
static int filesapp_path_from_list(const char *list, int length, char *path, size_t capacity)
{
    int found = 0;
    int start = 0;

    while (start < length)
    {
        int end = start;
        int stop;

        while (end < length && list[end] != '\n')
        {
            ++end;
        }
        stop = end > start && list[end - 1] == '\r' ? end - 1 : end;
        if (stop > start && list[start] != '#')
        {
            if (found || filesapp_path_from_uri(list + start, (size_t)(stop - start), path, capacity) != 0)
            {
                return -1;
            }
            found = 1;
        }
        start = end + 1;
    }
    return found;
}

/* What the clipboard offers to paste: a path and whether it was cut. 1 with
 * one, 0 with none, -1 with several items (not taken yet). */
static int filesapp_clipboard_source(char *path, size_t capacity, int *move)
{
    char list[FILESAPP_URI_CAPACITY * 2];
    char effect[16];
    int length = sxgui_clipboard_get_format(SAVANXP_CLIPBOARD_URI_LIST, list, (int)sizeof(list));
    int result;
    int effect_length;

    if (length <= 0)
    {
        return 0;
    }
    if (length > (int)sizeof(list))
    {
        return -1;
    }
    result = filesapp_path_from_list(list, length, path, capacity);
    if (result <= 0)
    {
        return result;
    }
    effect_length = sxgui_clipboard_get_format(FILESAPP_DROP_EFFECT, effect, (int)sizeof(effect) - 1);
    *move = effect_length == 4 && strncmp(effect, "move", 4) == 0;
    return 1;
}

static unsigned long long filesapp_clipboard_sequence(void)
{
    struct savanxp_clipboard_info info;

    return clipboard_get_info(&info) == 0 ? (unsigned long long)info.sequence : 0;
}

static int filesapp_can_paste(void)
{
    return g_paste_available;
}

/* ---- the cut entry, faded ----------------------------------------------------- */

static uint32_t g_faded_small[SXGUI_LISTBOX_ICON_SIZE * SXGUI_LISTBOX_ICON_SIZE];
static uint32_t g_faded_large[SXGUI_ICONVIEW_ICON_SIZE * SXGUI_ICONVIEW_ICON_SIZE];
static struct sx_bitmap g_faded_small_bitmap;
static struct sx_bitmap g_faded_large_bitmap;

/* `icon` at half its opacity, as the Explorer ghosts an item that is cut. */
static const struct sx_bitmap *filesapp_faded(
    const struct sx_bitmap *icon, uint32_t *pixels, int size, struct sx_bitmap *bitmap)
{
    struct savanxp_fb_info info;
    int width;
    int height;
    int row;

    if (icon == 0 || icon->pixels == 0 || icon->format != SX_PIXEL_FORMAT_BGRA8888)
    {
        return icon;
    }
    width = (int)icon->info.width < size ? (int)icon->info.width : size;
    height = (int)icon->info.height < size ? (int)icon->info.height : size;
    memset(&info, 0, sizeof(info));
    info.width = (uint32_t)width;
    info.height = (uint32_t)height;
    info.pitch = (uint32_t)size * (uint32_t)sizeof(uint32_t);
    info.bpp = 32u;
    info.buffer_size = info.pitch * (uint32_t)size;
    for (row = 0; row < height; ++row)
    {
        const uint32_t *source = (const uint32_t *)((const uint8_t *)icon->pixels + (size_t)row * icon->info.pitch);
        int column;

        for (column = 0; column < width; ++column)
        {
            uint32_t pixel = source[column];
            pixels[row * size + column] = (pixel & 0x00ffffffu) | (((pixel >> 24) / 2u) << 24);
        }
    }
    sx_bitmap_wrap(bitmap, pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    return bitmap;
}

static void filesapp_apply_cut_fade(void)
{
    char full_path[FILESAPP_PATH_CAPACITY];
    int index;

    if (g_cut_path[0] == '\0')
    {
        return;
    }
    for (index = 0; index < g_entry_count; ++index)
    {
        if (filesapp_join_path(g_current_path, g_entries[index].name, full_path, sizeof(full_path)) &&
            strcmp(full_path, g_cut_path) == 0)
        {
            g_item_icons[index] =
                filesapp_faded(g_item_icons[index], g_faded_small, SXGUI_LISTBOX_ICON_SIZE, &g_faded_small_bitmap);
            g_item_large_icons[index] = filesapp_faded(
                g_item_large_icons[index], g_faded_large, SXGUI_ICONVIEW_ICON_SIZE, &g_faded_large_bitmap);
            return;
        }
    }
}

/* The icons again, without touching the selection or the scroll. */
static void filesapp_refresh_icons(void)
{
    char full_path[FILESAPP_PATH_CAPACITY];
    int index;

    for (index = 0; index < g_entry_count; ++index)
    {
        const struct filesapp_entry *entry = &g_entries[index];
        int launchable = !entry->is_dir &&
            filesapp_join_path(g_current_path, entry->name, full_path, sizeof(full_path)) &&
            filesapp_path_is_launchable(full_path);

        filesapp_set_entry_icons(index, entry, launchable);
    }
    filesapp_apply_cut_fade();
    filesapp_sync_view_icons();
}

/* Notices another program's copy: Paste follows it, and a cut entry that
 * is no longer on the clipboard stops being faded. */
static void filesapp_check_clipboard(void)
{
    unsigned long long sequence = filesapp_clipboard_sequence();
    char path[FILESAPP_PATH_CAPACITY];
    int move = 0;

    if (sequence == g_seen_sequence)
    {
        return;
    }
    g_seen_sequence = sequence;
    g_paste_available = filesapp_clipboard_source(path, sizeof(path), &move) > 0;
    if (g_cut_path[0] != '\0' && sequence != g_cut_sequence)
    {
        g_cut_path[0] = '\0';
        filesapp_refresh_icons();
    }
    filesapp_update_commands();
    sxgui_app_request_repaint(&g_app);
}

static void filesapp_put_selected(int cut)
{
    char path[FILESAPP_PATH_CAPACITY];
    char uri[FILESAPP_URI_CAPACITY];
    char list[FILESAPP_URI_CAPACITY + 2];
    struct sxgui_clipboard_format formats[3];
    const char *effect = cut ? "move" : "copy";

    if (!filesapp_take_subject() || !filesapp_subject_path(path, sizeof(path)) ||
        filesapp_uri_from_path(path, uri, sizeof(uri)) != 0)
    {
        return;
    }
    snprintf(list, sizeof(list), "%s\r\n", uri);
    formats[0].name = SAVANXP_CLIPBOARD_URI_LIST;
    formats[0].data = list;
    formats[0].length = (unsigned int)strlen(list);
    formats[1].name = SAVANXP_CLIPBOARD_TEXT;
    formats[1].data = path;
    formats[1].length = (unsigned int)strlen(path);
    formats[2].name = FILESAPP_DROP_EFFECT;
    formats[2].data = effect;
    formats[2].length = 4;
    if (sxgui_clipboard_set_formats(formats, 3) < 0)
    {
        filesapp_set_status("Cannot put it on the clipboard.");
        return;
    }
    g_cut_path[0] = '\0';
    if (cut)
    {
        snprintf(g_cut_path, sizeof(g_cut_path), "%s", path);
        g_cut_sequence = filesapp_clipboard_sequence();
    }
    filesapp_refresh_icons();
    filesapp_check_clipboard();
    filesapp_set_status(cut ? "Cut. Paste it where it should go." : "Copied to the clipboard.");
}

static void filesapp_cut_selected(void)
{
    filesapp_put_selected(1);
}

static void filesapp_copy_selected(void)
{
    filesapp_put_selected(0);
}

/* ---- the paste and its progress dialog ---------------------------------------- */

#define FILESAPP_PROGRESS_WIDTH 380
#define FILESAPP_PROGRESS_HEIGHT \
    (SXGUI_DIALOG_MARGIN * 3 + FILESAPP_DLG_ROW * 3 + SXGUI_BUTTON_HEIGHT)
#define FILESAPP_PROGRESS_SCALE 1000

static struct file_copy g_op;
static int g_op_move;
static int g_op_dialog_open;
static unsigned long g_op_started_ms;
static char g_op_name[256];      /* the name the pasted entry gets */
static char g_op_source_name[256];
static char g_op_title[32];
static char g_op_current_text[FILESAPP_PATH_CAPACITY];
static char g_op_route_text[FILESAPP_PATH_CAPACITY * 2];
static char g_op_time_text[64];
static struct sxgui_dialog g_op_dialog;
static struct sxgui_widget g_op_widgets[5];
static struct sxgui_dialog g_message_dialog;
static struct sxgui_widget g_message_widgets[3];
static char g_message_text[2][FILESAPP_PATH_CAPACITY + 64];

/* The last component of `path`, "/" for the root. */
static const char *filesapp_base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash == 0 ? path : slash[1] == '\0' ? "/" : slash + 1;
}

static void filesapp_show_message(const char *title, const char *first, const char *second)
{
    snprintf(g_message_text[0], sizeof(g_message_text[0]), "%s", first);
    snprintf(g_message_text[1], sizeof(g_message_text[1]), "%s", second != 0 ? second : "");
    g_message_dialog.title = title;
    sxgui_dialog_begin(&g_app.ui, &g_message_dialog, FILESAPP_PROGRESS_WIDTH, FILESAPP_QUESTION_HEIGHT + 20);
}

/* Work done and to do, in one unit: bytes, plus a little for every entry,
 * so a tree of empty files still moves the bar. */
static unsigned long long filesapp_op_units(unsigned long long bytes, unsigned int items)
{
    return bytes + (unsigned long long)items * 4096ull;
}

static void filesapp_update_progress(void)
{
    unsigned long long total = filesapp_op_units(g_op.total_bytes, g_op.total_items);
    unsigned long long done = filesapp_op_units(g_op.done_bytes, g_op.done_items);
    unsigned long elapsed = uptime_ms() - g_op_started_ms;

    if (g_op.state == FILE_COPY_COUNTING)
    {
        snprintf(g_op_current_text, sizeof(g_op_current_text), "Preparing to %s...", g_op_move ? "move" : "copy");
        g_op_time_text[0] = '\0';
        g_op_widgets[2].value = 0;
        return;
    }
    if (g_op.state == FILE_COPY_REMOVING)
    {
        snprintf(g_op_current_text, sizeof(g_op_current_text), "Removing the originals...");
    }
    else
    {
        snprintf(g_op_current_text, sizeof(g_op_current_text), "%s", filesapp_base_name(g_op.current));
    }
    if (done > total)
    {
        done = total;
    }
    g_op_widgets[2].value = total == 0 ? 0 : (int)(done * FILESAPP_PROGRESS_SCALE / total);
    /* An estimate needs something to go on: a second of work and some of it
     * done. */
    if (elapsed >= 1000u && done > 0 && done < total)
    {
        unsigned long long remaining_ms = (unsigned long long)elapsed * (total - done) / done;
        unsigned long seconds = (unsigned long)(remaining_ms / 1000ull) + 1u;

        if (seconds < 60u)
        {
            snprintf(g_op_time_text, sizeof(g_op_time_text), "%lu Second%s Remaining", seconds,
                     seconds == 1u ? "" : "s");
        }
        else
        {
            snprintf(g_op_time_text, sizeof(g_op_time_text), "%lu Minute%s Remaining", (seconds + 59u) / 60u,
                     (seconds + 59u) / 60u == 1u ? "" : "s");
        }
    }
}

/* Runs the paste until it ends or `budget_ms` has passed. */
static void filesapp_run_op(unsigned long budget_ms)
{
    unsigned long started = uptime_ms();

    while (file_copy_running(&g_op) && uptime_ms() - started < budget_ms)
    {
        (void)file_copy_step(&g_op);
    }
}

/* The folder again with the pasted entry selected, and what happened. */
static void filesapp_finish_op(void)
{
    char line[FILESAPP_PATH_CAPACITY + 64];
    char left[96];
    const char *verb = g_op_move ? "moved" : "copied";
    int index;

    g_app.tick_interval_ms = FILESAPP_IDLE_TICK_MS;
    if (g_op_dialog_open)
    {
        g_op_dialog_open = 0;
        if (sxgui_dialog_active(&g_app.ui))
        {
            sxgui_dialog_end(&g_app.ui, 1);
        }
    }
    (void)filesapp_load_directory(g_current_path);
    for (index = 0; index < g_entry_count; ++index)
    {
        if (strcmp(g_entries[index].name, g_op_name) == 0)
        {
            filesapp_bind_list(index);
            break;
        }
    }
    snprintf(left, sizeof(left), "%u of %u item(s) were %s.", g_op.done_items, g_op.total_items, verb);
    switch (g_op.state)
    {
    case FILE_COPY_DONE:
        if (g_op_move)
        {
            /* What was cut is where it belongs now: the clipboard no
             * longer offers it, as in Windows. */
            g_cut_path[0] = '\0';
            (void)clipboard_clear();
        }
        snprintf(line, sizeof(line), g_op_move ? "Moved '%s'." : "Copied '%s'.", g_op_name);
        filesapp_set_status(line);
        break;
    case FILE_COPY_CANCELLED:
        if (g_op_move && g_op.done_items >= g_op.total_items)
        {
            /* Cancelled while the originals were being removed: the copy is
             * whole, some originals stay. */
            filesapp_set_status("Cancelled. The copy is complete; some originals are still in place.");
            break;
        }
        snprintf(line, sizeof(line), "Cancelled. %s", left);
        filesapp_set_status(line);
        break;
    case FILE_COPY_FAILED:
        snprintf(line, sizeof(line), "Cannot %s '%s': %s.", g_op_move ? "move" : "copy",
                 filesapp_base_name(g_op.failed_path), strerror(g_op.error));
        if (g_op_move && g_op.done_items >= g_op.total_items)
        {
            /* The copy is whole; only the originals could not all go. */
            snprintf(left, sizeof(left), "The copy is complete; some originals are still in place.");
        }
        filesapp_set_status("The paste did not finish.");
        filesapp_show_message(g_op_move ? "Error Moving File" : "Error Copying File", line, left);
        break;
    default:
        break;
    }
    filesapp_check_clipboard();
    filesapp_update_commands();
    sxgui_app_request_repaint(&g_app);
}

static void on_op_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    /* The tick sees the dialog gone and cancels, as for its X. */
    sxgui_dialog_end(&g_app.ui, 0);
}

static void filesapp_paste(void)
{
    char source[FILESAPP_PATH_CAPACITY];
    char source_parent[FILESAPP_PATH_CAPACITY];
    char target[FILESAPP_PATH_CAPACITY];
    char message[FILESAPP_PATH_CAPACITY + 64];
    int move = 0;
    int found;
    int result;

    /* My Computer holds volumes, not files: nothing is pasted into it. */
    if (file_copy_running(&g_op) || sxgui_dialog_active(&g_app.ui) || filesapp_in_computer())
    {
        return;
    }
    found = filesapp_clipboard_source(source, sizeof(source), &move);
    if (found < 0)
    {
        filesapp_set_status("Paste takes one item at a time.");
        return;
    }
    if (found == 0)
    {
        return;
    }
    snprintf(g_op_source_name, sizeof(g_op_source_name), "%s", filesapp_base_name(source));
    filesapp_parent_path(source, source_parent, sizeof(source_parent));
    if (move && strcmp(source_parent, g_current_path) == 0)
    {
        /* Cut and pasted in the same folder: it is already here. */
        g_cut_path[0] = '\0';
        filesapp_refresh_icons();
        filesapp_set_status("It is already in this folder.");
        return;
    }
    if (move)
    {
        snprintf(g_op_name, sizeof(g_op_name), "%s", g_op_source_name);
    }
    else if (file_copy_free_name(g_current_path, g_op_source_name, g_op_name, sizeof(g_op_name)) != 0)
    {
        filesapp_set_status("Cannot find a free name for the copy.");
        return;
    }
    if (file_copy_join(target, sizeof(target), g_current_path, g_op_name) != 0)
    {
        filesapp_set_status("Path too long.");
        return;
    }

    g_op_move = move;
    g_op.report = 0;
    result = file_copy_begin(&g_op, source, target, move ? FILE_COPY_MOVE : 0);
    if (result < 0)
    {
        if (result == -EINVAL)
        {
            snprintf(message, sizeof(message), "Cannot %s '%s' into itself.", move ? "move" : "copy",
                     g_op_source_name);
        }
        else if (result == -EEXIST)
        {
            snprintf(message, sizeof(message), "There is already an item named '%s' here.", g_op_name);
        }
        else
        {
            snprintf(message, sizeof(message), "Cannot paste '%s': %s.", g_op_source_name, strerror(-result));
        }
        filesapp_set_status(message);
        return;
    }

    g_op_started_ms = uptime_ms();
    filesapp_run_op(FILESAPP_DIALOG_DELAY_MS);
    if (!file_copy_running(&g_op))
    {
        filesapp_finish_op();
        return;
    }
    snprintf(g_op_title, sizeof(g_op_title), move ? "Moving..." : "Copying...");
    snprintf(g_op_route_text, sizeof(g_op_route_text), "From '%s' to '%s'", filesapp_base_name(source_parent),
             filesapp_base_name(g_current_path));
    g_op_time_text[0] = '\0';
    filesapp_update_progress();
    g_op_dialog.title = g_op_title;
    g_op_dialog_open = 1;
    sxgui_dialog_begin(&g_app.ui, &g_op_dialog, FILESAPP_PROGRESS_WIDTH, FILESAPP_PROGRESS_HEIGHT);
    g_app.tick_interval_ms = FILESAPP_BUSY_TICK_MS;
}

static void on_tick(struct sxgui_app *app)
{
    (void)app;
    if (!file_copy_running(&g_op))
    {
        if (!sxgui_dialog_active(&g_app.ui))
        {
            filesapp_check_clipboard();
        }
        return;
    }
    /* Cancel, or the dialog's X: either one ended the dialog. */
    if (g_op_dialog_open && !sxgui_dialog_active(&g_app.ui))
    {
        g_op_dialog_open = 0;
        file_copy_cancel(&g_op);
        filesapp_finish_op();
        return;
    }
    filesapp_run_op(FILESAPP_WORK_SLICE_MS);
    if (!file_copy_running(&g_op))
    {
        filesapp_finish_op();
        return;
    }
    filesapp_update_progress();
    sxgui_app_request_repaint(&g_app);
}

static void filesapp_build_op_dialogs(void)
{
    int inner = FILESAPP_PROGRESS_WIDTH - SXGUI_DIALOG_MARGIN * 2;
    int button_y = FILESAPP_PROGRESS_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT;

    g_op_widgets[0] = sxgui_label(sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN, inner, 18), g_op_current_text);
    g_op_widgets[1] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + FILESAPP_DLG_ROW, inner, 18), g_op_route_text);
    g_op_widgets[2] = sxgui_progress(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + FILESAPP_DLG_ROW * 2, inner, 18), 0,
        FILESAPP_PROGRESS_SCALE, 0);
    g_op_widgets[3] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, button_y + (SXGUI_BUTTON_HEIGHT - 18) / 2,
                     inner - SXGUI_BUTTON_WIDTH - SXGUI_GAP, 18),
        g_op_time_text);
    g_op_widgets[4] = sxgui_button(
        sx_rect_make(FILESAPP_PROGRESS_WIDTH - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_WIDTH, button_y,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Cancel", on_op_cancel, 0);
    g_op_dialog.widgets = g_op_widgets;
    g_op_dialog.widget_count = 5;
    g_op_dialog.initial_focus = 4;
    g_op_dialog.default_button = 0;

    g_message_widgets[0] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN, inner, 18), g_message_text[0]);
    g_message_widgets[1] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + FILESAPP_DLG_ROW, inner, 18), g_message_text[1]);
    g_message_widgets[2] = sxgui_button(
        sx_rect_make((FILESAPP_PROGRESS_WIDTH - SXGUI_BUTTON_WIDTH) / 2,
                     FILESAPP_QUESTION_HEIGHT + 20 - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_dialog_ok, 0);
    g_message_dialog.widgets = g_message_widgets;
    g_message_dialog.widget_count = 3;
    g_message_dialog.initial_focus = 2;
    g_message_dialog.default_button = 2;
}

static void on_context_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case FILESAPP_CMD_OPEN:
        filesapp_activate_selected();
        break;
    case FILESAPP_CMD_EDIT:
        {
            char path[FILESAPP_PATH_CAPACITY];
            if (filesapp_join_path(g_current_path, g_subject, path, sizeof(path)) &&
                shortcut_edit_path(&g_app.gfx, path) != SHORTCUT_OPEN_FAILED)
                filesapp_set_status("Opening with associated editor...");
            else filesapp_set_status("Cannot edit it.");
        }
        break;
    case FILESAPP_CMD_SEND_TO_DESKTOP:
        filesapp_send_to_desktop();
        break;
    case FILESAPP_CMD_PIN_TO_START:
        filesapp_pin_to_start();
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
    case FILESAPP_CMD_CUT:
        filesapp_cut_selected();
        break;
    case FILESAPP_CMD_COPY:
        filesapp_copy_selected();
        break;
    case FILESAPP_CMD_PASTE:
        filesapp_paste();
        break;
    case FILESAPP_CMD_NEW_FOLDER:
        filesapp_ask_new_folder();
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
    /* What My Computer lists opens and has properties, nothing more. */
    if (row >= 0 && row < g_entry_count && filesapp_in_computer())
    {
        FILESAPP_LIST->value = item;
        filesapp_update_commands();
        sxgui_context_menu_open(&g_app.ui, &k_volume_menu, x, y, on_context_command, 0);
        return;
    }
    if (row >= 0 && row < g_entry_count && strcmp(g_entries[row].name, "..") != 0)
    {
        FILESAPP_LIST->value = item;
        snprintf(g_subject, sizeof(g_subject), "%s", g_entries[row].name);
        filesapp_update_commands();
        {
            char path[FILESAPP_PATH_CAPACITY];
            filesapp_enable_item(&k_entry_items[1],
                filesapp_join_path(g_current_path, g_subject, path, sizeof(path)) && shortcut_can_edit_path(path));
        }
        filesapp_enable_item(&k_entry_items[4], filesapp_subject_can_pin());
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
    /* My Computer's own menu keeps only Refresh. */
    filesapp_enable_item(&k_folder_items[0], !filesapp_in_computer());
    filesapp_enable_item(&k_folder_items[3], filesapp_can_paste() && !filesapp_in_computer());
    filesapp_enable_item(&k_folder_items[5], !filesapp_in_computer());
    filesapp_enable_item(&k_folder_items[7], !filesapp_in_computer());
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
    if ((event->modifiers & SAVANXP_KEY_MOD_CTRL) != 0)
    {
        int letter = event->ascii >= 'A' && event->ascii <= 'Z' ? event->ascii - 'A' + 'a' : event->ascii;

        if (letter == 'x')
        {
            filesapp_cut_selected();
            return 1;
        }
        if (letter == 'c')
        {
            filesapp_copy_selected();
            return 1;
        }
        if (letter == 'v')
        {
            filesapp_paste();
            return 1;
        }
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
    W_BACK, W_FORWARD, W_SEP_HISTORY, W_UP, W_SEP_UP, W_CUT, W_COPY, W_PASTE, W_SEP_CLIPBOARD,
    W_DELETE, W_PROPERTIES, W_SEP_EDIT, W_VIEWS,
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

/* Wide enough for the whole toolbar, or the comfortable width if that is
 * wider: a narrower window hides the buttons that do not fit. */
static int filesapp_content_width(void)
{
    int width = SXGUI_MARGIN * 2;
    int index;

    for (index = 0; index < (int)(sizeof(k_toolbar_order) / sizeof(k_toolbar_order[0])); ++index)
    {
        width += g_widgets[k_toolbar_order[index]].kind == SXGUI_BUTTON ? filesapp_toolbar_button_width()
                                                                          : FILESAPP_SEPARATOR_WIDTH;
    }
    return width > FILESAPP_CONTENT_WIDTH ? width : FILESAPP_CONTENT_WIDTH;
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
    {"Edit", k_edit_items, (int)(sizeof(k_edit_items) / sizeof(k_edit_items[0]))},
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

/* ---- selftest: cut, copy and paste ---------------------------------------------
 *
 * The copy engine and the clipboard's URIs, on a scratch tree in /tmp. Each
 * failed check is its own FILESAPP SMOKE FAIL line. */

#define FILESAPP_TEST_ROOT "/tmp/filesapp-copytest"

static int g_copy_failures;

static void filesapp_copy_check(int ok, const char *label)
{
    if (!ok)
    {
        printf("FILESAPP SMOKE FAIL copy: %s\n", label);
        g_copy_failures += 1;
    }
}

static void filesapp_test_remove(const char *path)
{
    const char *argv[] = {"/bin/rm", "-rf", path, 0};
    int status = -1;
    long pid = spawn("/bin/rm", argv, 3);

    if (pid > 0)
    {
        (void)savanxp_waitpid((int)pid, &status);
    }
}

/* A file of `size` bytes whose every byte depends on its offset and `seed`. */
static int filesapp_test_write(const char *path, unsigned int size, unsigned int seed)
{
    unsigned char block[512];
    unsigned int written = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0)
    {
        return -1;
    }
    while (written < size)
    {
        unsigned int count = size - written < sizeof(block) ? size - written : (unsigned int)sizeof(block);
        unsigned int index;

        for (index = 0; index < count; ++index)
        {
            block[index] = (unsigned char)((written + index) * 7u + seed);
        }
        if (write(fd, block, count) != (long)count)
        {
            close(fd);
            return -1;
        }
        written += count;
    }
    return close(fd);
}

static int filesapp_test_matches(const char *path, unsigned int size, unsigned int seed)
{
    unsigned char block[512];
    unsigned int offset = 0;
    int fd = open(path, O_RDONLY);
    long count;

    if (fd < 0)
    {
        return 0;
    }
    while ((count = read(fd, block, sizeof(block))) > 0)
    {
        long index;

        for (index = 0; index < count; ++index)
        {
            if (block[index] != (unsigned char)((offset + (unsigned int)index) * 7u + seed))
            {
                close(fd);
                return 0;
            }
        }
        offset += (unsigned int)count;
    }
    close(fd);
    return count == 0 && offset == size;
}

static int filesapp_test_exists(const char *path)
{
    struct stat info;

    return lstat(path, &info) == 0;
}

static int filesapp_test_run(struct file_copy *op)
{
    int result;
    int steps = 0;

    while ((result = file_copy_step(op)) > 0 && steps < 1000000)
    {
        ++steps;
    }
    return result;
}

static void filesapp_copy_engine_selftest(void)
{
    static struct file_copy op;
    char name[128];
    int result;

    filesapp_test_remove(FILESAPP_TEST_ROOT);
    filesapp_copy_check(mkdir(FILESAPP_TEST_ROOT, 0755) == 0 && mkdir(FILESAPP_TEST_ROOT "/src", 0755) == 0 &&
                            mkdir(FILESAPP_TEST_ROOT "/src/sub", 0755) == 0 &&
                            mkdir(FILESAPP_TEST_ROOT "/src/empty", 0755) == 0 &&
                            mkdir(FILESAPP_TEST_ROOT "/dst", 0755) == 0 &&
                            filesapp_test_write(FILESAPP_TEST_ROOT "/src/a.txt", 5000, 1) == 0 &&
                            filesapp_test_write(FILESAPP_TEST_ROOT "/src/sub/b.bin", 40000, 2) == 0,
                        "build the scratch tree");

    /* A tree: counted first, then copied whole. */
    op.report = 0;
    result = file_copy_begin(&op, FILESAPP_TEST_ROOT "/src", FILESAPP_TEST_ROOT "/dst/src", 0);
    filesapp_copy_check(result == 0 && op.state == FILE_COPY_COUNTING, "begin a copy");
    result = filesapp_test_run(&op);
    filesapp_copy_check(result == 0 && op.state == FILE_COPY_DONE, "the copy ends DONE");
    filesapp_copy_check(op.total_items == 5 && op.done_items == 5, "five items counted and copied");
    filesapp_copy_check(op.total_bytes == 45000ull && op.done_bytes == 45000ull, "45000 bytes counted and copied");
    filesapp_copy_check(filesapp_test_matches(FILESAPP_TEST_ROOT "/dst/src/a.txt", 5000, 1) &&
                            filesapp_test_matches(FILESAPP_TEST_ROOT "/dst/src/sub/b.bin", 40000, 2) &&
                            filesapp_test_exists(FILESAPP_TEST_ROOT "/dst/src/empty"),
                        "the copy holds the same bytes and the empty folder");

    /* Names Explorer gives a second and a third copy. */
    filesapp_copy_check(file_copy_free_name(FILESAPP_TEST_ROOT "/dst", "src", name, sizeof(name)) == 0 &&
                            strcmp(name, "Copy of src") == 0,
                        "a taken name becomes 'Copy of'");
    (void)mkdir(FILESAPP_TEST_ROOT "/dst/Copy of src", 0755);
    filesapp_copy_check(file_copy_free_name(FILESAPP_TEST_ROOT "/dst", "src", name, sizeof(name)) == 0 &&
                            strcmp(name, "Copy (2) of src") == 0,
                        "then 'Copy (2) of'");
    filesapp_copy_check(file_copy_free_name(FILESAPP_TEST_ROOT "/dst", "new.txt", name, sizeof(name)) == 0 &&
                            strcmp(name, "new.txt") == 0,
                        "a free name stays");

    /* Refused before anything is touched. */
    filesapp_copy_check(file_copy_begin(&op, FILESAPP_TEST_ROOT "/src", FILESAPP_TEST_ROOT "/src/sub/x", 0) == -EINVAL,
                        "a folder into itself is EINVAL");
    filesapp_copy_check(file_copy_begin(&op, FILESAPP_TEST_ROOT "/src", FILESAPP_TEST_ROOT "/src", 0) == -EINVAL,
                        "a folder onto itself is EINVAL");
    filesapp_copy_check(file_copy_begin(&op, FILESAPP_TEST_ROOT "/src", FILESAPP_TEST_ROOT "/dst/src", 0) == -EEXIST,
                        "an existing target is EEXIST");

    /* A cancel in the middle of a file leaves no half file. */
    result = file_copy_begin(&op, FILESAPP_TEST_ROOT "/src/sub/b.bin", FILESAPP_TEST_ROOT "/dst/half.bin",
                             FILE_COPY_NO_COUNT);
    while (result >= 0 && file_copy_running(&op) && op.done_bytes == 0)
    {
        result = file_copy_step(&op);
    }
    filesapp_copy_check(file_copy_running(&op) && op.done_bytes > 0 && op.done_bytes < 40000ull,
                        "a copy stopped halfway through a file");
    file_copy_cancel(&op);
    filesapp_copy_check(op.state == FILE_COPY_CANCELLED && !filesapp_test_exists(FILESAPP_TEST_ROOT "/dst/half.bin"),
                        "cancel removes the half-written file");

    /* A move on one volume is a rename. */
    result = file_copy_begin(&op, FILESAPP_TEST_ROOT "/dst/src", FILESAPP_TEST_ROOT "/dst/moved", FILE_COPY_MOVE);
    filesapp_copy_check(result == 0 && op.state == FILE_COPY_DONE && op.renamed, "a move on one volume renames");
    filesapp_copy_check(!filesapp_test_exists(FILESAPP_TEST_ROOT "/dst/src") &&
                            filesapp_test_matches(FILESAPP_TEST_ROOT "/dst/moved/sub/b.bin", 40000, 2),
                        "the renamed tree is whole");

    /* Across volumes (the disk to the in-memory tree) a move copies, then
     * removes the source. */
    filesapp_copy_check(filesapp_test_write(FILESAPP_TEST_ROOT "/far.txt", 3000, 3) == 0, "write the file to move");
    (void)unlink("/rescue/filesapp-far.txt");
    result = file_copy_begin(&op, FILESAPP_TEST_ROOT "/far.txt", "/rescue/filesapp-far.txt", FILE_COPY_MOVE);
    if (result == 0)
    {
        filesapp_copy_check(!op.renamed, "a move across volumes is not a rename");
        result = filesapp_test_run(&op);
    }
    filesapp_copy_check(result == 0 && op.state == FILE_COPY_DONE, "a move across volumes ends DONE");
    filesapp_copy_check(!filesapp_test_exists(FILESAPP_TEST_ROOT "/far.txt") &&
                            filesapp_test_matches("/rescue/filesapp-far.txt", 3000, 3),
                        "the moved file is there and the source is gone");
    (void)unlink("/rescue/filesapp-far.txt");

    filesapp_test_remove(FILESAPP_TEST_ROOT);
    filesapp_copy_check(!filesapp_test_exists(FILESAPP_TEST_ROOT), "remove the scratch tree");
}

static void filesapp_uri_selftest(void)
{
    char uri[FILESAPP_URI_CAPACITY];
    char path[FILESAPP_PATH_CAPACITY];
    static const char k_list[] = "# a comment\r\nfile:///users/user/My%20notes.txt\r\n";
    static const char k_two[] = "file:///a\r\nfile:///b\r\n";

    filesapp_copy_check(filesapp_uri_from_path("/users/user/My notes \xc3\xbc.txt", uri, sizeof(uri)) == 0 &&
                            strcmp(uri, "file:///users/user/My%20notes%20%C3%BC.txt") == 0,
                        "a path becomes a percent-encoded URI");
    filesapp_copy_check(filesapp_path_from_uri(uri, strlen(uri), path, sizeof(path)) == 0 &&
                            strcmp(path, "/users/user/My notes \xc3\xbc.txt") == 0,
                        "the URI decodes back to the path");
    filesapp_copy_check(filesapp_path_from_uri("file://localhost/tmp/a", 22, path, sizeof(path)) == 0 &&
                            strcmp(path, "/tmp/a") == 0,
                        "a localhost URI is read");
    filesapp_copy_check(filesapp_path_from_uri("http://host/a", 13, path, sizeof(path)) != 0 &&
                            filesapp_path_from_uri("file:///a%00b", 13, path, sizeof(path)) != 0 &&
                            filesapp_path_from_uri("file://other/a", 14, path, sizeof(path)) != 0,
                        "another scheme, another host and a NUL are refused");
    filesapp_copy_check(filesapp_path_from_list(k_list, (int)sizeof(k_list) - 1, path, sizeof(path)) == 1 &&
                            strcmp(path, "/users/user/My notes.txt") == 0,
                        "a list skips its comments");
    filesapp_copy_check(filesapp_path_from_list(k_two, (int)sizeof(k_two) - 1, path, sizeof(path)) == -1,
                        "a list of two is not taken yet");
}

static int filesapp_copy_selftest(void)
{
    g_copy_failures = 0;
    filesapp_uri_selftest();
    filesapp_copy_engine_selftest();
    printf("FILESAPP SMOKE copy checks done, %d failed\n", g_copy_failures);
    return g_copy_failures;
}

/* ---- selftest: My Computer ------------------------------------------------------
 *
 * The names and sizes a volume is shown with, and the live list: the root is
 * there, first, with its space, and /boot is not. */

static int filesapp_computer_selftest(void)
{
    char text[96];
    int failures = 0;
    int index;
    int boot_listed = 0;

#define FILESAPP_COMPUTER_CHECK(ok, label)                                 \
    do                                                                      \
    {                                                                       \
        if (!(ok))                                                          \
        {                                                                   \
            printf("FILESAPP SMOKE FAIL computer: %s\n", label);           \
            failures += 1;                                                  \
        }                                                                   \
    } while (0)

    filesapp_volume_name("", "/", text, sizeof(text));
    FILESAPP_COMPUTER_CHECK(strcmp(text, "Local Disk (/)") == 0, "an unlabelled volume is a Local Disk");
    filesapp_volume_name("Data", "/mnt/data", text, sizeof(text));
    FILESAPP_COMPUTER_CHECK(strcmp(text, "Data (/mnt/data)") == 0, "a label names the volume");
    filesapp_format_capacity(512ull, text, sizeof(text));
    FILESAPP_COMPUTER_CHECK(strcmp(text, "512 bytes") == 0, "bytes");
    filesapp_format_capacity(1536ull * 1024ull * 1024ull, text, sizeof(text));
    FILESAPP_COMPUTER_CHECK(strcmp(text, "1.5 GB") == 0, "gigabytes");
    filesapp_format_capacity(5ull * 1024ull * 1024ull * 1024ull * 1024ull, text, sizeof(text));
    FILESAPP_COMPUTER_CHECK(strcmp(text, "5.0 TB") == 0, "past 32 bits");

    filesapp_collect_volumes();
    for (index = 0; index < g_entry_count; ++index)
    {
        printf("FILESAPP SMOKE computer volume '%s' at %s total=%llu free=%llu\n", g_entries[index].name,
               g_entries[index].mount_point, g_entries[index].total_bytes, g_entries[index].free_bytes);
        boot_listed |= strcmp(g_entries[index].mount_point, "/boot") == 0;
    }
    FILESAPP_COMPUTER_CHECK(g_entry_count > 0 && strcmp(g_entries[0].mount_point, "/") == 0,
                            "the root is the first volume");
    FILESAPP_COMPUTER_CHECK(g_entry_count > 0 && g_entries[0].volume && g_entries[0].total_bytes != 0 &&
                                g_entries[0].free_bytes <= g_entries[0].total_bytes,
                            "the root has its space");
    FILESAPP_COMPUTER_CHECK(!boot_listed, "/boot is not listed");

    /* Large Icons groups My Computer: the user's folder under Files Stored on
     * This Computer when the profile is there, then the disks under Hard
     * Disk Drives. Details list the same entries with no headers. */
    (void)filesapp_load_computer();
    FILESAPP_LIST->kind = SXGUI_ICONVIEW;
    filesapp_bind_list(-1);
    {
        int disks_header = -1;
        int documents_item = -1;

        for (index = 0; index < g_view_count; ++index)
        {
            int entry = g_item_entry[index];

            if (entry < 0 && strcmp(g_view_items[index], k_group_disks) == 0)
            {
                disks_header = index;
            }
            if (entry >= 0 && g_entries[entry].documents)
            {
                documents_item = index;
            }
            if (entry >= 0)
            {
                FILESAPP_COMPUTER_CHECK(filesapp_item_of(entry) == index, "an item maps back to its entry");
                FILESAPP_COMPUTER_CHECK(g_entries[entry].volume ? disks_header >= 0 && index > disks_header
                                                                : index < disks_header || disks_header < 0,
                                        "each entry is under its group");
            }
        }
        printf("FILESAPP SMOKE computer groups items=%d entries=%d documents=%d\n", g_view_count, g_entry_count,
               documents_item);
        FILESAPP_COMPUTER_CHECK(disks_header >= 0, "Large Icons has the Hard Disk Drives group");
        FILESAPP_COMPUTER_CHECK(documents_item < 0 || (documents_item == 1 && g_item_entry[0] < 0 &&
                                                       strcmp(g_view_items[0], k_group_files) == 0),
                                "the user's folder comes first, under Files Stored on This Computer");
        FILESAPP_COMPUTER_CHECK(filesapp_entry_of(disks_header) == -1, "a header is no entry");
    }
    FILESAPP_LIST->kind = SXGUI_LISTBOX;
    filesapp_bind_list(-1);
    FILESAPP_COMPUTER_CHECK(g_view_count == g_entry_count && g_item_entry[0] == 0, "Details have no headers");
#undef FILESAPP_COMPUTER_CHECK
    g_entry_count = 0;
    g_view_count = 0;
    g_current_path[0] = '/';
    g_current_path[1] = '\0';
    return failures;
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
                entry->from_policy ? "user policy" : "default/capability");
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
    failures += filesapp_copy_selftest();
    failures += filesapp_computer_selftest();

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
        g_widgets[W_CUT] = sxgui_toolbar_button(none, "Cut", k_glyph_cut, on_cut_clicked, 0);
        g_widgets[W_COPY] = sxgui_toolbar_button(none, "Copy", k_glyph_copy, on_copy_clicked, 0);
        g_widgets[W_PASTE] = sxgui_toolbar_button(none, "Paste", k_glyph_paste, on_paste_clicked, 0);
        g_widgets[W_SEP_CLIPBOARD] = sxgui_separator(none);
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
    filesapp_build_op_dialogs();

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

    g_new_folder_widgets[0] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN,
                     FILESAPP_QUESTION_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
        "Name of the new folder:");
    g_new_folder_widgets[1] = sxgui_textfield(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + FILESAPP_DLG_ROW,
                     FILESAPP_QUESTION_WIDTH - SXGUI_DIALOG_MARGIN * 2, SXGUI_FIELD_HEIGHT),
        g_new_folder_buffer, (int)sizeof(g_new_folder_buffer));
    g_new_folder_widgets[2] = sxgui_button(
        sx_rect_make(FILESAPP_QUESTION_WIDTH / 2 - SXGUI_BUTTON_WIDTH - 4,
                     FILESAPP_RENAME_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_new_folder_ok, 0);
    g_new_folder_widgets[3] = sxgui_button(
        sx_rect_make(FILESAPP_QUESTION_WIDTH / 2 + 4,
                     FILESAPP_RENAME_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Cancel", on_dialog_cancel, 0);
    g_new_folder_dialog.title = "New Folder";
    g_new_folder_dialog.widgets = g_new_folder_widgets;
    g_new_folder_dialog.widget_count = 4;
    g_new_folder_dialog.initial_focus = 1;
    g_new_folder_dialog.default_button = 2;

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
    g_app.on_tick = on_tick;
    g_app.tick_interval_ms = FILESAPP_IDLE_TICK_MS;
    sxgui_set_menubar(&g_app.ui, &g_menubar);
    filesapp_set_view(filesapp_load_view());
    (void)sxgui_app_set_content_size(&g_app, filesapp_content_width(), FILESAPP_CONTENT_HEIGHT);
    filesapp_layout(&g_app);

    /* The folder it was asked to show (a folder opened from the desktop);
     * else My Computer, the machine's volumes. */
    if ((argc < 2 || argv == 0 || argv[1] == 0 || filesapp_load_directory(argv[1]) < 0) &&
        filesapp_load_directory(FILESAPP_COMPUTER) < 0)
    {
        sxgui_app_quit(&g_app, 1);
    }
    /* El contenido de Files ES la lista: sin esto el foco arranca en ninguna
     * parte y las flechas no mueven la seleccion hasta que alguien tabula. */
    sxgui_focus(&g_app.ui, FILESAPP_LIST_INDEX);
    filesapp_check_clipboard();
    {
        int status = sxgui_app_run(&g_app);

        /* Closed in the middle of a paste: no half file is left behind. */
        file_copy_cancel(&g_op);
        return status;
    }
}
