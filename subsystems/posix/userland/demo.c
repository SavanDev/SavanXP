#include "libc.h"

int main(void) {
    const char* argv_a[] = {"/bin/ticker", "A", "6", "90", 0};
    const char* argv_b[] = {"/bin/ticker", "B", "6", "90", 0};
    int status_a = 0;
    int status_b = 0;

    long first = spawn("/bin/ticker", argv_a, 4);
    long second = spawn("/bin/ticker", argv_b, 4);
    if (first < 0 || second < 0) {
        puts("demo: spawn failed\n");
        return 1;
    }

    waitpid((int)first, &status_a);
    waitpid((int)second, &status_b);
    printf("demo done: %d %d\n", status_a, status_b);
    return 0;
}
