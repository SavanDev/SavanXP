#include <stdio.h>

#include "coreutils.h"

/* mv source... dest: renames; into dest when it is a directory. */
int main(int argc, char** argv) {
    int status = 0;

    if (argc < 3) {
        fputs("usage: mv <source>... <dest>\n", stderr);
        return 1;
    }
    const char* dest = argv[argc - 1];
    const int into_directory = cu_is_directory(dest);

    if (argc > 3 && !into_directory) {
        fprintf(stderr, "mv: '%s' is not a directory\n", dest);
        return 1;
    }
    for (int index = 1; index < argc - 1; ++index) {
        char target[256];
        const char* to = dest;

        if (into_directory) {
            if (cu_join(target, sizeof(target), dest, cu_basename(argv[index])) != 0) {
                cu_fail("mv", "can't move", argv[index]);
                status = 1;
                continue;
            }
            to = target;
        }
        if (rename(argv[index], to) != 0) {
            cu_fail("mv", "can't move", argv[index]);
            status = 1;
        }
    }
    return status;
}
