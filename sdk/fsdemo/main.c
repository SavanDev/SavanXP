#include "libc.h"

int main(void) {
    const char* path = "/disk/tmp/fsdemo.txt";
    const char* message = "fsdemo: persisted via sdk\n";
    char buffer[64];

    long fd = open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (fd < 0) {
        puts_fd(2, "fsdemo: open write failed\n");
        return 1;
    }
    if (write((int)fd, message, strlen(message)) < 0) {
        puts_fd(2, "fsdemo: write failed\n");
        close((int)fd);
        return 1;
    }
    close((int)fd);

    fd = open(path);
    if (fd < 0) {
        puts_fd(2, "fsdemo: open read failed\n");
        return 1;
    }

    memset(buffer, 0, sizeof(buffer));
    const long count = read((int)fd, buffer, sizeof(buffer) - 1);
    close((int)fd);
    if (count < 0) {
        puts_fd(2, "fsdemo: read failed\n");
        return 1;
    }

    puts(buffer);
    return 0;
}
