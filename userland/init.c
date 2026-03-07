#include "libc.h"

int main(void) {
    const char* shell_argv[] = {"/bin/sh", 0};

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
