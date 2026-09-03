#include "libc.h"

int main(void) {
    long result = savanxp_write(1, (const void*)1, 4);
    if (result >= 0) {
        puts_out("badptr: invalid write passed\n");
        return 1;
    }

    result = savanxp_open((const char*)1);
    if (result >= 0) {
        puts_out("badptr: invalid open passed\n");
        return 1;
    }

    result = proc_info(0, (struct savanxp_process_info*)1);
    if (result >= 0) {
        puts_out("badptr: invalid proc_info passed\n");
        return 1;
    }

    puts_out("badptr: ok\n");
    return 0;
}
