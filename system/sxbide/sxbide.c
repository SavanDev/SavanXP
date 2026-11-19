/* sxbide: Savan BASIC, the development environment of SxBASIC
 * (docs/userland/SXBASIC_IDE.md).
 *
 *   sxbide [project.sbp]
 *   sxbide --selftest <script>
 *
 * One window: a menu bar and a toolbar, the project's files on the left with
 * the property grid under them, the code editor, and the output of the last
 * Run or Make SXE under it. Forms are edited as text in this version; the
 * property grid shows the `Key = value` lines of the block the caret is in
 * (the project's, or a form's or control's Begin ... End) and writes a value
 * back into that line.
 *
 * Run and Make SXE start /system/sxbasic.sxe as a child process with its
 * output on a pipe, so a program that loops or crashes does not take the
 * environment with it. A compile or runtime error the runner prints names a
 * file and a line; the environment opens that file at that line.
 *
 * --selftest drives the same code with no window, from a script of what a
 * user does (sxbide-smoke). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxgfx/pixels.h"
#include "sxn/sxn.h"
#include "sxn/sxnfiledlg.h"
#include "sxn/sxngui.h"

#include "sxbide.sunres.h"
#include "icons.inc"

/* The runner, and the stub Make SXE copies (docs/userland/SXBASIC_IMAGE.md). */
#define IDE_RUNNER "/system/sxbasic.sxe"

#define IDE_MAX_FILES 16
#define IDE_MAX_ASSETS 8
#define IDE_PATH_MAX 512
#define IDE_NAME_MAX 128
#define IDE_OUT_LINES 256
#define IDE_OUT_WIDTH 200
#define IDE_MAX_PROPS 48
#define IDE_VALUE_MAX 128
#define IDE_DEFAULT_WIDTH 800
#define IDE_DEFAULT_HEIGHT 560
#define IDE_LEFT_WIDTH 220
#define IDE_OUTPUT_HEIGHT 128
#define IDE_LABEL_HEIGHT 18

#define IDE_COLOR_KEYWORD 0x000080u
#define IDE_COLOR_COMMENT 0x008000u

enum ide_kind { IDE_PROJECT, IDE_FORM, IDE_MODULE };

struct ide_file {
    char name[IDE_NAME_MAX]; /* as the project names it; the project file's own name for the first */
    char path[IDE_PATH_MAX];
    int kind;
    char row[IDE_NAME_MAX + 4]; /* what the explorer shows: the name, and * when modified */
};

enum {
    W_TB_OPEN,
    W_TB_SAVE,
    W_TB_SEP1,
    W_TB_START,
    W_TB_END,
    W_TB_SEP2,
    W_TB_MAKE,
    W_FILES_LABEL,
    W_FILES,
    W_PROPS_LABEL,
    W_PROPS,
    W_OUTPUT_LABEL,
    W_OUTPUT,
    W_STATUS,
    W_EDIT, /* one editor per file, IDE_MAX_FILES of them; the current one is shown */
    W_COUNT = W_EDIT + IDE_MAX_FILES,
};

enum {
    CMD_NEW = 1,
    CMD_OPEN,
    CMD_SAVE,
    CMD_SAVE_ALL,
    CMD_SAVE_AS,
    CMD_EXIT,
    CMD_UNDO,
    CMD_REDO,
    CMD_CUT,
    CMD_COPY,
    CMD_PASTE,
    CMD_SELECT_ALL,
    CMD_START,
    CMD_END,
    CMD_MAKE,
};

enum { CHILD_NONE, CHILD_RUN, CHILD_MAKE };

static struct sxngui_app g_app;
static int g_app_open;
static int g_headless;
static struct sxgui_context g_headless_ui;
static struct sxgui_context *g_ui;
static struct sxgui_widget g_widgets[W_COUNT];
static struct sxgui_menubar g_menubar;

static struct ide_file g_files[IDE_MAX_FILES];
static const char *g_file_rows[IDE_MAX_FILES];
static int g_file_count;
static int g_current = -1;
static char g_assets[IDE_MAX_ASSETS][IDE_NAME_MAX]; /* the project's Icon files, copied by Save Project As */
static int g_asset_count;
static char g_project_path[IDE_PATH_MAX];
static char g_project_dir[IDE_PATH_MAX];

static char g_out[IDE_OUT_LINES][IDE_OUT_WIDTH];
static const char *g_out_rows[IDE_OUT_LINES];
static int g_out_count;
static char g_status[256];
static char g_message[200];
static char g_props_title[96] = "Properties";

/* The child process of Run or Make SXE, and what it printed. */
static int g_child_kind;
static long g_child_pid = -1;
static long g_child_pipe = -1;
static char g_partial[IDE_OUT_WIDTH];
static int g_partial_length;
static char *g_run_output;
static size_t g_run_output_length;
static size_t g_run_output_capacity;
static int g_last_exit = -1;
static char g_made_path[IDE_PATH_MAX];

/* The first error the last run named, and the line marked for it. */
static int g_error_seen;
static char g_error_file[IDE_NAME_MAX];
static int g_error_line;
static int g_mark_file = -1;
static int g_mark_line;

/* A command that would drop unsaved changes runs when chosen twice in a row. */
static int g_discard_armed;
static int g_discard_pending;

/* ---- small helpers ------------------------------------------------------------------- */

static void put(long handle, const char *text)
{
    (void)sxn_write(handle, text, strlen(text));
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void directory_of(char *out, size_t size, const char *path)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        snprintf(out, size, "/");
        return;
    }
    snprintf(out, size, "%.*s", slash == path ? 1 : (int)(slash - path), path);
}

static int same_text_nocase(const char *a, int length, const char *b)
{
    int i;
    for (i = 0; i < length; i++) {
        char x = a[i], y = b[i];
        if (y == '\0') return 0;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return b[length] == '\0';
}

/* Byte offset of the start of 1-based `line`, or of the text's end. */
static int line_offset(const char *text, int line)
{
    int offset = 0;
    while (line > 1 && text[offset] != '\0') {
        if (text[offset++] == '\n') line--;
    }
    return offset;
}

/* 1-based line of the byte at `offset`. */
static int line_of(const char *text, int offset)
{
    int line = 1, i;
    for (i = 0; i < offset && text[i] != '\0'; i++) {
        if (text[i] == '\n') line++;
    }
    return line;
}

static struct sxgui_widget *editor(int file)
{
    return &g_widgets[W_EDIT + file];
}

static const char *document(int file)
{
    const char *text = file >= 0 && file < g_file_count ? editor(file)->edit_buffer : NULL;
    return text ? text : "";
}

static void repaint(void)
{
    if (g_app_open) sxngui_app_request_repaint(&g_app);
}

static void set_message(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(g_message, sizeof(g_message), format, args);
    va_end(args);
    repaint();
}

/* ---- files ------------------------------------------------------------------------- */

/* The whole file, NUL terminated; 0 or a negative error. */
static long read_whole(const char *path, char **data, size_t *size)
{
    struct sxn_file_info info;
    long result = sxn_query_path(path, &info);
    if (result < 0) return result;
    if (info.type != SXN_FILE_TYPE_FILE) return -SAVANXP_EISDIR;
    long handle = sxn_open_file(path, SXN_FILE_READ);
    if (handle < 0) return handle;
    size_t capacity = (size_t)info.size + 1u, used = 0;
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        (void)sxn_close(handle);
        return -SAVANXP_ENOMEM;
    }
    for (;;) {
        if (used + 1u >= capacity) {
            char *grown = realloc(buffer, capacity * 2u);
            if (grown == NULL) {
                free(buffer);
                (void)sxn_close(handle);
                return -SAVANXP_ENOMEM;
            }
            buffer = grown;
            capacity *= 2u;
        }
        long n = sxn_read(handle, buffer + used, capacity - used - 1u);
        if (n < 0) {
            free(buffer);
            (void)sxn_close(handle);
            return n;
        }
        if (n == 0) break;
        used += (size_t)n;
    }
    (void)sxn_close(handle);
    buffer[used] = '\0';
    *data = buffer;
    *size = used;
    return 0;
}

