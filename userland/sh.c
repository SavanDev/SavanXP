#include "libc.h"

#include "shared/syscall.h"

#define MAX_STAGES 8
#define MAX_ARGS 16

struct CommandStage {
    char* argv[MAX_ARGS];
    int argc;
    char* input_path;
    char* output_path;
};

void trim_newline(char* line) {
    for (size_t index = 0; line[index] != '\0'; ++index) {
        if (line[index] == '\n' || line[index] == '\r') {
            line[index] = '\0';
            return;
        }
    }
}

int is_space(char character) {
    return character == ' ' || character == '\t';
}

int is_operator(char character) {
    return character == '|' || character == '<' || character == '>';
}

int is_builtin(const char* command) {
    return strcmp(command, "help") == 0 ||
        strcmp(command, "clear") == 0 ||
        strcmp(command, "exit") == 0;
}

void build_path(const char* command, char* path, size_t capacity) {
    if (command[0] == '/') {
        strcpy(path, command);
        path[capacity - 1] = '\0';
        return;
    }

    strcpy(path, "/bin/");
    strcpy(path + 5, command);
    path[capacity - 1] = '\0';
}

void initialize_stage(struct CommandStage* stage) {
    memset(stage, 0, sizeof(*stage));
}

int parse_pipeline(char* line, struct CommandStage* stages, int capacity) {
    if (line == 0 || stages == 0 || capacity <= 0) {
        return -1;
    }

    for (int index = 0; index < capacity; ++index) {
        initialize_stage(&stages[index]);
    }

    int stage_count = 1;
    int current = 0;
    char* cursor = line;

    while (*cursor != '\0') {
        while (is_space(*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        if (*cursor == '|') {
            if (stages[current].argc == 0 || current + 1 >= capacity) {
                return -1;
            }
            stages[current].argv[stages[current].argc] = 0;
            ++current;
            ++stage_count;
            ++cursor;
            continue;
        }

        if (*cursor == '<' || *cursor == '>') {
            const char op = *cursor++;
            while (is_space(*cursor)) {
                ++cursor;
            }
            if (*cursor == '\0' || is_operator(*cursor)) {
                return -1;
            }

            char* start = cursor;
            while (*cursor != '\0' && !is_space(*cursor) && !is_operator(*cursor)) {
                ++cursor;
            }
            if (*cursor != '\0') {
                *cursor++ = '\0';
            }

            if (op == '<') {
                if (stages[current].input_path != 0) {
                    return -1;
                }
                stages[current].input_path = start;
            } else {
                if (stages[current].output_path != 0) {
                    return -1;
                }
                stages[current].output_path = start;
            }
            continue;
        }

        if (stages[current].argc + 1 >= MAX_ARGS) {
            return -1;
        }

        char* start = cursor;
        while (*cursor != '\0' && !is_space(*cursor) && !is_operator(*cursor)) {
            ++cursor;
        }
        if (*cursor != '\0') {
            *cursor++ = '\0';
        }

        stages[current].argv[stages[current].argc++] = start;
    }

    for (int index = 0; index < stage_count; ++index) {
        if (stages[index].argc == 0) {
            return -1;
        }
        stages[index].argv[stages[index].argc] = 0;
        if (index != 0 && stages[index].input_path != 0) {
            return -1;
        }
        if (index + 1 != stage_count && stages[index].output_path != 0) {
            return -1;
        }
    }

    return stage_count;
}

int run_builtin(struct CommandStage* stage) {
    if (strcmp(stage->argv[0], "help") == 0) {
        puts("Builtins: help clear exit\n");
        puts("Commands: echo uname ls cat sleep ticker demo true false ps fdtest waittest pipestress spawnloop badptr\n");
        puts("Examples: echo hola | cat\n");
        puts("          cat < /README\n");
        puts("          echo hola > /notes.txt\n");
        return 0;
    }

    if (strcmp(stage->argv[0], "clear") == 0) {
        clear_screen();
        return 0;
    }

    if (strcmp(stage->argv[0], "exit") == 0) {
        return 1;
    }

    return 0;
}

int wait_for_children(const long* pids, int count) {
    int last_status = 0;
    for (int index = 0; index < count; ++index) {
        waitpid((int)pids[index], &last_status);
    }
    return last_status;
}

int execute_pipeline(struct CommandStage* stages, int stage_count) {
    long pids[MAX_STAGES];
    int pid_count = 0;
    int previous_read_fd = -1;

    for (int index = 0; index < stage_count; ++index) {
        const int is_last = (index + 1) == stage_count;
        int pipe_fds[2] = {-1, -1};
        int input_fd = 0;
        int output_fd = 1;
        char path[128];

        if (!is_last && pipe(pipe_fds) < 0) {
            puts("sh: pipe failed\n");
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
                puts("sh: input path not found\n");
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

        if (stages[index].output_path != 0) {
            output_fd = (int)open_mode(
                stages[index].output_path,
                SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
            if (output_fd < 0) {
                puts("sh: unable to open output path\n");
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

        build_path(stages[index].argv[0], path, sizeof(path));
        const long pid = spawn_fd(path, (const char* const*)stages[index].argv, stages[index].argc, input_fd, output_fd);
        if (pid < 0) {
            puts("sh: command not found\n");
            if (input_fd > 2 && input_fd != previous_read_fd) {
                close(input_fd);
            }
            if (output_fd > 2) {
                close(output_fd);
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

    puts("Welcome to SavanXP\n");
    puts("Builtins: help clear exit\n");

    for (;;) {
        puts("SavanXP > ");
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
            puts("sh: parse error\n");
            continue;
        }

        if (stage_count == 1 &&
            stages[0].input_path == 0 &&
            stages[0].output_path == 0 &&
            is_builtin(stages[0].argv[0])) {
            if (run_builtin(&stages[0]) != 0) {
                return 0;
            }
            continue;
        }

        const int status = execute_pipeline(stages, stage_count);
        if (status < 0) {
            continue;
        }
        if (status != 0) {
            printf("sh: exit %d\n", status);
        }
    }
}
