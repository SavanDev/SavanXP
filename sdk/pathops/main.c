#include "savanxp/libc.h"

int main(void) {
    const char* dir = "/disk/tmp/sdk-pathops";
    const char* source = "/disk/tmp/sdk-pathops/file.txt";
    const char* renamed = "/disk/tmp/sdk-pathops/moved.txt";
    const char* text = "pathops demo\n";

    mkdir(dir);
    unlink(source);
    unlink(renamed);

    long fd = open_mode(source, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (fd < 0) {
        puts_fd(2, "pathops: open failed\n");
        return 1;
    }
    write((int)fd, text, strlen(text));
    close((int)fd);

    if (rename(source, renamed) < 0) {
        puts_fd(2, "pathops: rename failed\n");
        return 1;
    }
    if (truncate(renamed, 4) < 0) {
        puts_fd(2, "pathops: truncate failed\n");
        return 1;
    }

    fd = open(renamed);
    if (fd < 0) {
        puts_fd(2, "pathops: reopen failed\n");
        return 1;
    }

    char buffer[16];
    memset(buffer, 0, sizeof(buffer));
    read((int)fd, buffer, sizeof(buffer) - 1);
    close((int)fd);
    puts(buffer);
    putchar(1, '\n');

    unlink(renamed);
    rmdir(dir);
    return 0;
}
