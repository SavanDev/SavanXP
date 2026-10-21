#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "coreutils.h"

static int g_force = 0;
static int g_recursive = 0;

static int remove_path(const char* path);

/* Reads the first entry of a directory other than "." and "..". 1 if found. */
static int first_entry(const char* path, char* out, size_t capacity) {
    DIR* directory = opendir(path);
    struct dirent* entry;
    int found = 0;

    if (directory == NULL) {
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            found = cu_join(out, capacity, path, entry->d_name) == 0 ? 1 : -1;
            break;
        }
    }
    closedir(directory);
    return found;
}

static int remove_directory(const char* path) {
    char child[256];

    /* Reopen after every removal instead of deleting while iterating: a
     * directory changing under readdir may skip entries. */
    for (;;) {
        const int found = first_entry(path, child, sizeof(child));
        if (found < 0) {
            cu_fail("rm", "can't read", path);
            return 1;
        }
        if (found == 0) {
            break;
        }
        if (remove_path(child) != 0) {
            return 1;
        }
    }
    if (rmdir(path) != 0) {
        cu_fail("rm", "can't remove", path);
        return 1;
    }
    return 0;
}

static int remove_path(const char* path) {
    struct stat info;

    if (lstat(path, &info) != 0) {
        if (g_force && errno == ENOENT) {
            return 0;
        }
        cu_fail("rm", "can't remove", path);
        return 1;
    }
    if (S_ISDIR(info.st_mode)) {
        if (!g_recursive) {
            fprintf(stderr, "rm: '%s' is a directory\n", path);
            return 1;
        }
        return remove_directory(path);
    }
    if (unlink(path) != 0) {
        cu_fail("rm", "can't remove", path);
        return 1;
    }
    return 0;
}

/* rm [-f] [-r] path... (flags may be combined, as in -rf) */
int main(int argc, char** argv) {
    int status = 0;
    int index = 1;

    for (; index < argc && argv[index][0] == '-' && argv[index][1] != '\0'; ++index) {
        for (const char* flag = argv[index] + 1; *flag != '\0'; ++flag) {
            if (*flag == 'f') {
                g_force = 1;
            } else if (*flag == 'r' || *flag == 'R') {
                g_recursive = 1;
            } else {
                fprintf(stderr, "rm: unknown option '-%c'\n", *flag);
                return 1;
            }
        }
    }
    if (index >= argc) {
        if (g_force) {
            return 0;
        }
        fputs("usage: rm [-f] [-r] <path>...\n", stderr);
        return 1;
    }
    for (; index < argc; ++index) {
        status |= remove_path(argv[index]);
    }
    return status;
}
