#include "shell_core.h"

#include "shared/version.h"

#define SHELL_MAX_STAGES 8
#define SHELL_MAX_ARGS 16

struct shell_command_stage {
    char* argv[SHELL_MAX_ARGS];
    int argc;
    char* input_path;
    char* stdout_path;
    char* stderr_path;
    int stdout_append;
    int stderr_append;
    int stderr_to_stdout;
};

enum shell_pending_redirect {
    SHELL_REDIRECT_NONE = 0,
    SHELL_REDIRECT_STDIN = 1,
    SHELL_REDIRECT_STDOUT = 2,
    SHELL_REDIRECT_STDOUT_APPEND = 3,
    SHELL_REDIRECT_STDERR = 4,
    SHELL_REDIRECT_STDERR_APPEND = 5,
};

static void shell_emit_bytes(const struct shell_capture_sink* sink, int fd, const char* bytes, size_t length) {
    if (bytes == 0 || length == 0) {
        return;
    }

    if (sink != 0 && sink->emit != 0) {
        sink->emit(sink->context, fd, bytes, length);
        return;
    }

    (void)write(fd, bytes, length);
}

static void shell_emit_text(const struct shell_capture_sink* sink, int fd, const char* text) {
    if (text == 0) {
        return;
    }
    shell_emit_bytes(sink, fd, text, strlen(text));
}

static void shell_clear_output(const struct shell_capture_sink* sink) {
    if (sink != 0 && sink->clear != 0) {
        sink->clear(sink->context);
    } else {
        (void)clear_screen();
    }
}

void shell_trim_newline(char* line) {
    size_t index = 0;
    if (line == 0) {
        return;
    }

    while (line[index] != '\0') {
        if (line[index] == '\n' || line[index] == '\r') {
            line[index] = '\0';
            return;
        }
        ++index;
    }
}

static int shell_is_space(char character) {
    return character == ' ' || character == '\t';
}

static int shell_is_operator(char character) {
    return character == '|' || character == '<' || character == '>';
}

void shell_current_directory(char* buffer, size_t capacity) {
    if (buffer == 0 || capacity == 0) {
        return;
    }

    if (getcwd(buffer, capacity) < 0 || buffer[0] == '\0') {
        strcpy(buffer, "/");
    }
}

void shell_print_help(const struct shell_capture_sink* sink) {
    shell_emit_text(sink, 1, SAVANXP_DISPLAY_NAME " shell\n");
    shell_emit_text(sink, 1, "Builtins: help clear exit exec which mkdir cd pwd\n");
    shell_emit_text(sink, 1, "Core: sysinfo uname ps ls cat df echo sleep ticker demo true false\n");
    shell_emit_text(sink, 1, "Storage: mkdir mv rm rmdir truncate sync seektest renametest truncatetest\n");
    shell_emit_text(sink, 1, "Diagnostics: sysinfo errtest fdtest waittest pipestress spawnloop badptr\n");
    shell_emit_text(sink, 1, "Network: netinfo ping udptest udpsend udprecv tcpget\n");
    shell_emit_text(sink, 1, "Graphics/audio: desktop shellapp gfxdemo gputest keytest mousetest beep\n");
    shell_emit_text(sink, 1, "Examples: sysinfo\n");
    shell_emit_text(sink, 1, "          df\n");
    shell_emit_text(sink, 1, "          cd /disk\n");
    shell_emit_text(sink, 1, "          pwd\n");
    shell_emit_text(sink, 1, "          echo \"hola mundo\" | cat\n");
    shell_emit_text(sink, 1, "          cat < /README\n");
    shell_emit_text(sink, 1, "          echo hola > /disk/out.txt\n");
    shell_emit_text(sink, 1, "          errtest > /disk/mixed.txt 2>&1\n");
    shell_emit_text(sink, 1, "          ping 10.0.2.2  (QEMU user-net smoke test)\n");
}

