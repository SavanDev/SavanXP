#include "libc.h"
#include "savanxp/sxgui.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

#include "shared/version.h"

#include "file_assoc.h"
#include "mime_icon.h"
#include "shortcut.h"

#include <string.h>
#include <unistd.h>

/*
 * File browser, shaped like the Win95-era Explorer: menu bar, address bar with
 * "Up One Level", the folder as Large Icons or as a details list (Name / Size
 * / Type) with column headers -- View picks one and filesapp.cfg in the
 * profile remembers it -- and a two-pane status bar.
 *
 * It does NOT show what files contain: that is an editor's job. Opening a file
 * goes through shortcut_open_path, as on the desktop: a program starts, a
 * document goes to the program ASSOCIATED with its extension (file_assoc,
 * docs/userland/SXE_FORMAT.md phase 5) or to /bin/notepad, and a shortcut opens
 * its target. A right click on a row opens its context menu: Open, Send To
 * Desktop (a shortcut in the Desktop folder), Rename and Delete
 * (docs/desktop/DESKTOP_ICONS.md#phase-4-context-menus-in-applications).
 */

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

struct filesapp_entry
{
    char name[256];
    unsigned int size;
    int is_dir;
};

static struct sxgui_app g_app;
static struct sxgui_widget g_widgets[5];

static struct filesapp_entry g_entries[FILESAPP_MAX_ENTRIES];
static char g_item_labels[FILESAPP_MAX_ENTRIES][FILESAPP_LABEL_LENGTH];
static const char *g_item_ptrs[FILESAPP_MAX_ENTRIES];
/* Parallel to g_item_ptrs: each row's icon, or 0. One array per size: the
 * details list draws the 16x16 ones, Large Icons the 32x32 ones. */
static const struct sx_bitmap *g_item_icons[FILESAPP_MAX_ENTRIES];
static const struct sx_bitmap *g_item_large_icons[FILESAPP_MAX_ENTRIES];
static char g_current_path[FILESAPP_PATH_CAPACITY] = "/";
static char g_count_pane[64] = "0 object(s)";
static char g_size_pane[64] = "";
static int g_entry_count = 0;
static int g_mime_icons_loaded = 0;

static struct sxgui_dialog g_about_dialog;
static struct sxgui_widget g_about_widgets[4];

#define FILESAPP_ADDRESS (&g_widgets[0])
#define FILESAPP_UP_BUTTON (&g_widgets[1])
#define FILESAPP_LIST_INDEX 2
#define FILESAPP_LIST (&g_widgets[FILESAPP_LIST_INDEX])

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

    /* En la raiz no hay a donde subir: el boton se apaga en vez de no hacer
     * nada al apretarlo. */
    if (filesapp_is_root_path(g_current_path))
    {
        FILESAPP_UP_BUTTON->flags |= SXGUI_FLAG_DISABLED;
    }
    else
    {
        FILESAPP_UP_BUTTON->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
    }
    return 0;
}

/* ---- navegacion ----------------------------------------------------------- */

static void filesapp_go_up(void)
{
    char parent_path[FILESAPP_PATH_CAPACITY];

    if (filesapp_is_root_path(g_current_path))
    {
        return;
    }
    filesapp_parent_path(g_current_path, parent_path, sizeof(parent_path));
    (void)filesapp_load_directory(parent_path);
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
        (void)filesapp_load_directory(full_path);
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
};

static const struct sxgui_menu k_entry_menu = {
    "Entry",
    k_entry_items,
    (int)(sizeof(k_entry_items) / sizeof(k_entry_items[0])),
};

static const struct sxgui_menu_item k_folder_items[] = {
    {"Up One Level", FILESAPP_CMD_UP, 0},
    {"Refresh", FILESAPP_CMD_REFRESH, 0},
};

static const struct sxgui_menu k_folder_menu = {
    "Folder",
    k_folder_items,
    (int)(sizeof(k_folder_items) / sizeof(k_folder_items[0])),
};

/* The entry a menu or a dialog is about, by name: the list may be reloaded
 * in between. */
