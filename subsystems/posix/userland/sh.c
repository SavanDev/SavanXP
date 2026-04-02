#include "shell_core.h"

#include "shared/version.h"

#define SH_MAX_STAGES 8
#define SH_MAX_ARGS 16

struct sh_command_stage {
    char* argv[SH_MAX_ARGS];
    int argc;
    char* input_path;
    char* stdout_path;
    char* stderr_path;
    int stdout_append;
    int stderr_append;
    int stderr_to_stdout;
};

enum sh_pending_redirect {
    SH_REDIRECT_NONE = 0,
    SH_REDIRECT_STDIN = 1,
    SH_REDIRECT_STDOUT = 2,
    SH_REDIRECT_STDOUT_APPEND = 3,
    SH_REDIRECT_STDERR = 4,
    SH_REDIRECT_STDERR_APPEND = 5,
};

static int sh_is_space(char character) {
    return character == ' ' || character == '\t';
}

static int sh_is_operator(char character) {
    return character == '|' || character == '<' || character == '>';
}

static void sh_copy_path(char* path, size_t capacity, const char* prefix, const char* suffix) {
    size_t index = 0;
    while (*prefix != '\0' && index + 1 < capacity) {
        path[index++] = *prefix++;
    }
    while (*suffix != '\0' && index + 1 < capacity) {
        path[index++] = *suffix++;
    }
    path[index] = '\0';
}

static int sh_path_exists(const char* path) {
    long fd = open(path);
    if (fd < 0) {
        return 0;
    }
    close((int)fd);
    return 1;
}

static int sh_resolve_command_path(const char* command, char* path, size_t capacity) {
    if (command[0] == '/') {
        sh_copy_path(path, capacity, "", command);
        return sh_path_exists(path);
    }

    sh_copy_path(path, capacity, "/disk/bin/", command);
    if (sh_path_exists(path)) {
        return 1;
    }

    sh_copy_path(path, capacity, "/bin/", command);
    return sh_path_exists(path);
}

static void sh_initialize_stage(struct sh_command_stage* stage) {
    memset(stage, 0, sizeof(*stage));
}

