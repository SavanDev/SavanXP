#include "libc.h"
#include "savanxp/sxgui.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

#include "shared/version.h"

#define FILESAPP_PATH_CAPACITY 256
#define FILESAPP_MAX_ENTRIES 128
#define FILESAPP_LABEL_LENGTH 80
#define FILESAPP_PREVIEW_LINES 20
#define FILESAPP_PREVIEW_COLUMNS 76

#define FILESAPP_MENU_REFRESH 1
#define FILESAPP_MENU_GO_UP 2
#define FILESAPP_MENU_ABOUT 8
#define FILESAPP_MENU_EXIT 9

struct filesapp_entry
{
    char name[256];
    unsigned int size;
    int is_dir;
};

static struct sxgui_app g_app;
static struct sxgui_widget g_widgets[6];

static struct filesapp_entry g_entries[FILESAPP_MAX_ENTRIES];
static char g_item_labels[FILESAPP_MAX_ENTRIES][FILESAPP_LABEL_LENGTH];
static const char *g_item_ptrs[FILESAPP_MAX_ENTRIES];
static char g_preview[FILESAPP_PREVIEW_LINES][FILESAPP_PREVIEW_COLUMNS + 1];
static const char *g_preview_ptrs[FILESAPP_PREVIEW_LINES];
static char g_current_path[FILESAPP_PATH_CAPACITY] = "/";
static char g_status_line[128] = "Ready";
static int g_entry_count = 0;

static struct sxgui_dialog g_about_dialog;
static struct sxgui_widget g_about_widgets[3];

#define FILESAPP_LIST (&g_widgets[3])
#define FILESAPP_PREVIEW (&g_widgets[4])

static int filesapp_path_is_launchable(const char *path);

static int filesapp_printable_char(int value)
{
    return value >= 32 && value <= 126;
}

static void filesapp_set_status(const char *text)
{
    snprintf(g_status_line, sizeof(g_status_line), "%s", text != 0 ? text : "");
}

