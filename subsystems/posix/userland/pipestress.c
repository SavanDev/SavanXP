#include "libc.h"

int main(void) {
    const char* echo_argv[] = {"/bin/echo", "pipe", 0};
    const char* cat_argv[] = {"/bin/cat", 0};

    for (int index = 0; index < 8; ++index) {
        int fds[2] = {-1, -1};
        int status_a = 0;
        int status_b = 0;
        long first = -1;
        long second = -1;

        if (savanxp_pipe(fds) < 0) {
            puts_out("pipestress: pipe failed\n");
            return 1;
        }

        first = spawn_fd("/bin/echo", echo_argv, 2, 0, fds[1]);
        second = spawn_fd("/bin/cat", cat_argv, 1, fds[0], 1);
        savanxp_close(fds[0]);
        savanxp_close(fds[1]);

        if (first < 0 || second < 0) {
            puts_out("pipestress: spawn failed\n");
            return 1;
        }
        if (savanxp_waitpid((int)first, &status_a) < 0 || savanxp_waitpid((int)second, &status_b) < 0) {
            puts_out("pipestress: wait failed\n");
            return 1;
        }
        if (status_a != 0 || status_b != 0) {
            puts_out("pipestress: child failed\n");
            return 1;
        }
    }

    puts_out("pipestress: ok\n");
    return 0;
}
