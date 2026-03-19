#include "libc.h"

static unsigned long parse_number(const char* text) {
    unsigned long value = 0;
    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    for (size_t index = 0; text[index] != '\0'; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
        value = value * 10 + (unsigned long)(text[index] - '0');
    }
    return value;
}

const char* applet_name(const char* path) {
    const char* name = path;
    if (path == 0) {
        return "";
    }
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            name = cursor + 1;
        }
    }
    return name;
}

int dump_file(const char* path) {
    char buffer[256];
    const long fd = open(path);
    if (fd < 0) {
        printf("busybox: cat: %s\n", path);
        return 1;
    }

    for (;;) {
        const long count = read((int)fd, buffer, sizeof(buffer));
        if (count < 0) {
            close((int)fd);
            puts("busybox: cat failed\n");
            return 1;
        }
        if (count == 0) {
            break;
        }
        if (write(1, buffer, (size_t)count) != count) {
            close((int)fd);
            puts("busybox: write failed\n");
            return 1;
        }
    }

    close((int)fd);
    return 0;
}

int cmd_echo(int argc, const char* const* argv) {
    for (int index = 1; index < argc; ++index) {
        if (index > 1) {
            putchar(1, ' ');
        }
        puts_fd(1, argv[index]);
    }
    putchar(1, '\n');
    return 0;
}

int cmd_cat(int argc, const char* const* argv) {
    if (argc < 2) {
        char buffer[256];
        for (;;) {
            const long count = read(0, buffer, sizeof(buffer));
            if (count < 0) {
                puts("busybox: cat stdin failed\n");
                return 1;
            }
            if (count == 0) {
                break;
            }
            if (write(1, buffer, (size_t)count) != count) {
                puts("busybox: cat stdout failed\n");
                return 1;
            }
        }
        return 0;
    }
    for (int index = 1; index < argc; ++index) {
        if (dump_file(argv[index]) != 0) {
            return 1;
        }
    }
    return 0;
}

int cmd_ls(int argc, const char* const* argv) {
    char entry[64];
    const char* path = argc > 1 ? argv[1] : ".";
    const long fd = open(path);
    if (fd < 0) {
        puts("busybox: ls failed\n");
        return 1;
    }

    for (;;) {
        const long length = readdir((int)fd, entry, sizeof(entry));
        if (length < 0) {
            close((int)fd);
            puts("busybox: readdir failed\n");
            return 1;
        }
        if (length == 0) {
            break;
        }
        puts(entry);
        putchar(1, '\n');
    }

    close((int)fd);
    return 0;
}

int cmd_mkdir(int argc, const char* const* argv) {
    if (argc < 2) {
        puts("usage: busybox mkdir <path>...\n");
        return 1;
    }
    for (int index = 1; index < argc; ++index) {
        if (mkdir(argv[index]) < 0) {
            printf("busybox: mkdir failed for %s\n", argv[index]);
            return 1;
        }
    }
    return 0;
}

int cmd_rm(int argc, const char* const* argv) {
    if (argc < 2) {
        puts("usage: busybox rm <path>...\n");
        return 1;
    }
    for (int index = 1; index < argc; ++index) {
        if (unlink(argv[index]) < 0) {
            printf("busybox: rm failed for %s\n", argv[index]);
            return 1;
        }
    }
    return 0;
}

int cmd_mv(int argc, const char* const* argv) {
    if (argc != 3) {
        puts("usage: busybox mv <old> <new>\n");
        return 1;
    }
    if (rename(argv[1], argv[2]) < 0) {
        puts("busybox: mv failed\n");
        return 1;
    }
    return 0;
}

int cmd_cp(int argc, const char* const* argv) {
    char buffer[256];
    if (argc != 3) {
        puts("usage: busybox cp <src> <dst>\n");
        return 1;
    }

    const long input = open(argv[1]);
    if (input < 0) {
        puts("busybox: cp open src failed\n");
        return 1;
    }
    const long output = open_mode(argv[2], SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (output < 0) {
        close((int)input);
        puts("busybox: cp open dst failed\n");
        return 1;
    }

    for (;;) {
        const long count = read((int)input, buffer, sizeof(buffer));
        if (count < 0) {
            close((int)input);
            close((int)output);
            puts("busybox: cp read failed\n");
            return 1;
        }
        if (count == 0) {
            break;
        }
        if (write((int)output, buffer, (size_t)count) != count) {
            close((int)input);
            close((int)output);
            puts("busybox: cp write failed\n");
            return 1;
        }
    }

    close((int)input);
    close((int)output);
    return 0;
}

int cmd_true(void) {
    return 0;
}

int cmd_false(void) {
    return 1;
}

int cmd_sleep(int argc, const char* const* argv) {
    if (argc < 2) {
        puts("usage: busybox sleep <milliseconds>\n");
        return 1;
    }
    sleep_ms(parse_number(argv[1]));
    return 0;
}

int cmd_ps(void) {
    struct savanxp_process_info info = {};
    puts("PID  PPID STATE         NAME\n");
    for (unsigned long index = 0;; ++index) {
        const long result = proc_info(index, &info);
        if (result < 0) {
            puts("busybox: ps failed\n");
            return 1;
        }
        if (result == 0) {
            break;
        }
        printf("%u %u %s %s\n",
            (unsigned int)info.pid,
            (unsigned int)info.parent_pid,
            process_state_string(info.state),
            info.name);
    }
    return 0;
}

int cmd_sh(int argc, const char* const* argv) {
    const char* shell_argv[16] = {};
    int shell_argc = 0;
    shell_argv[shell_argc++] = "/bin/sh";
    for (int index = 1; index < argc && shell_argc + 1 < 16; ++index) {
        shell_argv[shell_argc++] = argv[index];
    }
    shell_argv[shell_argc] = 0;
    const long result = exec("/bin/sh", shell_argv, shell_argc);
    printf("busybox: sh exec failed (%s)\n", result_error_string(result));
    return 1;
}

int main(int argc, const char* const* argv) {
    const char* applet = applet_name(argc > 0 ? argv[0] : "");
    if (strcmp(applet, "busybox") == 0) {
        if (argc < 2) {
            puts("busybox applets: sh ls cat echo mkdir rm mv cp ps true false sleep\n");
            return 1;
        }
        applet = argv[1];
        argv += 1;
        argc -= 1;
    }

    if (strcmp(applet, "echo") == 0) {
        return cmd_echo(argc, argv);
    }
    if (strcmp(applet, "cat") == 0) {
        return cmd_cat(argc, argv);
    }
    if (strcmp(applet, "ls") == 0) {
        return cmd_ls(argc, argv);
    }
    if (strcmp(applet, "mkdir") == 0) {
        return cmd_mkdir(argc, argv);
    }
    if (strcmp(applet, "rm") == 0) {
        return cmd_rm(argc, argv);
    }
    if (strcmp(applet, "mv") == 0) {
        return cmd_mv(argc, argv);
    }
    if (strcmp(applet, "cp") == 0) {
        return cmd_cp(argc, argv);
    }
    if (strcmp(applet, "ps") == 0) {
        return cmd_ps();
    }
    if (strcmp(applet, "true") == 0) {
        return cmd_true();
    }
    if (strcmp(applet, "false") == 0) {
        return cmd_false();
    }
    if (strcmp(applet, "sleep") == 0) {
        return cmd_sleep(argc, argv);
    }
    if (strcmp(applet, "sh") == 0) {
        return cmd_sh(argc, argv);
    }

    printf("busybox: unknown applet %s\n", applet);
    return 1;
}
