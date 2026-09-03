#include "libc.h"

static int write_text(int fd, const char* text) {
    return savanxp_write(fd, text, strlen(text)) == (long)strlen(text) ? 0 : 1;
}

int main(void) {
    const char* path = "/disk/seektest.txt";
    long fd = savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (fd < 0) {
        puts_out("seektest: open failed\n");
        return 1;
    }

    if (write_text((int)fd, "ABCDE") != 0) {
        puts_out("seektest: initial write failed\n");
        savanxp_close((int)fd);
        return 1;
    }

    if (seek((int)fd, 2, SAVANXP_SEEK_SET) < 0 || write_text((int)fd, "xy") != 0) {
        puts_out("seektest: overwrite failed\n");
        savanxp_close((int)fd);
        return 1;
    }

    savanxp_close((int)fd);

    fd = savanxp_open(path);
    if (fd < 0) {
        puts_out("seektest: reopen failed\n");
        return 1;
    }

    char buffer[8] = {};
    const long count = savanxp_read((int)fd, buffer, 5);
    savanxp_close((int)fd);
    if (count != 5 || strncmp(buffer, "ABxyE", 5) != 0) {
        puts_out("seektest: unexpected data\n");
        return 1;
    }

    puts_out("seektest: ok\n");
    return 0;
}