static char g_subject[256];
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
        sxgui_context_menu_open(&g_app.ui, &k_entry_menu, x, y, on_context_command, 0);
        return;
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
    (void)app;
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return 0;
    }
    /* The keyboard's right click opens the selected row's menu next to it. */
    if (!sxgui_dialog_active(&g_app.ui) &&
        (event->key == SAVANXP_KEY_MENU ||
         (event->key == SAVANXP_KEY_F10 && (event->modifiers & SAVANXP_KEY_MOD_SHIFT) != 0)))
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
    if (event->key == SAVANXP_KEY_DELETE && !sxgui_dialog_active(&g_app.ui))
    {
        int row = filesapp_entry_of(FILESAPP_LIST->value);

        if (row >= 0 && row < g_entry_count && strcmp(g_entries[row].name, "..") != 0)
        {
            snprintf(g_subject, sizeof(g_subject), "%s", g_entries[row].name);
            filesapp_ask_delete();
        }
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

/* ---- layout --------------------------------------------------------------- */

/* La lista se estira con la ventana, asi que no hay bounding box del que
 * deducir el tamano: este es un explorador comodo sin comerse la pantalla. */
#define FILESAPP_CONTENT_WIDTH 560
#define FILESAPP_CONTENT_HEIGHT 400

/* La banda de la barra de direccion lleva UNA altura para los dos controles que
 * viven ahi: si el campo y el boton no miden lo mismo, quedan desalineados por
 * arriba o por abajo y no hay margen que lo tape. */
#define FILESAPP_TOOLBAR_HEIGHT SXGUI_FIELD_HEIGHT
#define FILESAPP_UP_BUTTON_WIDTH 110
/* Separacion entre los dos paneles de la barra de estado. Es menor que el GAP
 * general a proposito: son dos mitades de la misma barra, no dos controles. */
#define FILESAPP_STATUS_GAP 2

static void filesapp_layout(struct sxgui_app *app)
{
    int width = (int)app->gfx.info.width;
    int height = (int)app->gfx.info.height;
    int toolbar_y = sxgui_menubar_height() + SXGUI_MARGIN;
    int list_y = toolbar_y + FILESAPP_TOOLBAR_HEIGHT + SXGUI_GAP;
    int status_y = height - SXGUI_MARGIN - SXGUI_STATUS_HEIGHT;
    int list_height = status_y - SXGUI_GAP - list_y;
    int list_width = width - SXGUI_MARGIN * 2;
    int address_width = list_width - FILESAPP_UP_BUTTON_WIDTH - SXGUI_GAP;
    int name_width;
    int count_pane_width;

    if (list_height < 60)
    {
        list_height = 60;
    }
    if (address_width < 80)
    {
        address_width = 80;
    }
    if (list_width < 120)
    {
        list_width = 120;
    }

    FILESAPP_ADDRESS->rect = sx_rect_make(SXGUI_MARGIN, toolbar_y, address_width, FILESAPP_TOOLBAR_HEIGHT);
    FILESAPP_UP_BUTTON->rect = sx_rect_make(
        SXGUI_MARGIN + address_width + SXGUI_GAP, toolbar_y, FILESAPP_UP_BUTTON_WIDTH, FILESAPP_TOOLBAR_HEIGHT);
    FILESAPP_LIST->rect = sx_rect_make(SXGUI_MARGIN, list_y, list_width, list_height);

    /* Name se queda con lo que sobra: es la columna que crece al agrandar la
     * ventana, igual que en el explorador. Lo que se descuenta es el area util
     * REAL de la lista -- los dos biseles hundidos y la columna del scrollbar,
     * que se reserva siempre: si solo se descontara cuando la barra esta, las
     * columnas saltarian de ancho al agregar un archivo. */
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
    g_widgets[3].rect = sx_rect_make(SXGUI_MARGIN, status_y, count_pane_width, SXGUI_STATUS_HEIGHT);
    g_widgets[4].rect = sx_rect_make(
        SXGUI_MARGIN + count_pane_width + FILESAPP_STATUS_GAP, status_y,
        list_width - count_pane_width - FILESAPP_STATUS_GAP, SXGUI_STATUS_HEIGHT);
}

static void on_resize(struct sxgui_app *app)
{
    filesapp_layout(app);
}

/* ---- menus ----------------------------------------------------------------- */

static const struct sxgui_menu_item k_file_items[] = {
    {"Open", FILESAPP_MENU_OPEN, 0},
    {"Up One Level", FILESAPP_MENU_UP, 0},
    {0, 0, 0},
    {"Close", FILESAPP_MENU_CLOSE, 0},
};

static const struct sxgui_menu_item k_help_items[] = {
    {"About Files", FILESAPP_MENU_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"View", k_view_items, (int)(sizeof(k_view_items) / sizeof(k_view_items[0]))},
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
    failures += mime_icon_selftest();
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

    g_widgets[0] = sxgui_label(sx_rect_make(0, 0, 0, 0), g_current_path);
    g_widgets[0].flags |= SXGUI_FLAG_SUNKEN;
    g_widgets[1] = sxgui_button(sx_rect_make(0, 0, 0, 0), "Up One Level", on_up_clicked, 0);
    g_widgets[2] = sxgui_listbox(sx_rect_make(0, 0, 0, 0), g_item_ptrs, 0);
    g_widgets[2].columns = g_columns;
    g_widgets[2].column_count = FILESAPP_COLUMN_COUNT;
    g_widgets[2].on_action = on_list;
    g_widgets[2].flags |= SXGUI_FLAG_EMPTY_SELECTION;
    g_widgets[3] = sxgui_label(sx_rect_make(0, 0, 0, 0), g_count_pane);
    g_widgets[3].flags |= SXGUI_FLAG_SUNKEN;
    g_widgets[4] = sxgui_label(sx_rect_make(0, 0, 0, 0), g_size_pane);
    g_widgets[4].flags |= SXGUI_FLAG_SUNKEN;

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

    if (sxgui_app_init(&g_app, "filesapp", g_widgets, 5) < 0)
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
