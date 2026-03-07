#include "libc.h"

static const char* state_name(unsigned long state) {
    switch (state) {
        case SAVANXP_PROC_READY: return "ready";
        case SAVANXP_PROC_RUNNING: return "run";
        case SAVANXP_PROC_BLOCKED_READ: return "blk-r";
        case SAVANXP_PROC_BLOCKED_WRITE: return "blk-w";
        case SAVANXP_PROC_BLOCKED_WAIT: return "wait";
        case SAVANXP_PROC_SLEEPING: return "sleep";
        case SAVANXP_PROC_ZOMBIE: return "zombie";
        default: return "unused";
    }
}

int main(void) {
    struct savanxp_process_info info;

    puts("PID  PPID STATE   NAME\n");
    for (unsigned long index = 0;; ++index) {
        long result = proc_info(index, &info);
        if (result < 0) {
            puts("ps: proc_info failed\n");
            return 1;
        }
        if (result == 0) {
            break;
        }

        printf("%u %u %s %s\n",
            (unsigned int)info.pid,
            (unsigned int)info.parent_pid,
            state_name(info.state),
            info.name);
    }

    return 0;
}
