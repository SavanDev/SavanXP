#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "coreutils.h"

/* cat [file...]: copies each file to stdout; stdin when there is none, or for "-". */
int main(int argc, char** argv) {
    int status = 0;

    if (argc < 2) {
        return cu_copy_fd(0, 1) == 0 ? 0 : 1;
    }
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "-") == 0) {
            if (cu_copy_fd(0, 1) != 0) {
                status = 1;
            }
            continue;
        }
        const int fd = open(argv[index], O_RDONLY);
        if (fd < 0) {
            cu_fail("cat", "can't open", argv[index]);
            status = 1;
            continue;
        }
        if (cu_copy_fd(fd, 1) != 0) {
            cu_fail("cat", "read error on", argv[index]);
            status = 1;
        }
        close(fd);
    }
    return status;
}