static long write_whole(const char *path, const char *data, size_t size, unsigned disposition)
{
    long handle = sxn_create_file(path, disposition);
    if (handle < 0) return handle;
    for (size_t done = 0; done < size;) {
        long n = sxn_write(handle, data + done, (unsigned long)(size - done));
        if (n <= 0) {
            (void)sxn_close(handle);
            return n < 0 ? n : -SAVANXP_EIO;
        }
        done += (size_t)n;
    }
    long flushed = sxn_flush_file(handle);
    (void)sxn_close(handle);
    return flushed < 0 ? flushed : 0;
}

/* A safe save: the new text goes to a file beside the old one, which is
 * replaced by renaming over it only once the new one is whole. */
static long save_text(const char *path, const char *data, size_t size)
{
    char temporary[IDE_PATH_MAX + 8];
    snprintf(temporary, sizeof(temporary), "%s.saving", path);
    long result = write_whole(temporary, data, size, SXN_CREATE_ALWAYS);
    if (result < 0) return result;
    return sxn_rename_path(temporary, path);
}

/* Every missing directory of an absolute path. */
static long make_directories(const char *directory)
{
    char path[IDE_PATH_MAX];
    size_t length = strlen(directory);
    if (directory[0] != '/' || length >= sizeof(path)) return -SAVANXP_EINVAL;
    memcpy(path, directory, length + 1);
    for (size_t i = 1; i <= length; i++) {
        if (path[i] != '/' && path[i] != '\0') continue;
        char saved = path[i];
        struct sxn_file_info info;
        path[i] = '\0';
        if (sxn_query_path(path, &info) < 0) {
            long made = sxn_make_directory(path);
            if (made < 0 && made != -SAVANXP_EEXIST) return made;
        } else if (info.type != SXN_FILE_TYPE_DIRECTORY) {
            return -SAVANXP_ENOTDIR;
        }
        path[i] = saved;
    }
    return 0;
}

/* ---- the output pane ----------------------------------------------------------------- */

static int listbox_rows(const struct sxgui_widget *widget)
{
    /* The listbox's row height is private (PLATFORM_GRAPHICS.md, known gaps). */
    int rows = (widget->rect.height - SXGUI_BORDER_SUNKEN * 2) / (gfx_text_height() + 4);
    return rows > 0 ? rows : 1;
}

static void output_clear(void)
{
    g_out_count = 0;
    g_widgets[W_OUTPUT].item_count = 0;
    g_widgets[W_OUTPUT].scroll = 0;
    g_widgets[W_OUTPUT].value = -1;
}

static void output(const char *text)
{
    if (g_out_count == IDE_OUT_LINES) {
        memmove(g_out[0], g_out[1], sizeof(g_out[0]) * (IDE_OUT_LINES - 1));
        g_out_count--;
    }
    snprintf(g_out[g_out_count], IDE_OUT_WIDTH, "%s", text);
    g_out_rows[g_out_count] = g_out[g_out_count];
    g_out_count++;
    struct sxgui_widget *list = &g_widgets[W_OUTPUT];
    list->item_count = g_out_count;
    int first = g_out_count - listbox_rows(list);
    list->scroll = first > 0 ? first : 0;
    repaint();
}

static void outputf(const char *format, ...)
{
    char line[IDE_OUT_WIDTH];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    output(line);
}

/* ---- the code editor ----------------------------------------------------------------- */

static const char *const g_keywords[] = {
    "And", "As", "Begin", "Boolean", "ByRef", "ByVal", "Case", "Catch", "Const", "Dim", "Do", "Double", "Each",
    "Else", "ElseIf", "End", "Exit", "False", "For", "Function", "If", "In", "Integer", "Is", "Loop", "Me", "Mod",
    "Next", "Not", "Or", "Preserve", "Print", "Private", "Public", "ReDim", "Return", "Select", "Step", "String",
    "Sub", "Then", "To", "True", "Try", "Unload", "Until", "While", "Xor",
};

static int is_word_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_word_char(char c)
{
    return is_word_start(c) || (c >= '0' && c <= '9');
}

/* BASIC's colours, as Visual Basic had them: keywords navy, comments green,
 * the rest in the editor's own colour. */
static int colorize(const char *line, int length, int line_number, struct sxgui_text_span *spans, int max_spans,
                    void *user)
{
    int count = 0, i = 0;
    (void)line_number;
    (void)user;
    while (i < length && count < max_spans) {
        char c = line[i];
        if (c == '"') {
            for (i++; i < length; i++) {
                if (line[i] == '"' && !(i + 1 < length && line[i + 1] == '"')) break;
                if (line[i] == '"') i++;
            }
            i++;
        } else if (c == '\'') {
            spans[count++] = (struct sxgui_text_span){i, length - i, IDE_COLOR_COMMENT};
            break;
        } else if (is_word_start(c)) {
            int start = i;
            while (i < length && is_word_char(line[i])) i++;
            if (same_text_nocase(line + start, i - start, "Rem")) {
                spans[count++] = (struct sxgui_text_span){start, length - start, IDE_COLOR_COMMENT};
                break;
            }
            for (size_t k = 0; k < sizeof(g_keywords) / sizeof(g_keywords[0]); k++) {
                if (same_text_nocase(line + start, i - start, g_keywords[k])) {
                    spans[count++] = (struct sxgui_text_span){start, i - start, IDE_COLOR_KEYWORD};
                    break;
                }
            }
        } else {
            i++;
        }
    }
    return count;
}

static unsigned marks(int line_number, void *user)
{
    int file = (int)(intptr_t)user;
    return file == g_mark_file && line_number + 1 == g_mark_line ? SXGUI_TEXTEDIT_MARK_CURRENT : 0u;
}

/* ---- the property grid ---------------------------------------------------------------- */

struct ide_prop {
    char key[32];
    char value[IDE_VALUE_MAX];
    int quoted;
    int value_start;
    int value_end;
};

static struct ide_prop g_props[IDE_MAX_PROPS];
static struct sxgui_property g_prop_rows[IDE_MAX_PROPS];
static int g_prop_count;
/* What the grid shows: the file, the line of the block, and the text it was read from. */
static int g_props_file = -1;
static int g_props_block = -1;
static uint32_t g_props_hash;
static int g_props_caret_line = -1;
static const char *const g_kind_choices[] = {"Console", "Windows"};

static uint32_t hash_text(const char *text)
{
    uint32_t hash = 2166136261u;
    for (; *text; text++) hash = (hash ^ (unsigned char)*text) * 16777619u;
    return hash;
}

/* The line [start, end) without its indent and trailing blanks. */
static void trim_line(const char *text, int start, int end, int *first, int *last)
{
    while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) end--;
    *first = start;
    *last = end;
}

static int line_is_word(const char *text, int first, int last, const char *word)
{
    int n = (int)strlen(word);
    return last - first >= n && same_text_nocase(text + first, n, word) &&
           (last - first == n || text[first + n] == ' ' || text[first + n] == '\t');
}

/* A `Key = value` line into a row; 0 when the line is not one. */
static int parse_property(const char *text, int first, int last, struct ide_prop *prop)
{
    int key_end = first;
    while (key_end < last && is_word_char(text[key_end])) key_end++;
    if (key_end == first || key_end - first >= (int)sizeof(prop->key)) return 0;
    int equals = key_end;
    while (equals < last && text[equals] == ' ') equals++;
    if (equals >= last || text[equals] != '=') return 0;
    int value = equals + 1;
    while (value < last && text[value] == ' ') value++;
    memcpy(prop->key, text + first, (size_t)(key_end - first));
    prop->key[key_end - first] = '\0';
    prop->value_start = value;
    prop->quoted = value < last && text[value] == '"';
    int out = 0;
    if (prop->quoted) {
        int i = value + 1;
        for (; i < last; i++) {
            if (text[i] == '"') {
                if (i + 1 < last && text[i + 1] == '"') {
                    i++;
                } else {
                    break;
                }
            }
            if (out < IDE_VALUE_MAX - 1) prop->value[out++] = text[i];
        }
        prop->value_end = i < last ? i + 1 : last;
    } else {
        int end = value;
        while (end < last && text[end] != '\'') end++;
        while (end > value && text[end - 1] == ' ') end--;
        out = end - value < IDE_VALUE_MAX - 1 ? end - value : IDE_VALUE_MAX - 1;
        memcpy(prop->value, text + value, (size_t)out);
        prop->value_end = end;
    }
    prop->value[out] = '\0';
    return 1;
}

