#include "libc.h"

static int check_self_signals(void) {
    const int signals[] = {SAVANXP_SIGINT, SAVANXP_SIGKILL, SAVANXP_SIGPIPE};
    if (savanxp_kill(-1, SAVANXP_SIGCHLD) != -SAVANXP_EINVAL ||
        savanxp_kill(0, 0) != -SAVANXP_EINVAL ||
        savanxp_kill(0, SAVANXP_SIGCHLD) != 0) return 0;
    for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        long child = savanxp_fork();
        if (child == 0) {
            savanxp_kill(0, signals[i]);
            return 1;
        }
        int status;
        if (child < 0 || savanxp_waitpid((int)child, &status) != child ||
            status != 128 + signals[i]) return 0;
    }
    return 1;
}

static int check_zombie_and_pipe(void) {
    int fds[2];
    if (savanxp_pipe(fds) < 0) return 0;
    long child = savanxp_fork();
    if (child == 0) exit(42);
    savanxp_close(fds[1]);
    // EOF follows the child's descriptor cleanup. Its status is still waitable.
    char byte = 'x';
    if (child < 0 || savanxp_read(fds[0], &byte, 1) != 0) return 0;
    savanxp_close(fds[0]);
    int status;
    if (savanxp_kill((int)child, SAVANXP_SIGTERM) != 0 ||
        savanxp_waitpid((int)child, &status) != child || status != 42 ||
        savanxp_kill((int)child, SAVANXP_SIGCHLD) != -SAVANXP_ENOENT) return 0;

    if (savanxp_pipe(fds) < 0) return 0;
    savanxp_close(fds[0]);
    const long result = savanxp_write(fds[1], &byte, 1);
    savanxp_close(fds[1]);
    return result == -SAVANXP_EPIPE;
}

int main(void) {
    if (!check_self_signals() || !check_zombie_and_pipe()) {
        puts_out("sigtest: signal targeting or broken-pipe semantics failed\n");
        return 1;
    }
    struct savanxp_process_info info;
    int init_pid = 0;
    for (int index = 0; index < 64; ++index) {
        if (proc_info((unsigned int)index, &info) <= 0) {
            continue;
        }
        if (!info.parent_pid && !(info.flags & SAVANXP_PROC_FLAG_IDLE)) {
            init_pid = (int)info.pid;
            break;
        }
    }
    if (init_pid == 0) {
        puts_out("sigtest: init process not found\n");
        return 1;
    }

    long denied = savanxp_kill(init_pid, SAVANXP_SIGKILL);
    if (denied >= 0 || result_error_code(denied) != SAVANXP_EACCES) {
        printf("sigtest: init kill was not denied (%ld)\n", denied);
        return 1;
    }

    long child = savanxp_fork();
    if (child < 0) {
        puts_out("sigtest: fork failed\n");
        return 1;
    }

    if (child == 0) {
        for (;;) {
            sleep_ms(1000);
        }
    }

    sleep_ms(20);
    if (savanxp_kill((int)child, SAVANXP_SIGTERM) < 0) {
        puts_out("sigtest: kill failed\n");
        return 1;
    }

    int status = -1;
    if (savanxp_waitpid((int)child, &status) < 0) {
        puts_out("sigtest: waitpid failed\n");
        return 1;
    }
    if (status != 128 + SAVANXP_SIGTERM) {
        printf("sigtest: expected %d got %d\n", 128 + SAVANXP_SIGTERM, status);
        return 1;
    }

    printf("sigtest: ok child=%d status=%d\n", (int)child, status);
    return 0;
}
