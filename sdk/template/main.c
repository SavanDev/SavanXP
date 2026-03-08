#include "savanxp/libc.h"

int main(int argc, char** argv) {
    puts("template: your app is running\n");
    printf("template: argc=%d\n", argc);

    for (int index = 0; index < argc; ++index) {
        printf("argv[%d]=%s\n", index, argv[index]);
    }

    return 0;
}
