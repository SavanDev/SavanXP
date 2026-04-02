#include "libc.h"

int main(void) {
    int fds[2] = {-1, -1};
    char value = '\0';
    struct savanxp_pollfd ready = {
        .fd = -1,
        .events = SAVANXP_POLLIN,
        .revents = 0,
    };

    if (pipe(fds) < 0) {
        puts("polltest: pipe failed\n");
        return 1;
    }

    ready.fd = fds[0];
    if (poll(&ready, 1, 0) != 0 || ready.revents != 0) {
        puts("polltest: unexpected readiness before write\n");
        return 1;
    }

    if (fcntl(fds[0], SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0) {
        puts("polltest: fcntl set nonblock failed\n");
        return 1;
    }

    if (read(fds[0], &value, 1) != -SAVANXP_EAGAIN) {
        puts("polltest: expected EAGAIN on empty nonblocking read\n");
        return 1;
    }

    value = 'x';
    if (write(fds[1], &value, 1) != 1) {
        puts("polltest: write failed\n");
        return 1;
    }

    ready.revents = 0;
    if (poll(&ready, 1, 1000) != 1 || (ready.revents & SAVANXP_POLLIN) == 0) {
        puts("polltest: poll did not observe readable pipe\n");
        return 1;
    }

    value = '\0';
    if (read(fds[0], &value, 1) != 1 || value != 'x') {
        puts("polltest: readback failed\n");
        return 1;
    }

    close(fds[0]);
    close(fds[1]);
    puts("polltest: ok\n");
    return 0;
}