static void shell_copy_path(char* path, size_t capacity, const char* prefix, const char* suffix) {
    size_t index = 0;
    if (path == 0 || capacity == 0) {
        return;
    }

    while (*prefix != '\0' && index + 1 < capacity) {
        path[index++] = *prefix++;
    }
    while (*suffix != '\0' && index + 1 < capacity) {
        path[index++] = *suffix++;
    }
    path[index] = '\0';
}

static int shell_path_exists(const char* path) {
    long fd = open(path);
    if (fd < 0) {
        return 0;
    }
    close((int)fd);
    return 1;
}

static int shell_resolve_command_path(const char* command, char* path, size_t capacity) {
    if (command[0] == '/') {
        shell_copy_path(path, capacity, "", command);
        return shell_path_exists(path);
    }

    shell_copy_path(path, capacity, "/disk/bin/", command);
    if (shell_path_exists(path)) {
        return 1;
    }

    shell_copy_path(path, capacity, "/bin/", command);
    return shell_path_exists(path);
}

static void shell_initialize_stage(struct shell_command_stage* stage) {
    memset(stage, 0, sizeof(*stage));
}

static int shell_is_builtin(const char* command) {
    return strcmp(command, "help") == 0 ||
        strcmp(command, "clear") == 0 ||
        strcmp(command, "exit") == 0 ||
        strcmp(command, "exec") == 0 ||
        strcmp(command, "which") == 0 ||
        strcmp(command, "mkdir") == 0 ||
        strcmp(command, "cd") == 0 ||
        strcmp(command, "pwd") == 0;
}

static char* shell_parse_word(char** cursor_ptr, char* delimiter_out) {
    char* cursor = *cursor_ptr;
    char* start;
    char* write;
    char delimiter = '\0';
    char quote = 0;

    while (shell_is_space(*cursor)) {
        ++cursor;
    }

    if (*cursor == '\0' || shell_is_operator(*cursor)) {
        *cursor_ptr = cursor;
        *delimiter_out = *cursor;
        return 0;
    }

    start = cursor;
    write = cursor;
    while (*cursor != '\0') {
        const char current = *cursor;
        if (quote != 0) {
            if (current == quote) {
                quote = 0;
                ++cursor;
                continue;
            }
            *write++ = current;
            ++cursor;
            continue;
        }

        if (current == '\'' || current == '"') {
            quote = current;
            ++cursor;
            continue;
        }
        if (shell_is_space(current) || shell_is_operator(current)) {
            break;
        }

        *write++ = current;
        ++cursor;
    }

    if (quote != 0) {
        *cursor_ptr = cursor;
        *delimiter_out = '\0';
        return (char*)-1;
    }

    delimiter = *cursor;
    *write = '\0';
    *delimiter_out = delimiter;
    if (delimiter != '\0') {
        ++cursor;
        while (shell_is_space(*cursor)) {
            ++cursor;
        }
    }
    *cursor_ptr = cursor;
    return start;
}

