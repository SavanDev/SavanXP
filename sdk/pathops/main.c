#include "savanxp/libc.h"

int main(void) {
    const char* dir = "/disk/tmp/sdk-pathops";
    const char* source = "/disk/tmp/sdk-pathops/file.txt";
    const char* renamed = "/disk/tmp/sdk-pathops/moved.txt";
    const char* text = "pathops demo\n";

    savanxp_mkdir(dir);
    savanxp_unlink(source);
    savanxp_unlink(renamed);

    long fd = savanxp_open_mode(source, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (fd < 0) {
        puts_fd(2, "pathops: open failed\n");
        return 1;
    }
    savanxp_write((int)fd, text, strlen(text));
    savanxp_close((int)fd);

    if (savanxp_rename(source, renamed) < 0) {
        puts_fd(2, "pathops: rename failed\n");
        return 1;
    }
    if (savanxp_truncate(renamed, 4) < 0) {
        puts_fd(2, "pathops: truncate failed\n");
        return 1;
    }

    fd = savanxp_open(renamed);
    if (fd < 0) {
        puts_fd(2, "pathops: reopen failed\n");
        return 1;
    }

    char buffer[16];
    memset(buffer, 0, sizeof(buffer));
    savanxp_read((int)fd, buffer, sizeof(buffer) - 1);
    savanxp_close((int)fd);
    puts_out(buffer);
    putchar_fd(1, '\n');

    savanxp_unlink(renamed);
    savanxp_rmdir(dir);
    return 0;
}
