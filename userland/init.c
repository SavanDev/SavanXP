#include "libc.h"

int main(void) {
    const char* shell_argv[] = {"/bin/sh", 0};
    const char* smoke_argv[] = {"/disk/bin/smoke", 0};

    long smoke_trigger = open("/SMOKE");
    if (smoke_trigger >= 0) {
        int status = 0;
        close((int)smoke_trigger);
        long smoke_fd = open("/disk/bin/smoke");
        if (smoke_fd < 0) {
            printf("init: smoke binary missing (%s)\n", result_error_string(smoke_fd));
            return 1;
        }
        close((int)smoke_fd);
        long pid = spawn("/disk/bin/smoke", smoke_argv, 1);
        if (pid < 0) {
            printf("init: failed to spawn smoke runner (%s)\n", result_error_string(pid));
            return 1;
        }
        waitpid((int)pid, &status);
        printf("init: smoke runner exited with %d\n", status);
        for (;;) {
            sleep_ms(1000);
        }
    }

    for (;;) {
        int status = 0;
        long pid = spawn("/bin/sh", shell_argv, 1);
        if (pid < 0) {
            puts("init: failed to spawn shell\n");
            return 1;
        }

        waitpid((int)pid, &status);
        puts("init: shell exited, restarting\n");
    }
}
