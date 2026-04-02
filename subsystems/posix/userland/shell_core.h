#pragma once

#include <stddef.h>

#include "libc.h"

enum shell_execute_mode {
    SHELL_EXEC_STDIO = 0,
    SHELL_EXEC_CAPTURE = 1,
};

enum shell_execute_result {
    SHELL_EXEC_RESULT_OK = 0,
    SHELL_EXEC_RESULT_EXIT = 1,
    SHELL_EXEC_RESULT_ERROR = 2,
};

struct shell_capture_sink {
    void (*emit)(void* context, int fd, const char* bytes, size_t length);
    void (*clear)(void* context);
    void* context;
};

void shell_trim_newline(char* line);
void shell_current_directory(char* buffer, size_t capacity);
void shell_print_help(const struct shell_capture_sink* sink);
int shell_execute_line(char* line, enum shell_execute_mode mode, const struct shell_capture_sink* sink);
