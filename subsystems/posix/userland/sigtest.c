#include "libc.h"

int main(void) {
    struct savanxp_process_info info;
    int init_pid = 0;
    for (int index = 0; index < 64; ++index) {
        if (proc_info((unsigned int)index, &info) <= 0) {
            continue;
        }
        if (strncmp(info.name, "init", sizeof(info.name)) == 0) {
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