static void props_add(const struct ide_prop *prop)
{
    if (g_prop_count == IDE_MAX_PROPS) return;
    struct ide_prop *slot = &g_props[g_prop_count];
    struct sxgui_property *row = &g_prop_rows[g_prop_count];
    *slot = *prop;
    memset(row, 0, sizeof(*row));
    row->name = slot->key;
    row->value = slot->value;
    row->capacity = IDE_VALUE_MAX;
    row->kind = SXGUI_PROPERTY_TEXT;
    if (strcmp(slot->key, "Kind") == 0) {
        row->kind = SXGUI_PROPERTY_LIST;
        row->choices = g_kind_choices;
        row->choice_count = 2;
    } else if (!slot->quoted && (strcmp(slot->value, "True") == 0 || strcmp(slot->value, "False") == 0)) {
        row->kind = SXGUI_PROPERTY_BOOL;
    } else if (!slot->quoted && (slot->value[0] == '&') && (slot->value[1] == 'H' || slot->value[1] == 'h')) {
        row->kind = SXGUI_PROPERTY_COLOR;
    }
    g_prop_count++;
}

/* Reads the rows of the block the caret of `file` is in: the whole project
 * file, or the innermost Begin ... End of a form around the caret (the form's
 * own block when the caret is in its code). A module has none. Returns the
 * line the block starts on, or -1. */
static int props_read(int file)
{
    const char *text = document(file);
    int caret_line = line_of(text, editor(file)->caret);
    int block = -1, block_depth = 0, form_line = -1;
    int stack[16], depth = 0, line = 0, offset = 0;
    char title[96] = "Properties";
    struct ide_prop prop;

    g_prop_count = 0;
    if (g_files[file].kind == IDE_MODULE) {
        snprintf(g_props_title, sizeof(g_props_title), "Properties");
        return -1;
    }
    if (g_files[file].kind == IDE_PROJECT) {
        while (text[offset] != '\0') {
            int end = offset, first, last;
            while (text[end] != '\0' && text[end] != '\n') end++;
            trim_line(text, offset, end, &first, &last);
            if (first < last && text[first] != '\'' && parse_property(text, first, last, &prop)) props_add(&prop);
            offset = text[end] ? end + 1 : end;
        }
        snprintf(g_props_title, sizeof(g_props_title), "Properties - Project");
        return 1;
    }
    /* Pass 1: the block around the caret. The form's block ends where its
     * depth returns to 0; what follows is code, whose End Sub is not one. */
    while (text[offset] != '\0') {
        int end = offset, first, last;
        line++;
        while (text[end] != '\0' && text[end] != '\n') end++;
        trim_line(text, offset, end, &first, &last);
        if (line_is_word(text, first, last, "Begin") && depth < 16) {
            if (form_line < 0) form_line = line;
            stack[depth++] = line;
        } else if (depth > 0 && last - first == 3 && same_text_nocase(text + first, 3, "End")) {
            if (line == caret_line) {
                block = stack[depth - 1];
                block_depth = depth;
            }
            depth--;
            if (depth == 0) {
                if (block < 0 && line >= caret_line) {
                    block = stack[0];
                    block_depth = 1;
                }
                break;
            }
        }
        if (line == caret_line && block < 0 && depth > 0) {
            block = stack[depth - 1];
            block_depth = depth;
        }
        offset = text[end] ? end + 1 : end;
    }
    if (block < 0 && form_line > 0) {
        block = form_line;
        block_depth = 1;
    }
    if (block < 0) {
        snprintf(g_props_title, sizeof(g_props_title), "Properties");
        return -1;
    }
    /* Pass 2: the block's own lines, not those of the blocks inside it. */
    offset = line_offset(text, block);
    depth = block_depth - 1;
    line = block - 1;
    while (text[offset] != '\0') {
        int end = offset, first, last;
        line++;
        while (text[end] != '\0' && text[end] != '\n') end++;
        trim_line(text, offset, end, &first, &last);
        if (line_is_word(text, first, last, "Begin")) {
            depth++;
            if (line == block) snprintf(title, sizeof(title), "Properties - %.*s", last - first - 6, text + first + 6);
        } else if (last - first == 3 && same_text_nocase(text + first, 3, "End")) {
            if (--depth < block_depth) break;
        } else if (depth == block_depth && first < last && text[first] != '\'' &&
                   parse_property(text, first, last, &prop)) {
            props_add(&prop);
        }
        offset = text[end] ? end + 1 : end;
    }
    snprintf(g_props_title, sizeof(g_props_title), "%s", title);
    return block;
}

/* Brings the grid up to date with the caret and the text, keeping the
 * selected row while the block is the same one. */
static void props_sync(int force)
{
    struct sxgui_widget *grid = &g_widgets[W_PROPS];
    if (grid->prop_editing) return;
    if (g_current < 0) {
        if (g_prop_count != 0 || g_props_file != -1) {
            g_prop_count = 0;
            g_props_file = -1;
            grid->property_count = 0;
            snprintf(g_props_title, sizeof(g_props_title), "Properties");
            repaint();
        }
        return;
    }
    const char *text = document(g_current);
    uint32_t hash = hash_text(text);
    int caret_line = line_of(text, editor(g_current)->caret);
    if (!force && g_props_file == g_current && g_props_hash == hash && g_props_caret_line == caret_line) return;
    int block = props_read(g_current);
    int same = g_props_file == g_current && g_props_block == block;
    g_props_file = g_current;
    g_props_block = block;
    g_props_hash = hash;
    g_props_caret_line = caret_line;
    grid->properties = g_prop_rows;
    grid->property_count = g_prop_count;
    if (!same || grid->value >= g_prop_count) {
        grid->value = 0;
        grid->prop_first = 0;
    }
    repaint();
}

/* A value the grid changed, written back into its line. */
static void props_changed(int row)
{
    char text[IDE_VALUE_MAX * 2 + 3];
    int out = 0;
    if (row < 0 || row >= g_prop_count || g_props_file < 0) return;
    struct ide_prop *prop = &g_props[row];
    if (prop->quoted) {
        text[out++] = '"';
        for (const char *p = prop->value; *p; p++) {
            if (*p == '"') text[out++] = '"';
            text[out++] = *p;
        }
        text[out++] = '"';
    } else {
        out = snprintf(text, sizeof(text), "%s", prop->value);
    }
    text[out] = '\0';
    (void)sxgui_textedit_replace(editor(g_props_file), prop->value_start, prop->value_end, text);
    props_sync(1);
}

/* ---- the project ----------------------------------------------------------------------- */

static void refresh_rows(void)
{
    for (int i = 0; i < g_file_count; i++) {
        snprintf(g_files[i].row, sizeof(g_files[i].row), "%s%s", g_files[i].name, editor(i)->modified ? " *" : "");
        g_file_rows[i] = g_files[i].row;
    }
    g_widgets[W_FILES].item_count = g_file_count;
}

static int any_modified(void)
{
    for (int i = 0; i < g_file_count; i++) {
        if (editor(i)->modified) return 1;
    }
    return 0;
}

/* The project's Name, from the project file as it is now. */
static void project_name(char *out, size_t size)
{
    struct ide_prop prop;
    snprintf(out, size, "Project");
    if (g_file_count == 0) return;
    const char *text = document(0);
    for (int offset = 0; text[offset] != '\0';) {
        int end = offset, first, last;
        while (text[end] != '\0' && text[end] != '\n') end++;
        trim_line(text, offset, end, &first, &last);
        if (parse_property(text, first, last, &prop) && strcmp(prop.key, "Name") == 0 && prop.value[0]) {
            snprintf(out, size, "%s", prop.value);
            return;
        }
        offset = text[end] ? end + 1 : end;
    }
}

static void update_title(void)
{
    char name[IDE_VALUE_MAX], title[IDE_VALUE_MAX + 32];
    if (!g_app_open) return;
    project_name(name, sizeof(name));
    snprintf(title, sizeof(title), g_file_count ? "%s - Savan BASIC" : "Savan BASIC", name);
    (void)sxnwm_set_title(&g_app.wm, title);
}

