#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "coreutils.h"
#include "../file_copy.h"

/* The copy itself is Files' engine (file_copy.h), run to the end in one go:
 * a folder is filled when it exists, a file overwritten, and a failure on one
 * entry is reported and skipped, as cp always did. */

static struct file_copy g_copy;

static void cp_report(const char* path, const char* what, int error, void* user) {
    char action[32];

    (void)user;
    snprintf(action, sizeof(action), "can't %s", what);
    errno = error;
    cu_fail("cp", action, path);
}

static int copy_path(const char* from, const char* to, int recursive) {
    struct stat info;
    int result;

    if (stat(from, &info) != 0) {
        cu_fail("cp", "can't stat", from);
        return 1;
    }
    if (S_ISDIR(info.st_mode) && !recursive) {
        fprintf(stderr, "cp: '%s' is a directory (use -r)\n", from);
        return 1;
    }
    g_copy.report = cp_report;
    result = file_copy_begin(&g_copy, from, to, FILE_COPY_MERGE | FILE_COPY_NO_COUNT | FILE_COPY_KEEP_GOING);
    if (result == -EINVAL) {
        fprintf(stderr, "cp: can't copy '%s' onto or into itself\n", from);
        return 1;
    }
    if (result < 0) {
        errno = -result;
        cu_fail("cp", "can't copy", from);
        return 1;
    }
    while ((result = file_copy_step(&g_copy)) > 0) {
    }
    return result < 0 || g_copy.failures != 0 ? 1 : 0;
}

/* cp [-r] source... dest: into dest when it is a directory. */
int main(int argc, char** argv) {
    int status = 0;
    int recursive = 0;
    int index = 1;

    if (index < argc && (strcmp(argv[index], "-r") == 0 || strcmp(argv[index], "-R") == 0)) {
        recursive = 1;
        ++index;
    }
    if (argc - index < 2) {
        fputs("usage: cp [-r] <source>... <dest>\n", stderr);
        return 1;
    }
    const char* dest = argv[argc - 1];
    const int into_directory = cu_is_directory(dest);

    if (argc - index > 2 && !into_directory) {
        fprintf(stderr, "cp: '%s' is not a directory\n", dest);
        return 1;
    }
    for (; index < argc - 1; ++index) {
        char target[FILE_COPY_PATH_CAPACITY];
        const char* to = dest;

        if (into_directory) {
            if (cu_join(target, sizeof(target), dest, cu_basename(argv[index])) != 0) {
                cu_fail("cp", "can't copy", argv[index]);
                status = 1;
                continue;
            }
            to = target;
        }
        status |= copy_path(argv[index], to, recursive);
    }
    return status;
}
