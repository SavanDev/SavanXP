#include "libc.h"

#include "shared/syscall.h"
#include "shared/version.h"

#define MAX_STAGES 8
#define MAX_ARGS 16

struct CommandStage {
    char* argv[MAX_ARGS];
    int argc;
    char* input_path;
    char* stdout_path;
    char* stderr_path;
    int stdout_append;
    int stderr_append;
    int stderr_to_stdout;
};

static void trim_newline(char* line) {
    for (size_t index = 0; line[index] != '\0'; ++index) {
        if (line[index] == '\n' || line[index] == '\r') {
            line[index] = '\0';
            return;
        }
    }
}

static int is_space(char character) {
    return character == ' ' || character == '\t';
}

static int is_operator(char character) {
    return character == '|' || character == '<' || character == '>';
}

static void errputs(const char* text) {
    puts_fd(2, text);
}

static void current_directory(char* buffer, size_t capacity) {
    if (capacity == 0) {
        return;
    }

    if (getcwd(buffer, capacity) < 0 || buffer[0] == '\0') {
        strcpy(buffer, "/");
    }
}

static void print_prompt(void) {
    char cwd[256] = {};
    current_directory(cwd, sizeof(cwd));
    puts("savanxp:");
    puts(cwd);
    puts("$ ");
}

static void print_help(void) {
    printf("%s shell\n", SAVANXP_DISPLAY_NAME);
    puts("Builtins: help clear exit exec which mkdir cd pwd\n");
    puts("Core: sysinfo uname ps ls cat df echo sleep ticker demo true false\n");
    puts("Storage: mkdir mv rm rmdir truncate sync seektest renametest truncatetest\n");
    puts("Diagnostics: sysinfo errtest fdtest waittest pipestress spawnloop badptr\n");
    puts("Network: netinfo ping udptest udpsend udprecv tcpget\n");
    puts("Graphics/audio: gfxdemo beep\n");
    puts("Examples: sysinfo\n");
    puts("          df\n");
    puts("          cd /disk\n");
    puts("          pwd\n");
    puts("          echo \"hola mundo\" | cat\n");
    puts("          cat < /README\n");
    puts("          echo hola > /disk/out.txt\n");
    puts("          errtest > /disk/mixed.txt 2>&1\n");
    puts("          ping 10.0.2.2  (QEMU user-net smoke test)\n");
}

