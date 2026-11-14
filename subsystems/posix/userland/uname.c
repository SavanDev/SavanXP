#include "libc.h"

#include "shared/version.h"

int main(void) {
#if defined(__i386__)
    printf("%s i386 userland prototype\n", SAVANXP_DISPLAY_NAME);
#else
    printf("%s x86_64 userland prototype\n", SAVANXP_DISPLAY_NAME);
#endif
    return 0;
}
