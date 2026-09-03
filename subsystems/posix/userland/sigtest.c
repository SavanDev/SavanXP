#include "libc.h"

int main(void) {
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