static int shell_parse_pipeline(char* line, struct shell_command_stage* stages, int capacity) {
    int stage_count = 1;
    int current = 0;
    int pending_redirection = SHELL_REDIRECT_NONE;
    char* cursor = line;

    if (line == 0 || stages == 0 || capacity <= 0) {
        return -1;
    }

    while (current < capacity) {
        shell_initialize_stage(&stages[current]);
        ++current;
    }

    current = 0;
    while (*cursor != '\0') {
        char delimiter = '\0';
        char* word;

        while (shell_is_space(*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        if (*cursor == '|') {
            if (pending_redirection != SHELL_REDIRECT_NONE || stages[current].argc == 0 || current + 1 >= capacity) {
                return -1;
            }
            stages[current].argv[stages[current].argc] = 0;
            ++current;
            ++stage_count;
            ++cursor;
            continue;
        }

        if (*cursor == '<') {
            if (pending_redirection != SHELL_REDIRECT_NONE) {
                return -1;
            }
            pending_redirection = SHELL_REDIRECT_STDIN;
            ++cursor;
            continue;
        }

        if (*cursor == '>') {
            if (pending_redirection != SHELL_REDIRECT_NONE) {
                return -1;
            }
            ++cursor;
            pending_redirection = *cursor == '>' ? SHELL_REDIRECT_STDOUT_APPEND : SHELL_REDIRECT_STDOUT;
            if (*cursor == '>') {
                ++cursor;
            }
            continue;
        }

        if (*cursor == '1' && cursor[1] == '>') {
            if (pending_redirection != SHELL_REDIRECT_NONE) {
                return -1;
            }
            cursor += 2;
            pending_redirection = *cursor == '>' ? SHELL_REDIRECT_STDOUT_APPEND : SHELL_REDIRECT_STDOUT;
            if (*cursor == '>') {
                ++cursor;
            }
            continue;
        }

        if (*cursor == '2' && cursor[1] == '>') {
            if (pending_redirection != SHELL_REDIRECT_NONE) {
                return -1;
            }
            cursor += 2;
            if (*cursor == '&' && cursor[1] == '1') {
                stages[current].stderr_to_stdout = 1;
                cursor += 2;
                continue;
            }
            pending_redirection = *cursor == '>' ? SHELL_REDIRECT_STDERR_APPEND : SHELL_REDIRECT_STDERR;
            if (*cursor == '>') {
                ++cursor;
            }
            continue;
        }

        word = shell_parse_word(&cursor, &delimiter);
        if (word == (char*)-1) {
            return -1;
        }
        if (word == 0) {
            return -1;
        }

        if (pending_redirection == SHELL_REDIRECT_STDIN) {
            stages[current].input_path = word;
            pending_redirection = SHELL_REDIRECT_NONE;
        } else if (pending_redirection == SHELL_REDIRECT_STDOUT || pending_redirection == SHELL_REDIRECT_STDOUT_APPEND) {
            stages[current].stdout_path = word;
            stages[current].stdout_append = pending_redirection == SHELL_REDIRECT_STDOUT_APPEND;
            pending_redirection = SHELL_REDIRECT_NONE;
        } else if (pending_redirection == SHELL_REDIRECT_STDERR || pending_redirection == SHELL_REDIRECT_STDERR_APPEND) {
            stages[current].stderr_path = word;
            stages[current].stderr_append = pending_redirection == SHELL_REDIRECT_STDERR_APPEND;
            pending_redirection = SHELL_REDIRECT_NONE;
        } else {
            if (stages[current].argc + 1 >= SHELL_MAX_ARGS) {
                return -1;
            }
            stages[current].argv[stages[current].argc++] = word;
        }

        if (delimiter == '|') {
            if (pending_redirection != SHELL_REDIRECT_NONE || stages[current].argc == 0 || current + 1 >= capacity) {
                return -1;
            }
            stages[current].argv[stages[current].argc] = 0;
            ++current;
            ++stage_count;
        } else if (delimiter == '<') {
            pending_redirection = SHELL_REDIRECT_STDIN;
        } else if (delimiter == '>') {
            pending_redirection = SHELL_REDIRECT_STDOUT;
        }
    }

    if (pending_redirection != SHELL_REDIRECT_NONE) {
        return -1;
    }

    for (current = 0; current < stage_count; ++current) {
        if (stages[current].argc == 0) {
            return -1;
        }
        stages[current].argv[stages[current].argc] = 0;
    }

    return stage_count;
}

static int shell_run_builtin(
    struct shell_command_stage* stage,
    enum shell_execute_mode mode,
    const struct shell_capture_sink* sink,
    int* exit_requested) {
    char path[256];

    if (stage == 0 || stage->argc == 0) {
        return 0;
    }

    if (strcmp(stage->argv[0], "help") == 0) {
        shell_print_help(sink);
        return 1;
    }

    if (strcmp(stage->argv[0], "clear") == 0) {
        shell_clear_output(sink);
        return 1;
    }

    if (strcmp(stage->argv[0], "exit") == 0) {
        if (exit_requested != 0) {
            *exit_requested = 1;
        }
        return 1;
    }

    if (strcmp(stage->argv[0], "exec") == 0) {
        if (mode != SHELL_EXEC_STDIO) {
            shell_emit_text(sink, 2, "shell: exec is only available on the console shell\n");
            return 1;
        }
        if (stage->argc < 2) {
            shell_emit_text(sink, 2, "sh: exec requires a command\n");
            return 1;
        }
        if (!shell_resolve_command_path(stage->argv[1], path, sizeof(path))) {
            shell_emit_text(sink, 2, "sh: command not found\n");
            return 1;
        }
        {
            char* const* argv = &stage->argv[1];
            const long result = exec(path, (const char* const*)argv, stage->argc - 1);
            if (result < 0) {
                shell_emit_text(sink, 2, "sh: exec failed\n");
            }
        }
        return 1;
    }

    if (strcmp(stage->argv[0], "which") == 0) {
        if (stage->argc < 2) {
            shell_emit_text(sink, 2, "sh: which requires a command\n");
            return 1;
        }
        if (!shell_resolve_command_path(stage->argv[1], path, sizeof(path))) {
            shell_emit_text(sink, 2, "sh: command not found\n");
            return 1;
        }
        shell_emit_text(sink, 1, path);
        shell_emit_text(sink, 1, "\n");
        return 1;
    }

    if (strcmp(stage->argv[0], "mkdir") == 0) {
        if (stage->argc < 2) {
            shell_emit_text(sink, 2, "sh: mkdir requires a path\n");
            return 1;
        }
        if (mkdir(stage->argv[1]) < 0) {
            shell_emit_text(sink, 2, "sh: mkdir failed\n");
        }
        return 1;
    }

    if (strcmp(stage->argv[0], "cd") == 0) {
        if (stage->argc < 2) {
            if (chdir("/") < 0) {
                shell_emit_text(sink, 2, "sh: cd failed\n");
            }
            return 1;
        }
        if (chdir(stage->argv[1]) < 0) {
            shell_emit_text(sink, 2, "sh: cd failed\n");
        }
        return 1;
    }

    if (strcmp(stage->argv[0], "pwd") == 0) {
        char cwd[256] = {};
        shell_current_directory(cwd, sizeof(cwd));
        shell_emit_text(sink, 1, cwd);
        shell_emit_text(sink, 1, "\n");
        return 1;
    }

    return 0;
}

static int shell_wait_for_children(const long* pids, int count) {
    int index = 0;
    int last_status = 0;
    while (index < count) {
        waitpid((int)pids[index], &last_status);
        ++index;
    }
    return last_status;
}

static int shell_execute_pipeline_stdio(struct shell_command_stage* stages, int stage_count) {
    long pids[SHELL_MAX_STAGES];
    int pid_count = 0;
    int previous_read_fd = -1;
    int index;

    for (index = 0; index < stage_count; ++index) {
        const int is_last = (index + 1) == stage_count;
        const int simple_spawn =
            (stage_count == 1 &&
             stages[index].input_path == 0 &&
             stages[index].stdout_path == 0 &&
             stages[index].stderr_path == 0);
        int pipe_fds[2] = {-1, -1};
        int input_fd = 0;
        int output_fd = 1;
        int error_fd = 2;
        char path[256];
        long pid = -1;

        if (!is_last && pipe(pipe_fds) < 0) {
            puts_fd(2, "sh: pipe failed\n");
            if (previous_read_fd >= 0) {
                close(previous_read_fd);
            }
            shell_wait_for_children(pids, pid_count);
            return -1;
        }

        if (stages[index].input_path != 0) {
            if (previous_read_fd >= 0) {
                close(previous_read_fd);
                previous_read_fd = -1;
            }
            input_fd = (int)open_mode(stages[index].input_path, SAVANXP_OPEN_READ);
            if (input_fd < 0) {
                puts_fd(2, "sh: input path not found\n");
                if (pipe_fds[0] >= 0) {
                    close(pipe_fds[0]);
                    close(pipe_fds[1]);
                }
                shell_wait_for_children(pids, pid_count);
                return -1;
            }
        } else if (previous_read_fd >= 0) {
            input_fd = previous_read_fd;
        }

        if (stages[index].stdout_path != 0) {
            output_fd = (int)open_mode(
                stages[index].stdout_path,
                SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE |
                    (stages[index].stdout_append ? SAVANXP_OPEN_APPEND : SAVANXP_OPEN_TRUNCATE));
            if (output_fd < 0) {
                puts_fd(2, "sh: unable to open output path\n");
                if (input_fd > 2 && input_fd != previous_read_fd) {
                    close(input_fd);
                }
                if (pipe_fds[0] >= 0) {
                    close(pipe_fds[0]);
                    close(pipe_fds[1]);
                }
                if (previous_read_fd >= 0) {
                    close(previous_read_fd);
                }
                shell_wait_for_children(pids, pid_count);
                return -1;
            }
        } else if (!is_last) {
            output_fd = pipe_fds[1];
        }

        if (stages[index].stderr_path != 0) {
            error_fd = (int)open_mode(
                stages[index].stderr_path,
                SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE |
                    (stages[index].stderr_append ? SAVANXP_OPEN_APPEND : SAVANXP_OPEN_TRUNCATE));
            if (error_fd < 0) {
                puts_fd(2, "sh: unable to open error path\n");
                if (input_fd > 2 && input_fd != previous_read_fd) {
                    close(input_fd);
                }
                if (output_fd > 2) {
                    close(output_fd);
                }
                if (pipe_fds[0] >= 0) {
                    close(pipe_fds[0]);
                    close(pipe_fds[1]);
                }
                if (previous_read_fd >= 0) {
                    close(previous_read_fd);
                }
                shell_wait_for_children(pids, pid_count);
                return -1;
            }
        }

        if (stages[index].stderr_to_stdout) {
            error_fd = output_fd;
        }

        if (!shell_resolve_command_path(stages[index].argv[0], path, sizeof(path))) {
            puts_fd(2, "sh: command not found\n");
            if (input_fd > 2 && input_fd != previous_read_fd) {
                close(input_fd);
            }
            if (output_fd > 2) {
                close(output_fd);
            }
            if (error_fd > 2 && error_fd != output_fd) {
                close(error_fd);
            }
            if (pipe_fds[0] >= 0) {
                close(pipe_fds[0]);
            }
            if (pipe_fds[1] >= 0) {
                close(pipe_fds[1]);
            }
            if (previous_read_fd >= 0) {
                close(previous_read_fd);
            }
            shell_wait_for_children(pids, pid_count);
            return -1;
        }

        if (simple_spawn) {
            pid = spawn(path, (const char* const*)stages[index].argv, stages[index].argc);
        } else {
            pid = spawn_fds(path, (const char* const*)stages[index].argv, stages[index].argc, input_fd, output_fd, error_fd);
        }
        if (pid < 0) {
            puts_fd(2, "sh: command not found\n");
            if (input_fd > 2 && input_fd != previous_read_fd) {
                close(input_fd);
            }
            if (output_fd > 2) {
                close(output_fd);
            }
            if (error_fd > 2 && error_fd != output_fd) {
                close(error_fd);
            }
            if (pipe_fds[0] >= 0) {
                close(pipe_fds[0]);
            }
            if (pipe_fds[1] >= 0) {
                close(pipe_fds[1]);
            }
            if (previous_read_fd >= 0) {
                close(previous_read_fd);
            }
            shell_wait_for_children(pids, pid_count);
            return -1;
        }

        pids[pid_count++] = pid;

        if (previous_read_fd >= 0) {
            close(previous_read_fd);
            previous_read_fd = -1;
        }
        if (input_fd > 2) {
            close(input_fd);
        }
        if (output_fd > 2) {
            close(output_fd);
        }
        if (error_fd > 2 && error_fd != output_fd) {
            close(error_fd);
        }
        if (!is_last) {
            previous_read_fd = pipe_fds[0];
        }
    }

    if (previous_read_fd >= 0) {
        close(previous_read_fd);
    }

    return shell_wait_for_children(pids, pid_count);
}

static void shell_emit_readable_pipe(int fd, int stream, const struct shell_capture_sink* sink) {
    char buffer[256];
    long count = 0;

    for (;;) {
        count = read(fd, buffer, sizeof(buffer));
        if (count <= 0) {
            return;
        }
        shell_emit_bytes(sink, stream, buffer, (size_t)count);
        if (count < (long)sizeof(buffer)) {
            return;
        }
    }
}

static int shell_execute_capture_external(char* line, const struct shell_capture_sink* sink) {
    const char* argv[] = {"/bin/sh", "-c", line, 0};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    struct savanxp_pollfd poll_fds[2];
    long pid = -1;
    int status = 0;
    int stdout_open = 1;
    int stderr_open = 1;

    if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
        if (stdout_pipe[0] >= 0) {
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);
        }
        if (stderr_pipe[0] >= 0) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }
        shell_emit_text(sink, 2, "sh: pipe failed\n");
        return 1;
    }

    pid = spawn_fds("/bin/sh", argv, 3, 0, stdout_pipe[1], stderr_pipe[1]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    stdout_pipe[1] = -1;
    stderr_pipe[1] = -1;

    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        shell_emit_text(sink, 2, "sh: command not found\n");
        return 1;
    }

    poll_fds[0].fd = stdout_pipe[0];
    poll_fds[0].events = SAVANXP_POLLIN;
    poll_fds[0].revents = 0;
    poll_fds[1].fd = stderr_pipe[0];
    poll_fds[1].events = SAVANXP_POLLIN;
    poll_fds[1].revents = 0;

    while (stdout_open || stderr_open) {
        long ready = poll(poll_fds, 2, -1);
        if (ready < 0) {
            break;
        }

        if (stdout_open && (poll_fds[0].revents & (SAVANXP_POLLIN | SAVANXP_POLLHUP)) != 0) {
            shell_emit_readable_pipe(stdout_pipe[0], 1, sink);
            if ((poll_fds[0].revents & SAVANXP_POLLHUP) != 0) {
                close(stdout_pipe[0]);
                stdout_pipe[0] = -1;
                stdout_open = 0;
            }
        }
        if (stderr_open && (poll_fds[1].revents & (SAVANXP_POLLIN | SAVANXP_POLLHUP)) != 0) {
            shell_emit_readable_pipe(stderr_pipe[0], 2, sink);
            if ((poll_fds[1].revents & SAVANXP_POLLHUP) != 0) {
                close(stderr_pipe[0]);
                stderr_pipe[0] = -1;
                stderr_open = 0;
            }
        }

        poll_fds[0].revents = 0;
        poll_fds[1].revents = 0;
    }

    if (stdout_pipe[0] >= 0) {
        close(stdout_pipe[0]);
    }
    if (stderr_pipe[0] >= 0) {
        close(stderr_pipe[0]);
    }
    waitpid((int)pid, &status);
    return status;
}

