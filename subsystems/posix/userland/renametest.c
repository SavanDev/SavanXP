#include "libc.h"

static int contains_line(int fd, const char* name) {
    char entry[64];
    for (;;) {
        const long count = readdir(fd, entry, sizeof(entry));
        if (count <= 0) {
            return 0;
        }
        if (strcmp(entry, name) == 0) {
            return 1;
        }
    }
}

int main(void) {
    const char* dir = "/disk/tmp/rename-a";
    const char* moved_dir = "/disk/tmp/rename-b";
    const char* file = "/disk/tmp/rename-a/file.txt";
    const char* moved_file = "/disk/tmp/rename-b/data.txt";
    const char* text = "rename works\n";
    char buffer[32];

    unlink(moved_file);
    unlink(file);
    rmdir(moved_dir);
    rmdir(dir);

    if (mkdir(dir) < 0) {
        puts_fd(2, "renametest: mkdir failed\n");
        return 1;
    }

    long fd = open_mode(file, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (fd < 0 || write((int)fd, text, strlen(text)) < 0) {
        puts_fd(2, "renametest: write failed\n");
        if (fd >= 0) {
            close((int)fd);
        }
        return 1;
    }
    close((int)fd);

    if (rename(dir, moved_dir) < 0) {
        puts_fd(2, "renametest: directory rename failed\n");
        return 1;
    }
    if (rename(moved_file, moved_dir) >= 0) {
        puts_fd(2, "renametest: rename into self should fail\n");
        return 1;
    }
    if (rename(moved_file, moved_file) >= 0) {
        puts_fd(2, "renametest: same-path rename should fail\n");
        return 1;
    }

    fd = open(moved_file);
    if (fd < 0) {
        puts_fd(2, "renametest: renamed file missing\n");
        return 1;
    }
    memset(buffer, 0, sizeof(buffer));
    if (read((int)fd, buffer, sizeof(buffer) - 1) <= 0 || strcmp(buffer, text) != 0) {
        puts_fd(2, "renametest: renamed file content mismatch\n");
        close((int)fd);
        return 1;
    }
    close((int)fd);

    fd = open("/disk/tmp");
    if (fd < 0 || !contains_line((int)fd, "rename-b")) {
        puts_fd(2, "renametest: parent listing missing renamed directory\n");
        if (fd >= 0) {
            close((int)fd);
        }
        return 1;
    }
    close((int)fd);

    if (unlink(moved_file) < 0 || rmdir(moved_dir) < 0) {
        puts_fd(2, "renametest: cleanup failed\n");
        return 1;
    }

    puts("renametest: ok\n");
    return 0;
}
