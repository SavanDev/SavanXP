/* sxbasic: runs Savan BASIC projects in SavanXP and makes executables of them
 * (docs/userland/SXBASIC.md), and is the stub a made executable is a copy of
 * (docs/userland/SXBASIC_IMAGE.md).
 *
 *   sxbasic <project.sbp> [arguments...]
 *   sxbasic --expect <expected output> <project.sbp>
 *   sxbasic --selftest <script> <project.sbp>
 *   sxbasic --make <project.sbp> <program.sxe>
 *   sxbasic --make-selftest <script> <project.sbp> <program.sxe>
 *
 * The second form keeps what a console program prints and compares it with a
 * file: the sxbasic-smoke scenario runs the hello sample that way. The third
 * runs a Windows program with no window, doing what the script says a user
 * does and checking what it says to expect (sxbasic-forms-smoke). The fourth
 * is Make SXE; the fifth makes the executable, then starts it with --selftest
 * and passes on how it ended (sxbasic-make-smoke).
 *
 * A made executable is this program with the compiled project in its
 * .sxbasic section: started, it finds that section in its own file and runs
 * it, with its title, accent and icon from its own .sxmeta and .sxicon. Paths
 * are absolute, as SUN's are. */
#include "core/sxbasic.h"
#include "core/sxb_internal.h"
#include "forms.h"
#include "sxn/sxngui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savanxp/img.h"
#include "sxe/sxe_format.h"
#include "sxn/sxn.h"

/* Where the system keeps the stub Make SXE copies: this program. */
#define SXBASIC_STUB_PATH "/system/sxbasic.sxe"

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
/* A made executable's accent and icon, from its own file; empty otherwise. */
static struct sxnwm_presentation_source g_presentation;
static uint32_t g_icon[SAVANXP_WM_PRESENTATION_ICON_EXTENT * SAVANXP_WM_PRESENTATION_ICON_EXTENT];
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
    struct sxnwm_presentation_source presentation = g_presentation;
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
    } else if (strcmp(command, "expect-accent") == 0) {
        /* The accent the window was given, read from the executable's own file. */
        char got[16] = "none";
        if (g_presentation.flags & SAVANXP_WM_PRESENTATION_HAS_ACCENT) {
            snprintf(got, sizeof(got), "%06X", (unsigned)g_presentation.accent);
        }
        if (strcmp(got, path) != 0) check_failed(line, "accent", got);
    } else if (strcmp(command, "expect-icon") == 0) {
        /* The window icon's pixel at x y, as AARRGGBB. */
        char got[16] = "no icon";
        int x = atoi(path), y = atoi(rest), side = (int)g_presentation.icon_extent;
        if (side > 0 && x >= 0 && y >= 0 && x < side && y < side) {
            snprintf(got, sizeof(got), "%08X", (unsigned)g_presentation.icon_pixels[y * side + x]);
        }
        const char *expected = strchr(rest, ' ');
        if (expected == NULL || strcmp(got, expected + 1) != 0) check_failed(line, "icon pixel", got);
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
           "       sxbasic --selftest <script> <project.sbp>\n"
           "       sxbasic --make <project.sbp> <program.sxe>\n"
           "       sxbasic --make-selftest <script> <project.sbp> <program.sxe>\n");
}

static void print_diagnostic(const struct sxb_diagnostic *diag)
{
    char line[512];
    if (diag->line > 0) {
        snprintf(line, sizeof(line), "%s:%d:%d: %s\n", diag->file, diag->line, diag->column, diag->message);
    } else {
        snprintf(line, sizeof(line), "%s: %s\n", diag->file, diag->message);
    }
    put(2, line);
}

