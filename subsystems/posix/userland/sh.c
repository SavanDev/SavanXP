#include "shell_core.h"

#include <string.h>

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

/* Console mode (docs/desktop/CONSOLE.md): `sh --console <fd>` runs under the
 * Command Prompt, which reads lines from us on 0, shows 1 and 2, and listens
 * on <fd> for the control channel. -1 on the kernel console. */
static int g_console_fd = -1;

/* Programs `start` launched and has not reaped yet. */
#define SH_MAX_STARTED 32
static long g_started[SH_MAX_STARTED];
static int g_started_count = 0;

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
    long fd = savanxp_open(path);
    if (fd < 0) {
        return 0;
    }
    savanxp_close((int)fd);
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

/* One line on the control channel. Nothing on the kernel console. */
static void sh_console_send(const char* text) {
    if (g_console_fd >= 0) {
        (void)savanxp_write(g_console_fd, text, strlen(text));
    }
}

/* Tells the console which processes the foreground job is (count 0: none), so
 * Ctrl+C can end them. */
static void sh_report_foreground(const long* pids, int count) {
    char line[16 + (SH_MAX_STAGES * 12)];
    size_t length = 0;
    int index = 0;

    if (g_console_fd < 0) {
        return;
    }
    memcpy(line, "fg", 2);
    length = 2;
    for (index = 0; index < count; ++index) {
        char number[24];
        size_t digits = 0;
        long value = pids[index];

        line[length++] = ' ';
        do {
            number[digits++] = (char)('0' + (value % 10));
            value /= 10;
        } while (value > 0 && digits < sizeof(number));
        while (digits > 0) {
            line[length++] = number[--digits];
        }
    }
    line[length++] = '\n';
    line[length] = '\0';
    sh_console_send(line);
}

/* Reaps the programs `start` launched that have ended: a zombie holds a
 * process slot until its parent waits for it. Same scan as the gfx runtime's
 * launcher (gfx_reap_launched_now). */
static void sh_reap_started(void) {
    struct savanxp_process_info info;
    unsigned long index = 0;
    int slot = 0;
    char alive[SH_MAX_STARTED];

    if (g_started_count == 0) {
        return;
    }
    memset(alive, 0, sizeof(alive));
    while (proc_info(index, &info) > 0) {
        for (slot = 0; slot < g_started_count; ++slot) {
            if ((long)info.pid == g_started[slot]) {
                alive[slot] = info.state == SAVANXP_PROC_ZOMBIE ? 2 : 1;
            }
        }
        ++index;
    }
    slot = 0;
    while (slot < g_started_count) {
        if (alive[slot] == 1) {
            ++slot;
            continue;
        }
        if (alive[slot] == 2) {
            int status = 0;
            (void)savanxp_waitpid((int)g_started[slot], &status);
        }
        --g_started_count;
        g_started[slot] = g_started[g_started_count];
        alive[slot] = alive[g_started_count];
    }
}

/* `start <program> [args]`: launch and do not wait, as cmd's start does. The
 * program is not part of the foreground job, so Ctrl+C does not reach it. */
static int sh_start(char* line) {
    struct sh_command_stage stages[SH_MAX_STAGES];
    char path[256];
    int stage_count = sh_parse_pipeline(line, stages, SH_MAX_STAGES);
    long pid = -1;

    if (stage_count != 1 || stages[0].argc < 2 || stages[0].input_path != 0 ||
        stages[0].stdout_path != 0 || stages[0].stderr_path != 0) {
        puts_fd(2, "usage: start <program> [arguments]\n");
        return 1;
    }
    if (!sh_resolve_command_path(stages[0].argv[1], path, sizeof(path))) {
        puts_fd(2, "start: program not found\n");
        return 1;
    }
    pid = spawn(path, (const char* const*)&stages[0].argv[1], stages[0].argc - 1);
    if (pid < 0) {
        puts_fd(2, "start: cannot start the program\n");
        return 1;
    }
    sh_reap_started();
    if (g_started_count < SH_MAX_STARTED) {
        g_started[g_started_count++] = pid;
    }
    return 0;
}

