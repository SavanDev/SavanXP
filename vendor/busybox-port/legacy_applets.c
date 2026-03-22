#include "libbb.h"

static int list_path(const char* path) {
    struct stat info = {};
    if (stat(path, &info) != 0) {
        bb_perror_msg("can't stat '%s'", path);
        return 1;
    }

    if (!S_ISDIR(info.st_mode)) {
        puts(path);
        return 0;
    }

    DIR* directory = opendir(path);
    if (directory == NULL) {
        bb_perror_msg("can't open '%s'", path);
        return 1;
    }

    struct dirent* entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (DOT_OR_DOTDOT(entry->d_name)) {
            continue;
        }
        puts(entry->d_name);
    }
    closedir(directory);
    return 0;
}

int bb_ls_main(int argc, char** argv) {
    int status = 0;
    (void)argc;
    (void)getopt32(argv, "");
    argv += optind;
    if (*argv == NULL) {
        return list_path(".");
    }
    do {
        status |= list_path(*argv);
    } while (*++argv != NULL);
    return status;
}

int bb_ps_main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    struct savanxp_process_info info = {};
    puts("PID  PPID STATE         NAME");
    for (unsigned long index = 0;; ++index) {
        const long result = proc_info(index, &info);
        if (result < 0) {
            bb_simple_error_msg("proc_info failed");
            return 1;
        }
        if (result == 0) {
            break;
        }
        printf("%u %u %-13s %s\n",
            (unsigned)info.pid,
            (unsigned)info.parent_pid,
            info.state == SAVANXP_PROC_READY ? "ready" :
            info.state == SAVANXP_PROC_RUNNING ? "running" :
            info.state == SAVANXP_PROC_BLOCKED_READ ? "blocked-read" :
            info.state == SAVANXP_PROC_BLOCKED_WRITE ? "blocked-write" :
            info.state == SAVANXP_PROC_BLOCKED_WAIT ? "blocked-wait" :
            info.state == SAVANXP_PROC_SLEEPING ? "sleeping" :
            info.state == SAVANXP_PROC_ZOMBIE ? "zombie" : "unused",
            info.name);
    }
    return 0;
}

int bb_true_main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return 0;
}

int bb_false_main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return 1;
}

int bb_sleep_main(int argc, char** argv) {
    if (argc < 2) {
        bb_show_usage();
    }
    sleep((unsigned)strtoul(argv[1], NULL, 10));
    return 0;
}