static void show_file(int file)
{
    if (file < 0 || file >= g_file_count) return;
    for (int i = 0; i < IDE_MAX_FILES; i++) {
        if (i == file) {
            editor(i)->flags |= SXGUI_FLAG_VISIBLE;
        } else {
            editor(i)->flags &= ~SXGUI_FLAG_VISIBLE;
        }
    }
    g_current = file;
    g_widgets[W_FILES].value = file;
    if (g_ui) sxgui_focus(g_ui, W_EDIT + file);
    props_sync(1);
    repaint();
}

static int find_file(const char *name)
{
    for (int i = 0; i < g_file_count; i++) {
        if (strcmp(g_files[i].name, name) == 0 || strcmp(base_name(g_files[i].path), base_name(name)) == 0) return i;
    }
    return -1;
}

static void close_project(void)
{
    for (int i = 0; i < IDE_MAX_FILES; i++) {
        sxgui_textedit_release(editor(i));
        editor(i)->flags &= ~SXGUI_FLAG_VISIBLE;
    }
    g_file_count = 0;
    g_asset_count = 0;
    g_current = -1;
    g_project_path[0] = g_project_dir[0] = '\0';
    g_mark_file = -1;
    g_widgets[W_FILES].item_count = 0;
    props_sync(1);
}

static int add_file(const char *name, int kind)
{
    if (g_file_count == IDE_MAX_FILES) {
        outputf("%s: a project here has at most %d files; %s is left out", base_name(g_project_path),
                IDE_MAX_FILES, name);
        return -1;
    }
    struct ide_file *file = &g_files[g_file_count];
    snprintf(file->name, sizeof(file->name), "%s", name);
    if (name[0] == '/') {
        snprintf(file->path, sizeof(file->path), "%s", name);
    } else {
        snprintf(file->path, sizeof(file->path), "%s/%s", g_project_dir, name);
    }
    file->kind = kind;
    char *text = NULL;
    size_t size = 0;
    long result = read_whole(file->path, &text, &size);
    if (result < 0) outputf("Cannot read %s (error %ld); it opens empty", file->path, -result);
    struct sxgui_widget *edit = editor(g_file_count);
    if (sxgui_textedit_document(edit, text) != 0) {
        free(text);
        outputf("Not enough memory for %s", name);
        return -1;
    }
    free(text);
    edit->edit_flags = SXGUI_TEXTEDIT_LINE_NUMBERS | SXGUI_TEXTEDIT_AUTO_INDENT;
    edit->edit_colorize = colorize;
    edit->edit_marks = marks;
    edit->edit_user = (void *)(intptr_t)g_file_count;
    return g_file_count++;
}

static int open_project(const char *path)
{
    char *text = NULL;
    size_t size = 0;
    struct ide_prop prop;
    if (path[0] != '/') {
        set_message("%s: the project's path has to be absolute", path);
        return -1;
    }
    long result = read_whole(path, &text, &size);
    if (result < 0) {
        set_message("Cannot open %s (error %ld)", path, -result);
        return -1;
    }
    close_project();
    snprintf(g_project_path, sizeof(g_project_path), "%s", path);
    directory_of(g_project_dir, sizeof(g_project_dir), path);
    (void)add_file(base_name(path), IDE_PROJECT);
    for (int offset = 0; text[offset] != '\0';) {
        int end = offset, first, last;
        while (text[end] != '\0' && text[end] != '\n') end++;
        trim_line(text, offset, end, &first, &last);
        if (parse_property(text, first, last, &prop) && prop.quoted) {
            if (strcmp(prop.key, "Form") == 0) (void)add_file(prop.value, IDE_FORM);
            if (strcmp(prop.key, "Module") == 0) (void)add_file(prop.value, IDE_MODULE);
            if (strcmp(prop.key, "Icon") == 0 && g_asset_count < IDE_MAX_ASSETS) {
                snprintf(g_assets[g_asset_count++], IDE_NAME_MAX, "%s", prop.value);
            }
        }
        offset = text[end] ? end + 1 : end;
    }
    free(text);
    refresh_rows();
    output_clear();
    outputf("Opened %s", path);
    set_message("");
    update_title();
    show_file(g_file_count > 1 ? 1 : 0);
    return 0;
}

static int save_file(int file)
{
    const char *text = document(file);
    long result = save_text(g_files[file].path, text, strlen(text));
    if (result < 0) {
        outputf("Cannot save %s (error %ld)", g_files[file].path, -result);
        return -1;
    }
    editor(file)->modified = 0;
    return 0;
}

static int save_all(void)
{
    int failed = 0;
    for (int i = 0; i < g_file_count; i++) {
        if (editor(i)->modified && save_file(i) != 0) failed = 1;
    }
    refresh_rows();
    update_title();
    repaint();
    return failed ? -1 : 0;
}

/* Writes every file of the project into the folder of `path` (made if
 * missing), the project file under the new name, and goes on from there. */
static int save_project_as(const char *path)
{
    char directory[IDE_PATH_MAX], target[IDE_PATH_MAX * 2];
    if (g_file_count == 0) return -1;
    directory_of(directory, sizeof(directory), path);
    long result = make_directories(directory);
    if (result < 0) {
        set_message("Cannot make the folder %s (error %ld)", directory, -result);
        return -1;
    }
    for (int i = 0; i < g_asset_count; i++) {
        char source[IDE_PATH_MAX * 2], *data = NULL;
        size_t size = 0;
        snprintf(source, sizeof(source), "%s/%s", g_project_dir, g_assets[i]);
        snprintf(target, sizeof(target), "%s/%s", directory, g_assets[i]);
        if (strcmp(source, target) == 0) continue;
        result = read_whole(source, &data, &size);
        if (result == 0) result = write_whole(target, data, size, SXN_CREATE_ALWAYS);
        free(data);
        if (result < 0) outputf("Cannot copy %s (error %ld)", g_assets[i], -result);
    }
    for (int i = 0; i < g_file_count; i++) {
        const char *name = i == 0 ? base_name(path) : g_files[i].name;
        snprintf(target, sizeof(target), "%s/%s", directory, name);
        if (strlen(target) >= sizeof(g_files[i].path)) {
            set_message("%s: the path is too long", target);
            return -1;
        }
        snprintf(g_files[i].path, sizeof(g_files[i].path), "%s", target);
        editor(i)->modified = 1;
    }
    snprintf(g_files[0].name, sizeof(g_files[0].name), "%s", base_name(path));
    snprintf(g_project_path, sizeof(g_project_path), "%s", path);
    snprintf(g_project_dir, sizeof(g_project_dir), "%s", directory);
    if (save_all() != 0) return -1;
    outputf("Saved the project as %s", path);
    return 0;
}

static int valid_project_name(const char *name)
{
    if (name[0] == '\0') return 0;
    for (; *name; name++) {
        if (!is_word_char(*name) && *name != ' ' && *name != '-') return 0;
    }
    return 1;
}

/* A new Windows project with one form: `path` names the project file, and
 * the project gets a folder of that name beside it. */
