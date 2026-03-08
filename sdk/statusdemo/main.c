#include "savanxp/libc.h"

int main(void) {
    const long missing = open("/disk/tmp/does-not-exist.txt");
    if (result_is_error(missing)) {
        eprintf(
            "statusdemo: open failed (%d: %s)\n",
            result_error_code(missing),
            result_error_string(missing));
    }

    struct savanxp_process_info info;
    if (proc_info(0, &info) > 0) {
        printf(
            "statusdemo: pid=%u state=%s sdk=%u.%u\n",
            info.pid,
            process_state_string(info.state),
            SAVANXP_SDK_VERSION_MAJOR,
            SAVANXP_SDK_VERSION_MINOR);
    }

    return 0;
}