static int filesapp_is_root_path(const char *path)
{
    return path != 0 && path[0] == '/' && path[1] == '\0';
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

static void filesapp_sanitize_line(char *text)
{
    size_t index;

    if (text == 0)
    {
        return;
    }
    for (index = 0; text[index] != '\0'; ++index)
    {
        if (text[index] == '\r' || text[index] == '\n')
        {
            text[index] = '\0';
            break;
        }
        if (!filesapp_printable_char((unsigned char)text[index]))
        {
            text[index] = '.';
        }
    }
}

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

static void filesapp_clear_preview(void)
{
    int index;
    for (index = 0; index < FILESAPP_PREVIEW_LINES; ++index)
    {
        memset(g_preview[index], 0, sizeof(g_preview[index]));
    }
    FILESAPP_PREVIEW->scroll = 0;
}

static void filesapp_update_preview(void)
{
    char full_path[FILESAPP_PATH_CAPACITY];
    struct stat info = {0};
    FILE *stream = 0;
    int line_index = 0;
    int selected = FILESAPP_LIST->value;

    filesapp_clear_preview();
    if (g_entry_count <= 0 || selected < 0 || selected >= g_entry_count)
    {
        snprintf(g_preview[0], sizeof(g_preview[0]), "No entries.");
        return;
    }

    if (strcmp(g_entries[selected].name, "..") == 0)
    {
        snprintf(g_preview[0], sizeof(g_preview[0]), "Go to parent directory");
        return;
    }

    if (!filesapp_join_path(g_current_path, g_entries[selected].name, full_path, sizeof(full_path)))
    {
        snprintf(g_preview[0], sizeof(g_preview[0]), "Path too long.");
        return;
    }
    if (stat(full_path, &info) < 0)
    {
        snprintf(g_preview[0], sizeof(g_preview[0]), "stat failed for %s", g_entries[selected].name);
        return;
    }

    snprintf(g_preview[0], sizeof(g_preview[0]), "%s", full_path);
    if (S_ISDIR(info.st_mode))
    {
        snprintf(g_preview[2], sizeof(g_preview[2]), "Directory");
        snprintf(g_preview[3], sizeof(g_preview[3]), "Open: Enter or double click");
        return;
    }

    snprintf(g_preview[2], sizeof(g_preview[2]), "File size: %u bytes", info.st_size);
    if (filesapp_path_is_launchable(full_path))
    {
        snprintf(g_preview[3], sizeof(g_preview[3]), "Launch: Enter or double click");
    }
    stream = fopen(full_path, "r");
    if (stream == 0)
    {
        snprintf(g_preview[4], sizeof(g_preview[4]), "Preview unavailable.");
        return;
    }

    for (line_index = 0; line_index + 5 < FILESAPP_PREVIEW_LINES; ++line_index)
    {
        if (fgets(g_preview[line_index + 5], FILESAPP_PREVIEW_COLUMNS, stream) == 0)
        {
            break;
        }
        filesapp_sanitize_line(g_preview[line_index + 5]);
    }
    if (line_index == 0)
    {
        snprintf(g_preview[5], sizeof(g_preview[5]), "(empty or binary-looking file)");
    }
    fclose(stream);
}

/* Rebuild the "[DIR] name" labels the listbox points at. */
static void filesapp_rebuild_labels(void)
{
    int index;

    for (index = 0; index < g_entry_count; ++index)
    {
        snprintf(
            g_item_labels[index],
            sizeof(g_item_labels[index]),
            "%s %s",
            g_entries[index].is_dir ? "[DIR]" : "[FILE]",
            g_entries[index].name);
        g_item_ptrs[index] = g_item_labels[index];
    }
    FILESAPP_LIST->items = g_item_ptrs;
    FILESAPP_LIST->item_count = g_entry_count;
    FILESAPP_LIST->value = 0;
    FILESAPP_LIST->scroll = 0;
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
    filesapp_update_preview();
    snprintf(g_status_line, sizeof(g_status_line), "%d item(s)", g_entry_count);
    return 0;
}

static int filesapp_path_is_launchable(const char *path)
{
    return path != 0 &&
        (strncmp(path, "/bin/", 5) == 0 || strncmp(path, "/disk/bin/", 10) == 0);
}

static void filesapp_go_up(void)
{
    char parent_path[FILESAPP_PATH_CAPACITY];

    filesapp_parent_path(g_current_path, parent_path, sizeof(parent_path));
    (void)filesapp_load_directory(parent_path);
}

static void filesapp_activate_selected(void)
{
    char full_path[FILESAPP_PATH_CAPACITY];
    int selected = FILESAPP_LIST->value;

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
    if (!g_entries[selected].is_dir)
    {
        if (filesapp_path_is_launchable(full_path))
        {
            if (gfx_desktop_launch(&g_app.gfx, full_path) < 0)
            {
                filesapp_set_status("Launch failed.");
                return;
            }
            filesapp_set_status("Launch requested.");
            return;
        }
        filesapp_update_preview();
        filesapp_set_status("Preview refreshed.");
        return;
    }
    (void)filesapp_load_directory(full_path);
}

/* ---- widget callbacks ---------------------------------------------------- */

static void on_list(struct sxgui_widget *widget, void *user)
{
    (void)user;
    if (widget->action == SXGUI_ACTION_ACTIVATE)
    {
        filesapp_activate_selected();
    }
    else
    {
        filesapp_update_preview();
    }
}

static void on_dialog_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
}

static void on_menu_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case FILESAPP_MENU_REFRESH:
        (void)filesapp_load_directory(g_current_path);
        break;
    case FILESAPP_MENU_GO_UP:
        filesapp_go_up();
        break;
    case FILESAPP_MENU_ABOUT:
        sxgui_dialog_begin(&g_app.ui, &g_about_dialog, 280, 96);
        break;
    case FILESAPP_MENU_EXIT:
        sxgui_app_quit(&g_app, 0);
        break;
    default:
        break;
    }
}

