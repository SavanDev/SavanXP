/* sxbasic: runs a Savan BASIC console project in SavanXP
 * (docs/userland/SXBASIC.md, phase 1 of docs/roadmaps/open/SXBASIC_ROADMAP.md).
 *
 *   sxbasic <project.sbp> [arguments...]
 *   sxbasic --expect <expected output> <project.sbp>
 *   sxbasic --selftest <script> <project.sbp>
 *
 * The second form keeps what a console program prints and compares it with a
 * file: the sxbasic-smoke scenario runs the hello sample that way. The third
 * runs a Windows program with no window, doing what the script says a user
 * does and checking what it says to expect (sxbasic-forms-smoke). Paths are
 * absolute, as SUN's are. */
#include "core/sxbasic.h"
#include "core/sxb_internal.h"
#include "forms.h"
#include "sxn/sxngui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxn/sxn.h"

static char *g_capture;
static size_t g_capture_length;
static size_t g_capture_capacity;
static int g_capturing;

static void put(long handle, const char *text)
{
    (void)sxn_write(handle, text, strlen(text));
}

static void host_write(void *user, const char *text, size_t length)
{
    (void)user;
    if (!g_capturing) {
        (void)sxn_write(1, text, length);
        return;
    }
    if (g_capture_length + length + 1u > g_capture_capacity) {
        size_t capacity = (g_capture_capacity ? g_capture_capacity * 2u : 4096u) + length;
        char *grown = realloc(g_capture, capacity);
        if (grown == NULL) return;
        g_capture = grown;
        g_capture_capacity = capacity;
    }
    memcpy(g_capture + g_capture_length, text, length);
    g_capture_length += length;
    g_capture[g_capture_length] = '\0';
}

static int file_error(long result)
{
    switch (-result) {
    case SAVANXP_ENOENT: return SXB_ERR_FILE_NOT_FOUND;
    case SAVANXP_EACCES: return SXB_ERR_PERMISSION;
    case SAVANXP_EISDIR:
    case SAVANXP_EINVAL: return SXB_ERR_PATH;
    default: return SXB_ERR_IO;
    }
}