static int new_project(const char *path)
{
    char name[IDE_NAME_MAX], parent[IDE_PATH_MAX], directory[IDE_PATH_MAX + IDE_NAME_MAX];
    char project[IDE_PATH_MAX * 2], form[IDE_PATH_MAX * 2], text[1024];
    snprintf(name, sizeof(name), "%s", base_name(path));
    char *dot = strrchr(name, '.');
    if (dot != NULL && strcmp(dot, ".sbp") == 0) *dot = '\0';
    if (!valid_project_name(name)) {
        set_message("A project's name has letters, digits, spaces, - and _ only");
        return -1;
    }
    directory_of(parent, sizeof(parent), path);
    snprintf(directory, sizeof(directory), "%s%s%s", parent, strcmp(parent, "/") == 0 ? "" : "/", name);
    long result = make_directories(directory);
    if (result < 0) {
        set_message("Cannot make the folder %s (error %ld)", directory, -result);
        return -1;
    }
    snprintf(project, sizeof(project), "%s/%s.sbp", directory, name);
    snprintf(form, sizeof(form), "%s/MainForm.sbf", directory);
    int length = snprintf(text, sizeof(text),
                          "SxBASIC Project 1\n\nName = \"%s\"\nVersion = \"1.0\"\nKind = Windows\n"
                          "Startup = MainForm\nForm = \"MainForm.sbf\"\n",
                          name);
    result = write_whole(project, text, (size_t)length, SXN_CREATE_NEW);
    if (result == 0) {
        length = snprintf(text, sizeof(text),
                          "SxBASIC Form 1\n\nBegin Form MainForm\n    Text = \"%s\"\n    Width = 320\n"
                          "    Height = 120\n    Begin Button HelloButton\n        Text = \"Hello\"\n"
                          "        Left = 118\n        Top = 40\n        Width = 84\n        Height = 26\n"
                          "    End\nEnd\n\nPrivate Sub HelloButton_Click()\n    MsgBox \"Hello, world!\"\n"
                          "End Sub\n",
                          name);
        result = write_whole(form, text, (size_t)length, SXN_CREATE_NEW);
    }
    if (result < 0) {
        set_message(result == -SAVANXP_EEXIST ? "%s already has a project in it" : "Cannot write the project in %s",
                    directory);
        return -1;
    }
    return open_project(project);
}

/* ---- Run, End, Make SXE ----------------------------------------------------------------- */

/* "file:line:column: message" from the compiler, or "Runtime error n:
 * description (file, line n)" from the virtual machine. */
static int parse_location(const char *text, char *file, size_t size, int *line)
{
    if (strncmp(text, "Runtime error ", 14) == 0) {
        const char *open = strrchr(text, '(');
        const char *mark = open ? strstr(open, ", line ") : NULL;
        if (mark == NULL) return 0;
        snprintf(file, size, "%.*s", (int)(mark - open - 1), open + 1);
        *line = atoi(mark + 7);
        return *line > 0;
    }
    const char *colon = strchr(text, ':');
    if (colon == NULL || colon == text || colon[1] < '0' || colon[1] > '9') return 0;
    snprintf(file, size, "%.*s", (int)(colon - text), text);
    *line = atoi(colon + 1);
    return *line > 0;
}

/* Opens `name` at `line` and marks it; 0 when the project has no such file. */
static int goto_location(const char *name, int line)
{
    int file = find_file(name);
    if (file < 0) return 0;
    show_file(file);
    sxgui_textedit_set_caret(editor(file), line_offset(document(file), line));
    g_mark_file = file;
    g_mark_line = line;
    props_sync(1);
    repaint();
    return 1;
}

static void child_line(const char *line)
{
    char file[IDE_NAME_MAX];
    int number;
    output(line);
    if (!g_error_seen && parse_location(line, file, sizeof(file), &number)) {
        g_error_seen = 1;
        snprintf(g_error_file, sizeof(g_error_file), "%s", file);
        g_error_line = number;
    }
}

static void child_bytes(const char *bytes, long count)
{
    if (g_run_output_length + (size_t)count + 1u > g_run_output_capacity) {
        size_t capacity = (g_run_output_capacity ? g_run_output_capacity * 2u : 4096u) + (size_t)count;
        char *grown = realloc(g_run_output, capacity);
        if (grown != NULL) {
            g_run_output = grown;
            g_run_output_capacity = capacity;
        }
    }
    if (g_run_output_length + (size_t)count + 1u <= g_run_output_capacity) {
        memcpy(g_run_output + g_run_output_length, bytes, (size_t)count);
        g_run_output_length += (size_t)count;
        g_run_output[g_run_output_length] = '\0';
    }
    for (long i = 0; i < count; i++) {
        char c = bytes[i];
        if (c == '\r') continue;
        if (c == '\n' || g_partial_length == IDE_OUT_WIDTH - 1) {
            g_partial[g_partial_length] = '\0';
            child_line(g_partial);
            g_partial_length = 0;
            if (c == '\n') continue;
        }
        g_partial[g_partial_length++] = c;
    }
}

static void update_commands(void);

static void child_ended(int status)
{
    char name[IDE_VALUE_MAX];
    if (g_partial_length > 0) {
        g_partial[g_partial_length] = '\0';
        child_line(g_partial);
        g_partial_length = 0;
    }
    g_last_exit = status;
    project_name(name, sizeof(name));
    if (g_child_kind == CHILD_MAKE) {
        if (status == 0) {
            set_message("Made %s", g_made_path);
        } else {
            set_message("Make SXE failed");
        }
    } else {
        outputf("%s ended with exit code %d", name, status);
        set_message(status == 0 ? "%s ended" : "%s ended with an error", name);
    }
    g_child_kind = CHILD_NONE;
    g_child_pid = -1;
    if (g_error_seen && !goto_location(g_error_file, g_error_line)) {
        outputf("%s is not a file of this project", g_error_file);
    }
    update_commands();
    repaint();
}

/* Reads what the child printed; when its output ends, waits for it. */
static void poll_child(void)
{
    char buffer[512];
    for (;;) {
        long n = sxn_read(g_child_pipe, buffer, sizeof(buffer));
        if (n == -SAVANXP_EAGAIN) return;
        if (n <= 0) break;
        child_bytes(buffer, n);
    }
    (void)sxn_close(g_child_pipe);
    g_child_pipe = -1;
    int status = 1;
    if (sxn_waitpid(g_child_pid, &status) < 0) status = -1;
    child_ended(status);
}

static int start_child(int kind)
{
    char name[IDE_VALUE_MAX];
    int32_t pipe[2];
    char *argv[5];
    int argc = 0;
    if (g_child_kind != CHILD_NONE || g_file_count == 0) return -1;
    if (save_all() != 0) {
        set_message("The project could not be saved; nothing was started");
        return -1;
    }
    project_name(name, sizeof(name));
    argv[argc++] = (char *)IDE_RUNNER;
    if (kind == CHILD_MAKE) {
        snprintf(g_made_path, sizeof(g_made_path), "%s/%s.sxe", g_project_dir, name);
        argv[argc++] = (char *)"--make";
    }
    argv[argc++] = g_project_path;
    if (kind == CHILD_MAKE) argv[argc++] = g_made_path;
    argv[argc] = NULL;
    if (sxn_pipe_create(pipe) < 0) {
        set_message("Cannot start %s: no pipe for its output", IDE_RUNNER);
        return -1;
    }
    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, IDE_RUNNER, argv, argc);
    request.std_handles[1] = pipe[1];
    request.std_handles[2] = pipe[1];
    long pid = sxn_spawn_ex(&request);
    (void)sxn_close(pipe[1]);
    if (pid < 0) {
        (void)sxn_close(pipe[0]);
        set_message("Cannot start %s (error %ld)", IDE_RUNNER, -pid);
        return -1;
    }
    if (!g_headless) (void)sxn_set_nonblocking(pipe[0], 1);
    g_child_pipe = pipe[0];
    g_child_pid = pid;
    g_child_kind = kind;
    g_partial_length = 0;
    g_run_output_length = 0;
    if (g_run_output) g_run_output[0] = '\0';
    g_error_seen = 0;
    g_mark_file = -1;
    g_last_exit = -1;
    output_clear();
    if (kind == CHILD_MAKE) {
        outputf("Making %s", g_made_path);
        set_message("Making %s...", base_name(g_made_path));
    } else {
        outputf("Running %s", name);
        set_message("Running %s", name);
    }
    update_commands();
    return 0;
}

static void end_child(void)
{
    if (g_child_kind == CHILD_RUN && g_child_pid > 0) {
        (void)sxn_terminate(g_child_pid, 1);
        output("Ended by End");
    }
}

/* ---- commands --------------------------------------------------------------------------- */

