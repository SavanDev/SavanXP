#include "savanxp/libc.h"

int main(void) {
    const char* argv[] = {"/bin/uname", 0};
    int status = 0;

    long pid = spawn("/bin/uname", argv, 1);
    if (pid < 0) {
        eprintf("spawnwait: spawn failed: %s\n", result_error_string(pid));
        return 1;
    }

    if (waitpid((int)pid, &status) < 0) {
        puts_err("spawnwait: waitpid failed\n");
        return 1;
    }

    printf("spawnwait: child exit=%d\n", status);
    return 0;
}