static char* sh_parse_word(char** cursor_ptr, char* delimiter_out) {
    char* cursor = *cursor_ptr;
    char* start = 0;
    char* write = 0;
    char delimiter = '\0';
    char quote = 0;

    while (sh_is_space(*cursor)) {
        ++cursor;
    }
    if (*cursor == '\0' || sh_is_operator(*cursor)) {
        *cursor_ptr = cursor;
        *delimiter_out = *cursor;
        return 0;
    }

    start = cursor;
    write = cursor;
    while (*cursor != '\0') {
        char current = *cursor;
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
        if (sh_is_space(current) || sh_is_operator(current)) {
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
        while (sh_is_space(*cursor)) {
            ++cursor;
        }
    }
    *cursor_ptr = cursor;
    return start;
}

static int sh_parse_pipeline(char* line, struct sh_command_stage* stages, int capacity) {
    int stage_count = 1;
    int current = 0;
    int pending_redirection = SH_REDIRECT_NONE;
    char* cursor = line;

    if (line == 0 || stages == 0 || capacity <= 0) {
        return -1;
    }
    while (current < capacity) {
        sh_initialize_stage(&stages[current]);
        ++current;
    }

    current = 0;
    while (*cursor != '\0') {
        char delimiter = '\0';
        char* token = 0;

        while (sh_is_space(*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        if (*cursor == '|') {
            if (pending_redirection != SH_REDIRECT_NONE || stages[current].argc == 0 || current + 1 >= capacity) {
                return -1;
            }
            stages[current].argv[stages[current].argc] = 0;
            ++current;
            ++stage_count;
            ++cursor;
            continue;
        }
        if (*cursor == '<') {
            if (pending_redirection != SH_REDIRECT_NONE) {
                return -1;
            }
            pending_redirection = SH_REDIRECT_STDIN;
            ++cursor;
            continue;
        }
        if (*cursor == '>') {
            if (pending_redirection != SH_REDIRECT_NONE) {
                return -1;
            }
            ++cursor;
            pending_redirection = *cursor == '>' ? SH_REDIRECT_STDOUT_APPEND : SH_REDIRECT_STDOUT;
            if (*cursor == '>') {
                ++cursor;
            }
            continue;
        }
        if (*cursor == '1' && cursor[1] == '>') {
            if (pending_redirection != SH_REDIRECT_NONE) {
                return -1;
            }
            cursor += 2;
            pending_redirection = *cursor == '>' ? SH_REDIRECT_STDOUT_APPEND : SH_REDIRECT_STDOUT;
            if (*cursor == '>') {
                ++cursor;
            }
            continue;
        }
        if (*cursor == '2' && cursor[1] == '>' && cursor[2] == '&' && cursor[3] == '1') {
            if (pending_redirection != SH_REDIRECT_NONE || stages[current].stderr_path != 0) {
                return -1;
            }
            stages[current].stderr_to_stdout = 1;
            cursor += 4;
            while (sh_is_space(*cursor)) {
                ++cursor;
            }
            continue;
        }
        if (*cursor == '2' && cursor[1] == '>') {
            if (pending_redirection != SH_REDIRECT_NONE) {
                return -1;
            }
            cursor += 2;
            pending_redirection = *cursor == '>' ? SH_REDIRECT_STDERR_APPEND : SH_REDIRECT_STDERR;
            if (*cursor == '>') {
                ++cursor;
            }
            continue;
        }

        if (stages[current].argc + 1 >= SH_MAX_ARGS) {
            return -1;
        }

        token = sh_parse_word(&cursor, &delimiter);
        if (token == 0 || token == (char*)-1) {
            return -1;
        }

        if (pending_redirection != SH_REDIRECT_NONE) {
            if (pending_redirection == SH_REDIRECT_STDIN) {
                stages[current].input_path = token;
            } else if (pending_redirection == SH_REDIRECT_STDOUT || pending_redirection == SH_REDIRECT_STDOUT_APPEND) {
                stages[current].stdout_path = token;
                stages[current].stdout_append = pending_redirection == SH_REDIRECT_STDOUT_APPEND;
            } else {
                stages[current].stderr_path = token;
                stages[current].stderr_append = pending_redirection == SH_REDIRECT_STDERR_APPEND;
            }
            pending_redirection = SH_REDIRECT_NONE;
        } else {
            stages[current].argv[stages[current].argc++] = token;
        }

        if (delimiter == '|') {
            if (stages[current].argc == 0 || current + 1 >= capacity) {
                return -1;
            }
            stages[current].argv[stages[current].argc] = 0;
            ++current;
            ++stage_count;
        } else if (delimiter == '<') {
            pending_redirection = SH_REDIRECT_STDIN;
        } else if (delimiter == '>') {
            pending_redirection = SH_REDIRECT_STDOUT;
        }
    }

    if (pending_redirection != SH_REDIRECT_NONE) {
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

static int sh_wait_for_children(const long* pids, int count) {
    int index = 0;
    int last_status = 0;
    while (index < count) {
        waitpid((int)pids[index], &last_status);
        ++index;
    }
    return last_status;
}

static int sh_execute_pipeline(char* line) {
    struct sh_command_stage stages[SH_MAX_STAGES];
    long pids[SH_MAX_STAGES];
    int stage_count = sh_parse_pipeline(line, stages, SH_MAX_STAGES);
    int pid_count = 0;
    int previous_read_fd = -1;
    int index = 0;

    if (stage_count == 0) {
        return 0;
    }
    if (stage_count < 0) {
        puts_fd(2, "sh: parse error\n");
        return 1;
    }
    for (index = 0; index < stage_count; ++index) {
        int is_last = (index + 1) == stage_count;
        int simple_spawn = stage_count == 1 && stages[index].input_path == 0 && stages[index].stdout_path == 0 && stages[index].stderr_path == 0;
        int pipe_fds[2] = {-1, -1};
        int input_fd = 0;
        int output_fd = 1;
        int error_fd = 2;
        char path[256];
        long pid = -1;

        if (!is_last && pipe(pipe_fds) < 0) {
            puts_fd(2, "sh: pipe failed\n");
            return 1;
        }
        if (stages[index].input_path != 0) {
            if (previous_read_fd >= 0) {
                close(previous_read_fd);
                previous_read_fd = -1;
            }
            input_fd = (int)open_mode(stages[index].input_path, SAVANXP_OPEN_READ);
            if (input_fd < 0) {
                puts_fd(2, "sh: input path not found\n");
                return 1;
            }
        } else if (previous_read_fd >= 0) {
            input_fd = previous_read_fd;
        }
        if (stages[index].stdout_path != 0) {
            output_fd = (int)open_mode(stages[index].stdout_path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE |
                (stages[index].stdout_append ? SAVANXP_OPEN_APPEND : SAVANXP_OPEN_TRUNCATE));
            if (output_fd < 0) {
                puts_fd(2, "sh: unable to open output path\n");
                return 1;
            }
        } else if (!is_last) {
            output_fd = pipe_fds[1];
        }
        if (stages[index].stderr_path != 0) {
            error_fd = (int)open_mode(stages[index].stderr_path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE |
                (stages[index].stderr_append ? SAVANXP_OPEN_APPEND : SAVANXP_OPEN_TRUNCATE));
            if (error_fd < 0) {
                puts_fd(2, "sh: unable to open error path\n");
                return 1;
            }
        }
        if (stages[index].stderr_to_stdout) {
            error_fd = output_fd;
        }
        if (!sh_resolve_command_path(stages[index].argv[0], path, sizeof(path))) {
            puts_fd(2, "sh: command not found\n");
            return 1;
        }

        pid = simple_spawn
            ? spawn(path, (const char* const*)stages[index].argv, stages[index].argc)
            : spawn_fds(path, (const char* const*)stages[index].argv, stages[index].argc, input_fd, output_fd, error_fd);
        if (pid < 0) {
            puts_fd(2, "sh: command not found\n");
            return 1;
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
    return sh_wait_for_children(pids, pid_count);
}

static int sh_line_is_builtin(const char* line) {
    char token[32] = {};
    size_t index = 0;

    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    while (*line != '\0' && !sh_is_space(*line) && !sh_is_operator(*line) && index + 1 < sizeof(token)) {
        token[index++] = *line++;
    }
    token[index] = '\0';

    return strcmp(token, "help") == 0 ||
        strcmp(token, "clear") == 0 ||
        strcmp(token, "exit") == 0 ||
        strcmp(token, "exec") == 0 ||
        strcmp(token, "which") == 0 ||
        strcmp(token, "mkdir") == 0 ||
        strcmp(token, "cd") == 0 ||
        strcmp(token, "pwd") == 0;
}

static void shell_print_prompt(void) {
    char cwd[256] = {};
    shell_current_directory(cwd, sizeof(cwd));
    puts("savanxp:");
    puts(cwd);
    puts("$ ");
}

int main(int argc, char** argv) {
    char line[256] = {};

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        int result = SHELL_EXEC_RESULT_OK;
        size_t index = 0;
        while (argv[2][index] != '\0' && index + 1 < sizeof(line)) {
            line[index] = argv[2][index];
            ++index;
        }
        line[index] = '\0';
        shell_trim_newline(line);
        if (sh_line_is_builtin(line)) {
            result = shell_execute_line(line, SHELL_EXEC_STDIO, 0);
            return result == SHELL_EXEC_RESULT_ERROR ? 1 : 0;
        }
        return sh_execute_pipeline(line);
    }

    printf("%s\n", SAVANXP_DISPLAY_NAME);
    puts("Type 'help' for commands and 'sysinfo' for diagnostics.\n");

    for (;;) {
        long count = 0;

        shell_print_prompt();
        memset(line, 0, sizeof(line));
        count = read(0, line, sizeof(line) - 1);
        if (count <= 0) {
            continue;
        }

        shell_trim_newline(line);
        if (sh_line_is_builtin(line)) {
            if (shell_execute_line(line, SHELL_EXEC_STDIO, 0) == SHELL_EXEC_RESULT_EXIT) {
                return 0;
            }
            continue;
        }
        (void)sh_execute_pipeline(line);
    }
}