static int sh_wait_for_children(const long* pids, int count) {
    int index = 0;
    int last_status = 0;
    while (index < count) {
        savanxp_waitpid((int)pids[index], &last_status);
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

        if (!is_last && savanxp_pipe(pipe_fds) < 0) {
            puts_fd(2, "sh: pipe failed\n");
            return 1;
        }
        if (stages[index].input_path != 0) {
            if (previous_read_fd >= 0) {
                savanxp_close(previous_read_fd);
                previous_read_fd = -1;
            }
            input_fd = (int)savanxp_open_mode(stages[index].input_path, SAVANXP_OPEN_READ);
            if (input_fd < 0) {
                puts_fd(2, "sh: input path not found\n");
                return 1;
            }
        } else if (previous_read_fd >= 0) {
            input_fd = previous_read_fd;
        }
        if (stages[index].stdout_path != 0) {
            output_fd = (int)savanxp_open_mode(stages[index].stdout_path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE |
                (stages[index].stdout_append ? SAVANXP_OPEN_APPEND : SAVANXP_OPEN_TRUNCATE));
            if (output_fd < 0) {
                puts_fd(2, "sh: unable to open output path\n");
                return 1;
            }
        } else if (!is_last) {
            output_fd = pipe_fds[1];
        }
        if (stages[index].stderr_path != 0) {
            error_fd = (int)savanxp_open_mode(stages[index].stderr_path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE |
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
            savanxp_close(previous_read_fd);
            previous_read_fd = -1;
        }
        if (input_fd > 2) {
            savanxp_close(input_fd);
        }
        if (output_fd > 2) {
            savanxp_close(output_fd);
        }
        if (error_fd > 2 && error_fd != output_fd) {
            savanxp_close(error_fd);
        }
        if (!is_last) {
            previous_read_fd = pipe_fds[0];
        }
    }

    if (previous_read_fd >= 0) {
        savanxp_close(previous_read_fd);
    }
    sh_report_foreground(pids, pid_count);
    {
        const int status = sh_wait_for_children(pids, pid_count);
        sh_report_foreground(0, 0);
        return status;
    }
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
    if (g_console_fd >= 0) {
        /* cmd's default prompt, $P$G. */
        puts_out(cwd);
        puts_out(">");
        return;
    }
    puts_out("savanxp:");
    puts_out(cwd);
    puts_out("$ ");
}

/* The builtins' output goes to 1 and 2 as always; `clear` asks the console to
 * clear its screen buffer, since there is no kernel screen behind a pipe. */
static void sh_console_emit(void* context, int fd, const char* bytes, size_t length) {
    (void)context;
    (void)savanxp_write(fd, bytes, length);
}

static void sh_console_clear(void* context) {
    (void)context;
    sh_console_send("cls\n");
}

/* Runs one command of a line. 1 when it was `exit`. */
static int sh_run_command(char* command) {
    struct shell_capture_sink console_sink = {
        .emit = sh_console_emit,
        .clear = sh_console_clear,
        .context = 0,
    };
    char* start = command;

    while (sh_is_space(*start)) {
        ++start;
    }
    if (*start == '\0') {
        return 0;
    }
    if (strncmp(start, "start", 5) == 0 && (start[5] == '\0' || sh_is_space(start[5]))) {
        (void)sh_start(start);
        return 0;
    }
    if (sh_line_is_builtin(start)) {
        return shell_execute_line(start, SHELL_EXEC_STDIO, g_console_fd >= 0 ? &console_sink : 0) ==
            SHELL_EXEC_RESULT_EXIT;
    }
    (void)sh_execute_pipeline(start);
    return 0;
}

/* Runs a line: commands separated by `;`, one after the other. A `;` inside
 * quotes is part of a word. 1 when one of them was `exit`. */
static int sh_run_line(char* line) {
    char* command = line;
    char quote = 0;
    char* cursor = line;

    for (;;) {
        const char current = *cursor;
        if (quote != 0) {
            if (current == quote) {
                quote = 0;
            } else if (current == '\0') {
                return sh_run_command(command);
            }
            ++cursor;
            continue;
        }
        if (current == '\'' || current == '"') {
            quote = current;
            ++cursor;
            continue;
        }
        if (current == ';' || current == '\0') {
            *cursor = '\0';
            if (sh_run_command(command)) {
                return 1;
            }
            if (current == '\0') {
                return 0;
            }
            command = cursor + 1;
        }
        ++cursor;
    }
}

/* Lines from fd 0, which may hold several per read (a pipe) or one (the
 * kernel tty). 1 with a line in `line`, 0 at end of input. */
static char g_input[512];
static size_t g_input_length = 0;

static int sh_read_line(char* line, size_t capacity) {
    for (;;) {
        size_t index = 0;
        long count = 0;

        while (index < g_input_length && g_input[index] != '\n') {
            ++index;
        }
        if (index < g_input_length || g_input_length == sizeof(g_input)) {
            const size_t take = index < capacity - 1 ? index : capacity - 1;
            const size_t consumed = index < g_input_length ? index + 1 : g_input_length;

            memcpy(line, g_input, take);
            line[take] = '\0';
            memmove(g_input, g_input + consumed, g_input_length - consumed);
            g_input_length -= consumed;
            shell_trim_newline(line);
            return 1;
        }
        count = savanxp_read(0, g_input + g_input_length, sizeof(g_input) - g_input_length);
        if (count > 0) {
            g_input_length += (size_t)count;
            continue;
        }
        /* End of input is only real on a pipe: the kernel console never has
         * one, and used to come back here forever. */
        if (g_console_fd >= 0) {
            if (g_input_length == 0) {
                return 0;
            }
            /* A last line without its newline. */
            g_input[g_input_length] = '\0';
            memcpy(line, g_input, g_input_length < capacity ? g_input_length + 1 : capacity);
            line[capacity - 1] = '\0';
            g_input_length = 0;
            return 1;
        }
    }
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

    if (argc >= 3 && strcmp(argv[1], "--console") == 0) {
        long fd = 0;
        const char* digit = argv[2];
        while (*digit >= '0' && *digit <= '9') {
            fd = fd * 10 + (*digit - '0');
            ++digit;
        }
        g_console_fd = *digit == '\0' && digit != argv[2] ? (int)fd : -1;
    }

    if (g_console_fd >= 0) {
        /* The banner of the Windows 2000 console, with our name. */
        printf("%s [Version %d.%d.%d]\n", SAVANXP_SYSTEM_NAME,
            SAVANXP_VERSION_MAJOR, SAVANXP_VERSION_MINOR, SAVANXP_VERSION_PATCH);
        puts_out("Type 'help' for commands. 'start <program>' opens a program.\n\n");
    } else {
        printf("%s\n", SAVANXP_DISPLAY_NAME);
        puts_out("Type 'help' for commands and 'sysinfo' for diagnostics.\n");
    }

    for (;;) {
        sh_reap_started();
        shell_print_prompt();
        memset(line, 0, sizeof(line));
        if (!sh_read_line(line, sizeof(line))) {
            return 0;
        }
        {
            const int had_command = line[0] != '\0';
            if (sh_run_line(line)) {
                return 0;
            }
            if (g_console_fd >= 0 && had_command) {
                /* A blank line between one command's output and the next
                 * prompt, as cmd prints. */
                puts_out("\n");
            }
        }
    }
}
