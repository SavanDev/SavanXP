#include "savanxp/libc.h"

#include "message.h"

int main(int argc, char** argv) {
    print_banner();
    printf("multifile: argc=%d\n", argc);

    for (int index = 0; index < argc; ++index) {
        printf("multifile: argv[%d]=%s\n", index, argv[index]);
    }

    return 0;
}