static int host_read_file(void *user, const char *path, char **data, size_t *size)
{
    (void)user;
    struct sxn_file_info info;
    long result = sxn_query_path(path, &info);
    if (result < 0) return file_error(result);
    if (info.type != SXN_FILE_TYPE_FILE) return SXB_ERR_PATH;
    long handle = sxn_open_file(path, SXN_FILE_READ);
    if (handle < 0) return file_error(handle);
    size_t capacity = (size_t)info.size + 1u, used = 0;
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        (void)sxn_close(handle);
        return SXB_ERR_OUT_OF_MEMORY;
    }
    for (;;) {
        if (used + 1u >= capacity) {
            char *grown = realloc(buffer, capacity * 2u);
            if (grown == NULL) {
                free(buffer);
                (void)sxn_close(handle);
                return SXB_ERR_OUT_OF_MEMORY;
            }
            buffer = grown;
            capacity *= 2u;
        }
        long n = sxn_read(handle, buffer + used, capacity - used - 1u);
        if (n < 0) {
            free(buffer);
            (void)sxn_close(handle);
            return file_error(n);
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

static int host_write_file(void *user, const char *path, const char *data, size_t size)
{
    (void)user;
    long handle = sxn_create_file(path, SXN_CREATE_ALWAYS);
    if (handle < 0) return file_error(handle);
    size_t done = 0;
    while (done < size) {
        long n = sxn_write(handle, data + done, size - done);
        if (n <= 0) {
            (void)sxn_close(handle);
            return n < 0 ? file_error(n) : SXB_ERR_IO;
        }
        done += (size_t)n;
    }
    long flushed = sxn_flush_file(handle);
    (void)sxn_close(handle);
    return flushed < 0 ? file_error(flushed) : 0;
}

static int host_file_exists(void *user, const char *path)
{
    (void)user;
    struct sxn_file_info info;
    return sxn_query_path(path, &info) == 0 && info.type == SXN_FILE_TYPE_FILE;
}

static long host_read_line(void *user, char **line)
{
    (void)user;
    size_t capacity = 128, used = 0;
    char *buffer = malloc(capacity);
    if (buffer == NULL) return -1;
    for (;;) {
        char c;
        long n = sxn_read(0, &c, 1);
        if (n <= 0) {
            if (used == 0) {
                free(buffer);
                return -1;
            }
            break;
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (used + 1u >= capacity) {
            char *grown = realloc(buffer, capacity * 2u);
            if (grown == NULL) break;
            buffer = grown;
            capacity *= 2u;
        }
        buffer[used++] = c;
    }
    buffer[used] = '\0';
    *line = buffer;
    return (long)used;
}

static uint64_t host_clock(void *user)
{
    (void)user;
    return sxn_clock_ns();
}

static const struct sxb_host g_host = {NULL, host_write, host_read_file, host_write_file, host_file_exists,
                                       host_read_line, host_clock};

/* ---- Windows programs ----------------------------------------------------------------
 *
 * The forms runtime in a SUN window (sxngui_app), or with --selftest in a
 * surface of its own with no window at all, driven by a script of what a
 * user would do: the same widgets, the same events, the same dialogs. */

static struct sxngui_app g_app;
static int g_app_open;
static int g_headless;
static int g_quit;
static int g_exit_code;
static struct sxb_forms *g_forms_runtime;
static uint32_t *g_pixels;
static struct savanxp_fb_info g_info;
static struct sxgui_context g_context;

static void app_tick(struct sxngui_app *app)
{
    (void)app;
    sxb_forms_tick(g_forms_runtime);
}

static void app_close(struct sxngui_app *app)
{
    if (!sxb_forms_close_main(g_forms_runtime)) sxngui_app_decline_close(app);
}

static void app_dialog_closed(struct sxngui_app *app, struct sxgui_dialog *dialog, void *user)
{
    (void)app;
    (void)user;
    sxb_forms_dialog_closed(g_forms_runtime, dialog);
}

static struct sxgui_context *window_open(void *user, const char *title, int width, int height,
                                         struct sxgui_widget *widgets, int count)
{
    (void)user;
    if (g_headless) {
        g_pixels = calloc((size_t)width * (size_t)height, sizeof(*g_pixels));
        if (g_pixels == NULL) return NULL;
        g_info.width = (uint32_t)width;
        g_info.height = (uint32_t)height;
        g_info.pitch = (uint32_t)width * 4u;
        g_info.bpp = 32;
        g_info.buffer_size = g_info.pitch * (uint32_t)height;
        sxgui_set_host(sxngui_host());
        sxgui_context_init(&g_context, g_pixels, &g_info, widgets, count);
        return &g_context;
    }
    struct sxnwm_presentation_source presentation;
    memset(&presentation, 0, sizeof(presentation));
    presentation.title = title;
    if (sxngui_app_init(&g_app, &presentation, widgets, count) != 0) return NULL;
    g_app.tick_interval_ms = 20;
    g_app.on_tick = app_tick;
    g_app.on_close = app_close;
    g_app_open = 1;
    (void)sxngui_app_set_content_size(&g_app, width, height);
    return &g_app.ui;
}

static void window_set_title(void *user, const char *title)
{
    (void)user;
    if (g_app_open) (void)sxnwm_set_title(&g_app.wm, title);
}

static void window_repaint(void *user)
{
    (void)user;
    if (g_app_open) sxngui_app_request_repaint(&g_app);
}

static void window_watch(void *user, struct sxgui_dialog *dialog)
{
    (void)user;
    if (g_app_open) sxngui_app_watch_dialog(&g_app, dialog, app_dialog_closed, NULL);
}

static void window_quit(void *user, int code)
{
    (void)user;
    g_quit = 1;
    g_exit_code = code;
    if (g_app_open) sxngui_app_quit(&g_app, code);
}

static void window_fail(void *user, const char *message)
{
    (void)user;
    put(2, message);
    put(2, "\n");
}

static unsigned long window_now(void *user)
{
    (void)user;
    return sxngui_uptime_ms();
}

/* ---- the selftest script ----------------------------------------------------------- */

static int g_failures;

static void check_failed(int line, const char *what, const char *detail)
{
    char text[640];
    snprintf(text, sizeof(text), "SXBASIC SELFTEST: line %d: %s%s%s\n", line, what, detail ? ": " : "",
             detail ? detail : "");
    put(2, text);
    g_failures++;
}

static void fire_widget(struct sxgui_widget *widget, int action)
{
    widget->action = action;
    if (widget->on_action) widget->on_action(widget, widget->user);
}

static void run_command(struct sxb_forms *forms, int line, char *command)
{
    char *space = strchr(command, ' ');
    char *path = space ? space + 1 : (char *)"";
    if (space) *space = '\0';
    char *rest = strchr(path, ' ');
    if (rest) *rest++ = '\0';
    else rest = path + strlen(path);
    if (g_quit && strcmp(command, "expect-ended") != 0) {
        check_failed(line, "the program has already ended", NULL);
        return;
    }
    if (strcmp(command, "type") == 0 || strcmp(command, "click") == 0 || strcmp(command, "select") == 0) {
        struct sxgui_widget *widget = sxb_forms_widget(forms, path);
        if (widget == NULL) {
            check_failed(line, "no loaded control", path);
            return;
        }
        if (command[0] == 't') {
            if (widget->edit_buffer == NULL) {
                check_failed(line, "not a text box", path);
                return;
            }
            snprintf(widget->edit_buffer, (size_t)widget->edit_capacity, "%s", rest);
            widget->caret = (int)strlen(widget->edit_buffer);
            fire_widget(widget, SXGUI_ACTION_CHANGE);
        } else if (command[0] == 's') {
            widget->value = atoi(rest);
            fire_widget(widget, SXGUI_ACTION_CHANGE);
        } else {
            if (widget->kind == SXGUI_CHECKBOX) widget->value = !widget->value;
            if (widget->kind == SXGUI_RADIO) widget->value = 1;
            fire_widget(widget, SXGUI_ACTION_CLICK);
        }
    } else if (strcmp(command, "menu") == 0 || strcmp(command, "tick") == 0) {
        int form, control;
        if (!sxb_forms_find(forms, path, &form, &control) || control < 0) {
            check_failed(line, "no such control", path);
            return;
        }
        sxb_forms_fire(forms, form, control, command[0] == 'm' ? SXB_EV_CLICK : SXB_EV_TICK);
    } else if (strcmp(command, "expect") == 0) {
        char *dot = strrchr(path, '.');
        int form, control, property = dot ? sxb_find_property(dot + 1, (int)strlen(dot + 1)) : -1;
        if (dot) *dot = '\0';
        char got[512];
        if (property < 0 || !sxb_forms_find(forms, path, &form, &control)) {
            check_failed(line, "no such form, control or property", path);
        } else if (sxb_forms_property_text(forms, form, control, property, got, sizeof(got)) != 0) {
            check_failed(line, "the property cannot be read", path);
        } else if (strcmp(got, rest) != 0) {
            char detail[600];
            snprintf(detail, sizeof(detail), "expected \"%s\", found \"%s\"", rest, got);
            check_failed(line, path, detail);
        }
    } else if (strcmp(command, "expect-message") == 0) {
        /* The text after the command, with | for a line end. */
        char expected[512];
        snprintf(expected, sizeof(expected), "%s%s%s", path, *rest ? " " : "", rest);
        for (char *p = expected; *p; p++) if (*p == '|') *p = '\n';
        const char *shown = sxb_forms_message_text(forms);
        if (shown == NULL || strcmp(shown, expected) != 0) check_failed(line, "message box", shown ? shown : "(none)");
    } else if (strcmp(command, "answer") == 0) {
        int result = strcmp(path, "OK") == 0 ? 1 : strcmp(path, "Cancel") == 0 ? 2 : strcmp(path, "Yes") == 0 ? 6 : 7;
        if (sxb_forms_answer(forms, result, *rest ? rest : NULL) != 0) check_failed(line, "no message box to answer", path);
    } else if (strcmp(command, "expect-ended") == 0) {
        if (!g_quit) check_failed(line, "the program is still running", NULL);
    } else {
        check_failed(line, "unknown command", command);
        return;
    }
    if (!g_quit) sxb_forms_tick(forms);
}

static void run_script(struct sxb_forms *forms, const char *script)
{
    char *text = NULL;
    size_t size = 0;
    if (host_read_file(NULL, script, &text, &size) != 0) {
        check_failed(0, "cannot read the script", script);
        return;
    }
    int line = 0;
    for (char *p = text; *p;) {
        char *end = strchr(p, '\n');
        if (end) *end = '\0';
        line++;
        size_t n = strlen(p);
        if (n && p[n - 1] == '\r') p[--n] = '\0';
        if (n && p[0] != '#') run_command(forms, line, p);
        if (!end) break;
        p = end + 1;
    }
    free(text);
}

static int run_windows(struct sxb_program *program, struct sxb_vm *vm, const char *script)
{
    struct sxb_window window = {NULL, window_open, window_set_title, window_repaint, window_watch,
                                window_quit, window_fail, window_now};
    g_headless = script != NULL;
    g_forms_runtime = sxb_forms_create(program, vm, &window);
    if (g_forms_runtime == NULL) {
        put(2, "sxbasic: out of memory\n");
        return 1;
    }
    sxb_forms_start(g_forms_runtime);
    if (script != NULL) {
        if (!g_quit || g_exit_code == 0) run_script(g_forms_runtime, script);
        if (g_exit_code != 0) g_failures++;
        put(1, g_failures ? "SXBASIC SELFTEST: FAIL\n" : "SXBASIC SELFTEST: all checks passed\n");
        sxb_forms_destroy(g_forms_runtime);
        free(g_pixels);
        return g_failures ? 1 : 0;
    }
    if (!g_quit && g_app_open) g_exit_code = sxngui_app_run(&g_app);
    sxb_forms_destroy(g_forms_runtime);
    return g_exit_code;
}

static void usage(void)
{
    put(2, "usage: sxbasic <project.sbp> [arguments...]\n"
           "       sxbasic --expect <expected output> <project.sbp>\n"
           "       sxbasic --selftest <script> <project.sbp>\n");
}

int main(int argc, char **argv)
{
    const char *expect = NULL;
    const char *script = NULL;
    int first = 1;
    if (argc >= 2 && (strcmp(argv[1], "--expect") == 0 || strcmp(argv[1], "--selftest") == 0)) {
        if (argc != 4) {
            usage();
            sxn_exit(2);
        }
        if (argv[1][2] == 'e') expect = argv[2];
        else script = argv[2];
        first = 3;
    }
    if (first >= argc) {
        usage();
        sxn_exit(2);
    }
    const char *project = argv[first];
    char line[512];
    if (project[0] != '/') {
        snprintf(line, sizeof(line), "sxbasic: %s: the project's path has to be absolute\n", project);
        put(2, line);
        sxn_exit(2);
    }

    struct sxb_diagnostic diag;
    struct sxb_program *program = sxb_compile_project(project, &g_host, &diag);
    if (program == NULL) {
        if (diag.line > 0) {
            snprintf(line, sizeof(line), "%s:%d:%d: %s\n", diag.file, diag.line, diag.column, diag.message);
        } else {
            snprintf(line, sizeof(line), "%s: %s\n", diag.file, diag.message);
        }
        put(2, line);
        sxn_exit(1);
    }
    char arguments[1024] = "";
    for (int i = first + 1; i < argc; i++) {
        size_t used = strlen(arguments);
        snprintf(arguments + used, sizeof(arguments) - used, "%s%s", used ? " " : "", argv[i]);
    }
    char directory[512];
    const char *slash = strrchr(project, '/');
    snprintf(directory, sizeof(directory), "%.*s", slash == project ? 1 : (int)(slash - project), project);

    g_capturing = expect != NULL;
    struct sxb_vm *vm = sxb_vm_create(program, &g_host, arguments, directory);
    if (vm == NULL) {
        put(2, "sxbasic: out of memory\n");
        sxn_exit(1);
    }
    if (sxb_program_is_windows(program)) {
        int windows_code = run_windows(program, vm, script);
        sxb_vm_destroy(vm);
        sxb_program_free(program);
        sxn_exit(windows_code);
    }
    if (script != NULL) {
        put(2, "sxbasic: --selftest is for Windows programs\n");
        sxn_exit(2);
    }
    enum sxb_status status = sxb_vm_run(vm, 0);
    int code = 0;
    if (status == SXB_ERROR) {
        const struct sxb_runtime_error *error = sxb_vm_error(vm);
        snprintf(line, sizeof(line), "Runtime error %d: %s (%s, line %d)\n", error->number, error->description,
                 error->file, error->line);
        put(2, line);
        if (g_capturing) put(1, line);
        code = 1;
    }
    sxb_vm_destroy(vm);
    sxb_program_free(program);

    if (expect != NULL) {
        char *expected = NULL;
        size_t size = 0;
        int error = host_read_file(NULL, expect, &expected, &size);
        if (error != 0) {
            snprintf(line, sizeof(line), "sxbasic: cannot read %s (error %d)\n", expect, error);
            put(2, line);
            sxn_exit(1);
        }
        const char *got = g_capture ? g_capture : "";
        if (code == 0 && strcmp(expected, got) == 0) {
            put(1, "SXBASIC: output matches the expected output\n");
        } else {
            put(1, "SXBASIC: output differs; it printed:\n");
            put(1, got);
            put(1, "SXBASIC: expected:\n");
            put(1, expected);
            code = 1;
        }
        free(expected);
    }
    sxn_exit(code);
}
