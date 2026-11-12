/*
 * The common Open and Save As dialogs for SUN programs (include/sxn/sxnfiledlg.h,
 * docs/desktop/FILE_DIALOG.md).
 *
 * One dialog per process at a time, so its state is static: the folder shown,
 * its listing, the request's strings. The listing's buffers are allocated the
 * first time a dialog opens and kept, so a program that never asks for a file
 * never pays for them.
 *
 * The confirmation before replacing a file is a second dialog that replaces
 * this one (the toolkit has one at a time); "No" brings the file dialog back
 * as it was.
 */

#include "sxn/sxnfiledlg.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi/savanxp_abi.h"
#include "sxgfx/mime_icon.h"
#include "sxgfx/sxgui.h"
#include "sxn/sxn.h"

/* The Up button's glyph, shared with Files. */
#include "../gfx/toolbar_glyphs/icons.inc"

#define FD_MAX_ENTRIES 512
#define FD_ROW_CAPACITY (SXN_NAME_MAX + 48)
#define FD_TEXT_CAPACITY 96
#define FD_WIDTH 520
#define FD_HEIGHT 360
#define FD_CONFIRM_WIDTH 380
#define FD_CONFIRM_HEIGHT 112
#define FD_LABEL_WIDTH 88
#define FD_UP_SIZE 28
#define FD_SIZE_COLUMN 72
#define FD_TYPE_COLUMN 112

enum {
    FD_LOOK_IN_LABEL,
    FD_FOLDER,
    FD_UP,
    FD_LIST,
    FD_NAME_LABEL,
    FD_NAME,
    FD_OK,
    FD_TYPE_LABEL,
    FD_TYPE,
    FD_CANCEL,
    FD_MESSAGE,
    FD_WIDGET_COUNT
};

enum {
    FD_CONFIRM_LINE1,
    FD_CONFIRM_LINE2,
    FD_CONFIRM_YES,
    FD_CONFIRM_NO,
    FD_CONFIRM_COUNT
};

struct fd_entry {
    char name[SXN_NAME_MAX];
    uint64_t size;
    int is_dir;
};

/* The listing: allocated once, see the top of the file. */
static struct fd_entry *g_entries;
static int *g_order;
static char (*g_rows)[FD_ROW_CAPACITY];
static const char **g_row_ptrs;
static const struct sx_bitmap **g_row_icons;
static int g_entry_count;

/* The request, copied: the caller's strings need not outlive the call. */
static int g_mode;
static char g_title[FD_TEXT_CAPACITY];
static char g_filter_description[FD_TEXT_CAPACITY];
static char g_filter_patterns[FD_TEXT_CAPACITY];
static char g_default_extension[16];
static void (*g_on_done)(struct sxngui_app *app, const char *path, void *user);
static void *g_user;

static struct sxngui_app *g_app;
static char g_folder[SXN_FILE_DIALOG_PATH_MAX];
static char g_last_folder[SXN_FILE_DIALOG_PATH_MAX];
static char g_name[SXN_FILE_DIALOG_PATH_MAX];
static char g_result[SXN_FILE_DIALOG_PATH_MAX];
static char g_message[160];
static char g_confirm_text[FD_TEXT_CAPACITY + 32];

static struct sxgui_widget g_widgets[FD_WIDGET_COUNT];
static struct sxgui_dialog g_dialog;
static struct sxgui_widget g_confirm_widgets[FD_CONFIRM_COUNT];
static struct sxgui_dialog g_confirm;
static struct sxgui_column g_columns[3] = {
    {"Name", 200, 0},
    {"Size", FD_SIZE_COLUMN, SXGUI_COLUMN_RIGHT},
    {"Type", FD_TYPE_COLUMN, 0},
};

static struct sx_bitmap g_up_bitmap;
static const struct sx_bitmap *g_up_glyph[1];
static int g_icons_ready;

/* ---- paths ----------------------------------------------------------------- */

static char fd_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* `input`, absolute, with "." and ".." resolved and repeated slashes
 * collapsed, into `out`. 0 when it does not fit. */
