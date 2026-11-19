/* sxbide: Savan BASIC, the development environment of SxBASIC
 * (docs/userland/SXBASIC_IDE.md).
 *
 *   sxbide [project.sbp]
 *   sxbide --selftest <script>
 *
 * One window: a menu bar and a toolbar, the project's files on the left with
 * the property grid under them, the code editor, and the output of the last
 * Run or Make SXE under it. A form opens in the designer: a toolbox, the
 * form drawn with its controls, which are placed by drawing a rectangle,
 * moved, resized and deleted there; F7 shows its text instead. Either way the
 * text is the form: the property grid shows the `Key = value` lines of the
 * block the caret is in (the project's, or a form's or control's
 * Begin ... End), the designer selects a control by putting the caret on its
 * block, and both write what changes back into the text, as one step of undo.
 *
 * Run and Make SXE start /system/sxbasic.sxe as a child process with its
 * output on a pipe, so a program that loops or crashes does not take the
 * environment with it. A compile or runtime error the runner prints names a
 * file and a line; the environment opens that file at that line. Start runs
 * the program under the debugger (sxbasic --debug): breakpoints from the
 * gutter or F9, stepping, a runtime error stopping where it happened, and the
 * procedure's variables and a watch list while it is stopped.
 *
 * --selftest drives the same code with no window, from a script of what a
 * user does (sxbide-smoke). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/sxbasic.h"
#include "core/sxb_internal.h"
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
    W_TB_BREAK,
    W_TB_END,
    W_TB_SEP2,
    W_TB_MAKE,
    W_FILES_LABEL,
    W_FILES,
    W_PROPS_LABEL,
    W_PROPS,
    W_OUTPUT_LABEL,
    W_OUTPUT,
    W_LOCALS_LABEL,
    W_LOCALS,
    W_WATCH_LABEL,
    W_WATCH,
    W_STATUS,
    W_TOOLBOX,
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
    CMD_VIEW_CODE,
    CMD_VIEW_OBJECT,
    CMD_BREAK,
    CMD_STEP_INTO,
    CMD_STEP_OVER,
    CMD_STEP_OUT,
    CMD_TOGGLE_BREAKPOINT,
    CMD_ADD_WATCH,
    CMD_DELETE_WATCH,
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

/* A form shows its designer (1) or its text (0). */
static int g_object_view[IDE_MAX_FILES];
/* `--selftest <script>` for the runner, from the IDE's own selftest. */
static const char *g_child_script;

/* The debugger (the section below Run and Make SXE). */
#define IDE_MAX_BREAKS 32
#define IDE_MAX_WATCHES 16
#define IDE_MAX_LOCALS 64
static int g_breaks[IDE_MAX_FILES][IDE_MAX_BREAKS];
static int g_break_count[IDE_MAX_FILES];
static char g_watch_names[IDE_MAX_WATCHES][64];
static char g_watch_text[IDE_MAX_WATCHES][IDE_OUT_WIDTH];
static const char *g_watch_rows[IDE_MAX_WATCHES];
static int g_watch_count;
static char g_locals[IDE_MAX_LOCALS][IDE_OUT_WIDTH];
static const char *g_locals_rows[IDE_MAX_LOCALS];
static int g_locals_count;
static long g_debug_commands = -1; /* the child's handle 3: what the IDE says */
static long g_debug_reports = -1;  /* the child's handle 4: what it reports */
static char g_report_buffer[4096];
static size_t g_report_length;
static const char *g_start_command = "go";
static int g_paused;
static char g_stop_reason[16];
static char g_stop_file[IDE_NAME_MAX];
static int g_stop_line;
static char g_stop_proc[64];
static int g_stop_error;
static char g_stop_description[128];

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

static int break_index(int file, int line)
{
    for (int i = 0; file >= 0 && file < IDE_MAX_FILES && i < g_break_count[file]; i++) {
        if (g_breaks[file][i] == line) return i;
    }
    return -1;
}

