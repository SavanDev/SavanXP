/* L2 fixture "args": arguments and environment through musl's start-up. */
#include <stdio.h>
#include <stdlib.h>

extern char **environ;

int main(int argc, char **argv)
{
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("argv[%d]=%s\n", i, argv[i]);
    }
    int n = 0;
    for (char **e = environ; *e; e++, n++) {
        printf("env=%s\n", *e);
    }
    printf("envc=%d\n", n);
    const char *b = getenv("B");
    printf("getenv(B)=%s\n", b ? b : "(null)");
    return argc;
}
