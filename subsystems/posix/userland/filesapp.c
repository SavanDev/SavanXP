#include "libc.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

#define FILESAPP_MAX_WIDTH 1920
#define FILESAPP_MAX_HEIGHT 1080
#define FILESAPP_PATH_CAPACITY 256
#define FILESAPP_MAX_ENTRIES 128
#define FILESAPP_PREVIEW_LINES 20
#define FILESAPP_PREVIEW_COLUMNS 76
#define FILESAPP_ROW_HEIGHT 18

struct filesapp_entry
{
    char name[256];
    unsigned int size;
    int is_dir;
};

static uint32_t g_backbuffer[FILESAPP_MAX_WIDTH * FILESAPP_MAX_HEIGHT];
static struct filesapp_entry g_entries[FILESAPP_MAX_ENTRIES];
static char g_current_path[FILESAPP_PATH_CAPACITY] = "/";
static char g_preview[FILESAPP_PREVIEW_LINES][FILESAPP_PREVIEW_COLUMNS + 1];
static char g_status_line[128];
static int g_entry_count = 0;
static int g_selected_index = 0;
static int g_scroll_index = 0;
static int g_cursor_x = 0;
static int g_cursor_y = 0;
static unsigned long g_last_click_ms = 0;
static int g_last_click_index = -1;

static int filesapp_path_is_launchable(const char *path);

static int filesapp_min_int(int left, int right)
{
    return left < right ? left : right;
}

static int filesapp_max_int(int left, int right)
{
    return left > right ? left : right;
}

static int filesapp_point_in_rect(int x, int y, int rect_x, int rect_y, int rect_w, int rect_h)
{
    return x >= rect_x && y >= rect_y && x < rect_x + rect_w && y < rect_y + rect_h;
}

static int filesapp_printable_char(int value)
{
    return value >= 32 && value <= 126;
}

static void filesapp_clear_preview(void)
{
    int index;
    for (index = 0; index < FILESAPP_PREVIEW_LINES; ++index)
    {
        memset(g_preview[index], 0, sizeof(g_preview[index]));
    }
}

static int filesapp_is_root_path(const char *path)
{
    return path != 0 && path[0] == '/' && path[1] == '\0';
}

