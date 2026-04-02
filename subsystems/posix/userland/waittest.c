#include "libc.h"

int main(void) {
    const char* true_argv[] = {"/bin/true", 0};
    const char* false_argv[] = {"/bin/false", 0};
    int first_status = -1;
    int second_status = -1;
    int saw_zero = 0;
    int saw_one = 0;

    if (spawn("/bin/true", true_argv, 1) < 0 || spawn("/bin/false", false_argv, 1) < 0) {
        puts("waittest: spawn failed\n");
        return 1;
    }

    if (waitpid(-1, &first_status) < 0 || waitpid(-1, &second_status) < 0) {
        puts("waittest: waitpid(-1) failed\n");
        return 1;
    }

    saw_zero = (first_status == 0) || (second_status == 0);
    saw_one = (first_status == 1) || (second_status == 1);
    if (!saw_zero || !saw_one) {
        printf("waittest: unexpected statuses %d %d\n", first_status, second_status);
        return 1;
    }

    puts("waittest: ok\n");
    return 0;
}
