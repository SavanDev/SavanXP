#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "coreutils.h"

static int g_recursive = 0;

static int copy_path(const char* from, const char* to);

static int copy_file(const char* from, const char* to) {
    const int in_fd = open(from, O_RDONLY);
    if (in_fd < 0) {
        cu_fail("cp", "can't open", from);
        return 1;
    }
    const int out_fd = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        cu_fail("cp", "can't create", to);
        close(in_fd);
        return 1;
    }
    int status = 0;
    if (cu_copy_fd(in_fd, out_fd) != 0) {
        cu_fail("cp", "error copying to", to);
        status = 1;
    }
    close(in_fd);
    if (close(out_fd) != 0 && status == 0) {
        cu_fail("cp", "error closing", to);
        status = 1;
    }
    return status;
}

static int copy_directory(const char* from, const char* to) {
    if (!cu_is_directory(to) && mkdir(to, 0755) != 0 && errno != EEXIST) {
        cu_fail("cp", "can't create directory", to);
        return 1;
    }
    DIR* directory = opendir(from);
    if (directory == NULL) {
        cu_fail("cp", "can't open", from);
        return 1;
    }
    int status = 0;
    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        char child_from[256];
        char child_to[256];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (cu_join(child_from, sizeof(child_from), from, entry->d_name) != 0 ||
            cu_join(child_to, sizeof(child_to), to, entry->d_name) != 0) {
            cu_fail("cp", "can't copy", entry->d_name);
            status = 1;
            continue;
        }
        status |= copy_path(child_from, child_to);
    }
    closedir(directory);
    return status;
}

static int copy_path(const char* from, const char* to) {
    struct stat info;

    if (stat(from, &info) != 0) {
        cu_fail("cp", "can't stat", from);
        return 1;
    }
    if (S_ISDIR(info.st_mode)) {
        if (!g_recursive) {
            fprintf(stderr, "cp: '%s' is a directory (use -r)\n", from);
            return 1;
        }
        return copy_directory(from, to);
    }
    return copy_file(from, to);
}

/* cp [-r] source... dest: into dest when it is a directory. */
int main(int argc, char** argv) {
    int status = 0;
    int index = 1;

    if (index < argc && (strcmp(argv[index], "-r") == 0 || strcmp(argv[index], "-R") == 0)) {
        g_recursive = 1;
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
        char target[256];
        const char* to = dest;

        if (into_directory) {
            if (cu_join(target, sizeof(target), dest, cu_basename(argv[index])) != 0) {
                cu_fail("cp", "can't copy", argv[index]);
                status = 1;
                continue;
            }
            to = target;
        }
        status |= copy_path(argv[index], to);
    }
    return status;
}
