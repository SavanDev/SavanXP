#include "savanxp_compat.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "savanxp/libc.h"

#define SX_SHUTDOWN_CAPACITY 8

static void (*g_shutdown_callbacks[SX_SHUTDOWN_CAPACITY])(void) = {};
static int g_shutdown_ran = 0;
static size_t g_shutdown_count = 0;

static void sx_run_shutdowns(void) {
    size_t index = g_shutdown_count;

    if (g_shutdown_ran) {
        return;
    }

    g_shutdown_ran = 1;
    while (index > 0) {
        --index;
        if (g_shutdown_callbacks[index] != 0) {
            g_shutdown_callbacks[index]();
        }
    }
}

int sx_parse_int(const char *text, int *result) {
    char *end = 0;
    long value = 0;

    if (text == 0 || result == 0) {
        return 0;
    }

    errno = 0;
    value = strtol(text, &end, 0);
    if (end == text || errno != 0) {
        return 0;
    }

    while (*end != '\0' && isspace((unsigned char)*end)) {
        ++end;
    }

    if (*end != '\0') {
        return 0;
    }

    *result = (int)value;
    return 1;
}

int sx_make_dirs(const char *path) {
    char partial[256];
    size_t length = 0;

    if (path == 0 || path[0] == '\0') {
        return 0;
    }

    length = strlen(path);
    if (length >= sizeof(partial)) {
        errno = ENOMEM;
        return -1;
    }

    memcpy(partial, path, length + 1);

    for (size_t index = 1; index < length; ++index) {
        int result = 0;

        if (partial[index] != '/') {
            continue;
        }

        partial[index] = '\0';
        result = mkdir(partial);
        partial[index] = '/';

        if (result < 0 && errno != EEXIST) {
            return -1;
        }
    }

    if (mkdir(partial) < 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

void sx_register_shutdown(void (*callback)(void)) {
    if (callback == 0 || g_shutdown_count >= SX_SHUTDOWN_CAPACITY) {
        return;
    }

    g_shutdown_callbacks[g_shutdown_count++] = callback;
}

void sx_shutdown_exit(int code) {
    sx_run_shutdowns();
    exit(code);
}