int shell_execute_line(char* line, enum shell_execute_mode mode, const struct shell_capture_sink* sink) {
    struct shell_command_stage stages[SHELL_MAX_STAGES];
    int stage_count = 0;
    int exit_requested = 0;
    int builtin_result = 0;

    stage_count = shell_parse_pipeline(line, stages, SHELL_MAX_STAGES);
    if (stage_count == 0) {
        return SHELL_EXEC_RESULT_OK;
    }
    if (stage_count < 0) {
        shell_emit_text(sink, 2, "sh: parse error\n");
        return SHELL_EXEC_RESULT_ERROR;
    }

    if (stage_count == 1 &&
        stages[0].input_path == 0 &&
        stages[0].stdout_path == 0 &&
        stages[0].stderr_path == 0 &&
        shell_is_builtin(stages[0].argv[0])) {
        builtin_result = shell_run_builtin(&stages[0], mode, sink, &exit_requested);
        if (builtin_result != 0) {
            return exit_requested != 0 ? SHELL_EXEC_RESULT_EXIT : SHELL_EXEC_RESULT_OK;
        }
    }

    if (mode == SHELL_EXEC_CAPTURE) {
        return shell_execute_capture_external(line, sink) == 0 ? SHELL_EXEC_RESULT_OK : SHELL_EXEC_RESULT_ERROR;
    }

    return shell_execute_pipeline_stdio(stages, stage_count) < 0 ? SHELL_EXEC_RESULT_ERROR : SHELL_EXEC_RESULT_OK;
}