static struct sxgui_menu_item g_file_items[] = {
    {"New Project...", CMD_NEW, 0},
    {"Open Project...\tCtrl+O", CMD_OPEN, 0},
    {0, 0, 0},
    {"Save\tCtrl+S", CMD_SAVE, 0},
    {"Save All", CMD_SAVE_ALL, 0},
    {"Save Project As...", CMD_SAVE_AS, 0},
    {0, 0, 0},
    {"Exit", CMD_EXIT, 0},
};
static struct sxgui_menu_item g_edit_items[] = {
    {"Undo\tCtrl+Z", CMD_UNDO, 0},
    {"Redo\tCtrl+Y", CMD_REDO, 0},
    {0, 0, 0},
    {"Cut\tCtrl+X", CMD_CUT, 0},
    {"Copy\tCtrl+C", CMD_COPY, 0},
    {"Paste\tCtrl+V", CMD_PASTE, 0},
    {0, 0, 0},
    {"Select All\tCtrl+A", CMD_SELECT_ALL, 0},
};
static struct sxgui_menu_item g_run_items[] = {
    {"Start\tF5", CMD_START, 0},
    {"End", CMD_END, SXGUI_MENU_DISABLED},
    {0, 0, 0},
    {"Make SXE...", CMD_MAKE, 0},
};
static const struct sxgui_menu g_menus[] = {
    {"File", g_file_items, (int)(sizeof(g_file_items) / sizeof(g_file_items[0]))},
    {"Edit", g_edit_items, (int)(sizeof(g_edit_items) / sizeof(g_edit_items[0]))},
    {"Run", g_run_items, (int)(sizeof(g_run_items) / sizeof(g_run_items[0]))},
};

static void enable(struct sxgui_menu_item *item, int widget, int on)
{
    if (on) {
        item->flags &= ~SXGUI_MENU_DISABLED;
        if (widget >= 0) g_widgets[widget].flags &= ~SXGUI_FLAG_DISABLED;
    } else {
        item->flags |= SXGUI_MENU_DISABLED;
        if (widget >= 0) g_widgets[widget].flags |= SXGUI_FLAG_DISABLED;
    }
}

static void update_commands(void)
{
    int open = g_file_count > 0, idle = g_child_kind == CHILD_NONE;
    enable(&g_file_items[3], W_TB_SAVE, open);
    enable(&g_file_items[4], -1, open);
    enable(&g_file_items[5], -1, open);
    enable(&g_run_items[0], W_TB_START, open && idle);
    enable(&g_run_items[1], W_TB_END, g_child_kind == CHILD_RUN);
    enable(&g_run_items[3], W_TB_MAKE, open && idle);
    repaint();
}

/* 1 when nothing unsaved would be lost, or `command` was chosen again
 * right after being warned. */
static int may_discard(int command)
{
    if (!any_modified() || g_discard_armed == command) return 1;
    g_discard_pending = command;
    set_message("The project has unsaved changes: save them, or choose this again to discard them");
    return 0;
}

static void on_new_chosen(struct sxngui_app *app, const char *path, void *user)
{
    (void)app;
    (void)user;
    if (path != NULL) (void)new_project(path);
    update_commands();
}

static void on_open_chosen(struct sxngui_app *app, const char *path, void *user)
{
    (void)app;
    (void)user;
    if (path != NULL) (void)open_project(path);
    update_commands();
}

static void on_save_as_chosen(struct sxngui_app *app, const char *path, void *user)
{
    (void)app;
    (void)user;
    if (path != NULL) (void)save_project_as(path);
    update_title();
}

static void ask_path(int mode, const char *title, const char *file_name,
                     void (*done)(struct sxngui_app *, const char *, void *))
{
    struct sxn_file_dialog_request request;
    if (!g_app_open) return;
    memset(&request, 0, sizeof(request));
    request.mode = mode;
    request.title = title;
    request.initial_directory = g_project_dir[0] ? g_project_dir : NULL;
    request.file_name = file_name;
    request.filter_description = "Savan BASIC projects (*.sbp)";
    request.filter_patterns = "*.sbp";
    request.default_extension = "sbp";
    request.on_done = done;
    if (sxn_file_dialog_begin(&g_app, &request) != 0) set_message("Not enough memory for the dialog");
}

static void command(int id)
{
    struct sxgui_widget *edit = g_current >= 0 ? editor(g_current) : NULL;
    g_discard_pending = 0;
    switch (id) {
    case CMD_NEW:
        if (may_discard(id)) ask_path(SXN_FILE_DIALOG_SAVE, "New Project", "Project1.sbp", on_new_chosen);
        break;
    case CMD_OPEN:
        if (may_discard(id)) ask_path(SXN_FILE_DIALOG_OPEN, "Open Project", NULL, on_open_chosen);
        break;
    case CMD_SAVE:
        if (g_current >= 0 && save_file(g_current) == 0) set_message("Saved %s", g_files[g_current].name);
        refresh_rows();
        break;
    case CMD_SAVE_ALL:
        if (g_file_count > 0 && save_all() == 0) set_message("Saved the project");
        break;
    case CMD_SAVE_AS:
        if (g_file_count > 0) {
            ask_path(SXN_FILE_DIALOG_SAVE, "Save Project As", base_name(g_project_path), on_save_as_chosen);
        }
        break;
    case CMD_EXIT:
        if (may_discard(id) && g_app_open) sxngui_app_quit(&g_app, 0);
        break;
    case CMD_UNDO:
        if (edit) (void)sxgui_textedit_undo(edit);
        break;
    case CMD_REDO:
        if (edit) (void)sxgui_textedit_redo(edit);
        break;
    case CMD_CUT:
        if (edit) (void)sxgui_textedit_cut(edit);
        break;
    case CMD_COPY:
        if (edit) (void)sxgui_textedit_copy(edit);
        break;
    case CMD_PASTE:
        if (edit) (void)sxgui_textedit_paste(edit);
        break;
    case CMD_SELECT_ALL:
        if (edit) sxgui_textedit_select_all(edit);
        break;
    case CMD_START:
        (void)start_child(CHILD_RUN);
        break;
    case CMD_END:
        end_child();
        break;
    case CMD_MAKE:
        (void)start_child(CHILD_MAKE);
        break;
    default:
        break;
    }
    g_discard_armed = g_discard_pending;
    repaint();
}

static void on_menu_command(int id, void *user)
{
    (void)user;
    command(id);
}

/* ---- the window ------------------------------------------------------------------------- */

static void on_widget(struct sxgui_widget *widget, void *user)
{
    (void)user;
    int index = (int)(widget - g_widgets);
    switch (index) {
    case W_TB_OPEN:
        command(CMD_OPEN);
        break;
    case W_TB_SAVE:
        command(CMD_SAVE_ALL);
        break;
    case W_TB_START:
        command(CMD_START);
        break;
    case W_TB_END:
        command(CMD_END);
        break;
    case W_TB_MAKE:
        command(CMD_MAKE);
        break;
    case W_FILES:
        if (widget->action == SXGUI_ACTION_CHANGE && widget->value >= 0 && widget->value != g_current) {
            show_file(widget->value);
            sxgui_focus(g_ui, W_FILES);
        } else if (widget->action == SXGUI_ACTION_ACTIVATE) {
            show_file(widget->value);
        }
        break;
    case W_PROPS:
        if (widget->action == SXGUI_ACTION_CHANGE) props_changed(widget->value);
        break;
    case W_OUTPUT:
        if (widget->action == SXGUI_ACTION_ACTIVATE && widget->value >= 0 && widget->value < g_out_count) {
            char file[IDE_NAME_MAX];
            int line;
            if (parse_location(g_out[widget->value], file, sizeof(file), &line)) (void)goto_location(file, line);
        }
        break;
    default:
        break;
    }
    repaint();
}

/* What changes without an event of its own: the status bar, the explorer's
 * unsaved marks, the grid following the caret. */
static void sync_views(void)
{
    char status[sizeof(g_status)];
    int marks_changed = 0;
    for (int i = 0; i < g_file_count; i++) {
        size_t length = strlen(g_files[i].row);
        int shown = length > 2 && strcmp(g_files[i].row + length - 2, " *") == 0;
        if (shown != (editor(i)->modified != 0)) marks_changed = 1;
    }
    if (marks_changed) {
        refresh_rows();
        repaint();
    }
    props_sync(0);
    if (g_current >= 0) {
        const char *text = document(g_current);
        int caret = editor(g_current)->caret;
        int line = line_of(text, caret);
        snprintf(status, sizeof(status), "Ln %d, Col %d    %s", line, caret - line_offset(text, line) + 1, g_message);
    } else {
        snprintf(status, sizeof(status), "%s", g_message[0] ? g_message : "Open a project, or start a new one");
    }
    if (strcmp(status, g_status) != 0) {
        snprintf(g_status, sizeof(g_status), "%s", status);
        repaint();
    }
}

