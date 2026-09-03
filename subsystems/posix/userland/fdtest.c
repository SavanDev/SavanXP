#include "libc.h"

static int check_dup2_stdout(void) {
    char buffer[64];
    int read_fd = -1;
    long file_fd = savanxp_open_mode("/fdtest.txt", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    long saved_stdout = savanxp_dup(1);
    if (file_fd < 0 || saved_stdout < 0) {
        return 0;
    }

    if (savanxp_dup2((int)file_fd, 1) < 0) {
        return 0;
    }
    puts_out("dup2 works\n");
    if (savanxp_dup2((int)saved_stdout, 1) < 0) {
        return 0;
    }

    savanxp_close((int)saved_stdout);
    savanxp_close((int)file_fd);

    read_fd = (int)savanxp_open("/fdtest.txt");
    if (read_fd < 0) {
        return 0;
    }

    memset(buffer, 0, sizeof(buffer));
    if (savanxp_read(read_fd, buffer, sizeof(buffer) - 1) <= 0) {
        savanxp_close(read_fd);
        return 0;
    }

    savanxp_close(read_fd);
    return strncmp(buffer, "dup2 works", 10) == 0;
}

int main(void) {
    long fd = savanxp_open("/README");
    long other = -1;
    char first[16];
    char second[16];

    if (fd < 0) {
        puts_out("fdtest: open failed\n");
        return 1;
    }

    other = savanxp_dup((int)fd);
    if (other < 0) {
        puts_out("fdtest: dup failed\n");
        return 1;
    }

    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));
    if (savanxp_read((int)fd, first, 8) != 8 || savanxp_read((int)other, second, 8) != 8) {
        puts_out("fdtest: read failed\n");
        return 1;
    }

    if (strncmp(first, second, 8) == 0) {
        puts_out("fdtest: shared offset check failed\n");
        return 1;
    }

    if (!check_dup2_stdout()) {
        puts_out("fdtest: dup2 failed\n");
        return 1;
    }

    puts_out("fdtest: ok\n");
    return 0;
}
