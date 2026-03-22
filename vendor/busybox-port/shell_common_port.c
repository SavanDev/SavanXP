#include "libbb.h"
#include "../busybox/shell/shell_common.h"

const char defifsvar[] ALIGN1 = "IFS= \t\n";
const char defoptindvar[] ALIGN1 = "OPTIND=1";

int FAST_FUNC varcmp(const char* left, const char* right) {
    int lhs = 0;
    int rhs = 0;

    while ((lhs = *left) == (rhs = *right)) {
        if (lhs == '\0' || lhs == '=') {
            return 0;
        }
        left += 1;
        right += 1;
    }
    if (lhs == '=') {
        lhs = '\0';
    }
    if (rhs == '=') {
        rhs = '\0';
    }
    return lhs - rhs;
}

static const char* shell_read_ifs(const struct builtin_read_params* params) {
    return (params != NULL && params->ifs != NULL) ? params->ifs : defifs;
}

static int shell_is_ifs_char(char character, const char* ifs) {
    return character != '\0' && strchr(ifs, character) != NULL;
}

static void shell_assign_read_fields(
    const struct builtin_read_params* params,
    char* buffer
) {
    char** name = params->argv;
    const char* ifs = shell_read_ifs(params);
    char* cursor = buffer;

    if (name == NULL || *name == NULL) {
        params->setvar("REPLY", buffer);
        return;
    }

    while (name[1] != NULL) {
        char* start = NULL;
        while (shell_is_ifs_char(*cursor, ifs)) {
            cursor += 1;
        }
        start = cursor;
        while (*cursor != '\0' && !shell_is_ifs_char(*cursor, ifs)) {
            cursor += 1;
        }
        if (*cursor != '\0') {
            *cursor++ = '\0';
        }
        params->setvar(*name++, start);
    }

    while (shell_is_ifs_char(*cursor, ifs)) {
        cursor += 1;
    }
    params->setvar(*name++, cursor);
    while (*name != NULL) {
        params->setvar(*name++, "");
    }
}

const char* FAST_FUNC shell_builtin_read(struct builtin_read_params* params) {
    enum { SHELL_READ_CAPACITY = 4096 };
    char* buffer = NULL;
    size_t length = 0;
    size_t limit = SHELL_READ_CAPACITY - 1;
    int fd = STDIN_FILENO;
    int saw_data = 0;

    if (params == NULL || params->setvar == NULL) {
        return "invalid read parameters";
    }

    if (params->opt_u != NULL) {
        fd = (int)bb_strtou(params->opt_u, NULL, 10);
        if (errno != 0) {
            return "invalid file descriptor";
        }
    }

    if (params->opt_n != NULL) {
        limit = bb_strtou(params->opt_n, NULL, 10);
        if (errno != 0) {
            return "invalid count";
        }
        if (limit >= SHELL_READ_CAPACITY) {
            limit = SHELL_READ_CAPACITY - 1;
        }
    }

    if ((params->read_flags & BUILTIN_READ_SILENT) != 0) {
        return "read -s is not supported yet";
    }
    if (params->opt_t != NULL) {
        return "read -t is not supported yet";
    }
    if (params->opt_p != NULL && isatty(fd)) {
        fdprintf(STDERR_FILENO, "%s", params->opt_p);
    }

    buffer = xzalloc(SHELL_READ_CAPACITY);
    while (length < limit) {
        char character = '\0';
        ssize_t result = read(fd, &character, 1);
        if (result < 0) {
            free(buffer);
            return (errno == EINTR) ? (const char*)(uintptr_t)1 : "read error";
        }
        if (result == 0) {
            break;
        }
        saw_data = 1;
        if (params->opt_d != NULL && params->opt_d[0] != '\0') {
            if (character == params->opt_d[0]) {
                break;
            }
        } else if (character == '\n') {
            break;
        }
        buffer[length++] = character;
    }
    buffer[length] = '\0';

    if (!saw_data && length == 0) {
        free(buffer);
        return (const char*)(uintptr_t)1;
    }

    shell_assign_read_fields(params, buffer);
    free(buffer);
    return NULL;
}

int FAST_FUNC shell_builtin_ulimit(char** argv) {
    (void)argv;
    bb_simple_error_msg("ulimit: not supported yet");
    return 1;
}