static void on_tick(struct sxngui_app *app)
{
    (void)app;
    if (g_child_kind != CHILD_NONE) poll_child();
    sync_views();
}

static int on_key(struct sxngui_app *app, const struct savanxp_input_event *event)
{
    (void)app;
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || sxgui_dialog_active(g_ui)) return 0;
    int ctrl = (event->modifiers & SAVANXP_KEY_MOD_CTRL) != 0;
    if (event->key == SAVANXP_KEY_F5) {
        command(CMD_START);
        return 1;
    }
    if (ctrl && (event->ascii == 's' || event->ascii == 'S' || event->ascii == 19)) {
        command(CMD_SAVE);
        return 1;
    }
    if (ctrl && (event->ascii == 'o' || event->ascii == 'O' || event->ascii == 15)) {
        command(CMD_OPEN);
        return 1;
    }
    return 0;
}

static void on_close(struct sxngui_app *app)
{
    g_discard_pending = 0;
    if (may_discard(CMD_EXIT)) {
        sxngui_app_quit(app, 0);
    } else {
        g_discard_armed = g_discard_pending;
        (void)sxngui_app_decline_close(app);
    }
}

static void layout_size(int width, int height)
{
    int top = g_app_open || g_headless ? sxgui_menubar_height() + 2 : 2;
    int bar = sxgui_toolbar_height();
    int button = sxgui_toolbar_button_width("Make SXE");
    int x = SXGUI_MARGIN;
    int status_y = height - SXGUI_STATUS_HEIGHT;
    int body = top + bar + SXGUI_GAP;
    int left = IDE_LEFT_WIDTH;
    int right_x = x + left + SXGUI_GAP;
    int right_width = width - right_x - SXGUI_MARGIN;
    int column_height = status_y - SXGUI_GAP - body;
    int files_height = (column_height - 2 * IDE_LABEL_HEIGHT) / 3;
    int output_y = status_y - SXGUI_GAP - IDE_OUTPUT_HEIGHT;
    static const int toolbar[] = {W_TB_OPEN, W_TB_SAVE, W_TB_SEP1, W_TB_START, W_TB_END, W_TB_SEP2, W_TB_MAKE};

    for (size_t i = 0; i < sizeof(toolbar) / sizeof(toolbar[0]); i++) {
        int separator = toolbar[i] == W_TB_SEP1 || toolbar[i] == W_TB_SEP2;
        g_widgets[toolbar[i]].rect = sx_rect_make(x, top, separator ? 2 : button, bar);
        x += (separator ? 2 : button) + 2;
    }
    x = SXGUI_MARGIN;
    g_widgets[W_FILES_LABEL].rect = sx_rect_make(x, body, left, IDE_LABEL_HEIGHT);
    g_widgets[W_FILES].rect = sx_rect_make(x, body + IDE_LABEL_HEIGHT, left, files_height);
    int props_y = body + IDE_LABEL_HEIGHT + files_height + SXGUI_GAP;
    g_widgets[W_PROPS_LABEL].rect = sx_rect_make(x, props_y, left, IDE_LABEL_HEIGHT);
    g_widgets[W_PROPS].rect = sx_rect_make(x, props_y + IDE_LABEL_HEIGHT, left,
                                           status_y - SXGUI_GAP - props_y - IDE_LABEL_HEIGHT);
    for (int i = 0; i < IDE_MAX_FILES; i++) {
        editor(i)->rect = sx_rect_make(right_x, body, right_width, output_y - IDE_LABEL_HEIGHT - SXGUI_GAP - body);
    }
    g_widgets[W_OUTPUT_LABEL].rect = sx_rect_make(right_x, output_y - IDE_LABEL_HEIGHT, right_width, IDE_LABEL_HEIGHT);
    g_widgets[W_OUTPUT].rect = sx_rect_make(right_x, output_y, right_width, IDE_OUTPUT_HEIGHT);
    g_widgets[W_STATUS].rect = sx_rect_make(0, status_y, width, SXGUI_STATUS_HEIGHT);
}

static void layout(struct sxngui_app *app)
{
    layout_size((int)app->info.width, (int)app->info.height);
    repaint();
}

static struct sx_bitmap g_icons[5];
static const struct sx_bitmap *g_icon_refs[5];

static void build_widgets(void)
{
    static const uint32_t *const pixels[] = {ide_glyph_document_open, ide_glyph_document_save,
                                             ide_glyph_media_playback_start, ide_glyph_media_playback_stop,
                                             ide_glyph_application_x_executable};
    static const char *const labels[] = {"Open", "Save", "Start", "End", "Make SXE"};
    static const int slots[] = {W_TB_OPEN, W_TB_SAVE, W_TB_START, W_TB_END, W_TB_MAKE};
    struct savanxp_fb_info info = {0};
    struct sx_rect none = sx_rect_make(0, 0, 0, 0);
    info.width = info.height = 20;
    info.pitch = 80;
    info.bpp = 32;
    info.buffer_size = 1600;
    for (int i = 0; i < 5; i++) {
        sx_bitmap_wrap(&g_icons[i], (uint32_t *)pixels[i], &info, SX_PIXEL_FORMAT_BGRA8888);
        g_icon_refs[i] = &g_icons[i];
        g_widgets[slots[i]] = sxgui_toolbar_button(none, labels[i], &g_icon_refs[i], on_widget, 0);
    }
    g_widgets[W_TB_SEP1] = sxgui_separator(none);
    g_widgets[W_TB_SEP2] = sxgui_separator(none);
    g_widgets[W_FILES_LABEL] = sxgui_label(none, "Project");
    g_widgets[W_FILES] = sxgui_listbox(none, g_file_rows, 0);
    g_widgets[W_FILES].on_action = on_widget;
    g_widgets[W_PROPS_LABEL] = sxgui_label(none, g_props_title);
    g_widgets[W_PROPS] = sxgui_propgrid(none, g_prop_rows, 0);
    g_widgets[W_PROPS].on_action = on_widget;
    g_widgets[W_OUTPUT_LABEL] = sxgui_label(none, "Output");
    g_widgets[W_OUTPUT] = sxgui_listbox(none, g_out_rows, 0);
    g_widgets[W_OUTPUT].flags |= SXGUI_FLAG_EMPTY_SELECTION;
    g_widgets[W_OUTPUT].value = -1;
    g_widgets[W_OUTPUT].on_action = on_widget;
    g_widgets[W_STATUS] = sxgui_label(none, g_status);
    g_widgets[W_STATUS].flags |= SXGUI_FLAG_SUNKEN;
    for (int i = 0; i < IDE_MAX_FILES; i++) {
        g_widgets[W_EDIT + i] = sxgui_textedit(none, NULL, 0);
        g_widgets[W_EDIT + i].flags &= ~SXGUI_FLAG_VISIBLE;
    }
}

static int run_window(const char *project)
{
    build_widgets();
    if (sxngui_app_init(&g_app, &sun_resources, g_widgets, W_COUNT) < 0) {
        (void)sxn_log("sxbide: no window from windowd");
        return 1;
    }
    g_app_open = 1;
    g_ui = &g_app.ui;
    g_menubar.menus = g_menus;
    g_menubar.menu_count = (int)(sizeof(g_menus) / sizeof(g_menus[0]));
    g_menubar.on_command = on_menu_command;
    sxgui_set_menubar(g_ui, &g_menubar);
    g_app.on_resize = layout;
    g_app.on_key = on_key;
    g_app.on_tick = on_tick;
    g_app.on_close = on_close;
    g_app.tick_interval_ms = 100;
    (void)sxngui_app_set_content_size(&g_app, IDE_DEFAULT_WIDTH, IDE_DEFAULT_HEIGHT);
    layout(&g_app);
    if (project != NULL) (void)open_project(project);
    update_commands();
    sync_views();
    return sxngui_app_run(&g_app);
}

/* ---- the selftest script ------------------------------------------------------------------ */

static int g_failures;

