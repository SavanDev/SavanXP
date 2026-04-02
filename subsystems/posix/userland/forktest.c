#include "libc.h"

int main(void) {
    puts("forktest: start\n");

    long child = fork();
    if (child < 0) {
        printf("forktest: fork failed (%s)\n", result_error_string(child));
        return 1;
    }

    if (child == 0) {
        puts("forktest: child\n");
        exit(42);
    }

    int status = -1;
    if (waitpid((int)child, &status) < 0) {
        puts("forktest: waitpid failed\n");
        return 1;
    }
    if (status != 42) {
        printf("forktest: expected 42 got %d\n", status);
        return 1;
    }

    printf("forktest: ok child=%d status=%d\n", (int)child, status);
    return 0;
}
