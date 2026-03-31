#include "libc.h"

int main(void) {
    const char* desktop_argv[] = {"/bin/desktop", 0};
    const char* shell_argv[] = {"/bin/sh", 0};
    const char* smoke_argv[] = {"/disk/bin/smoke", 0};
    unsigned long last_desktop_start_ms = 0;
    int rapid_failures = 0;

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
        unsigned long runtime_ms = 0;
        long pid = spawn("/bin/desktop", desktop_argv, 1);
        if (pid < 0) {
            printf("init: failed to spawn desktop (%s)\n", result_error_string(pid));
            sleep_ms(1000);
            continue;
        }

        last_desktop_start_ms = uptime_ms();
        waitpid((int)pid, &status);
        runtime_ms = uptime_ms() - last_desktop_start_ms;
        printf("init: desktop exited with %d, restarting\n", status);

        if (status != 0 && runtime_ms < 2000UL) {
            rapid_failures += 1;
        } else {
            rapid_failures = 0;
        }

        if (rapid_failures >= 3) {
            printf("init: desktop unstable, falling back to /bin/sh\n");
            pid = spawn("/bin/sh", shell_argv, 1);
            if (pid < 0) {
                printf("init: failed to spawn fallback shell (%s)\n", result_error_string(pid));
                sleep_ms(1000);
            } else {
                waitpid((int)pid, &status);
                printf("init: fallback shell exited with %d, retrying desktop\n", status);
            }
            rapid_failures = 0;
        }

        sleep_ms(250);
    }
}