static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    (void)app;
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return 0;
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

static void filesapp_layout(struct sxgui_app *app)
{
    int width = (int)app->gfx.info.width;
    int height = (int)app->gfx.info.height;
    int top = sxgui_menubar_height() + 6;
    int panel_y = top + 42;
    int panel_height = height - panel_y - 40;
    int list_x = 12;
    int list_width = width / 2 - 18;
    int preview_x = list_x + list_width + 12;
    int preview_width = width - preview_x - 12;

    if (panel_height < 60)
    {
        panel_height = 60;
    }
    if (list_width < 80)
    {
        list_width = 80;
    }
    if (preview_width < 80)
    {
        preview_width = 80;
    }

    g_widgets[0].rect = sx_rect_make(12, top, width - 24, 16);                      /* path */
    g_widgets[1].rect = sx_rect_make(list_x, panel_y - 20, 160, 16);                /* "Directory" */
    g_widgets[2].rect = sx_rect_make(preview_x, panel_y - 20, 160, 16);             /* "Preview" */
    g_widgets[3].rect = sx_rect_make(list_x, panel_y, list_width, panel_height);    /* listbox */
    g_widgets[4].rect = sx_rect_make(preview_x, panel_y, preview_width, panel_height); /* textview */
    g_widgets[5].rect = sx_rect_make(12, height - 32, width - 24, 22);              /* status */
}

static void on_resize(struct sxgui_app *app)
{
    filesapp_layout(app);
}

/* ---- menu tables ----------------------------------------------------------- */

static const struct sxgui_menu_item k_file_items[] = {
    {"Refresh", FILESAPP_MENU_REFRESH, 0},
    {"Go up", FILESAPP_MENU_GO_UP, 0},
    {0, 0, 0},
    {"Exit", FILESAPP_MENU_EXIT, 0},
};

static const struct sxgui_menu_item k_help_items[] = {
    {"About Files", FILESAPP_MENU_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
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

int main(void)
{
    int index;

    for (index = 0; index < FILESAPP_PREVIEW_LINES; ++index)
    {
        g_preview_ptrs[index] = g_preview[index];
    }

    g_widgets[0] = sxgui_label(sx_rect_make(0, 0, 0, 0), g_current_path);
    g_widgets[1] = sxgui_label(sx_rect_make(0, 0, 0, 0), "Directory");
    g_widgets[2] = sxgui_label(sx_rect_make(0, 0, 0, 0), "Preview");
    g_widgets[3] = sxgui_listbox(sx_rect_make(0, 0, 0, 0), g_item_ptrs, 0);
    g_widgets[3].on_action = on_list;
    g_widgets[4] = sxgui_textview(sx_rect_make(0, 0, 0, 0), g_preview_ptrs, FILESAPP_PREVIEW_LINES);
    g_widgets[5] = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status_line);
    g_widgets[5].flags |= SXGUI_FLAG_SUNKEN;

    g_about_widgets[0] = sxgui_label(sx_rect_make(10, 8, 260, 16), "SavanXP Files");
    g_about_widgets[1] = sxgui_label(sx_rect_make(10, 28, 260, 16), "Browse directories and preview files");
    g_about_widgets[2] = sxgui_button(sx_rect_make(90, 56, 100, 26), "OK", on_dialog_ok, 0);
    g_about_dialog.title = "About";
    g_about_dialog.widgets = g_about_widgets;
    g_about_dialog.widget_count = 3;

    if (sxgui_app_init(&g_app, "filesapp", g_widgets, 6) < 0)
    {
        return 1;
    }
    g_app.on_key = on_key;
    g_app.on_resize = on_resize;
    sxgui_set_menubar(&g_app.ui, &g_menubar);
    filesapp_layout(&g_app);

    if (filesapp_load_directory("/") < 0)
    {
        sxgui_app_quit(&g_app, 1);
    }
    return sxgui_app_run(&g_app);
}
