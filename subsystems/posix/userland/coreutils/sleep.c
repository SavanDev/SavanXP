#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* sleep <seconds> */
int main(int argc, char** argv) {
    char* end = NULL;

    if (argc < 2) {
        fputs("usage: sleep <seconds>\n", stderr);
        return 1;
    }
    const unsigned long seconds = strtoul(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0') {
        fprintf(stderr, "sleep: invalid number '%s'\n", argv[1]);
        return 1;
    }
    sleep((unsigned)seconds);
    return 0;
}
