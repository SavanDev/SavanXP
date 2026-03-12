#include "libc.h"

int main(void) {
    const long result = sync();
    if (result < 0) {
        eprintf("sync: failed (%s)\n", result_error_string(result));
        return 1;
    }
    puts("sync: ok\n");
    return 0;
}
