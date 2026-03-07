#include "libc.h"

int main(void) {
    long result = write(1, (const void*)1, 4);
    if (result >= 0) {
        puts("badptr: invalid write passed\n");
        return 1;
    }

    result = open((const char*)1);
    if (result >= 0) {
        puts("badptr: invalid open passed\n");
        return 1;
    }

    result = proc_info(0, (struct savanxp_process_info*)1);
    if (result >= 0) {
        puts("badptr: invalid proc_info passed\n");
        return 1;
    }

    puts("badptr: ok\n");
    return 0;
}