static int fd_normalize(const char *input, char *out, size_t capacity)
{
    size_t length = 1;
    const char *cursor = input;

    if (input == 0 || input[0] != '/' || capacity < 2)
    {
        return 0;
    }
    out[0] = '/';
    out[1] = '\0';
    while (*cursor != '\0')
    {
        const char *start;
        size_t part;

        while (*cursor == '/')
        {
            ++cursor;
        }
        start = cursor;
        while (*cursor != '\0' && *cursor != '/')
        {
            ++cursor;
        }
        part = (size_t)(cursor - start);
        if (part == 0 || (part == 1 && start[0] == '.'))
        {
            continue;
        }
        if (part == 2 && start[0] == '.' && start[1] == '.')
        {
            /* Up one: drop the last component; "/.." stays "/". */
            while (length > 1 && out[length - 1] != '/')
            {
                --length;
            }
            if (length > 1)
            {
                --length;
            }
            out[length] = '\0';
            continue;
        }
        if (length + (length > 1 ? 1 : 0) + part + 1 > capacity)
        {
            return 0;
        }
        if (length > 1)
        {
            out[length++] = '/';
        }
        memcpy(out + length, start, part);
        length += part;
        out[length] = '\0';
    }
    return 1;
}

/* `name` in `folder`, or `name` itself when it is absolute, normalized. */
static int fd_join(const char *folder, const char *name, char *out, size_t capacity)
{
    char joined[SXN_FILE_DIALOG_PATH_MAX * 2];

    if (name[0] == '/')
    {
        return fd_normalize(name, out, capacity);
    }
    if (snprintf(joined, sizeof(joined), "%s/%s", folder, name) >= (int)sizeof(joined))
    {
        return 0;
    }
    return fd_normalize(joined, out, capacity);
}

/* Whether `name` matches one glob: '*' any run, '?' one character, case
 * folded as FAT and Files compare extensions. */
static int fd_glob(const char *pattern, size_t pattern_length, const char *name)
{
    size_t p = 0;
    const char *star_name = 0;
    size_t star_pattern = 0;
    int starred = 0;

    while (*name != '\0')
    {
        if (p < pattern_length && pattern[p] == '*')
        {
            starred = 1;
            star_pattern = ++p;
            star_name = name;
            continue;
        }
        if (p < pattern_length && (pattern[p] == '?' || fd_lower(pattern[p]) == fd_lower(*name)))
        {
            ++p;
            ++name;
            continue;
        }
        if (!starred)
        {
            return 0;
        }
        p = star_pattern;
        name = ++star_name;
    }
    while (p < pattern_length && pattern[p] == '*')
    {
        ++p;
    }
    return p == pattern_length;
}

/* Whether a file passes the filter: patterns separated by ';'. "*.*" is
 * every file, with an extension or not, as in Windows. */
static int fd_matches(const char *patterns, const char *name)
{
    const char *cursor = patterns;

    if (patterns == 0 || patterns[0] == '\0')
    {
        return 1;
    }
    while (*cursor != '\0')
    {
        const char *start = cursor;
        size_t length;

        while (*cursor != '\0' && *cursor != ';')
        {
            ++cursor;
        }
        length = (size_t)(cursor - start);
        while (length > 0 && *start == ' ')
        {
            ++start;
            --length;
        }
        if ((length == 3 && memcmp(start, "*.*", 3) == 0) || fd_glob(start, length, name))
        {
            return 1;
        }
        if (*cursor == ';')
        {
            ++cursor;
        }
    }
    return 0;
}

/* The last component of `path`. */
static const char *fd_leaf(const char *path)
{
    const char *leaf = path;

    for (; *path != '\0'; ++path)
    {
        if (*path == '/')
        {
            leaf = path + 1;
        }
    }
    return leaf;
}

/* Whether a name has an extension: a dot that is neither its first nor its
 * last character, as mime_icon reads one. */
static int fd_has_extension(const char *name)
{
    size_t index;
    int found = 0;

    for (index = 1; name[index] != '\0'; ++index)
    {
        if (name[index] == '.' && name[index + 1] != '\0')
        {
            found = 1;
        }
    }
    return found;
}

/* ---- the listing ------------------------------------------------------------- */

static long fd_read_file(const char *path, void *buffer, size_t capacity)
{
    long handle = sxn_open_file(path, SXN_FILE_READ);
    long total = 0;

    if (handle < 0)
    {
        return -1;
    }
    while ((size_t)total < capacity)
    {
        long got = sxn_read(handle, (char *)buffer + total, capacity - (size_t)total);

        if (got <= 0)
        {
            break;
        }
        total += got;
    }
    (void)sxn_close(handle);
    return total;
}