static void filesapp_set_status(const char *text)
{
    snprintf(g_status_line, sizeof(g_status_line), "%s", text != 0 ? text : "");
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

static void filesapp_update_preview(void)
{
    char full_path[FILESAPP_PATH_CAPACITY];
    struct stat info = {0};
    FILE *stream = 0;
    int line_index = 0;

    filesapp_clear_preview();
    if (g_entry_count <= 0 || g_selected_index < 0 || g_selected_index >= g_entry_count)
    {
        snprintf(g_preview[0], sizeof(g_preview[0]), "No entries.");
        return;
    }

    if (strcmp(g_entries[g_selected_index].name, "..") == 0)
    {
        snprintf(g_preview[0], sizeof(g_preview[0]), "Go to parent directory");
        return;
    }

    if (!filesapp_join_path(g_current_path, g_entries[g_selected_index].name, full_path, sizeof(full_path)))
    {
        snprintf(g_preview[0], sizeof(g_preview[0]), "Path too long.");
        return;
    }
    if (stat(full_path, &info) < 0)
    {
        snprintf(g_preview[0], sizeof(g_preview[0]), "stat failed for %s", g_entries[g_selected_index].name);
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
    g_selected_index = 0;
    g_scroll_index = 0;
    filesapp_update_preview();
    snprintf(g_status_line, sizeof(g_status_line), "%d item(s)", g_entry_count);
    return 0;
}

static void filesapp_ensure_selection_visible(int visible_rows)
{
    if (g_selected_index < g_scroll_index)
    {
        g_scroll_index = g_selected_index;
    }
    if (g_selected_index >= g_scroll_index + visible_rows)
    {
        g_scroll_index = g_selected_index - visible_rows + 1;
    }
    if (g_scroll_index < 0)
    {
        g_scroll_index = 0;
    }
}

static int filesapp_list_index_from_point(const struct savanxp_gfx_context *gfx, int x, int y)
{
    const int list_x = 24;
    const int list_y = 78;
    const int list_width = ((int)gfx->info.width / 2) - 36;
    const int list_height = (int)gfx->info.height - 156;
    const int row_origin_y = list_y + 10;
    const int visible_rows = filesapp_max_int(1, (list_height - 20) / FILESAPP_ROW_HEIGHT);
    int row = 0;

    if (!filesapp_point_in_rect(x, y, list_x, list_y, list_width, list_height))
    {
        return -1;
    }
    if (y < row_origin_y)
    {
        return -1;
    }
    row = (y - row_origin_y) / FILESAPP_ROW_HEIGHT;
    if (row < 0 || row >= visible_rows)
    {
        return -1;
    }
    return g_scroll_index + row < g_entry_count ? g_scroll_index + row : -1;
}

static int filesapp_path_is_launchable(const char *path)
{
    return path != 0 &&
        (strncmp(path, "/bin/", 5) == 0 || strncmp(path, "/disk/bin/", 10) == 0);
}

static int filesapp_activate_selected(struct savanxp_gfx_context *gfx)
{
    char full_path[FILESAPP_PATH_CAPACITY];
    char parent_path[FILESAPP_PATH_CAPACITY];

    if (g_entry_count <= 0 || g_selected_index < 0 || g_selected_index >= g_entry_count)
    {
        return 0;
    }
    if (strcmp(g_entries[g_selected_index].name, "..") == 0)
    {
        filesapp_parent_path(g_current_path, parent_path, sizeof(parent_path));
        return filesapp_load_directory(parent_path);
    }
    if (!filesapp_join_path(g_current_path, g_entries[g_selected_index].name, full_path, sizeof(full_path)))
    {
        filesapp_set_status("Path too long.");
        return -1;
    }
    if (!g_entries[g_selected_index].is_dir)
    {
        if (gfx != 0 && filesapp_path_is_launchable(full_path))
        {
            if (gfx_desktop_launch(gfx, full_path) < 0)
            {
                filesapp_set_status("Launch failed.");
                return -1;
            }
            filesapp_set_status("Launch requested.");
            return 0;
        }
        filesapp_update_preview();
        filesapp_set_status("Preview refreshed.");
        return 0;
    }
    return filesapp_load_directory(full_path);
}

static void filesapp_draw_scene(struct savanxp_gfx_context *gfx)
{
    const int list_x = 24;
    const int list_y = 78;
    const int list_width = ((int)gfx->info.width / 2) - 36;
    const int list_height = (int)gfx->info.height - 156;
    const int preview_x = list_x + list_width + 24;
    const int preview_width = (int)gfx->info.width - preview_x - 24;
    const int visible_rows = filesapp_max_int(1, (list_height - 20) / FILESAPP_ROW_HEIGHT);
    int index;
    int row_y = list_y + 10;

    filesapp_ensure_selection_visible(visible_rows);

    gfx_clear(g_backbuffer, &gfx->info, gfx_rgb(0, 128, 128));
    gfx_rect(g_backbuffer, &gfx->info, 0, 0, (int)gfx->info.width, 36, gfx_rgb(0, 90, 90));
    gfx_hline(g_backbuffer, &gfx->info, 0, 36, (int)gfx->info.width, gfx_rgb(220, 255, 255));
    gfx_blit_text(g_backbuffer, &gfx->info, 20, 10, "SavanXP Files", gfx_rgb(255, 255, 255));
    gfx_blit_text(g_backbuffer, &gfx->info, 220, 10, g_current_path, gfx_rgb(226, 244, 244));

    gfx_rect(g_backbuffer, &gfx->info, list_x, list_y, list_width, list_height, gfx_rgb(216, 220, 224));
    gfx_frame(g_backbuffer, &gfx->info, list_x, list_y, list_width, list_height, gfx_rgb(46, 50, 56));
    gfx_rect(g_backbuffer, &gfx->info, preview_x, list_y, preview_width, list_height, gfx_rgb(232, 235, 239));
    gfx_frame(g_backbuffer, &gfx->info, preview_x, list_y, preview_width, list_height, gfx_rgb(46, 50, 56));

    gfx_blit_text(g_backbuffer, &gfx->info, list_x + 12, list_y - 18, "Directory", gfx_rgb(255, 255, 255));
    gfx_blit_text(g_backbuffer, &gfx->info, preview_x + 12, list_y - 18, "Preview", gfx_rgb(255, 255, 255));

    for (index = g_scroll_index; index < g_entry_count && index < g_scroll_index + visible_rows; ++index)
    {
        const int row_index = index - g_scroll_index;
        const int item_y = row_y + (row_index * FILESAPP_ROW_HEIGHT);
        char line[120];

        if (index == g_selected_index)
        {
            gfx_rect(g_backbuffer, &gfx->info, list_x + 8, item_y - 2, list_width - 16, FILESAPP_ROW_HEIGHT, gfx_rgb(176, 201, 233));
        }
        snprintf(
            line,
            sizeof(line),
            "%s %s",
            g_entries[index].is_dir ? "[DIR]" : "[FILE]",
            g_entries[index].name);
        gfx_blit_text(g_backbuffer, &gfx->info, list_x + 14, item_y, line, gfx_rgb(18, 22, 26));
    }

    for (index = 0; index < FILESAPP_PREVIEW_LINES; ++index)
    {
        if (g_preview[index][0] == '\0')
        {
            continue;
        }
        gfx_blit_text(g_backbuffer, &gfx->info, preview_x + 14, row_y + (index * FILESAPP_ROW_HEIGHT), g_preview[index], gfx_rgb(24, 28, 34));
    }

    gfx_rect(g_backbuffer, &gfx->info, 24, (int)gfx->info.height - 62, (int)gfx->info.width - 48, 32, gfx_rgb(192, 196, 201));
    gfx_frame(g_backbuffer, &gfx->info, 24, (int)gfx->info.height - 62, (int)gfx->info.width - 48, 32, gfx_rgb(74, 79, 86));
    gfx_blit_text(g_backbuffer, &gfx->info, 36, (int)gfx->info.height - 50, "ESC exit  Enter open dir  Backspace up  F5 refresh", gfx_rgb(18, 22, 26));
    gfx_blit_text(g_backbuffer, &gfx->info, 520, (int)gfx->info.height - 50, g_status_line, gfx_rgb(18, 22, 26));
}

int main(void)
{
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event key_event;
    struct savanxp_gui_pointer_event pointer_event;
    long mouse_fd = -1;
    int needs_redraw = 1;
    uint32_t last_buttons = 0;

    if (gfx_open(&gfx) < 0)
    {
        puts_fd(2, "filesapp: gfx_open failed\n");
        return 1;
    }
    if (gfx.info.width > FILESAPP_MAX_WIDTH || gfx.info.height > FILESAPP_MAX_HEIGHT || (gfx.info.pitch / 4u) > FILESAPP_MAX_WIDTH)
    {
        puts_fd(2, "filesapp: framebuffer too large\n");
        gfx_close(&gfx);
        return 1;
    }
    if (gfx_acquire(&gfx) < 0)
    {
        puts_fd(2, "filesapp: gfx_acquire failed\n");
        gfx_close(&gfx);
        return 1;
    }

    mouse_fd = gfx_pointer_open();
    if (filesapp_load_directory("/") < 0)
    {
        gfx_release(&gfx);
        if (mouse_fd >= 0)
        {
            close((int)mouse_fd);
        }
        gfx_close(&gfx);
        return 1;
    }

    for (;;)
    {
        while (gfx_poll_event(&gfx, &key_event) > 0)
        {
            if (key_event.type == SAVANXP_INPUT_EVENT_RESIZED)
            {
                (void)gfx_apply_resize_event(&gfx, &key_event);
                if (g_cursor_x >= (int)gfx.info.width)
                {
                    g_cursor_x = (int)gfx.info.width - 1;
                }
                if (g_cursor_y >= (int)gfx.info.height)
                {
                    g_cursor_y = (int)gfx.info.height - 1;
                }
                needs_redraw = 1;
                continue;
            }
            if (key_event.type != SAVANXP_INPUT_EVENT_KEY_DOWN)
            {
                continue;
            }
            if (key_event.key == SAVANXP_KEY_ESC)
            {
                gfx_release(&gfx);
                if (mouse_fd >= 0)
                {
                    close((int)mouse_fd);
                }
                gfx_close(&gfx);
                return 0;
            }
            if (key_event.key == SAVANXP_KEY_UP && g_selected_index > 0)
            {
                g_selected_index -= 1;
                filesapp_update_preview();
                needs_redraw = 1;
            }
            else if (key_event.key == SAVANXP_KEY_DOWN && g_selected_index + 1 < g_entry_count)
            {
                g_selected_index += 1;
                filesapp_update_preview();
                needs_redraw = 1;
            }
            else if (key_event.key == SAVANXP_KEY_HOME)
            {
                g_selected_index = 0;
                filesapp_update_preview();
                needs_redraw = 1;
            }
            else if (key_event.key == SAVANXP_KEY_END && g_entry_count > 0)
            {
                g_selected_index = g_entry_count - 1;
                filesapp_update_preview();
                needs_redraw = 1;
            }
            else if (key_event.key == SAVANXP_KEY_PAGE_UP)
            {
                g_selected_index = filesapp_max_int(0, g_selected_index - 10);
                filesapp_update_preview();
                needs_redraw = 1;
            }
            else if (key_event.key == SAVANXP_KEY_PAGE_DOWN && g_entry_count > 0)
            {
                g_selected_index = filesapp_min_int(g_entry_count - 1, g_selected_index + 10);
                filesapp_update_preview();
                needs_redraw = 1;
            }
            else if (key_event.key == SAVANXP_KEY_BACKSPACE)
            {
                char parent_path[FILESAPP_PATH_CAPACITY];
                filesapp_parent_path(g_current_path, parent_path, sizeof(parent_path));
                if (filesapp_load_directory(parent_path) == 0)
                {
                    needs_redraw = 1;
                }
            }
            else if (key_event.key == SAVANXP_KEY_ENTER)
            {
                if (filesapp_activate_selected(&gfx) == 0)
                {
                    needs_redraw = 1;
                }
            }
            else if (key_event.key == SAVANXP_KEY_F5)
            {
                if (filesapp_load_directory(g_current_path) == 0)
                {
                    needs_redraw = 1;
                }
            }
        }

        while (mouse_fd >= 0 && gfx_poll_pointer((int)mouse_fd, &pointer_event) > 0)
        {
            g_cursor_x = pointer_event.x;
            g_cursor_y = pointer_event.y;
            if (g_cursor_x < 0)
            {
                g_cursor_x = 0;
            }
            if (g_cursor_y < 0)
            {
                g_cursor_y = 0;
            }
            if (g_cursor_x >= (int)gfx.info.width)
            {
                g_cursor_x = (int)gfx.info.width - 1;
            }
            if (g_cursor_y >= (int)gfx.info.height)
            {
                g_cursor_y = (int)gfx.info.height - 1;
            }

            if ((pointer_event.buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0 && (last_buttons & SAVANXP_MOUSE_BUTTON_LEFT) == 0)
            {
                int clicked_index = filesapp_list_index_from_point(&gfx, g_cursor_x, g_cursor_y);
                if (clicked_index >= 0 && clicked_index < g_entry_count)
                {
                    unsigned long now_ms = uptime_ms();
                    if (g_selected_index != clicked_index)
                    {
                        g_selected_index = clicked_index;
                        filesapp_update_preview();
                        needs_redraw = 1;
                    }
                    else if (g_last_click_index == clicked_index && now_ms - g_last_click_ms <= 450UL)
                    {
                        if (filesapp_activate_selected(&gfx) == 0)
                        {
                            needs_redraw = 1;
                        }
                    }
                    g_last_click_index = clicked_index;
                    g_last_click_ms = now_ms;
                }
            }
            last_buttons = pointer_event.buttons;
        }

        if (!needs_redraw)
        {
            sleep_ms(16);
            continue;
        }

        filesapp_draw_scene(&gfx);
        if (gfx_present(&gfx, g_backbuffer) < 0)
        {
            break;
        }
        needs_redraw = 0;
    }

    gfx_release(&gfx);
    if (mouse_fd >= 0)
    {
        close((int)mouse_fd);
    }
    gfx_close(&gfx);
    puts_fd(2, "filesapp: present failed\n");
    return 1;
}
