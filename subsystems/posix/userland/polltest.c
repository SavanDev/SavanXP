#include "libc.h"

int main(void) {
    int fds[2] = {-1, -1};
    char value = '\0';
    struct savanxp_pollfd ready = {
        .fd = -1,
        .events = SAVANXP_POLLIN,
        .revents = 0,
    };

    if (savanxp_pipe(fds) < 0) {
        puts_out("polltest: pipe failed\n");
        return 1;
    }

    ready.fd = fds[0];
    if (savanxp_poll(&ready, 1, 0) != 0 || ready.revents != 0) {
        puts_out("polltest: unexpected readiness before write\n");
        return 1;
    }

    if (savanxp_fcntl(fds[0], SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0) {
        puts_out("polltest: fcntl set nonblock failed\n");
        return 1;
    }

    if (savanxp_read(fds[0], &value, 1) != -SAVANXP_EAGAIN) {
        puts_out("polltest: expected EAGAIN on empty nonblocking read\n");
        return 1;
    }

    value = 'x';
    if (savanxp_write(fds[1], &value, 1) != 1) {
        puts_out("polltest: write failed\n");
        return 1;
    }

    ready.revents = 0;
    if (savanxp_poll(&ready, 1, 1000) != 1 || (ready.revents & SAVANXP_POLLIN) == 0) {
        puts_out("polltest: poll did not observe readable pipe\n");
        return 1;
    }

    value = '\0';
    if (savanxp_read(fds[0], &value, 1) != 1 || value != 'x') {
        puts_out("polltest: readback failed\n");
        return 1;
    }

    savanxp_close(fds[0]);
    savanxp_close(fds[1]);
    puts_out("polltest: ok\n");
    return 0;
}
