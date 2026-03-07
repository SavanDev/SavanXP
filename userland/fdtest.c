#include "libc.h"

static int check_dup2_stdout(void) {
    char buffer[64];
    int read_fd = -1;
    long file_fd = open_mode("/fdtest.txt", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    long saved_stdout = dup(1);
    if (file_fd < 0 || saved_stdout < 0) {
        return 0;
    }

    if (dup2((int)file_fd, 1) < 0) {
        return 0;
    }
    puts("dup2 works\n");
    if (dup2((int)saved_stdout, 1) < 0) {
        return 0;
    }

    close((int)saved_stdout);
    close((int)file_fd);

    read_fd = (int)open("/fdtest.txt");
    if (read_fd < 0) {
        return 0;
    }

    memset(buffer, 0, sizeof(buffer));
    if (read(read_fd, buffer, sizeof(buffer) - 1) <= 0) {
        close(read_fd);
        return 0;
    }

    close(read_fd);
    return strncmp(buffer, "dup2 works", 10) == 0;
}

int main(void) {
    long fd = open("/README");
    long other = -1;
    char first[16];
    char second[16];

    if (fd < 0) {
        puts("fdtest: open failed\n");
        return 1;
    }

    other = dup((int)fd);
    if (other < 0) {
        puts("fdtest: dup failed\n");
        return 1;
    }

    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));
    if (read((int)fd, first, 8) != 8 || read((int)other, second, 8) != 8) {
        puts("fdtest: read failed\n");
        return 1;
    }

    if (strncmp(first, second, 8) == 0) {
        puts("fdtest: shared offset check failed\n");
        return 1;
    }

    if (!check_dup2_stdout()) {
        puts("fdtest: dup2 failed\n");
        return 1;
    }

    puts("fdtest: ok\n");
    return 0;
}