static void copy_path(char* path, size_t capacity, const char* prefix, const char* suffix) {
    size_t index = 0;
    if (capacity == 0) {
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

static int path_exists(const char* path) {
    const long fd = open(path);
    if (fd < 0) {
        return 0;
    }
    close((int)fd);
    return 1;
}

static int resolve_command_path(const char* command, char* path, size_t capacity) {
    if (command[0] == '/') {
        copy_path(path, capacity, "", command);
        return path_exists(path);
    }

    copy_path(path, capacity, "/disk/bin/", command);
    if (path_exists(path)) {
        return 1;
    }

    copy_path(path, capacity, "/bin/", command);
    return path_exists(path);
}

static void initialize_stage(struct CommandStage* stage) {
    memset(stage, 0, sizeof(*stage));
}

static int is_builtin(const char* command) {
    return strcmp(command, "help") == 0 ||
        strcmp(command, "clear") == 0 ||
        strcmp(command, "exit") == 0 ||
        strcmp(command, "exec") == 0 ||
        strcmp(command, "which") == 0 ||
        strcmp(command, "mkdir") == 0 ||
        strcmp(command, "cd") == 0 ||
        strcmp(command, "pwd") == 0;
}

static char* parse_word(char** cursor_ptr, char* delimiter_out) {
    char* cursor = *cursor_ptr;
    while (is_space(*cursor)) {
        ++cursor;
    }

    if (*cursor == '\0' || is_operator(*cursor)) {
        *cursor_ptr = cursor;
        *delimiter_out = *cursor;
        return 0;
    }

    char* start = cursor;
    char* write = cursor;
    char quote = 0;

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
        if (is_space(current) || is_operator(current)) {
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

    const char delimiter = *cursor;
    *write = '\0';
    if (delimiter == '\0') {
        *cursor_ptr = cursor;
    } else {
        ++cursor;
        while (is_space(*cursor)) {
            ++cursor;
        }
        *cursor_ptr = cursor;
    }
    *delimiter_out = delimiter;
    return start;
}

enum PendingRedirect {
    REDIRECT_NONE = 0,
    REDIRECT_STDIN = 1,
    REDIRECT_STDOUT = 2,
    REDIRECT_STDOUT_APPEND = 3,
    REDIRECT_STDERR = 4,
    REDIRECT_STDERR_APPEND = 5,
};

static int parse_pipeline(char* line, struct CommandStage* stages, int capacity) {
    if (line == 0 || stages == 0 || capacity <= 0) {
        return -1;
    }

    for (int index = 0; index < capacity; ++index) {
        initialize_stage(&stages[index]);
    }

    int stage_count = 1;
    int current = 0;
    char* cursor = line;
    int pending_redirection = REDIRECT_NONE;

    while (*cursor != '\0') {
        while (is_space(*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        if (*cursor == '|') {
            if (pending_redirection != REDIRECT_NONE || stages[current].argc == 0 || current + 1 >= capacity) {
                return -1;
            }
            stages[current].argv[stages[current].argc] = 0;
            ++current;
            ++stage_count;
            ++cursor;
            continue;
        }

        if (*cursor == '<') {
            if (pending_redirection != REDIRECT_NONE) {
                return -1;
            }
            pending_redirection = REDIRECT_STDIN;
            ++cursor;
            continue;
        }

        if (*cursor == '>') {
            if (pending_redirection != REDIRECT_NONE) {
                return -1;
            }
            ++cursor;
            if (*cursor == '>') {
                pending_redirection = REDIRECT_STDOUT_APPEND;
                ++cursor;
            } else {
                pending_redirection = REDIRECT_STDOUT;
            }
            continue;
        }

        if (*cursor == '1' && cursor[1] == '>') {
            if (pending_redirection != REDIRECT_NONE) {
                return -1;
            }
            cursor += 2;
            if (*cursor == '>') {
                pending_redirection = REDIRECT_STDOUT_APPEND;
                ++cursor;
            } else {
                pending_redirection = REDIRECT_STDOUT;
            }
            continue;
        }

        if (*cursor == '2' && cursor[1] == '>' && cursor[2] == '&' && cursor[3] == '1') {
            if (pending_redirection != REDIRECT_NONE || stages[current].stderr_path != 0) {
                return -1;
            }
            stages[current].stderr_to_stdout = 1;
            cursor += 4;
            while (is_space(*cursor)) {
                ++cursor;
            }
            continue;
        }

        if (*cursor == '2' && cursor[1] == '>') {
            if (pending_redirection != REDIRECT_NONE) {
                return -1;
            }
            cursor += 2;
            if (*cursor == '>') {
                pending_redirection = REDIRECT_STDERR_APPEND;
                ++cursor;
            } else {
                pending_redirection = REDIRECT_STDERR;
            }
            continue;
        }

        if (stages[current].argc + 1 >= MAX_ARGS) {
            return -1;
        }

        char delimiter = '\0';
        char* token = parse_word(&cursor, &delimiter);
        if (token == 0 || token == (char*)-1) {
            return -1;
        }

        if (pending_redirection != REDIRECT_NONE) {
            if (pending_redirection == REDIRECT_STDIN) {
                if (stages[current].input_path != 0) {
                    return -1;
                }
                stages[current].input_path = token;
            } else if (pending_redirection == REDIRECT_STDOUT || pending_redirection == REDIRECT_STDOUT_APPEND) {
                if (stages[current].stdout_path != 0) {
                    return -1;
                }
                stages[current].stdout_path = token;
                stages[current].stdout_append = pending_redirection == REDIRECT_STDOUT_APPEND;
            } else {
                if (stages[current].stderr_path != 0) {
                    return -1;
                }
                stages[current].stderr_path = token;
                stages[current].stderr_append = pending_redirection == REDIRECT_STDERR_APPEND;
            }
            pending_redirection = REDIRECT_NONE;
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
            if (pending_redirection != REDIRECT_NONE) {
                return -1;
            }
            pending_redirection = REDIRECT_STDIN;
        } else if (delimiter == '>') {
            if (pending_redirection != REDIRECT_NONE) {
                return -1;
            }
            pending_redirection = REDIRECT_STDOUT;
        }
    }

    if (pending_redirection != REDIRECT_NONE) {
        return -1;
    }

    for (int index = 0; index < stage_count; ++index) {
        if (stages[index].argc == 0) {
            return -1;
        }
        stages[index].argv[stages[index].argc] = 0;
        if (index != 0 && stages[index].input_path != 0) {
            return -1;
        }
        if (index + 1 != stage_count && stages[index].stdout_path != 0) {
            return -1;
        }
        if (index + 1 != stage_count && stages[index].stderr_path != 0) {
            return -1;
        }
        if (stages[index].stderr_to_stdout && stages[index].stderr_path != 0) {
            return -1;
        }
    }

    return stage_count;
}

static int run_builtin(struct CommandStage* stage) {
    if (strcmp(stage->argv[0], "help") == 0) {
        print_help();
        return 0;
    }

    if (strcmp(stage->argv[0], "clear") == 0) {
        clear_screen();
        return 0;
    }

    if (strcmp(stage->argv[0], "exit") == 0) {
        return 1;
    }

    if (strcmp(stage->argv[0], "exec") == 0) {
        char path[256];
        if (stage->argc < 2) {
            errputs("sh: exec needs a command\n");
            return -1;
        }
        if (!resolve_command_path(stage->argv[1], path, sizeof(path))) {
            errputs("sh: exec failed\n");
            return -1;
        }
        {
            const char** argv = (const char**)&stage->argv[1];
            const long result = exec(path, argv, stage->argc - 1);
            if (result < 0) {
                errputs("sh: exec failed\n");
                return -1;
            }
        }
        return 0;
    }

    if (strcmp(stage->argv[0], "which") == 0) {
        char path[256];
        if (stage->argc < 2) {
            errputs("sh: which needs a command\n");
            return -1;
        }
        if (!resolve_command_path(stage->argv[1], path, sizeof(path))) {
            errputs("sh: command not found\n");
            return -1;
        }
        puts(path);
        putchar(1, '\n');
        return 0;
    }

    if (strcmp(stage->argv[0], "mkdir") == 0) {
        if (stage->argc < 2) {
            errputs("sh: mkdir needs a path\n");
            return -1;
        }
        if (mkdir(stage->argv[1]) < 0) {
            errputs("sh: mkdir failed\n");
            return -1;
        }
        return 0;
    }

    if (strcmp(stage->argv[0], "cd") == 0) {
        const char* path = stage->argc >= 2 ? stage->argv[1] : "/";
        if (stage->argc > 2) {
            errputs("sh: cd takes one path\n");
            return -1;
        }
        const long result = chdir(path);
        if (result < 0) {
            eprintf("sh: cd failed (%s)\n", result_error_string(result));
            return -1;
        }
        return 0;
    }

    if (strcmp(stage->argv[0], "pwd") == 0) {
        char cwd[256] = {};
        if (getcwd(cwd, sizeof(cwd)) < 0) {
            errputs("sh: pwd failed\n");
            return -1;
        }
        puts(cwd);
        putchar(1, '\n');
        return 0;
    }

    return 0;
}

static int wait_for_children(const long* pids, int count) {
    int last_status = 0;
    for (int index = 0; index < count; ++index) {
        waitpid((int)pids[index], &last_status);
    }
    return last_status;
}

static int execute_pipeline(struct CommandStage* stages, int stage_count) {
    long pids[MAX_STAGES];
    int pid_count = 0;
    int previous_read_fd = -1;

    for (int index = 0; index < stage_count; ++index) {
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
            errputs("sh: pipe failed\n");
            if (previous_read_fd >= 0) {
                close(previous_read_fd);
            }
            wait_for_children(pids, pid_count);
            return -1;
        }

        if (stages[index].input_path != 0) {
            if (previous_read_fd >= 0) {
                close(previous_read_fd);
                previous_read_fd = -1;
            }
            input_fd = (int)open_mode(stages[index].input_path, SAVANXP_OPEN_READ);
            if (input_fd < 0) {
                errputs("sh: input path not found\n");
                if (pipe_fds[0] >= 0) {
                    close(pipe_fds[0]);
                    close(pipe_fds[1]);
                }
                wait_for_children(pids, pid_count);
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
                errputs("sh: unable to open output path\n");
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
                wait_for_children(pids, pid_count);
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
                errputs("sh: unable to open error path\n");
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
                wait_for_children(pids, pid_count);
                return -1;
            }
        }

        if (stages[index].stderr_to_stdout) {
            error_fd = output_fd;
        }

        if (!resolve_command_path(stages[index].argv[0], path, sizeof(path))) {
            errputs("sh: command not found\n");
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
            wait_for_children(pids, pid_count);
            return -1;
        }
        if (simple_spawn) {
            pid = spawn(path, (const char* const*)stages[index].argv, stages[index].argc);
        } else {
            pid = spawn_fds(path, (const char* const*)stages[index].argv, stages[index].argc, input_fd, output_fd, error_fd);
        }
        if (pid < 0) {
            errputs("sh: command not found\n");
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
            wait_for_children(pids, pid_count);
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

    return wait_for_children(pids, pid_count);
}

int main(void) {
    char line[256];
    struct CommandStage stages[MAX_STAGES];

    printf("%s\n", SAVANXP_DISPLAY_NAME);
    puts("Type 'help' for commands and 'sysinfo' for diagnostics.\n");

    for (;;) {
        print_prompt();
        memset(line, 0, sizeof(line));

        const long count = read(0, line, sizeof(line) - 1);
        if (count <= 0) {
            continue;
        }

        trim_newline(line);
        const int stage_count = parse_pipeline(line, stages, MAX_STAGES);
        if (stage_count == 0) {
            continue;
        }
        if (stage_count < 0) {
            errputs("sh: parse error\n");
            continue;
        }

        if (stage_count == 1 &&
            stages[0].input_path == 0 &&
            stages[0].stdout_path == 0 &&
            stages[0].stderr_path == 0 &&
            is_builtin(stages[0].argv[0])) {
            const int status = run_builtin(&stages[0]);
            if (status > 0) {
                return 0;
            }
            continue;
        }

        {
            const int status = execute_pipeline(stages, stage_count);
            if (status < 0) {
                continue;
            }
            (void)status;
        }
    }
}
