#include "libc.h"

int main(void) {
    puts_fd(2, "errdemo: stderr works\n");
    return 0;
}