static int fd_allocate(void)
{
    if (g_entries != 0)
    {
        return 1;
    }
    g_entries = (struct fd_entry *)malloc(sizeof(*g_entries) * FD_MAX_ENTRIES);
    g_order = (int *)malloc(sizeof(*g_order) * FD_MAX_ENTRIES);
    g_rows = (char (*)[FD_ROW_CAPACITY])malloc(sizeof(*g_rows) * FD_MAX_ENTRIES);
    g_row_ptrs = (const char **)malloc(sizeof(*g_row_ptrs) * FD_MAX_ENTRIES);
    g_row_icons = (const struct sx_bitmap **)malloc(sizeof(*g_row_icons) * FD_MAX_ENTRIES);
    if (g_entries == 0 || g_order == 0 || g_rows == 0 || g_row_ptrs == 0 || g_row_icons == 0)
    {
        free(g_entries);
        free(g_order);
        free(g_rows);
        free(g_row_ptrs);
        free(g_row_icons);
        g_entries = 0;
        return 0;
    }
    return 1;
}

/* Folders first, then by name with case folded: the Explorer's order. */
static int fd_before(const struct fd_entry *left, const struct fd_entry *right)
{
    const char *a = left->name;
    const char *b = right->name;

    if (left->is_dir != right->is_dir)
    {
        return left->is_dir;
    }
    while (*a != '\0' && fd_lower(*a) == fd_lower(*b))
    {
        ++a;
        ++b;
    }
    return fd_lower(*a) < fd_lower(*b);
}

static void fd_type_of(const struct fd_entry *entry, char *out, size_t capacity)
{
    const char *dot = 0;
    const char *cursor;
    size_t length = 0;

    if (entry->is_dir)
    {
        snprintf(out, capacity, "File Folder");
        return;
    }
    for (cursor = entry->name + 1; *cursor != '\0'; ++cursor)
    {
        if (*cursor == '.' && cursor[1] != '\0')
        {
            dot = cursor;
        }
    }
    if (dot == 0)
    {
        snprintf(out, capacity, "File");
        return;
    }
    for (cursor = dot + 1; *cursor != '\0' && length + 6 < capacity && length < 12; ++cursor)
    {
        out[length++] = *cursor >= 'a' && *cursor <= 'z' ? (char)(*cursor - 'a' + 'A') : *cursor;
    }
    snprintf(out + length, capacity - length, " File");
}

/* Reads `folder` into the listing, filtered and sorted. 0 or -errno; on an
 * error the listing is left as it was. */
static long fd_list(const char *folder, const char *patterns)
{
    struct sxn_directory_entry entry;
    long handle = sxn_open_directory(folder);
    long result;
    int count = 0;
    int truncated = 0;

    if (handle < 0)
    {
        return handle;
    }
    while ((result = sxn_read_directory(handle, &entry)) == 1)
    {
        const int is_dir = entry.info.type == SXN_FILE_TYPE_DIRECTORY;
        int at;

        if (!is_dir && (entry.info.type != SXN_FILE_TYPE_FILE || !fd_matches(patterns, entry.name)))
        {
            continue;
        }
        if (count >= FD_MAX_ENTRIES)
        {
            truncated = 1;
            continue;
        }
        snprintf(g_entries[count].name, sizeof(g_entries[count].name), "%s", entry.name);
        g_entries[count].size = entry.info.size;
        g_entries[count].is_dir = is_dir;
        /* Insertion into the order: a folder holds a few hundred at most. */
        at = count;
        while (at > 0 && fd_before(&g_entries[count], &g_entries[g_order[at - 1]]))
        {
            g_order[at] = g_order[at - 1];
            --at;
        }
        g_order[at] = count;
        ++count;
    }
    (void)sxn_close(handle);
    g_entry_count = count;
    g_message[0] = '\0';
    if (truncated)
    {
        snprintf(g_message, sizeof(g_message), "Only the first %d items are listed.", FD_MAX_ENTRIES);
    }
    return result < 0 ? result : 0;
}

/* The listbox's rows, from the listing. */
static void fd_fill_rows(void)
{
    int row;

    for (row = 0; row < g_entry_count; ++row)
    {
        const struct fd_entry *entry = &g_entries[g_order[row]];
        char size[32];
        char type[24];

        size[0] = '\0';
        if (!entry->is_dir)
        {
            snprintf(size, sizeof(size), "%llu KB", (unsigned long long)((entry->size + 1023u) / 1024u));
        }
        fd_type_of(entry, type, sizeof(type));
        snprintf(g_rows[row], FD_ROW_CAPACITY, "%s\t%s\t%s", entry->name, size, type);
        g_row_ptrs[row] = g_rows[row];
        g_row_icons[row] = mime_icon_for_file(entry->name, entry->is_dir, 0);
    }
    g_widgets[FD_LIST].items = g_row_ptrs;
    g_widgets[FD_LIST].item_icons = g_row_icons;
    g_widgets[FD_LIST].item_count = g_entry_count;
    g_widgets[FD_LIST].value = -1;
    g_widgets[FD_LIST].scroll = 0;
}

