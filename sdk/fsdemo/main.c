#include "savanxp/libc.h"

int main(void) {
    const char* path = "/disk/tmp/fsdemo.txt";
    const char* message = "fsdemo: persisted via sdk\n";
    char buffer[64];

    long fd = savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (fd < 0) {
        puts_fd(2, "fsdemo: open write failed\n");
        return 1;
    }
    if (savanxp_write((int)fd, message, strlen(message)) < 0) {
        puts_fd(2, "fsdemo: write failed\n");
        savanxp_close((int)fd);
        return 1;
    }
    savanxp_close((int)fd);

    fd = savanxp_open(path);
    if (fd < 0) {
        puts_fd(2, "fsdemo: open read failed\n");
        return 1;
    }

    memset(buffer, 0, sizeof(buffer));
    const long count = savanxp_read((int)fd, buffer, sizeof(buffer) - 1);
    savanxp_close((int)fd);
    if (count < 0) {
        puts_fd(2, "fsdemo: read failed\n");
        return 1;
    }

    puts_out(buffer);
    return 0;
}