static void check_failed(int line, const char *what, const char *detail)
{
    char text[700];
    snprintf(text, sizeof(text), "SXBIDE SELFTEST: line %d: %s%s%s\n", line, what, detail ? ": " : "",
             detail ? detail : "");
    put(2, text);
    g_failures++;
}

static void wait_child(void)
{
    while (g_child_kind != CHILD_NONE) poll_child();
}

static void line_text(int file, int line, char *out, size_t size)
{
    const char *text = document(file);
    int start = line_offset(text, line), end = start;
    while (text[end] != '\0' && text[end] != '\n') end++;
    snprintf(out, size, "%.*s", end - start, text + start);
}

static void run_command(int line, char *command_text)
{
    char *argument = strchr(command_text, ' ');
    char detail[600];
    if (argument) *argument++ = '\0';
    else argument = command_text + strlen(command_text);
    const char *name = command_text;

    if (strcmp(name, "open") == 0) {
        if (open_project(argument) != 0) check_failed(line, "cannot open", g_message);
    } else if (strcmp(name, "save-as") == 0) {
        if (save_project_as(argument) != 0) check_failed(line, "save as", g_message);
    } else if (strcmp(name, "save") == 0) {
        if (save_all() != 0) check_failed(line, "save", NULL);
    } else if (strcmp(name, "select") == 0) {
        int file = find_file(argument);
        if (file < 0) check_failed(line, "no such file", argument);
        else show_file(file);
    } else if (strcmp(name, "goto") == 0) {
        if (g_current >= 0) sxgui_textedit_set_caret(editor(g_current), line_offset(document(g_current), atoi(argument)));
    } else if (strcmp(name, "insert") == 0) {
        for (char *p = argument; *p; p++) if (*p == '|') *p = '\n';
        struct sxgui_widget *edit = g_current >= 0 ? editor(g_current) : NULL;
        if (edit == NULL || !sxgui_textedit_replace(edit, edit->caret, edit->caret, argument)) {
            check_failed(line, "cannot insert", NULL);
        }
    } else if (strcmp(name, "undo") == 0) {
        if (g_current < 0 || !sxgui_textedit_undo(editor(g_current))) check_failed(line, "nothing to undo", NULL);
    } else if (strcmp(name, "run") == 0 || strcmp(name, "make") == 0) {
        if (start_child(name[0] == 'r' ? CHILD_RUN : CHILD_MAKE) != 0) check_failed(line, "cannot start", g_message);
        wait_child();
    } else if (strcmp(name, "set-property") == 0) {
        char *value = strchr(argument, ' ');
        int row = -1;
        if (value) *value++ = '\0';
        props_sync(1);
        for (int i = 0; i < g_prop_count; i++) {
            if (strcmp(g_props[i].key, argument) == 0) row = i;
        }
        if (row < 0 || value == NULL) {
            check_failed(line, "no such property", argument);
        } else {
            snprintf(g_props[row].value, sizeof(g_props[row].value), "%s", value);
            g_widgets[W_PROPS].value = row;
            props_changed(row);
        }
    } else if (strcmp(name, "expect-files") == 0) {
        char got[IDE_MAX_FILES * IDE_NAME_MAX] = "";
        for (int i = 0; i < g_file_count; i++) {
            size_t used = strlen(got);
            snprintf(got + used, sizeof(got) - used, "%s%s", i ? " " : "", g_files[i].name);
        }
        if (strcmp(got, argument) != 0) check_failed(line, "files", got);
    } else if (strcmp(name, "expect-current") == 0) {
        if (g_current < 0 || strcmp(g_files[g_current].name, argument) != 0) {
            check_failed(line, "current file", g_current >= 0 ? g_files[g_current].name : "(none)");
        }
    } else if (strcmp(name, "expect-property") == 0) {
        char *value = strchr(argument, ' ');
        int found = 0;
        if (value) *value++ = '\0';
        props_sync(0);
        for (int i = 0; i < g_prop_count; i++) {
            if (strcmp(g_props[i].key, argument) == 0) {
                found = 1;
                if (value == NULL || strcmp(g_props[i].value, value) != 0) check_failed(line, argument, g_props[i].value);
            }
        }
        if (!found) check_failed(line, "no such property", argument);
    } else if (strcmp(name, "expect-line") == 0) {
        char *text = strchr(argument, ' ');
        char got[512];
        if (text) *text++ = '\0';
        line_text(g_current, atoi(argument), got, sizeof(got));
        if (text == NULL || strcmp(got, text) != 0) check_failed(line, "line", got);
    } else if (strcmp(name, "expect-exit") == 0) {
        if (g_last_exit != atoi(argument)) {
            snprintf(detail, sizeof(detail), "%d", g_last_exit);
            check_failed(line, "exit code", detail);
        }
    } else if (strcmp(name, "expect-output-file") == 0) {
        char *expected = NULL;
        size_t size = 0;
        if (read_whole(argument, &expected, &size) != 0) {
            check_failed(line, "cannot read", argument);
        } else if (strcmp(expected, g_run_output ? g_run_output : "") != 0) {
            check_failed(line, "the program's output differs; it printed", g_run_output ? g_run_output : "");
        }
        free(expected);
    } else if (strcmp(name, "expect-error") == 0) {
        char *number = strchr(argument, ' ');
        if (number) *number++ = '\0';
        int wanted = number ? atoi(number) : 0;
        int at = g_current >= 0 ? line_of(document(g_current), editor(g_current)->caret) : 0;
        if (!g_error_seen || g_current < 0 || strcmp(g_files[g_current].name, argument) != 0 || at != wanted ||
            g_mark_file != g_current || g_mark_line != wanted) {
            snprintf(detail, sizeof(detail), "%s line %d, the caret at %s line %d, marked line %d",
                     g_error_seen ? g_error_file : "no error", g_error_line,
                     g_current >= 0 ? g_files[g_current].name : "(none)", at, g_mark_line);
            check_failed(line, "error location", detail);
        }
    } else if (strcmp(name, "expect-exists") == 0) {
        struct sxn_file_info info;
        if (sxn_query_path(argument, &info) < 0 || info.type != SXN_FILE_TYPE_FILE) check_failed(line, "no file", argument);
    } else {
        check_failed(line, "unknown command", name);
    }
    sync_views();
}

static int run_selftest(const char *script)
{
    static struct savanxp_fb_info info;
    char *text = NULL;
    size_t size = 0;
    g_headless = 1;
    info.width = IDE_DEFAULT_WIDTH;
    info.height = IDE_DEFAULT_HEIGHT;
    info.pitch = IDE_DEFAULT_WIDTH * 4u;
    info.bpp = 32;
    info.buffer_size = info.pitch * info.height;
    uint32_t *pixels = calloc((size_t)IDE_DEFAULT_WIDTH * IDE_DEFAULT_HEIGHT, sizeof(*pixels));
    if (pixels == NULL) return 1;
    build_widgets();
    sxgui_set_host(sxngui_host());
    sxgui_context_init(&g_headless_ui, pixels, &info, g_widgets, W_COUNT);
    g_ui = &g_headless_ui;
    layout_size(IDE_DEFAULT_WIDTH, IDE_DEFAULT_HEIGHT);
    if (read_whole(script, &text, &size) != 0) {
        check_failed(0, "cannot read the script", script);
    } else {
        int line = 0;
        for (char *p = text; *p;) {
            char *end = strchr(p, '\n');
            if (end) *end = '\0';
            line++;
            size_t n = strlen(p);
            if (n && p[n - 1] == '\r') p[--n] = '\0';
            if (n && p[0] != '#') run_command(line, p);
            if (!end) break;
            p = end + 1;
        }
    }
    /* The whole window, once, with what the script left in it. */
    sxgui_paint_content(g_ui);
    free(text);
    free(pixels);
    put(1, g_failures ? "SXBIDE SELFTEST: FAIL\n" : "SXBIDE SELFTEST: all checks passed\n");
    return g_failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--selftest") == 0) sxn_exit(run_selftest(argv[2]));
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        put(2, "usage: sxbide [project.sbp]\n       sxbide --selftest <script>\n");
        sxn_exit(2);
    }
    sxn_exit(run_window(argc == 2 ? argv[1] : NULL));
    return 0;
}
