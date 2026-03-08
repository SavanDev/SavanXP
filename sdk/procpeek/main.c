#include "savanxp/libc.h"

int main(void) {
    struct savanxp_process_info info;

    for (unsigned long index = 0; proc_info(index, &info) > 0; ++index) {
        printf(
            "pid=%u ppid=%u state=%s exit=%d name=%s\n",
            info.pid,
            info.parent_pid,
            process_state_string(info.state),
            info.exit_code,
            info.name);
    }

    return 0;
}
