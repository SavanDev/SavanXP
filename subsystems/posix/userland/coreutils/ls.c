#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "coreutils.h"

static int list_path(const char* path) {
    struct stat info;

    if (stat(path, &info) != 0) {
        cu_fail("ls", "can't stat", path);
        return 1;
    }
    if (!S_ISDIR(info.st_mode)) {
        puts(path);
        return 0;
    }

    DIR* directory = opendir(path);
    if (directory == NULL) {
        cu_fail("ls", "can't open", path);
        return 1;
    }
    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        puts(entry->d_name);
    }
    closedir(directory);
    return 0;
}

/* ls [path...]: one name per line, in directory order. */
int main(int argc, char** argv) {
    int status = 0;

    if (argc < 2) {
        return list_path(".");
    }
    for (int index = 1; index < argc; ++index) {
        status |= list_path(argv[index]);
    }
    return status;
}
