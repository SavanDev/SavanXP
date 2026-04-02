#include "libc.h"

static int expect_success(long result, const char* label) {
    if (result < 0) {
        eprintf("mmaptest: %s failed (%s)\n", label, result_error_string(result));
        return 0;
    }
    return 1;
}

static int expect_mapping(void* value, const char* label) {
    const long result = (long)value;
    if (result_is_error(result)) {
        eprintf("mmaptest: %s failed (%s)\n", label, result_error_string(result));
        return 0;
    }
    return 1;
}

int main(void) {
    volatile unsigned char* shared = (volatile unsigned char*)mmap(
        0,
        4096,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS,
        -1,
        0);
    if (!expect_mapping((void*)shared, "map shared anonymous")) {
        return 1;
    }

    shared[0] = 0x41;
    long pid = fork();
    if (pid < 0) {
        eprintf("mmaptest: fork shared failed (%s)\n", result_error_string(pid));
        return 1;
    }
    if (pid == 0) {
        if (shared[0] != 0x41) {
            eprintf("mmaptest: child shared initial mismatch (%u)\n", (unsigned)shared[0]);
            return 2;
        }
        shared[0] = 0x42;
        return 0;
    }

    int status = -1;
    if (waitpid((int)pid, &status) < 0) {
        eprintf("mmaptest: waitpid shared failed\n");
        return 1;
    }
    if (status != 0) {
        eprintf("mmaptest: shared child status %d\n", status);
        return 1;
    }
    if (shared[0] != 0x42) {
        eprintf("mmaptest: parent did not observe shared write (%u)\n", (unsigned)shared[0]);
        return 1;
    }

    volatile unsigned char* private_map = (volatile unsigned char*)mmap(
        0,
        4096,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    if (!expect_mapping((void*)private_map, "map private anonymous")) {
        return 1;
    }

    private_map[0] = 0x21;
    pid = fork();
    if (pid < 0) {
        eprintf("mmaptest: fork private failed (%s)\n", result_error_string(pid));
        return 1;
    }
    if (pid == 0) {
        if (private_map[0] != 0x21) {
            eprintf("mmaptest: child private initial mismatch (%u)\n", (unsigned)private_map[0]);
            return 3;
        }
        private_map[0] = 0x22;
        return 0;
    }

    status = -1;
    if (waitpid((int)pid, &status) < 0) {
        eprintf("mmaptest: waitpid private failed\n");
        return 1;
    }
    if (status != 0) {
        eprintf("mmaptest: private child status %d\n", status);
        return 1;
    }
    if (private_map[0] != 0x21) {
        eprintf("mmaptest: private mapping leaked child write (%u)\n", (unsigned)private_map[0]);
        return 1;
    }

    if (!expect_success((long)munmap((void*)private_map, 4096), "unmap private")) {
        return 1;
    }
    if (!expect_success((long)munmap((void*)shared, 4096), "unmap shared")) {
        return 1;
    }
    return 0;
}
