#include <stdio.h>

#include "libc.h"

/* ps: one line per process. */
int main(void) {
    struct savanxp_process_info info;

    puts("PID  PPID STATE         NAME");
    for (unsigned long index = 0;; ++index) {
        const long result = proc_info(index, &info);
        if (result < 0) {
            fputs("ps: proc_info failed\n", stderr);
            return 1;
        }
        if (result == 0) {
            break;
        }
        /* State names come from the SDK's process_state_string(); the longest
         * is "blocked_write", hence the 13-wide STATE column. */
        printf("%-4u %-4u %-13s %s\n",
            (unsigned)info.pid,
            (unsigned)info.parent_pid,
            process_state_string(info.state),
            info.name);
    }
    return 0;
}