static const char *fd_error_text(long error)
{
    switch (-error)
    {
    case SAVANXP_EACCES:
        return "access is denied";
    case SAVANXP_ENOENT:
        return "it does not exist";
    case SAVANXP_ENOTDIR:
        return "it is not a folder";
    case SAVANXP_ENAMETOOLONG:
        return "the path is too long";
    default:
        return "it cannot be read";
    }
}

static void fd_repaint(void)
{
    if (g_app != 0)
    {
        sxngui_app_request_repaint(g_app);
    }
}

/* Shows `folder`. 1, or 0 with the reason in the message line and the
 * folder shown before left in place. */
static int fd_go_to(const char *folder)
{
    char normalized[SXN_FILE_DIALOG_PATH_MAX];
    long listed;

    if (!fd_normalize(folder, normalized, sizeof(normalized)))
    {
        snprintf(g_message, sizeof(g_message), "The path is too long.");
        fd_repaint();
        return 0;
    }
    listed = fd_list(normalized, g_filter_patterns);
    if (listed < 0)
    {
        snprintf(g_message, sizeof(g_message), "%.100s cannot be opened: %s.", normalized, fd_error_text(listed));
        fd_repaint();
        return 0;
    }
    snprintf(g_folder, sizeof(g_folder), "%s", normalized);
    fd_fill_rows();
    if (strcmp(g_folder, "/") == 0)
    {
        g_widgets[FD_UP].flags |= SXGUI_FLAG_DISABLED;
    }
    else
    {
        g_widgets[FD_UP].flags &= ~SXGUI_FLAG_DISABLED;
    }
    fd_repaint();
    return 1;
}

/* ---- finishing ----------------------------------------------------------------- */

static void fd_watch(struct sxgui_dialog *dialog);

static void fd_finish(const char *path)
{
    void (*done)(struct sxngui_app *, const char *, void *) = g_on_done;

    sxngui_app_watch_dialog(g_app, 0, 0, 0);
    if (sxgui_dialog_active(&g_app->ui))
    {
        sxgui_dialog_end(&g_app->ui, path != 0 ? 1 : 0);
    }
    snprintf(g_last_folder, sizeof(g_last_folder), "%s", g_folder);
    g_on_done = 0;
    if (path != 0 && path != g_result)
    {
        snprintf(g_result, sizeof(g_result), "%s", path);
    }
    if (done != 0)
    {
        done(g_app, path != 0 ? g_result : 0, g_user);
    }
    fd_repaint();
}

static void fd_reopen(void)
{
    g_widgets[FD_NAME].caret = (int)strlen(g_name);
    sxgui_dialog_begin(&g_app->ui, &g_dialog, FD_WIDTH, FD_HEIGHT);
    fd_watch(&g_dialog);
    fd_repaint();
}

static void fd_on_confirm(struct sxgui_widget *widget, void *user)
{
    (void)user;
    if (widget == &g_confirm_widgets[FD_CONFIRM_YES])
    {
        fd_finish(g_result);
        return;
    }
    /* No: back to the file dialog, as it was. */
    fd_reopen();
}