/* Runs a compiled program to its end. `expect` and `script` as in usage. */
static int run_program(struct sxb_program *program, const char *expect, const char *script, const char *arguments,
                       const char *directory)
{
    char line[512];
    g_capturing = expect != NULL;
    struct sxb_vm *vm = sxb_vm_create(program, &g_host, arguments, directory);
    if (vm == NULL) {
        put(2, "sxbasic: out of memory\n");
        return 1;
    }
    if (sxb_program_is_windows(program)) {
        int windows_code = run_windows(program, vm, script);
        sxb_vm_destroy(vm);
        return windows_code;
    }
    if (script != NULL) {
        put(2, "sxbasic: --selftest is for Windows programs\n");
        sxb_vm_destroy(vm);
        return 2;
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

    if (expect != NULL) {
        char *expected = NULL;
        size_t size = 0;
        int error = host_read_file(NULL, expect, &expected, &size);
        if (error != 0) {
            snprintf(line, sizeof(line), "sxbasic: cannot read %s (error %d)\n", expect, error);
            put(2, line);
            return 1;
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
    return code;
}

/* `argv[first..]` joined by spaces: what Command() returns. */
static void join_arguments(char *out, size_t size, int argc, char **argv, int first)
{
    out[0] = '\0';
    for (int i = first; i < argc; i++) {
        size_t used = strlen(out);
        snprintf(out + used, size - used, "%s%s", used ? " " : "", argv[i]);
    }
}

static void directory_of(char *out, size_t size, const char *path)
{
    const char *slash = strrchr(path, '/');
    snprintf(out, size, "%.*s", slash == path ? 1 : (int)(slash - path), path);
}

/* ---- the stub: a made executable reads its own file ------------------------------- */

/* The whole of a file, mapped read-only; NULL when it cannot be. The mapping
 * and its handles live as long as the process. */
static const unsigned char *map_file(const char *path, size_t *size)
{
    struct sxn_file_info info;
    if (path == NULL || path[0] != '/' || sxn_query_path(path, &info) < 0 || info.type != SXN_FILE_TYPE_FILE ||
        info.size == 0) {
        return NULL;
    }
    long file = sxn_open_file(path, SXN_FILE_READ);
    if (file < 0) return NULL;
    long section = sxn_section_open_range(file, 0, 0, SXN_SECTION_READ);
    long base = section < 0 ? -1 : sxn_map_view(section, SXN_SECTION_READ);
    if (base <= 0) {
        if (section >= 0) (void)sxn_close(section);
        (void)sxn_close(file);
        return NULL;
    }
    *size = (size_t)info.size;
    return (const unsigned char *)base;
}

static void load_presentation(const unsigned char *self, size_t size)
{
    const unsigned char *bytes;
    size_t length;
    if (sxb_elf_section(self, size, SXE_SECTION_META, &bytes, &length) &&
        sxb_exe_meta_accent(bytes, length, &g_presentation.accent)) {
        g_presentation.flags |= SAVANXP_WM_PRESENTATION_HAS_ACCENT;
    }
    if (sxb_elf_section(self, size, SXE_SECTION_ICON, &bytes, &length) &&
        sxb_exe_icon(bytes, length, SAVANXP_WM_PRESENTATION_ICON_EXTENT, g_icon)) {
        g_presentation.icon_extent = SAVANXP_WM_PRESENTATION_ICON_EXTENT;
        g_presentation.icon_pixels = g_icon;
    }
}

/* A made executable: `sxe [--selftest <script>] [arguments...]`. */
static int run_made(int argc, char **argv, const unsigned char *self, size_t self_size, const unsigned char *image,
                    size_t image_size)
{
    struct sxb_diagnostic diag;
    struct sxb_program *program = sxb_program_load(image, image_size, &diag);
    if (program == NULL) {
        snprintf(diag.file, sizeof(diag.file), "%s", argv[0]);
        print_diagnostic(&diag);
        return 1;
    }
    load_presentation(self, self_size);
    const char *script = NULL;
    int first = 1;
    if (argc >= 3 && strcmp(argv[1], "--selftest") == 0) {
        script = argv[2];
        first = 3;
    }
    char arguments[1024], directory[512];
    join_arguments(arguments, sizeof(arguments), argc, argv, first);
    directory_of(directory, sizeof(directory), argv[0]);
    int code = run_program(program, NULL, script, arguments, directory);
    sxb_program_free(program);
    return code;
}

/* ---- Make SXE ---------------------------------------------------------------------- */

/* A side x side icon from one of the project's PNGs: the one drawn at that
 * size, else the first scaled by a whole factor, pixel by pixel, as
 * gen_sxe_resources.py does for a .sxres's icon_file. */
static int icon_at(const struct sx_png_image *pngs, int count, int side, uint32_t *out)
{
    for (int i = 0; i < count; i++) {
        if ((int)pngs[i].width == side && (int)pngs[i].height == side) {
            memcpy(out, pngs[i].pixels, sizeof(*out) * (size_t)side * (size_t)side);
            return 1;
        }
    }
    int w = (int)pngs[0].width, h = (int)pngs[0].height;
    if (w != h || (w % side != 0 && side % w != 0)) return 0;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) out[y * side + x] = pngs[0].pixels[(y * w / side) * w + x * w / side];
    }
    return 1;
}

static int make_failed(const char *project, const char *what)
{
    char line[512];
    snprintf(line, sizeof(line), "sxbasic: %s: %s\n", project, what);
    put(2, line);
    return 1;
}

static int write_program(const char *path, const unsigned char *bytes, size_t size)
{
    long handle = sxn_create_file(path, SXN_CREATE_ALWAYS | SXN_CREATE_EXECUTABLE);
    if (handle < 0) return (int)handle;
    for (size_t done = 0; done < size;) {
        long n = sxn_write(handle, bytes + done, (unsigned long)(size - done));
        if (n <= 0) {
            (void)sxn_close(handle);
            return n < 0 ? (int)n : -1;
        }
        done += (size_t)n;
    }
    long flushed = sxn_flush_file(handle);
    (void)sxn_close(handle);
    return flushed < 0 ? (int)flushed : 0;
}

static int make(const char *project, const char *output)
{
    char line[512], message[128] = "";
    if (project[0] != '/' || output[0] != '/') return make_failed(project, "the paths have to be absolute");
    struct sxb_diagnostic diag;
    struct sxb_program *program = sxb_compile_project(project, &g_host, &diag);
    if (program == NULL) {
        print_diagnostic(&diag);
        return 1;
    }
    const struct sxb_project_info *info = sxb_program_info(program);
    unsigned char *image = NULL, *meta = NULL, *icons = NULL, *made = NULL;
    size_t image_size = 0, meta_size = 0, icons_size = 0, made_size = 0, stub_size = 0;
    struct sx_png_image pngs[SXB_MAX_ICONS];
    int png_count = 0, code = 1;
    static uint32_t small[16 * 16], large[32 * 32];
    memset(pngs, 0, sizeof(pngs));

    char directory[512];
    directory_of(directory, sizeof(directory), project);
    for (int i = 0; i < info->icon_count; i++) {
        char path[640];
        char *data = NULL;
        size_t size = 0;
        snprintf(path, sizeof(path), "%s/%s", directory, info->icons[i]);
        if (host_read_file(NULL, path, &data, &size) != 0) {
            snprintf(message, sizeof(message), "cannot read the icon %s", info->icons[i]);
            goto done;
        }
        int decoded = sx_png_decode_argb((const unsigned char *)data, size, &pngs[png_count]);
        free(data);
        if (decoded != 0) {
            snprintf(message, sizeof(message), "%s is not a PNG this system reads", info->icons[i]);
            goto done;
        }
        png_count++;
    }
    if (png_count > 0) {
        struct sxb_icon_image images[2] = {{16, small}, {32, large}};
        if (!icon_at(pngs, png_count, 16, small) || !icon_at(pngs, png_count, 32, large)) {
            snprintf(message, sizeof(message), "an icon is drawn at 16x16 and 32x32, or square at a whole multiple");
            goto done;
        }
        if (sxb_exe_icons(images, 2, &icons, &icons_size, message) != 0) goto done;
    }
    if (sxb_program_save(program, &image, &image_size) != 0) {
        snprintf(message, sizeof(message), "out of memory");
        goto done;
    }
    if (sxb_exe_meta(sxb_program_name(program), info, &meta, &meta_size, message) != 0) goto done;
    const unsigned char *stub = map_file(SXBASIC_STUB_PATH, &stub_size);
    if (stub == NULL) {
        snprintf(message, sizeof(message), "cannot read the stub %s", SXBASIC_STUB_PATH);
        goto done;
    }
    struct sxb_elf_add sections[3] = {{SXB_IMAGE_SECTION, image, image_size},
                                      {SXE_SECTION_META, meta, meta_size},
                                      {SXE_SECTION_ICON, icons, icons_size}};
    if (sxb_elf_add_sections(stub, stub_size, sections, icons ? 3 : 2, &made, &made_size, message) != 0) goto done;
    int written = write_program(output, made, made_size);
    if (written != 0) {
        snprintf(message, sizeof(message), "cannot write %s (error %d)", output, written);
        goto done;
    }
    snprintf(line, sizeof(line), "sxbasic: made %s (%zu bytes, its program %zu)\n", output, made_size, image_size);
    put(1, line);
    code = 0;
done:
    if (code != 0) make_failed(project, message);
    for (int i = 0; i < png_count; i++) sx_png_free(&pngs[i]);
    free(image);
    free(meta);
    free(icons);
    free(made);
    sxb_program_free(program);
    return code;
}

/* Makes the executable, starts it with its selftest and waits for it: Make
 * EXE and the made program, as a user would run them, in one scenario. */
static int make_selftest(const char *script, const char *project, const char *output)
{
    char line[512];
    if (make(project, output) != 0) return 1;
    char *child_argv[] = {(char *)output, (char *)"--selftest", (char *)script, NULL};
    long pid = sxn_spawn_stdio(output, child_argv, 3);
    if (pid < 0) {
        snprintf(line, sizeof(line), "sxbasic: cannot start %s (error %ld)\n", output, pid);
        put(2, line);
        return 1;
    }
    int status = 1;
    if (sxn_waitpid(pid, &status) < 0) return 1;
    snprintf(line, sizeof(line), "sxbasic: %s ended with %d\n", output, status);
    put(1, line);
    return status;
}

int main(int argc, char **argv)
{
    /* A made executable carries its program in its own file. */
    size_t self_size = 0;
    const unsigned char *self = argc > 0 ? map_file(argv[0], &self_size) : NULL;
    const unsigned char *image = NULL;
    size_t image_size = 0;
    if (self != NULL && sxb_elf_section(self, self_size, SXB_IMAGE_SECTION, &image, &image_size)) {
        sxn_exit(run_made(argc, argv, self, self_size, image, image_size));
    }
    if (argc == 4 && strcmp(argv[1], "--make") == 0) sxn_exit(make(argv[2], argv[3]));
    if (argc == 5 && strcmp(argv[1], "--make-selftest") == 0) sxn_exit(make_selftest(argv[2], argv[3], argv[4]));

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
    if (first >= argc || argv[first][0] == '-') {
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
        print_diagnostic(&diag);
        sxn_exit(1);
    }
    char arguments[1024], directory[512];
    join_arguments(arguments, sizeof(arguments), argc, argv, first + 1);
    directory_of(directory, sizeof(directory), project);
    int code = run_program(program, expect, script, arguments, directory);
    sxb_program_free(program);
    sxn_exit(code);
}
