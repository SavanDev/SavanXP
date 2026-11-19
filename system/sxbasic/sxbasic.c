/* sxbasic: runs a Savan BASIC console project in SavanXP
 * (docs/userland/SXBASIC.md, phase 1 of docs/roadmaps/open/SXBASIC_ROADMAP.md).
 *
 *   sxbasic <project.sbp> [arguments...]
 *   sxbasic --expect <expected output> <project.sbp>
 *
 * The second form keeps what the program prints and compares it with a file:
 * the sxbasic-smoke scenario runs the hello sample that way. Paths are
 * absolute, as SUN's are. */
#include "core/sxbasic.h"

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

static void usage(void)
{
    put(2, "usage: sxbasic <project.sbp> [arguments...]\n"
           "       sxbasic --expect <expected output> <project.sbp>\n");
}

int main(int argc, char **argv)
{
    const char *expect = NULL;
    int first = 1;
    if (argc >= 2 && strcmp(argv[1], "--expect") == 0) {
        if (argc != 4) {
            usage();
            sxn_exit(2);
        }
        expect = argv[2];
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
    if (sxb_program_is_windows(program)) {
        put(2, "sxbasic: this is a Windows program; this SxBASIC runs console programs only\n");
        sxb_program_free(program);
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
