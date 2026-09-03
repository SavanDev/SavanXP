#include "libc.h"

static int check_prefix(const char* buffer, const char* expected, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (buffer[index] != expected[index]) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    const char* dir = "/disk/tmp/trunc-test";
    const char* path = "/disk/tmp/trunc-test/data.txt";
    const char* message = "abcdef";
    char buffer[16];

    savanxp_unlink(path);
    savanxp_rmdir(dir);

    if (savanxp_mkdir(dir) < 0) {
        puts_fd(2, "truncatetest: mkdir failed\n");
        return 1;
    }

    long fd = savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (fd < 0) {
        puts_fd(2, "truncatetest: open failed\n");
        return 1;
    }
    if (savanxp_write((int)fd, message, 6) != 6) {
        puts_fd(2, "truncatetest: write failed\n");
        savanxp_close((int)fd);
        return 1;
    }
    savanxp_close((int)fd);

    if (savanxp_rmdir(dir) >= 0) {
        puts_fd(2, "truncatetest: rmdir should fail on non-empty dir\n");
        return 1;
    }

    if (savanxp_truncate(path, 3) < 0) {
        puts_fd(2, "truncatetest: shrink failed\n");
        return 1;
    }

    memset(buffer, 0, sizeof(buffer));
    fd = savanxp_open(path);
    if (fd < 0) {
        puts_fd(2, "truncatetest: reopen failed\n");
        return 1;
    }
    if (savanxp_read((int)fd, buffer, sizeof(buffer)) != 3 || !check_prefix(buffer, "abc", 3)) {
        puts_fd(2, "truncatetest: shrink verification failed\n");
        savanxp_close((int)fd);
        return 1;
    }
    savanxp_close((int)fd);

    if (savanxp_truncate(path, 8) < 0) {
        puts_fd(2, "truncatetest: extend failed\n");
        return 1;
    }

    memset(buffer, 0xff, sizeof(buffer));
    fd = savanxp_open(path);
    if (fd < 0) {
        puts_fd(2, "truncatetest: reopen after extend failed\n");
        return 1;
    }
    if (savanxp_read((int)fd, buffer, sizeof(buffer)) != 8 || !check_prefix(buffer, "abc", 3)) {
        puts_fd(2, "truncatetest: extend verification failed\n");
        savanxp_close((int)fd);
        return 1;
    }
    for (int index = 3; index < 8; ++index) {
        if (buffer[index] != 0) {
            puts_fd(2, "truncatetest: extension must be zero-filled\n");
            savanxp_close((int)fd);
            return 1;
        }
    }
    savanxp_close((int)fd);

    if (savanxp_unlink(path) < 0) {
        puts_fd(2, "truncatetest: unlink failed\n");
        return 1;
    }
    if (savanxp_rmdir(dir) < 0) {
        puts_fd(2, "truncatetest: rmdir failed on empty dir\n");
        return 1;
    }

    puts_out("truncatetest: ok\n");
    return 0;
}