static unsigned marks(int line_number, void *user)
{
    int file = (int)(intptr_t)user;
    return (file == g_mark_file && line_number + 1 == g_mark_line ? SXGUI_TEXTEDIT_MARK_CURRENT : 0u) |
           (break_index(file, line_number + 1) >= 0 ? SXGUI_TEXTEDIT_MARK_BREAKPOINT : 0u);
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

static void update_view(void);

static void show_file(int file)
{
    if (file < 0 || file >= g_file_count) return;
    g_current = file;
    g_widgets[W_FILES].value = file;
    update_view();
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
    memset(g_break_count, 0, sizeof(g_break_count));
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
    g_object_view[g_file_count] = kind == IDE_FORM;
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
    g_object_view[file] = 0;
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
static void debug_closed(void);

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
    debug_closed();
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
    char *argv[6];
    int argc = 0;
    int32_t commands[2] = {-1, -1}, reports[2] = {-1, -1};
    if (g_child_kind != CHILD_NONE || g_file_count == 0) return -1;
    if (save_all() != 0) {
        set_message("The project could not be saved; nothing was started");
        return -1;
    }
    project_name(name, sizeof(name));
    argv[argc++] = (char *)IDE_RUNNER;
    int debugging = kind == CHILD_RUN && g_child_script == NULL;
    if (kind == CHILD_RUN && g_child_script != NULL) {
        argv[argc++] = (char *)"--selftest";
        argv[argc++] = (char *)g_child_script;
    }
    if (debugging) argv[argc++] = (char *)"--debug";
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
    if (debugging && (sxn_pipe_create(commands) < 0 || sxn_pipe_create(reports) < 0)) {
        for (int i = 0; i < 2; i++) {
            if (commands[i] >= 0) (void)sxn_close(commands[i]);
            if (reports[i] >= 0) (void)sxn_close(reports[i]);
        }
        (void)sxn_close(pipe[0]);
        (void)sxn_close(pipe[1]);
        set_message("Cannot start %s: no pipe for the debugger", IDE_RUNNER);
        return -1;
    }
    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, IDE_RUNNER, argv, argc);
    request.std_handles[1] = pipe[1];
    request.std_handles[2] = pipe[1];
    if (debugging) {
        request.handles[0] = commands[0];
        request.handles[1] = reports[1];
        request.handle_count = 2;
    }
    long pid = sxn_spawn_ex(&request);
    (void)sxn_close(pipe[1]);
    if (debugging) {
        (void)sxn_close(commands[0]);
        (void)sxn_close(reports[1]);
    }
    if (pid < 0) {
        (void)sxn_close(pipe[0]);
        if (debugging) {
            (void)sxn_close(commands[1]);
            (void)sxn_close(reports[0]);
        }
        set_message("Cannot start %s (error %ld)", IDE_RUNNER, -pid);
        return -1;
    }
    if (!g_headless) (void)sxn_set_nonblocking(pipe[0], 1);
    if (debugging) {
        g_debug_commands = commands[1];
        g_debug_reports = reports[0];
        g_report_length = 0;
        if (!g_headless) (void)sxn_set_nonblocking(reports[0], 1);
    }
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
    if (debugging) {
        /* What the debugger has to know before the first statement runs. */
        char line[160];
        for (int f = 0; f < g_file_count; f++) {
            for (int i = 0; i < g_break_count[f]; i++) {
                int length = snprintf(line, sizeof(line), "break %d %s\n", g_breaks[f][i], g_files[f].name);
                (void)sxn_write(g_debug_commands, line, (unsigned long)length);
            }
        }
        for (int i = 0; i < g_watch_count; i++) {
            int length = snprintf(line, sizeof(line), "watch %s\n", g_watch_names[i]);
            (void)sxn_write(g_debug_commands, line, (unsigned long)length);
        }
        int length = snprintf(line, sizeof(line), "%s\n", g_start_command);
        (void)sxn_write(g_debug_commands, line, (unsigned long)length);
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

/* ---- the debugger ------------------------------------------------------------------------
 *
 * Start runs the program under `sxbasic --debug` with two pipes more: what
 * the IDE says goes in on the child's handle 3, what it reports comes back on
 * its handle 4 (the protocol is described in sxbasic.c). Breakpoints are the
 * IDE's, by file and line, sent before the first statement and whenever they
 * change. At a stop the child reports where it is, the procedure's variables
 * and the watched names, and waits for go, step, over or out. */

static int designer_active(void);

static void debug_send(const char *format, ...)
{
    char line[300];
    va_list args;
    if (g_debug_commands < 0) return;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (length < 0) return;
    if (length > (int)sizeof(line) - 2) length = (int)sizeof(line) - 2;
    line[length++] = '\n';
    (void)sxn_write(g_debug_commands, line, (unsigned long)length);
}

static void toggle_breakpoint(int file, int line)
{
    if (file < 0 || file >= g_file_count || g_files[file].kind == IDE_PROJECT || line < 1) return;
    int at = break_index(file, line);
    if (at >= 0) {
        memmove(&g_breaks[file][at], &g_breaks[file][at + 1], sizeof(int) * (size_t)(--g_break_count[file] - at));
        debug_send("clear %d %s", line, g_files[file].name);
    } else if (g_break_count[file] < IDE_MAX_BREAKS) {
        g_breaks[file][g_break_count[file]++] = line;
        debug_send("break %d %s", line, g_files[file].name);
    }
    repaint();
}

/* The Watch pane without values: what it shows while the program runs. */
static void watch_reset(void)
{
    for (int i = 0; i < g_watch_count; i++) {
        snprintf(g_watch_text[i], IDE_OUT_WIDTH, "%s", g_watch_names[i]);
        g_watch_rows[i] = g_watch_text[i];
    }
    g_widgets[W_WATCH].item_count = g_watch_count;
    repaint();
}

static void add_watch(const char *name)
{
    for (int i = 0; i < g_watch_count; i++) {
        if (same_text_nocase(g_watch_names[i], (int)strlen(g_watch_names[i]), name)) return;
    }
    if (g_watch_count == IDE_MAX_WATCHES || name[0] == '\0') return;
    snprintf(g_watch_names[g_watch_count], sizeof(g_watch_names[0]), "%s", name);
    snprintf(g_watch_text[g_watch_count], IDE_OUT_WIDTH, "%s", name);
    g_watch_rows[g_watch_count] = g_watch_text[g_watch_count];
    g_watch_count++;
    g_widgets[W_WATCH].item_count = g_watch_count;
    debug_send("watch %s", name);
    repaint();
}

static void delete_watch(int index)
{
    if (index < 0 || index >= g_watch_count) return;
    debug_send("unwatch %s", g_watch_names[index]);
    g_watch_count--;
    memmove(g_watch_names[index], g_watch_names[index + 1], sizeof(g_watch_names[0]) * (size_t)(g_watch_count - index));
    memmove(g_watch_text[index], g_watch_text[index + 1], sizeof(g_watch_text[0]) * (size_t)(g_watch_count - index));
    for (int i = 0; i < g_watch_count; i++) g_watch_rows[i] = g_watch_text[i];
    g_widgets[W_WATCH].item_count = g_watch_count;
    if (g_widgets[W_WATCH].value >= g_watch_count) g_widgets[W_WATCH].value = g_watch_count - 1;
    repaint();
}

/* The name at the caret (or the selection) of the code shown, as a watch. */
static void watch_at_caret(void)
{
    char name[64];
    if (g_current < 0 || designer_active()) return;
    struct sxgui_widget *edit = editor(g_current);
    const char *text = document(g_current);
    int start = edit->caret, end = edit->caret;
    if (sxgui_textedit_has_selection(edit)) {
        start = edit->sel_anchor < edit->caret ? edit->sel_anchor : edit->caret;
        end = edit->sel_anchor < edit->caret ? edit->caret : edit->sel_anchor;
    } else {
        while (start > 0 && is_word_char(text[start - 1])) start--;
        while (is_word_char(text[end])) end++;
    }
    while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
    if (end <= start || end - start >= (int)sizeof(name) || memchr(text + start, '\n', (size_t)(end - start))) return;
    snprintf(name, sizeof(name), "%.*s", end - start, text + start);
    add_watch(name);
}

/* One line of a report; 1 for the `end` that closes it. */
static int report_line(char *line)
{
    char *field[5];
    int count = 0;
    for (char *p = line; count < 5;) {
        field[count++] = p;
        p = strchr(p, '\t');
        if (p == NULL) break;
        *p++ = '\0';
    }
    if (strcmp(field[0], "end") == 0) return 1;
    if (strcmp(field[0], "stop") == 0 && count == 5) {
        snprintf(g_stop_reason, sizeof(g_stop_reason), "%s", field[1]);
        snprintf(g_stop_file, sizeof(g_stop_file), "%s", field[2]);
        g_stop_line = atoi(field[3]);
        snprintf(g_stop_proc, sizeof(g_stop_proc), "%s", field[4]);
        g_stop_error = 0;
        g_locals_count = 0;
    } else if (strcmp(field[0], "error") == 0 && count >= 3) {
        g_stop_error = atoi(field[1]);
        snprintf(g_stop_description, sizeof(g_stop_description), "%s", field[2]);
    } else if (strcmp(field[0], "local") == 0 && count >= 3 && g_locals_count < IDE_MAX_LOCALS) {
        snprintf(g_locals[g_locals_count], IDE_OUT_WIDTH, "%s = %s", field[1], field[2]);
        g_locals_rows[g_locals_count] = g_locals[g_locals_count];
        g_locals_count++;
    } else if (strcmp(field[0], "watch") == 0 && count >= 3) {
        for (int i = 0; i < g_watch_count; i++) {
            if (strcmp(g_watch_names[i], field[1]) == 0) {
                snprintf(g_watch_text[i], IDE_OUT_WIDTH, "%s = %s", field[1], field[2]);
            }
        }
    }
    return 0;
}

/* A whole report came: the program waits at a statement. */
static void report_end(void)
{
    g_paused = 1;
    g_widgets[W_LOCALS].item_count = g_locals_count;
    g_widgets[W_LOCALS].scroll = 0;
    g_widgets[W_WATCH].item_count = g_watch_count;
    if (!goto_location(g_stop_file, g_stop_line)) outputf("Stopped in %s, which is not a file of this project", g_stop_file);
    if (strcmp(g_stop_reason, "error") == 0) {
        set_message("Runtime error %d: %s. Continue ends the program", g_stop_error, g_stop_description);
    } else {
        set_message("Break in %s, line %d", g_stop_proc, g_stop_line);
    }
    update_commands();
}

/* Reads the child's reports: 1 when one is complete (the program waits), -1
 * when the channel closed, 0 when nothing more has come. In the selftest the
 * pipe blocks, so it returns only with a report or at the end. */
static int poll_reports(void)
{
    for (;;) {
        char *end = memchr(g_report_buffer, '\n', g_report_length);
        if (end != NULL) {
            size_t length = (size_t)(end - g_report_buffer);
            *end = '\0';
            int done = report_line(g_report_buffer);
            g_report_length -= length + 1;
            memmove(g_report_buffer, end + 1, g_report_length);
            if (done) {
                report_end();
                return 1;
            }
            continue;
        }
        if (g_report_length == sizeof(g_report_buffer)) g_report_length = 0;
        long n = sxn_read(g_debug_reports, g_report_buffer + g_report_length, sizeof(g_report_buffer) - g_report_length);
        if (n == -SAVANXP_EAGAIN) return 0;
        if (n <= 0) {
            if (n < 0) {
                char text[96];
                snprintf(text, sizeof(text), "sxbide: the debugger's reports ended with error %ld", -n);
                (void)sxn_log(text);
            }
            (void)sxn_close(g_debug_reports);
            g_debug_reports = -1;
            return -1;
        }
        g_report_length += (size_t)n;
    }
}

/* Goes on from a stop with go, step, over or out. */
static void debug_resume(const char *command)
{
    if (!g_paused) return;
    debug_send("%s", command);
    g_paused = 0;
    g_mark_file = -1;
    g_locals_count = 0;
    g_widgets[W_LOCALS].item_count = 0;
    watch_reset();
    set_message("Running");
    update_commands();
}

/* Start, Continue and the steps: on from a stop, or a start that stops at
 * the first statement when it is a step. */
static void debug_command(const char *command)
{
    if (g_paused) {
        debug_resume(command);
    } else if (g_child_kind == CHILD_NONE) {
        g_start_command = strcmp(command, "go") == 0 ? "go" : "step";
        (void)start_child(CHILD_RUN);
        g_start_command = "go";
    }
}

static void debug_closed(void)
{
    if (g_debug_commands >= 0) (void)sxn_close(g_debug_commands);
    if (g_debug_reports >= 0) (void)sxn_close(g_debug_reports);
    g_debug_commands = g_debug_reports = -1;
    g_report_length = 0;
    g_paused = 0;
    g_locals_count = 0;
    g_widgets[W_LOCALS].item_count = 0;
    watch_reset();
}

/* ---- the designer ------------------------------------------------------------------------
 *
 * A form drawn instead of written. The designer reads the form's block from
 * the text on every paint (design_parse) and writes what a drag or a Delete
 * changes back into it, one step of undo each; between the two, the text is
 * the only model. Its controls are SXGUI widgets painted with no context
 * behind them (sxgui_paint_widgets), so they look as they will at run time
 * and do not react. The selected control is the block the caret is in. */

#define IDE_MAX_CONTROLS 64
#define IDE_GRID 8
#define IDE_FORM_INSET 12
#define IDE_FORM_BORDER 4
#define IDE_FORM_CAPTION 18
#define IDE_FORM_MIN 64
#define IDE_TOOLBOX_WIDTH 104
#define IDE_TRAY_WIDTH 72
#define IDE_TRAY_HEIGHT 22
#define IDE_DOUBLE_CLICK_MS 500

struct ide_control {
    int kind; /* SXB_CTL_* */
    char name[48];
    char text[IDE_VALUE_MAX];
    int begin_line; /* its Begin and End lines, 1-based */
    int end_line;
    int depth; /* 1: the form; 2: its controls and menus; 3: a menu's items */
    int64_t values[SXB_P_COUNT];
};

static struct ide_control g_controls[IDE_MAX_CONTROLS];
static int g_control_count;
static struct sxgui_widget g_design_widgets[IDE_MAX_CONTROLS];
static int g_design_widget_count;
static struct sx_rect g_design_area;
static char g_selected[48];

/* The toolbox: the pointer, then every kind a form holds directly. */
static const int g_tool_kinds[] = {-1,
                                   SXB_CTL_LABEL,
                                   SXB_CTL_BUTTON,
                                   SXB_CTL_CHECKBOX,
                                   SXB_CTL_RADIO,
                                   SXB_CTL_TEXTBOX,
                                   SXB_CTL_LISTBOX,
                                   SXB_CTL_COMBOBOX,
                                   SXB_CTL_GROUPBOX,
                                   SXB_CTL_SCROLLBAR,
                                   SXB_CTL_TRACKBAR,
                                   SXB_CTL_PROGRESS,
                                   SXB_CTL_TIMER};
#define IDE_TOOL_COUNT ((int)(sizeof(g_tool_kinds) / sizeof(g_tool_kinds[0])))
static const char *g_tool_rows[IDE_TOOL_COUNT];

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_FORM, DRAG_DRAW };
static int g_drag;
static int g_drag_control;
static int g_drag_grab;
static int g_drag_x;
static int g_drag_y;
static struct sx_rect g_drag_start;
static struct sx_rect g_drag_rect;
static uint32_t g_design_buttons;
static unsigned long g_last_click_ms;
static int g_last_click_control = -1;

static int designer_active(void)
{
    return g_current >= 0 && g_current < g_file_count && g_files[g_current].kind == IDE_FORM && g_object_view[g_current];
}

/* A control drawn on the form, as against a timer or a menu. */
static int placed(const struct ide_control *control)
{
    return control->depth == 2 && control->kind != SXB_CTL_TIMER && control->kind != SXB_CTL_MENU &&
           control->kind != SXB_CTL_MENUITEM;
}

static struct sx_rect control_rect(const struct ide_control *control)
{
    return sx_rect_make((int)control->values[SXB_P_LEFT], (int)control->values[SXB_P_TOP],
                        (int)control->values[SXB_P_WIDTH], (int)control->values[SXB_P_HEIGHT]);
}

/* A literal as the form file writes it: True, False, &HFF, 12, -3. */
static int64_t parse_value(const char *text)
{
    int64_t value = 0;
    int negative = 0, base = 10;
    if (same_text_nocase(text, (int)strlen(text), "True")) return 1;
    if (same_text_nocase(text, (int)strlen(text), "False")) return 0;
    if (*text == '-') {
        negative = 1;
        text++;
    }
    if (text[0] == '&' && (text[1] == 'H' || text[1] == 'h')) {
        base = 16;
        text += 2;
    }
    for (; *text; text++) {
        int digit;
        if (*text >= '0' && *text <= '9') digit = *text - '0';
        else if (base == 16 && *text >= 'a' && *text <= 'f') digit = *text - 'a' + 10;
        else if (base == 16 && *text >= 'A' && *text <= 'F') digit = *text - 'A' + 10;
        else break;
        value = value * base + digit;
    }
    return negative ? -value : value;
}

/* The form's block of the current file into g_controls: the form first,
 * then each Begin inside it in order, with its values (defaults for what the
 * text leaves out) and its lines. */
static void design_parse(void)
{
    const char *text = document(g_current);
    int stack[8], depth = 0, line = 0, offset = 0;
    struct ide_prop prop;
    g_control_count = 0;
    while (text[offset] != '\0') {
        int end = offset, first, last;
        line++;
        while (text[end] != '\0' && text[end] != '\n') end++;
        trim_line(text, offset, end, &first, &last);
        if (line_is_word(text, first, last, "Begin")) {
            int p = first + 5;
            while (p < last && text[p] == ' ') p++;
            int type = p;
            while (p < last && is_word_char(text[p])) p++;
            int kind = sxb_find_control_kind(text + type, p - type);
            while (p < last && text[p] == ' ') p++;
            if (depth < 8) {
                int index = -1;
                if (g_control_count < IDE_MAX_CONTROLS && kind >= 0 && (depth > 0 || g_control_count == 0)) {
                    struct sxb_control_def def;
                    struct ide_control *control = &g_controls[g_control_count];
                    index = g_control_count++;
                    sxb_control_defaults(kind, &def);
                    memcpy(control->values, def.values, sizeof(control->values));
                    control->kind = kind;
                    control->depth = depth + 1;
                    control->begin_line = control->end_line = line;
                    control->text[0] = '\0';
                    snprintf(control->name, sizeof(control->name), "%.*s", last - p, text + p);
                }
                stack[depth++] = index;
            }
        } else if (depth > 0 && last - first == 3 && same_text_nocase(text + first, 3, "End")) {
            int index = stack[--depth];
            if (index >= 0) g_controls[index].end_line = line;
            if (depth == 0) break;
        } else if (depth > 0 && stack[depth - 1] >= 0 && first < last && text[first] != '\'' &&
                   parse_property(text, first, last, &prop)) {
            struct ide_control *control = &g_controls[stack[depth - 1]];
            int property = sxb_find_property(prop.key, (int)strlen(prop.key));
            if (property == SXB_P_TEXT) {
                snprintf(control->text, sizeof(control->text), "%s", prop.value);
            } else if (property >= 0) {
                control->values[property] = parse_value(prop.value);
            }
        }
        offset = text[end] ? end + 1 : end;
    }
}

static int find_control(const char *name)
{
    for (int i = 0; i < g_control_count; i++) {
        if (same_text_nocase(g_controls[i].name, (int)strlen(g_controls[i].name), name)) return i;
    }
    return -1;
}

/* The placed controls as widgets, the one being dragged where the drag has it. */
static void design_widgets(void)
{
    g_design_widget_count = 0;
    for (int i = 1; i < g_control_count; i++) {
        struct ide_control *control = &g_controls[i];
        const int64_t *v = control->values;
        struct sx_rect rect = control_rect(control);
        struct sxgui_widget widget;
        if (!placed(control)) continue;
        if ((g_drag == DRAG_MOVE || g_drag == DRAG_RESIZE) && g_drag_control == i) rect = g_drag_rect;
        switch (control->kind) {
        case SXB_CTL_LABEL: widget = sxgui_label(rect, control->text); break;
        case SXB_CTL_BUTTON: widget = sxgui_button(rect, control->text, NULL, NULL); break;
        case SXB_CTL_CHECKBOX: widget = sxgui_checkbox(rect, control->text, v[SXB_P_VALUE] != 0); break;
        case SXB_CTL_RADIO: widget = sxgui_radio(rect, control->text, (int)v[SXB_P_GROUP], v[SXB_P_VALUE] != 0); break;
        case SXB_CTL_TEXTBOX:
            widget = v[SXB_P_MULTILINE] ? sxgui_textedit(rect, control->text, (int)sizeof(control->text))
                                        : sxgui_textfield(rect, control->text, (int)sizeof(control->text));
            break;
        case SXB_CTL_LISTBOX: widget = sxgui_listbox(rect, NULL, 0); break;
        case SXB_CTL_COMBOBOX: widget = sxgui_combobox(rect, NULL, 0, 0); break;
        case SXB_CTL_GROUPBOX: widget = sxgui_groupbox(rect, control->text); break;
        case SXB_CTL_SCROLLBAR:
            widget = sxgui_scrollbar(rect, (int)v[SXB_P_MIN], (int)v[SXB_P_MAX], (int)v[SXB_P_LARGECHANGE],
                                     (int)v[SXB_P_VALUE]);
            if (v[SXB_P_HORIZONTAL]) widget.flags |= SXGUI_FLAG_HSCROLL;
            break;
        case SXB_CTL_TRACKBAR: widget = sxgui_trackbar(rect, (int)v[SXB_P_MIN], (int)v[SXB_P_MAX], (int)v[SXB_P_VALUE]); break;
        case SXB_CTL_PROGRESS: widget = sxgui_progress(rect, (int)v[SXB_P_MIN], (int)v[SXB_P_MAX], (int)v[SXB_P_VALUE]); break;
        default: continue;
        }
        /* Shown while designed even when Visible = False, as Visual Basic did. */
        widget.flags |= SXGUI_FLAG_VISIBLE;
        if (!v[SXB_P_ENABLED]) widget.flags |= SXGUI_FLAG_DISABLED;
        g_design_widgets[g_design_widget_count++] = widget;
    }
}

/* Where the form is drawn, in window coordinates: its frame and its client area. */
static void design_geometry(struct sx_rect *outer, struct sx_rect *client)
{
    int width = g_control_count ? (int)g_controls[0].values[SXB_P_WIDTH] : 0;
    int height = g_control_count ? (int)g_controls[0].values[SXB_P_HEIGHT] : 0;
    if (g_drag == DRAG_FORM) {
        width = g_drag_rect.width;
        height = g_drag_rect.height;
    }
    *outer = sx_rect_make(g_design_area.x + IDE_FORM_INSET, g_design_area.y + IDE_FORM_INSET,
                          width + 2 * IDE_FORM_BORDER, height + 2 * IDE_FORM_BORDER + IDE_FORM_CAPTION);
    *client = sx_rect_make(outer->x + IDE_FORM_BORDER, outer->y + IDE_FORM_BORDER + IDE_FORM_CAPTION, width, height);
}

/* The tray under the form, for what has no place on it (timers). */
static struct sx_rect tray_rect(struct sx_rect outer, int slot)
{
    return sx_rect_make(outer.x + slot * (IDE_TRAY_WIDTH + IDE_GRID), outer.y + outer.height + IDE_FORM_INSET,
                        IDE_TRAY_WIDTH, IDE_TRAY_HEIGHT);
}

static int design_selected(void)
{
    int index = find_control(g_selected);
    return index >= 0 ? index : g_control_count > 0 ? 0 : -1;
}

static void design_select(int index)
{
    if (index < 0 || index >= g_control_count) return;
    snprintf(g_selected, sizeof(g_selected), "%s", g_controls[index].name);
    sxgui_textedit_set_caret(editor(g_current), line_offset(document(g_current), g_controls[index].begin_line));
    props_sync(1);
    repaint();
}

/* The selection follows the caret: the innermost block it is in. */
static void design_follow_caret(void)
{
    int line = line_of(document(g_current), editor(g_current)->caret), best = 0;
    design_parse();
    for (int i = 1; i < g_control_count; i++) {
        if (g_controls[i].begin_line <= line && line <= g_controls[i].end_line) best = i;
    }
    if (g_control_count > 0) snprintf(g_selected, sizeof(g_selected), "%s", g_controls[best].name);
}

static void update_view(void)
{
    int design = designer_active();
    for (int i = 0; i < IDE_MAX_FILES; i++) {
        if (i == g_current && !design) {
            editor(i)->flags |= SXGUI_FLAG_VISIBLE;
        } else {
            editor(i)->flags &= ~SXGUI_FLAG_VISIBLE;
        }
    }
    if (design) {
        g_widgets[W_TOOLBOX].flags |= SXGUI_FLAG_VISIBLE;
        design_follow_caret();
    } else {
        g_widgets[W_TOOLBOX].flags &= ~SXGUI_FLAG_VISIBLE;
    }
    g_drag = DRAG_NONE;
    if (g_ui && g_current >= 0) sxgui_focus(g_ui, design ? -1 : W_EDIT + g_current);
    repaint();
}

static int indent_of(const char *text, int line)
{
    int offset = line_offset(text, line), indent = 0;
    while (text[offset + indent] == ' ') indent++;
    return indent;
}

/* Sets integer properties of the block starting on `begin_line`, adding the
 * ones it does not write yet before its End: one step of undo. */
static void design_set_values(int begin_line, int count, const int *properties, const int *values)
{
    const char *text = document(g_current);
    int start = line_offset(text, begin_line), offset = start, depth = 0, end_offset = -1;
    int done[4] = {0, 0, 0, 0}, child_indent = indent_of(text, begin_line) + 4;
    size_t capacity = strlen(text) + 256, used = 0;
    char *out = malloc(capacity);
    if (out == NULL || count > 4) {
        free(out);
        return;
    }
    while (text[offset] != '\0') {
        int end = offset, first, last, k = count;
        struct ide_prop prop;
        while (text[end] != '\0' && text[end] != '\n') end++;
        int next = text[end] ? end + 1 : end;
        trim_line(text, offset, end, &first, &last);
        if (line_is_word(text, first, last, "Begin")) {
            depth++;
        } else if (last - first == 3 && same_text_nocase(text + first, 3, "End")) {
            if (--depth == 0) {
                end_offset = offset;
                break;
            }
        } else if (depth == 1 && parse_property(text, first, last, &prop)) {
            for (k = 0; k < count; k++) {
                if (strcmp(prop.key, sxb_property_name(properties[k])) == 0) break;
            }
        }
        if (k < count) {
            used += (size_t)snprintf(out + used, capacity - used, "%.*s%s = %d\n", first - offset, text + offset,
                                     prop.key, values[k]);
            done[k] = 1;
        } else {
            memcpy(out + used, text + offset, (size_t)(next - offset));
            used += (size_t)(next - offset);
        }
        offset = next;
    }
    if (end_offset >= 0) {
        for (int k = 0; k < count; k++) {
            if (!done[k]) {
                used += (size_t)snprintf(out + used, capacity - used, "%*s%s = %d\n", child_indent, "",
                                         sxb_property_name(properties[k]), values[k]);
            }
        }
        out[used] = '\0';
        (void)sxgui_textedit_replace(editor(g_current), start, end_offset, out);
        sxgui_textedit_set_caret(editor(g_current), start);
        props_sync(1);
    }
    free(out);
}

static void design_set_rect(int index, struct sx_rect rect)
{
    static const int keys[4] = {SXB_P_LEFT, SXB_P_TOP, SXB_P_WIDTH, SXB_P_HEIGHT};
    int values[4] = {rect.x, rect.y, rect.width, rect.height};
    if (index == 0) {
        design_set_values(g_controls[0].begin_line, 2, keys + 2, values + 2);
    } else {
        design_set_values(g_controls[index].begin_line, 4, keys, values);
    }
}

/* A new control of `kind` at `rect` (its default size when the rectangle is
 * a click), named after its kind, at the end of the form's block. */
static void design_insert(int kind, struct sx_rect rect)
{
    char name[48], block[640];
    const char *kind_name = sxb_control_kind_name(kind);
    const char *text = document(g_current);
    struct sxb_control_def def;
    int used, indent, at;
    if (g_control_count == 0) return;
    for (int n = 1;; n++) {
        snprintf(name, sizeof(name), "%s%d", kind_name, n);
        if (find_control(name) < 0) break;
    }
    sxb_control_defaults(kind, &def);
    if (rect.width < IDE_GRID || rect.height < IDE_GRID) {
        rect.width = (int)def.values[SXB_P_WIDTH];
        rect.height = (int)def.values[SXB_P_HEIGHT];
    }
    indent = indent_of(text, g_controls[0].begin_line) + 4;
    used = snprintf(block, sizeof(block), "%*sBegin %s %s\n", indent, "", kind_name, name);
    if (kind != SXB_CTL_TEXTBOX && kind != SXB_CTL_COMBOBOX && sxb_property_access(SXB_P_TEXT, kind) == SXB_ACCESS_WRITE) {
        used += snprintf(block + used, sizeof(block) - (size_t)used, "%*sText = \"%s\"\n", indent + 4, "", name);
    }
    if (kind == SXB_CTL_TIMER) {
        used += snprintf(block + used, sizeof(block) - (size_t)used, "%*sInterval = 1000\n", indent + 4, "");
    } else {
        used += snprintf(block + used, sizeof(block) - (size_t)used,
                         "%*sLeft = %d\n%*sTop = %d\n%*sWidth = %d\n%*sHeight = %d\n", indent + 4, "", rect.x,
                         indent + 4, "", rect.y, indent + 4, "", rect.width, indent + 4, "", rect.height);
    }
    snprintf(block + used, sizeof(block) - (size_t)used, "%*sEnd\n", indent, "");
    at = line_offset(text, g_controls[0].end_line);
    (void)sxgui_textedit_replace(editor(g_current), at, at, block);
    design_parse();
    design_select(find_control(name));
}

/* Deletes the selected control's block; the form is selected after. */
static void design_delete(void)
{
    design_parse();
    int index = design_selected();
    if (index <= 0) return;
    const char *text = document(g_current);
    int start = line_offset(text, g_controls[index].begin_line);
    int end = line_offset(text, g_controls[index].end_line + 1);
    (void)sxgui_textedit_replace(editor(g_current), start, end, "");
    design_parse();
    design_select(0);
}

static int default_event(int kind)
{
    switch (kind) {
    case SXB_CTL_FORM: return SXB_EV_LOAD;
    case SXB_CTL_BUTTON:
    case SXB_CTL_CHECKBOX:
    case SXB_CTL_RADIO:
    case SXB_CTL_LISTBOX:
    case SXB_CTL_COMBOBOX:
    case SXB_CTL_MENUITEM: return SXB_EV_CLICK;
    case SXB_CTL_TEXTBOX:
    case SXB_CTL_SCROLLBAR:
    case SXB_CTL_TRACKBAR: return SXB_EV_CHANGE;
    case SXB_CTL_TIMER: return SXB_EV_TICK;
    default: return -1;
    }
}

/* The next word of [*at, last), skipping blanks; its length. */
static int next_word(const char *text, int *at, int last, int *start)
{
    while (*at < last && (text[*at] == ' ' || text[*at] == '\t')) (*at)++;
    *start = *at;
    while (*at < last && is_word_char(text[*at])) (*at)++;
    return *at - *start;
}

/* Double-click: the code view at the control's default event, its Sub
 * written first if the form's code has none (Form_Load for the form). */
static void design_open_event(int index)
{
    struct ide_control *control = &g_controls[index];
    struct sxgui_widget *edit = editor(g_current);
    int event = default_event(control->kind), caret;
    char handler[112], sub[160];
    g_object_view[g_current] = 0;
    update_view();
    if (event < 0) {
        sxgui_textedit_set_caret(edit, line_offset(document(g_current), control->begin_line));
        props_sync(1);
        return;
    }
    snprintf(handler, sizeof(handler), "%s_%s", control->kind == SXB_CTL_FORM ? "Form" : control->name,
             sxb_event_name(event));
    const char *text = document(g_current);
    int offset = line_offset(text, g_controls[0].end_line + 1), found = -1;
    while (text[offset] != '\0' && found < 0) {
        int end = offset, at = offset, start, length;
        while (text[end] != '\0' && text[end] != '\n') end++;
        while ((length = next_word(text, &at, end, &start)) > 0) {
            if (same_text_nocase(text + start, length, "Sub")) {
                length = next_word(text, &at, end, &start);
                if (same_text_nocase(text + start, length, handler)) found = end;
                break;
            }
        }
        offset = text[end] ? end + 1 : end;
    }
    if (found >= 0) {
        int line = line_of(text, found) + 1;
        caret = line_offset(text, line) + indent_of(text, line);
    } else {
        size_t length = strlen(text);
        const char *gap = length == 0 ? "" : text[length - 1] != '\n' ? "\n\n" : "\n";
        int head = snprintf(sub, sizeof(sub), "%sPrivate Sub %s()\n    ", gap, handler);
        snprintf(sub + head, sizeof(sub) - (size_t)head, "\nEnd Sub\n");
        (void)sxgui_textedit_replace(edit, (int)length, (int)length, sub);
        caret = (int)length + head;
    }
    sxgui_textedit_set_caret(edit, caret);
    props_sync(1);
}

static int point_in(struct sx_rect rect, int x, int y)
{
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

/* The pointer over the designer: the IDE's on_pointer, before the widgets. */
static int designer_pointer(const struct savanxp_gui_pointer_event *event)
{
    uint32_t was = g_design_buttons;
    int down = (event->buttons & 1u) != 0, pressed = down && !(was & 1u), released = !down && (was & 1u);
    struct sx_rect outer, client;
    g_design_buttons = event->buttons;
    if (!designer_active()) {
        g_drag = DRAG_NONE;
        return 0;
    }
    if (g_ui != NULL && g_ui->menubar != NULL && g_ui->menubar->open_menu >= 0) return 0;
    if (g_drag == DRAG_NONE && !(pressed && point_in(g_design_area, event->x, event->y))) return 0;
    design_parse();
    design_geometry(&outer, &client);
    int x = event->x - client.x, y = event->y - client.y;

    if (pressed) {
        int tool = g_widgets[W_TOOLBOX].value;
        int kind = tool > 0 && tool < IDE_TOOL_COUNT ? g_tool_kinds[tool] : -1;
        int selected = design_selected(), hit = -1, grab, slot = 0;
        int in_client = x >= 0 && y >= 0 && x < client.width && y < client.height;
        unsigned long now = sxngui_uptime_ms();
        /* A double click is two presses in a row on the same control: any
         * other press between them (a handle, the toolbox) breaks the pair. */
        int last = g_last_click_control;
        g_last_click_control = -1;
        if (g_ui) sxgui_focus(g_ui, -1);
        g_drag = DRAG_NONE;
        g_drag_x = x;
        g_drag_y = y;
        if (kind == SXB_CTL_TIMER) {
            design_insert(kind, sx_rect_make(0, 0, 0, 0));
            g_widgets[W_TOOLBOX].value = 0;
            return 1;
        }
        if (kind >= 0) {
            if (in_client) {
                g_drag = DRAG_DRAW;
                g_drag_start = g_drag_rect = sx_rect_make(sxgui_snap(x, IDE_GRID), sxgui_snap(y, IDE_GRID), 0, 0);
            }
            return 1;
        }
        if (selected > 0 && placed(&g_controls[selected]) &&
            (grab = sxgui_grab_at(control_rect(&g_controls[selected]), x, y)) != SXGUI_GRAB_NONE) {
            g_drag = DRAG_RESIZE;
            g_drag_grab = grab;
            g_drag_control = selected;
            g_drag_start = g_drag_rect = control_rect(&g_controls[selected]);
            return 1;
        }
        grab = selected == 0 ? sxgui_grab_at(sx_rect_make(0, 0, client.width, client.height), x, y) : SXGUI_GRAB_NONE;
        if (grab == SXGUI_GRAB_E || grab == SXGUI_GRAB_S || grab == SXGUI_GRAB_SE) {
            g_drag = DRAG_FORM;
            g_drag_grab = grab;
            g_drag_start = g_drag_rect = sx_rect_make(0, 0, client.width, client.height);
            return 1;
        }
        for (int i = g_control_count - 1; i > 0 && hit < 0; i--) {
            if (placed(&g_controls[i]) && point_in(control_rect(&g_controls[i]), x, y)) hit = i;
        }
        for (int i = 1; i < g_control_count && hit < 0; i++) {
            if (g_controls[i].depth == 2 && g_controls[i].kind == SXB_CTL_TIMER &&
                point_in(tray_rect(outer, slot++), event->x, event->y)) {
                hit = i;
            }
        }
        if (hit < 0 && (in_client || point_in(outer, event->x, event->y))) hit = 0;
        if (hit < 0) return 1;
        if (hit == last && now - g_last_click_ms < IDE_DOUBLE_CLICK_MS) {
            design_open_event(hit);
            return 1;
        }
        g_last_click_control = hit;
        g_last_click_ms = now;
        design_select(hit);
        if (hit > 0 && placed(&g_controls[hit])) {
            g_drag = DRAG_MOVE;
            g_drag_control = hit;
            g_drag_start = g_drag_rect = control_rect(&g_controls[hit]);
        }
        return 1;
    }
    if (g_drag == DRAG_NONE) return 0;

    int dx = x - g_drag_x, dy = y - g_drag_y;
    if (g_drag == DRAG_MOVE && (dx != 0 || dy != 0)) {
        g_drag_rect.x = sxgui_snap(g_drag_start.x + dx, IDE_GRID);
        g_drag_rect.y = sxgui_snap(g_drag_start.y + dy, IDE_GRID);
    } else if (g_drag == DRAG_RESIZE || g_drag == DRAG_FORM) {
        g_drag_rect = sxgui_grab_resize(g_drag_start, g_drag_grab, sxgui_snap(dx, IDE_GRID), sxgui_snap(dy, IDE_GRID),
                                        g_drag == DRAG_FORM ? IDE_FORM_MIN : IDE_GRID);
    } else if (g_drag == DRAG_DRAW) {
        int x1 = sxgui_snap(x, IDE_GRID), y1 = sxgui_snap(y, IDE_GRID);
        int x0 = g_drag_start.x, y0 = g_drag_start.y;
        g_drag_rect = sx_rect_make(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 < x1 ? x1 - x0 : x0 - x1,
                                   y0 < y1 ? y1 - y0 : y0 - y1);
    }
    if (released) {
        int drag = g_drag;
        struct sx_rect rect = g_drag_rect, start = g_drag_start;
        int changed = rect.x != start.x || rect.y != start.y || rect.width != start.width || rect.height != start.height;
        g_drag = DRAG_NONE;
        if ((drag == DRAG_MOVE || drag == DRAG_RESIZE) && changed) {
            design_set_rect(g_drag_control, rect);
        } else if (drag == DRAG_FORM && changed) {
            design_set_rect(0, rect);
        } else if (drag == DRAG_DRAW) {
            int tool = g_widgets[W_TOOLBOX].value;
            if (tool > 0 && tool < IDE_TOOL_COUNT) design_insert(g_tool_kinds[tool], rect);
            g_widgets[W_TOOLBOX].value = 0;
        }
    }
    repaint();
    return 1;
}

static void designer_paint(void)
{
    struct sx_rect outer, client;
    if (!designer_active() || g_ui == NULL) return;
    struct sx_painter *painter = &g_ui->painter;
    design_parse();
    design_widgets();
    design_geometry(&outer, &client);
    int selected = design_selected(), slot = 0;
    int area_clip = sx_painter_push_clip(painter, g_design_area);
    sx_painter_fill_rect(painter, g_design_area, SXGUI_COLOR_SHADOW);
    if (g_control_count > 0) {
        struct sx_rect caption = sx_rect_make(outer.x + IDE_FORM_BORDER, outer.y + IDE_FORM_BORDER,
                                              outer.width - 2 * IDE_FORM_BORDER, IDE_FORM_CAPTION - 1);
        sx_painter_fill_rect(painter, outer, SXGUI_COLOR_FACE);
        sxgui_draw_raised_edge(painter, outer);
        sx_painter_fill_rect(painter, caption, 0x000080u);
        sx_painter_draw_text(painter, caption.x + 4, caption.y + (caption.height - gfx_text_height()) / 2,
                             g_controls[0].text, 0xFFFFFFu);
        int client_clip = sx_painter_push_clip(painter, client);
        sxgui_paint_snap_grid(painter, client, IDE_GRID);
        if (sx_painter_push_origin(painter, client.x, client.y)) {
            /* The grid shows where the form is bare: a label has no
             * background of its own, and at run time there are no dots. */
            for (int i = 0; i < g_design_widget_count; i++) {
                sx_painter_fill_rect(painter, g_design_widgets[i].rect, SXGUI_COLOR_FACE);
            }
            sxgui_paint_widgets(painter, g_design_widgets, g_design_widget_count);
            if (g_drag == DRAG_DRAW) sx_painter_draw_frame(painter, g_drag_rect, SXGUI_COLOR_TEXT);
            if (selected > 0 && placed(&g_controls[selected])) {
                struct sx_rect rect = control_rect(&g_controls[selected]);
                if (g_drag == DRAG_MOVE || g_drag == DRAG_RESIZE) rect = g_drag_rect;
                sxgui_paint_grab_handles(painter, rect);
            }
            sx_painter_pop_origin(painter);
        }
        if (client_clip) sx_painter_pop_clip(painter);
        if (selected == 0) sxgui_paint_grab_handles(painter, client);
        for (int i = 1; i < g_control_count; i++) {
            if (g_controls[i].depth != 2 || g_controls[i].kind != SXB_CTL_TIMER) continue;
            struct sx_rect rect = tray_rect(outer, slot++);
            sx_painter_fill_rect(painter, rect, SXGUI_COLOR_FACE);
            sxgui_draw_raised_edge(painter, rect);
            sx_painter_draw_text(painter, rect.x + 6, rect.y + (rect.height - gfx_text_height()) / 2,
                                 g_controls[i].name, SXGUI_COLOR_TEXT);
            if (i == selected) sxgui_paint_grab_handles(painter, rect);
        }
    }
    if (area_clip) sx_painter_pop_clip(painter);
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
static struct sxgui_menu_item g_view_items[] = {
    {"Code\tF7", CMD_VIEW_CODE, 0},
    {"Object\tShift+F7", CMD_VIEW_OBJECT, 0},
};
static struct sxgui_menu_item g_run_items[] = {
    {"Start\tF5", CMD_START, 0},
    {"Break", CMD_BREAK, SXGUI_MENU_DISABLED},
    {"End", CMD_END, SXGUI_MENU_DISABLED},
    {0, 0, 0},
    {"Step Into\tF8", CMD_STEP_INTO, 0},
    {"Step Over\tShift+F8", CMD_STEP_OVER, 0},
    {"Step Out\tCtrl+Shift+F8", CMD_STEP_OUT, 0},
    {0, 0, 0},
    {"Toggle Breakpoint\tF9", CMD_TOGGLE_BREAKPOINT, 0},
    {"Add Watch\tShift+F9", CMD_ADD_WATCH, 0},
    {"Delete Watch", CMD_DELETE_WATCH, 0},
    {0, 0, 0},
    {"Make SXE...", CMD_MAKE, 0},
};
static const struct sxgui_menu g_menus[] = {
    {"File", g_file_items, (int)(sizeof(g_file_items) / sizeof(g_file_items[0]))},
    {"Edit", g_edit_items, (int)(sizeof(g_edit_items) / sizeof(g_edit_items[0]))},
    {"View", g_view_items, (int)(sizeof(g_view_items) / sizeof(g_view_items[0]))},
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
    int can_go = (open && idle) || g_paused;
    enable(&g_run_items[0], W_TB_START, can_go);
    g_run_items[0].text = g_paused ? "Continue\tF5" : "Start\tF5";
    g_widgets[W_TB_START].text = g_paused ? "Continue" : "Start";
    enable(&g_run_items[1], W_TB_BREAK, g_child_kind == CHILD_RUN && g_debug_commands >= 0 && !g_paused);
    enable(&g_run_items[2], W_TB_END, g_child_kind == CHILD_RUN);
    enable(&g_run_items[4], -1, can_go);
    enable(&g_run_items[5], -1, can_go);
    enable(&g_run_items[6], -1, g_paused);
    enable(&g_run_items[8], -1, open);
    enable(&g_run_items[9], -1, open);
    enable(&g_run_items[10], -1, g_watch_count > 0);
    enable(&g_run_items[12], W_TB_MAKE, open && idle);
    int form = g_current >= 0 && g_files[g_current].kind == IDE_FORM;
    enable(&g_view_items[0], -1, open);
    enable(&g_view_items[1], -1, form);
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
        debug_command("go");
        break;
    case CMD_BREAK:
        if (!g_paused) debug_send("pause");
        break;
    case CMD_STEP_INTO:
        debug_command("step");
        break;
    case CMD_STEP_OVER:
        debug_command("over");
        break;
    case CMD_STEP_OUT:
        if (g_paused) debug_resume("out");
        break;
    case CMD_TOGGLE_BREAKPOINT:
        if (edit && !designer_active()) toggle_breakpoint(g_current, line_of(document(g_current), edit->caret));
        break;
    case CMD_ADD_WATCH:
        watch_at_caret();
        update_commands();
        break;
    case CMD_DELETE_WATCH:
        delete_watch(g_widgets[W_WATCH].value);
        update_commands();
        break;
    case CMD_END:
        end_child();
        break;
    case CMD_MAKE:
        (void)start_child(CHILD_MAKE);
        break;
    case CMD_VIEW_CODE:
    case CMD_VIEW_OBJECT:
        if (g_current >= 0 && g_files[g_current].kind == IDE_FORM) {
            g_object_view[g_current] = id == CMD_VIEW_OBJECT;
            update_view();
            props_sync(1);
        }
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
    case W_TB_BREAK:
        command(CMD_BREAK);
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
        update_commands();
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
    if (g_debug_reports >= 0) (void)poll_reports();
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
    int shift = (event->modifiers & SAVANXP_KEY_MOD_SHIFT) != 0;
    if (event->key == SAVANXP_KEY_F8) {
        command(ctrl && shift ? CMD_STEP_OUT : shift ? CMD_STEP_OVER : CMD_STEP_INTO);
        return 1;
    }
    if (event->key == SAVANXP_KEY_F9) {
        command(shift ? CMD_ADD_WATCH : CMD_TOGGLE_BREAKPOINT);
        return 1;
    }
    if (event->key == SAVANXP_KEY_DELETE && g_ui->focus_index == W_WATCH) {
        command(CMD_DELETE_WATCH);
        return 1;
    }
    if (event->key == SAVANXP_KEY_F7) {
        command((event->modifiers & SAVANXP_KEY_MOD_SHIFT) ? CMD_VIEW_OBJECT : CMD_VIEW_CODE);
        return 1;
    }
    /* The designer has the keyboard while no widget has the focus. */
    if (designer_active() && g_ui->focus_index < 0) {
        if (event->key == SAVANXP_KEY_DELETE) {
            design_delete();
            return 1;
        }
        if (event->key == SAVANXP_KEY_ESC) {
            g_widgets[W_TOOLBOX].value = 0;
            repaint();
            return 1;
        }
        if (ctrl && (event->ascii == 'z' || event->ascii == 'Z' || event->ascii == 26)) {
            command(CMD_UNDO);
            return 1;
        }
        if (ctrl && (event->ascii == 'y' || event->ascii == 'Y' || event->ascii == 25)) {
            command(CMD_REDO);
            return 1;
        }
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

static uint32_t g_editor_buttons;

static int on_pointer(struct sxngui_app *app, const struct savanxp_gui_pointer_event *event)
{
    (void)app;
    uint32_t was = g_editor_buttons;
    g_editor_buttons = event->buttons;
    /* A press on the code's gutter marks toggles a breakpoint there. */
    if ((event->buttons & 1u) && !(was & 1u) && g_current >= 0 && !designer_active() &&
        !(g_ui->menubar != NULL && g_ui->menubar->open_menu >= 0)) {
        int in_marks = 0;
        int line = sxgui_textedit_line_at(editor(g_current), event->x, event->y, &in_marks);
        if (line >= 0 && in_marks) {
            toggle_breakpoint(g_current, line + 1);
            return 1;
        }
    }
    return designer_pointer(event);
}

static void on_paint(struct sxngui_app *app)
{
    (void)app;
    designer_paint();
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
    static const int toolbar[] = {W_TB_OPEN, W_TB_SAVE, W_TB_SEP1, W_TB_START, W_TB_BREAK, W_TB_END, W_TB_SEP2, W_TB_MAKE};

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
    int editor_height = output_y - IDE_LABEL_HEIGHT - SXGUI_GAP - body;
    for (int i = 0; i < IDE_MAX_FILES; i++) {
        editor(i)->rect = sx_rect_make(right_x, body, right_width, editor_height);
    }
    g_widgets[W_TOOLBOX].rect = sx_rect_make(right_x, body, IDE_TOOLBOX_WIDTH, editor_height);
    g_design_area = sx_rect_make(right_x + IDE_TOOLBOX_WIDTH + SXGUI_GAP, body,
                                 right_width - IDE_TOOLBOX_WIDTH - SXGUI_GAP, editor_height);
    /* Under the code: Output, then the debugger's Locals and Watch. */
    int output_width = right_width * 46 / 100;
    int pane_width = (right_width - output_width - 2 * SXGUI_GAP) / 2;
    int locals_x = right_x + output_width + SXGUI_GAP;
    int watch_x = locals_x + pane_width + SXGUI_GAP;
    int watch_width = right_x + right_width - watch_x;
    g_widgets[W_OUTPUT_LABEL].rect = sx_rect_make(right_x, output_y - IDE_LABEL_HEIGHT, output_width, IDE_LABEL_HEIGHT);
    g_widgets[W_OUTPUT].rect = sx_rect_make(right_x, output_y, output_width, IDE_OUTPUT_HEIGHT);
    g_widgets[W_LOCALS_LABEL].rect = sx_rect_make(locals_x, output_y - IDE_LABEL_HEIGHT, pane_width, IDE_LABEL_HEIGHT);
    g_widgets[W_LOCALS].rect = sx_rect_make(locals_x, output_y, pane_width, IDE_OUTPUT_HEIGHT);
    g_widgets[W_WATCH_LABEL].rect = sx_rect_make(watch_x, output_y - IDE_LABEL_HEIGHT, watch_width, IDE_LABEL_HEIGHT);
    g_widgets[W_WATCH].rect = sx_rect_make(watch_x, output_y, watch_width, IDE_OUTPUT_HEIGHT);
    g_widgets[W_STATUS].rect = sx_rect_make(0, status_y, width, SXGUI_STATUS_HEIGHT);
}

static void layout(struct sxngui_app *app)
{
    layout_size((int)app->info.width, (int)app->info.height);
    repaint();
}

#define IDE_TOOLBAR_GLYPHS 6
static struct sx_bitmap g_icons[IDE_TOOLBAR_GLYPHS];
static const struct sx_bitmap *g_icon_refs[IDE_TOOLBAR_GLYPHS];

static void build_widgets(void)
{
    static const uint32_t *const pixels[] = {ide_glyph_document_open,        ide_glyph_document_save,
                                             ide_glyph_media_playback_start, ide_glyph_media_playback_pause,
                                             ide_glyph_media_playback_stop,  ide_glyph_application_x_executable};
    static const char *const labels[] = {"Open", "Save", "Start", "Break", "End", "Make SXE"};
    static const int slots[] = {W_TB_OPEN, W_TB_SAVE, W_TB_START, W_TB_BREAK, W_TB_END, W_TB_MAKE};
    struct savanxp_fb_info info = {0};
    struct sx_rect none = sx_rect_make(0, 0, 0, 0);
    info.width = info.height = 20;
    info.pitch = 80;
    info.bpp = 32;
    info.buffer_size = 1600;
    for (int i = 0; i < IDE_TOOLBAR_GLYPHS; i++) {
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
    g_widgets[W_LOCALS_LABEL] = sxgui_label(none, "Locals");
    g_widgets[W_LOCALS] = sxgui_listbox(none, g_locals_rows, 0);
    g_widgets[W_LOCALS].flags |= SXGUI_FLAG_EMPTY_SELECTION;
    g_widgets[W_LOCALS].value = -1;
    g_widgets[W_WATCH_LABEL] = sxgui_label(none, "Watch");
    g_widgets[W_WATCH] = sxgui_listbox(none, g_watch_rows, 0);
    g_widgets[W_STATUS] = sxgui_label(none, g_status);
    g_widgets[W_STATUS].flags |= SXGUI_FLAG_SUNKEN;
    g_tool_rows[0] = "Pointer";
    for (int i = 1; i < IDE_TOOL_COUNT; i++) g_tool_rows[i] = sxb_control_kind_name(g_tool_kinds[i]);
    g_widgets[W_TOOLBOX] = sxgui_listbox(none, g_tool_rows, IDE_TOOL_COUNT);
    g_widgets[W_TOOLBOX].value = 0;
    g_widgets[W_TOOLBOX].flags &= ~SXGUI_FLAG_VISIBLE;
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
    g_app.on_pointer = on_pointer;
    g_app.on_paint = on_paint;
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
    /* The scenario's console does not reach the serial log; the system log does. */
    text[strcspn(text, "\n")] = '\0';
    (void)sxn_log(text);
    g_failures++;
}

/* To the program's end, going on from every stop. */
static void wait_child(void)
{
    while (g_child_kind != CHILD_NONE) {
        if (g_debug_reports >= 0) {
            if (poll_reports() == 1) debug_resume("go");
        } else {
            poll_child();
        }
    }
}

/* To the next stop, or to the program's end. */
static void wait_stop(void)
{
    while (g_child_kind != CHILD_NONE) {
        if (g_debug_reports >= 0) {
            if (poll_reports() == 1) return;
        } else {
            poll_child();
        }
    }
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

    if (strcmp(name, "breakpoint") == 0) {
        char file[IDE_NAME_MAX];
        int at = 0, index;
        if (sscanf(argument, "%127s %d", file, &at) != 2 || (index = find_file(file)) < 0) {
            check_failed(line, "no such file or line", argument);
        } else {
            toggle_breakpoint(index, at);
        }
    } else if (strcmp(name, "start") == 0 || strcmp(name, "continue") == 0 || strncmp(name, "step-", 5) == 0) {
        const char *debug = name[0] != 's' ? "go" : strcmp(name, "start") == 0 ? "go"
                          : strcmp(name, "step-into") == 0 ? "step" : strcmp(name, "step-over") == 0 ? "over" : "out";
        if (name[0] == 'c' && !g_paused) check_failed(line, "not stopped", NULL);
        if (strcmp(name, "step-out") == 0) debug_resume(debug);
        else debug_command(debug);
        wait_stop();
    } else if (strcmp(name, "add-watch") == 0) {
        add_watch(argument);
        /* Stopped, the program reports again with the new watch. */
        if (g_paused) wait_stop();
    } else if (strcmp(name, "expect-stop") == 0) {
        char file[IDE_NAME_MAX];
        int at = 0;
        int caret = g_current >= 0 ? line_of(document(g_current), editor(g_current)->caret) : 0;
        if (sscanf(argument, "%127s %d", file, &at) != 2 || !g_paused || strcmp(g_stop_file, file) != 0 ||
            g_stop_line != at || g_current < 0 || strcmp(g_files[g_current].name, file) != 0 || caret != at ||
            g_mark_line != at || g_mark_file != g_current) {
            snprintf(detail, sizeof(detail), "%s at %s line %d (%s), the caret on line %d", g_paused ? "stopped" : "not stopped",
                     g_stop_file, g_stop_line, g_stop_reason, caret);
            check_failed(line, "stop", detail);
        }
    } else if (strcmp(name, "expect-stop-error") == 0) {
        if (!g_paused || strcmp(g_stop_reason, "error") != 0 || g_stop_error != atoi(argument)) {
            snprintf(detail, sizeof(detail), "%s %d %s", g_stop_reason, g_stop_error, g_stop_description);
            check_failed(line, "error stop", detail);
        }
    } else if (strcmp(name, "expect-local") == 0 || strcmp(name, "expect-watch") == 0) {
        char wanted[IDE_OUT_WIDTH];
        char *value = strchr(argument, ' ');
        int found = 0, locals = name[7] == 'l';
        if (value) *value++ = '\0';
        snprintf(wanted, sizeof(wanted), "%s = %s", argument, value ? value : "");
        for (int i = 0; i < (locals ? g_locals_count : g_watch_count); i++) {
            if (strcmp(locals ? g_locals[i] : g_watch_text[i], wanted) == 0) found = 1;
        }
        if (!found) {
            /* What the pane shows instead. */
            size_t used = (size_t)snprintf(detail, sizeof(detail), "%s; the pane has", wanted);
            for (int i = 0; i < (locals ? g_locals_count : g_watch_count) && used < sizeof(detail); i++) {
                used += (size_t)snprintf(detail + used, sizeof(detail) - used, " [%s]", locals ? g_locals[i] : g_watch_text[i]);
            }
            check_failed(line, locals ? "no such local" : "no such watch", detail);
        }
    } else if (strcmp(name, "new") == 0) {
        if (new_project(argument) != 0) check_failed(line, "new project", g_message);
    } else if (strcmp(name, "view") == 0) {
        command(strcmp(argument, "object") == 0 ? CMD_VIEW_OBJECT : CMD_VIEW_CODE);
    } else if (strcmp(name, "tool") == 0) {
        int tool = -1;
        for (int i = 0; i < IDE_TOOL_COUNT; i++) {
            if (strcmp(g_tool_rows[i], argument) == 0) tool = i;
        }
        if (tool < 0) check_failed(line, "no such tool", argument);
        else g_widgets[W_TOOLBOX].value = tool;
    } else if (strcmp(name, "drag") == 0 || strcmp(name, "click") == 0 || strcmp(name, "dblclick") == 0) {
        /* Points in the form's client area, as pointer events in the window. */
        int x0 = 0, y0 = 0, x1, y1;
        struct sx_rect outer, client;
        if (sscanf(argument, "%d %d %d %d", &x0, &y0, &x1, &y1) != (name[0] == 'd' && name[1] == 'r' ? 4 : 2)) {
            check_failed(line, "bad points", argument);
        } else if (!designer_active()) {
            check_failed(line, "the designer is not shown", NULL);
        } else {
            design_parse();
            design_geometry(&outer, &client);
            if (name[1] != 'r') {
                x1 = x0;
                y1 = y0;
            }
            for (int times = name[0] == 'd' && name[1] == 'b' ? 2 : 1; times > 0; times--) {
                struct savanxp_gui_pointer_event press = {client.x + x0, client.y + y0, 0, 1};
                struct savanxp_gui_pointer_event move = {client.x + x1, client.y + y1, 0, 1};
                struct savanxp_gui_pointer_event release = {client.x + x1, client.y + y1, 0, 0};
                (void)designer_pointer(&press);
                (void)designer_pointer(&move);
                (void)designer_pointer(&release);
            }
        }
    } else if (strcmp(name, "key") == 0) {
        struct savanxp_input_event event = {SAVANXP_INPUT_EVENT_KEY_DOWN, 0, 0, 0};
        event.key = strcmp(argument, "Delete") == 0 ? SAVANXP_KEY_DELETE : strcmp(argument, "Escape") == 0 ? SAVANXP_KEY_ESC : 0;
        if (event.key == 0 || !on_key(NULL, &event)) check_failed(line, "the key was not taken", argument);
    } else if (strcmp(name, "run-script") == 0) {
        g_child_script = argument;
        if (start_child(CHILD_RUN) != 0) check_failed(line, "cannot start", g_message);
        wait_child();
        g_child_script = NULL;
    } else if (strcmp(name, "expect-view") == 0) {
        if (designer_active() != (strcmp(argument, "object") == 0)) check_failed(line, "view", designer_active() ? "object" : "code");
    } else if (strcmp(name, "expect-selected") == 0) {
        design_parse();
        int index = design_selected();
        if (index < 0 || strcmp(g_controls[index].name, argument) != 0) {
            check_failed(line, "selected", index >= 0 ? g_controls[index].name : "(none)");
        }
    } else if (strcmp(name, "expect-control") == 0 || strcmp(name, "expect-no-control") == 0) {
        char control_name[48];
        int x = 0, y = 0, w = 0, h = 0;
        int fields = sscanf(argument, "%47s %d %d %d %d", control_name, &x, &y, &w, &h);
        design_parse();
        int index = find_control(control_name);
        if (name[7] == 'n') {
            if (index >= 0) check_failed(line, "the control is still there", control_name);
        } else if (index < 0 || fields != 5) {
            check_failed(line, "no such control", control_name);
        } else {
            struct sx_rect rect = index == 0 ? sx_rect_make(0, 0, (int)g_controls[0].values[SXB_P_WIDTH],
                                                            (int)g_controls[0].values[SXB_P_HEIGHT])
                                             : control_rect(&g_controls[index]);
            if (rect.x != x || rect.y != y || rect.width != w || rect.height != h) {
                snprintf(detail, sizeof(detail), "%s at %d %d %d %d", control_name, rect.x, rect.y, rect.width, rect.height);
                check_failed(line, "rectangle", detail);
            }
        }
    } else if (strcmp(name, "open") == 0) {
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
    designer_paint();
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