static void fd_ask_replace(void)
{
    snprintf(g_confirm_text, sizeof(g_confirm_text), "%.80s already exists.", fd_leaf(g_result));
    g_confirm_widgets[FD_CONFIRM_LINE1] = sxgui_label(sx_rect_make(SXGUI_DIALOG_MARGIN, 14, FD_CONFIRM_WIDTH - 16, 18),
        g_confirm_text);
    g_confirm_widgets[FD_CONFIRM_LINE2] = sxgui_label(sx_rect_make(SXGUI_DIALOG_MARGIN, 36, FD_CONFIRM_WIDTH - 16, 18),
        "Do you want to replace it?");
    g_confirm_widgets[FD_CONFIRM_YES] = sxgui_button(
        sx_rect_make(FD_CONFIRM_WIDTH - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_WIDTH * 2 - SXGUI_GAP,
            FD_CONFIRM_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Yes", fd_on_confirm, 0);
    g_confirm_widgets[FD_CONFIRM_NO] = sxgui_button(
        sx_rect_make(FD_CONFIRM_WIDTH - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_WIDTH,
            FD_CONFIRM_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "No", fd_on_confirm, 0);
    memset(&g_confirm, 0, sizeof(g_confirm));
    g_confirm.title = "Confirm Save As";
    g_confirm.widgets = g_confirm_widgets;
    g_confirm.widget_count = FD_CONFIRM_COUNT;
    /* As every confirmation here: the safe answer is the default. */
    g_confirm.initial_focus = FD_CONFIRM_NO;
    g_confirm.default_button = FD_CONFIRM_NO;
    sxgui_dialog_begin(&g_app->ui, &g_confirm, FD_CONFIRM_WIDTH, FD_CONFIRM_HEIGHT);
    fd_watch(&g_confirm);
}

/* The file dialog closed by ESC or its close button is a cancel; the
 * confirmation closed that way is a "No". Every other way it closes clears
 * the watch first. */
static void fd_on_closed(struct sxngui_app *app, struct sxgui_dialog *dialog, void *user)
{
    (void)app;
    (void)user;
    if (dialog == &g_confirm)
    {
        fd_reopen();
        return;
    }
    fd_finish(0);
}

static void fd_watch(struct sxgui_dialog *dialog)
{
    sxngui_app_watch_dialog(g_app, dialog, fd_on_closed, 0);
}

/* What the OK button does with the name typed. */
static void fd_accept(void)
{
    struct sxn_file_info info;
    char path[SXN_FILE_DIALOG_PATH_MAX];
    char name[SXN_FILE_DIALOG_PATH_MAX];
    const int selected = g_widgets[FD_LIST].value;
    const struct fd_entry *chosen =
        selected >= 0 && selected < g_entry_count ? &g_entries[g_order[selected]] : 0;
    size_t length;
    char *trimmed = g_name;
    long queried;

    /* Enter on a selected folder, or OK with nothing typed, opens it. */
    if (chosen != 0 && chosen->is_dir && (g_app->ui.focus_index == FD_LIST || g_name[0] == '\0'))
    {
        if (fd_join(g_folder, chosen->name, path, sizeof(path)))
        {
            (void)fd_go_to(path);
        }
        return;
    }
    while (*trimmed == ' ')
    {
        ++trimmed;
    }
    length = strlen(trimmed);
    while (length > 0 && trimmed[length - 1] == ' ')
    {
        --length;
    }
    if (length == 0)
    {
        snprintf(g_message, sizeof(g_message), "Type a file name, or pick one in the list.");
        fd_repaint();
        return;
    }
    snprintf(name, sizeof(name), "%.*s", (int)length, trimmed);
    if (strchr(name, '*') != 0 || strchr(name, '?') != 0)
    {
        snprintf(g_message, sizeof(g_message), "A file name cannot contain * or ?.");
        fd_repaint();
        return;
    }
    if (!fd_join(g_folder, name, path, sizeof(path)))
    {
        snprintf(g_message, sizeof(g_message), "The path is too long.");
        fd_repaint();
        return;
    }
    /* A folder typed in the field is opened, as in Windows. */
    queried = sxn_query_path(path, &info);
    if (queried == 0 && info.type == SXN_FILE_TYPE_DIRECTORY)
    {
        if (fd_go_to(path))
        {
            g_name[0] = '\0';
            g_widgets[FD_NAME].caret = 0;
        }
        return;
    }
    if (g_mode == SXN_FILE_DIALOG_SAVE && g_default_extension[0] != '\0' && !fd_has_extension(fd_leaf(path)))
    {
        char extended[SXN_FILE_DIALOG_PATH_MAX];

        if (snprintf(extended, sizeof(extended), "%s.%s", path, g_default_extension) >= (int)sizeof(extended))
        {
            snprintf(g_message, sizeof(g_message), "The path is too long.");
            fd_repaint();
            return;
        }
        snprintf(path, sizeof(path), "%s", extended);
        queried = sxn_query_path(path, &info);
    }
    if (queried == 0 && info.type != SXN_FILE_TYPE_FILE)
    {
        snprintf(g_message, sizeof(g_message), "%.100s is not a file.", fd_leaf(path));
        fd_repaint();
        return;
    }
    if (g_mode == SXN_FILE_DIALOG_OPEN)
    {
        if (queried != 0)
        {
            snprintf(g_message, sizeof(g_message), "%.100s: %s.", fd_leaf(path), fd_error_text(queried));
            fd_repaint();
            return;
        }
        fd_finish(path);
        return;
    }
    if (queried != 0 && queried != -SAVANXP_ENOENT)
    {
        snprintf(g_message, sizeof(g_message), "%.100s: %s.", fd_leaf(path), fd_error_text(queried));
        fd_repaint();
        return;
    }
    if (queried == -SAVANXP_ENOENT)
    {
        /* The folder it goes in has to exist: the dialog creates no folders. */
        char parent[SXN_FILE_DIALOG_PATH_MAX];

        if (!fd_join(path, "..", parent, sizeof(parent)) || sxn_query_path(parent, &info) != 0 ||
            info.type != SXN_FILE_TYPE_DIRECTORY)
        {
            snprintf(g_message, sizeof(g_message), "The folder for %.100s does not exist.", fd_leaf(path));
            fd_repaint();
            return;
        }
        fd_finish(path);
        return;
    }
    snprintf(g_result, sizeof(g_result), "%s", path);
    fd_ask_replace();
}

static void fd_on_widget(struct sxgui_widget *widget, void *user)
{
    (void)user;
    if (widget == &g_widgets[FD_OK])
    {
        fd_accept();
        return;
    }
    if (widget == &g_widgets[FD_CANCEL])
    {
        fd_finish(0);
        return;
    }
    if (widget == &g_widgets[FD_UP])
    {
        char parent[SXN_FILE_DIALOG_PATH_MAX];

        if (fd_join(g_folder, "..", parent, sizeof(parent)))
        {
            (void)fd_go_to(parent);
        }
        return;
    }
    if (widget == &g_widgets[FD_LIST])
    {
        const int selected = widget->value;
        const struct fd_entry *entry;

        if (selected < 0 || selected >= g_entry_count)
        {
            return;
        }
        entry = &g_entries[g_order[selected]];
        if (widget->action == SXGUI_ACTION_ACTIVATE)
        {
            char path[SXN_FILE_DIALOG_PATH_MAX];

            if (entry->is_dir)
            {
                if (fd_join(g_folder, entry->name, path, sizeof(path)))
                {
                    (void)fd_go_to(path);
                }
                return;
            }
            snprintf(g_name, sizeof(g_name), "%s", entry->name);
            g_widgets[FD_NAME].caret = (int)strlen(g_name);
            fd_accept();
            return;
        }
        /* Picking a file names it in the field; picking a folder does not. */
        if (!entry->is_dir)
        {
            snprintf(g_name, sizeof(g_name), "%s", entry->name);
            g_widgets[FD_NAME].caret = (int)strlen(g_name);
            g_widgets[FD_NAME].scroll = 0;
        }
        fd_repaint();
    }
}

/* ---- the dialog --------------------------------------------------------------- */

static void fd_prepare_icons(void)
{
    if (g_icons_ready)
    {
        return;
    }
    mime_icon_set_reader(fd_read_file);
    (void)mime_icon_load_file(MIME_ICON_SYSTEM_POLICY);
    {
        struct savanxp_fb_info info;

        memset(&info, 0, sizeof(info));
        info.width = TOOLBAR_GLYPH_SIZE;
        info.height = TOOLBAR_GLYPH_SIZE;
        info.pitch = TOOLBAR_GLYPH_SIZE * (uint32_t)sizeof(uint32_t);
        info.bpp = 32u;
        info.buffer_size = info.pitch * TOOLBAR_GLYPH_SIZE;
        sx_bitmap_wrap(&g_up_bitmap, (uint32_t *)toolbar_glyph_go_up, &info, SX_PIXEL_FORMAT_BGRA8888);
        g_up_glyph[0] = &g_up_bitmap;
    }
    g_icons_ready = 1;
}

static void fd_build_widgets(void)
{
    const int right = FD_WIDTH - SXGUI_DIALOG_MARGIN;
    const int field_x = SXGUI_DIALOG_MARGIN + FD_LABEL_WIDTH;
    const int button_x = right - SXGUI_BUTTON_WIDTH;
    const int field_width = button_x - SXGUI_GAP - field_x;
    const int message_y = FD_HEIGHT - SXGUI_DIALOG_MARGIN - 18;
    const int type_y = message_y - SXGUI_GAP - SXGUI_BUTTON_HEIGHT;
    const int name_y = type_y - SXGUI_GAP - SXGUI_BUTTON_HEIGHT;
    const int list_y = SXGUI_DIALOG_MARGIN + FD_UP_SIZE + SXGUI_GAP;
    const int list_width = FD_WIDTH - SXGUI_DIALOG_MARGIN * 2;
    const int save = g_mode == SXN_FILE_DIALOG_SAVE;
    int name_column;

    g_widgets[FD_LOOK_IN_LABEL] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + 6, FD_LABEL_WIDTH - 4, 18), save ? "Save in:" : "Look in:");
    g_widgets[FD_FOLDER] = sxgui_label(sx_rect_make(field_x, SXGUI_DIALOG_MARGIN + 2,
                                           right - FD_UP_SIZE - SXGUI_GAP - field_x, SXGUI_FIELD_HEIGHT),
        g_folder);
    g_widgets[FD_FOLDER].flags |= SXGUI_FLAG_SUNKEN;
    g_widgets[FD_UP] = sxgui_toolbar_button(sx_rect_make(right - FD_UP_SIZE, SXGUI_DIALOG_MARGIN, FD_UP_SIZE, FD_UP_SIZE),
        0, g_up_glyph, fd_on_widget, 0);
    g_widgets[FD_LIST] = sxgui_listbox(sx_rect_make(SXGUI_DIALOG_MARGIN, list_y, list_width, name_y - SXGUI_GAP - list_y),
        g_row_ptrs, 0);
    g_widgets[FD_LIST].columns = g_columns;
    g_widgets[FD_LIST].column_count = 3;
    g_widgets[FD_LIST].item_icons = g_row_icons;
    g_widgets[FD_LIST].flags |= SXGUI_FLAG_EMPTY_SELECTION;
    g_widgets[FD_LIST].value = -1;
    g_widgets[FD_LIST].on_action = fd_on_widget;
    name_column = list_width - SXGUI_BORDER_SUNKEN * 2 - SXGUI_SCROLLBAR_THICKNESS - FD_SIZE_COLUMN - FD_TYPE_COLUMN;
    g_columns[0].width = name_column > 80 ? name_column : 80;

    g_widgets[FD_NAME_LABEL] = sxgui_label(sx_rect_make(SXGUI_DIALOG_MARGIN, name_y + 5, FD_LABEL_WIDTH - 4, 18),
        "File name:");
    g_widgets[FD_NAME] = sxgui_textfield(sx_rect_make(field_x, name_y + 1, field_width, SXGUI_FIELD_HEIGHT), g_name,
        (int)sizeof(g_name));
    g_widgets[FD_NAME].caret = (int)strlen(g_name);
    g_widgets[FD_OK] = sxgui_button(sx_rect_make(button_x, name_y, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        save ? "Save" : "Open", fd_on_widget, 0);
    g_widgets[FD_TYPE_LABEL] = sxgui_label(sx_rect_make(SXGUI_DIALOG_MARGIN, type_y + 5, FD_LABEL_WIDTH - 4, 18),
        save ? "Save as type:" : "Files of type:");
    g_widgets[FD_TYPE] = sxgui_label(sx_rect_make(field_x, type_y + 1, field_width, SXGUI_FIELD_HEIGHT),
        g_filter_description);
    g_widgets[FD_TYPE].flags |= SXGUI_FLAG_SUNKEN;
    g_widgets[FD_CANCEL] = sxgui_button(sx_rect_make(button_x, type_y, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Cancel", fd_on_widget, 0);
    g_widgets[FD_MESSAGE] = sxgui_label(sx_rect_make(SXGUI_DIALOG_MARGIN, message_y, list_width, 18), g_message);
}

int sxn_file_dialog_begin(struct sxngui_app *app, const struct sxn_file_dialog_request *request)
{
    const char *start;

    if (app == 0 || request == 0 || request->on_done == 0 || !fd_allocate())
    {
        return -1;
    }
    g_app = app;
    g_mode = request->mode == SXN_FILE_DIALOG_SAVE ? SXN_FILE_DIALOG_SAVE : SXN_FILE_DIALOG_OPEN;
    snprintf(g_title, sizeof(g_title), "%s",
        request->title != 0 ? request->title : g_mode == SXN_FILE_DIALOG_SAVE ? "Save As" : "Open");
    snprintf(g_filter_patterns, sizeof(g_filter_patterns), "%s",
        request->filter_patterns != 0 ? request->filter_patterns : "*.*");
    snprintf(g_filter_description, sizeof(g_filter_description), "%s",
        request->filter_description != 0 ? request->filter_description : "All files (*.*)");
    snprintf(g_default_extension, sizeof(g_default_extension), "%s",
        request->default_extension != 0 && request->default_extension[0] == '.' ? request->default_extension + 1
        : request->default_extension != 0 ? request->default_extension : "");
    snprintf(g_name, sizeof(g_name), "%s", request->file_name != 0 ? request->file_name : "");
    g_on_done = request->on_done;
    g_user = request->user;
    g_message[0] = '\0';

    fd_prepare_icons();
    fd_build_widgets();
    g_entry_count = 0;
    fd_fill_rows();
    start = request->initial_directory;
    if (start == 0 || !fd_go_to(start))
    {
        char why[sizeof(g_message)];

        /* The folder asked for could not be shown: say why over the one
         * shown instead. */
        snprintf(why, sizeof(why), "%s", start != 0 ? g_message : "");
        if (g_last_folder[0] == '\0' || !fd_go_to(g_last_folder))
        {
            (void)fd_go_to("/");
        }
        if (why[0] != '\0')
        {
            snprintf(g_message, sizeof(g_message), "%s", why);
        }
    }

    memset(&g_dialog, 0, sizeof(g_dialog));
    g_dialog.title = g_title;
    g_dialog.widgets = g_widgets;
    g_dialog.widget_count = FD_WIDGET_COUNT;
    g_dialog.initial_focus = FD_NAME;
    g_dialog.default_button = FD_OK;
    sxgui_dialog_begin(&app->ui, &g_dialog, FD_WIDTH, FD_HEIGHT);
    fd_watch(&g_dialog);
    fd_repaint();
    return 0;
}

/* ---- selftest --------------------------------------------------------------------- */

static int g_failures;

static void fd_check(int ok, const char *label)
{
    if (!ok)
    {
        char line[128];

        snprintf(line, sizeof(line), "sxnfiledlg: selftest check failed: %s", label);
        (void)sxn_log(line);
        g_failures += 1;
    }
}

static int fd_normalized_is(const char *input, const char *expected)
{
    char out[SXN_FILE_DIALOG_PATH_MAX];

    return fd_normalize(input, out, sizeof(out)) && strcmp(out, expected) == 0;
}

int sxn_file_dialog_selftest(void)
{
    char out[SXN_FILE_DIALOG_PATH_MAX];
    char long_path[SXN_FILE_DIALOG_PATH_MAX + 16];
    int index;
    int found_ini = 0;
    int other_files = 0;
    int folders_first = 1;
    int seen_file = 0;

    g_failures = 0;
    fd_check(fd_normalized_is("/", "/"), "root");
    fd_check(fd_normalized_is("//users///user/", "/users/user"), "slashes");
    fd_check(fd_normalized_is("/users/./user/../user", "/users/user"), "dots");
    fd_check(fd_normalized_is("/..", "/"), "above the root");
    fd_check(fd_normalized_is("/a/b/../../..", "/"), "above the root twice");
    fd_check(!fd_normalize("relative", out, sizeof(out)), "relative refused");
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[0] = '/';
    long_path[sizeof(long_path) - 1] = '\0';
    fd_check(!fd_normalize(long_path, out, sizeof(out)), "too long refused");
    fd_check(fd_join("/users/user", "log.txt", out, sizeof(out)) && strcmp(out, "/users/user/log.txt") == 0, "join");
    fd_check(fd_join("/users/user", "/system", out, sizeof(out)) && strcmp(out, "/system") == 0, "join absolute");
    fd_check(fd_join("/users/user", "..", out, sizeof(out)) && strcmp(out, "/users") == 0, "join up");
    fd_check(fd_matches("*.txt;*.log", "SYSTEM.LOG"), "pattern folded");
    fd_check(fd_matches("*.txt;*.log", "notes.txt"), "pattern first");
    fd_check(!fd_matches("*.txt;*.log", "notes.txt.bak"), "pattern anchored");
    fd_check(fd_matches("*.*", "README"), "*.* takes names without a dot");
    fd_check(fd_matches("boot?.cfg", "boot1.cfg") && !fd_matches("boot?.cfg", "boot10.cfg"), "question mark");
    fd_check(fd_has_extension("log.txt") && !fd_has_extension(".profile") && !fd_has_extension("log."),
        "extension");

    fd_check(fd_allocate(), "listing memory");
    fd_check(fd_list("/", "*.ini") == 0, "list the root");
    for (index = 0; index < g_entry_count; ++index)
    {
        const struct fd_entry *entry = &g_entries[g_order[index]];

        if (entry->is_dir)
        {
            folders_first = folders_first && !seen_file;
            continue;
        }
        seen_file = 1;
        if (strcmp(entry->name, "mimeicon.ini") == 0)
        {
            found_ini = entry->size > 0;
        }
        else if (!fd_matches("*.ini", entry->name))
        {
            other_files += 1;
        }
    }
    fd_check(found_ini, "the root lists mimeicon.ini");
    fd_check(other_files == 0, "the filter keeps other files out");
    fd_check(folders_first, "folders come first");
    fd_check(fd_list("/no-such-folder", "*.*") == -SAVANXP_ENOENT, "a missing folder");
    fd_check(fd_list("/mimeicon.ini", "*.*") == -SAVANXP_ENOTDIR, "a file is not a folder");
    g_entry_count = 0;
    return g_failures;
}
