#include "libc.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        puts("rm: missing path\n");
        return 1;
    }

    int status = 0;
    for (int index = 1; index < argc; ++index) {
        if (unlink(argv[index]) < 0) {
            printf("rm: failed %s\n", argv[index]);
            status = 1;
        }
    }

    return status;
}
