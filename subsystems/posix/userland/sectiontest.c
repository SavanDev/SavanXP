#include "libc.h"

static int expect_success(long result, const char* label) {
    if (result < 0) {
        eprintf("sectiontest: %s failed (%s)\n", label, result_error_string(result));
        return 0;
    }
    return 1;
}

static int expect_pointer(void* value, const char* label) {
    const long result = (long)value;
    if (value == 0 || result_is_error(result)) {
        eprintf("sectiontest: %s failed (%s)\n", label, result_error_string(result));
        return 0;
    }
    return 1;
}

int main(void) {
    long section = section_create(4096, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (!expect_success(section, "create section")) {
        return 1;
    }

    void* first_view = map_view((int)section, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (!expect_pointer(first_view, "map first view")) {
        close((int)section);
        return 1;
    }

    volatile unsigned char* shared = (volatile unsigned char*)first_view;
    shared[0] = 0x11;

    long child_ready = event_create(SAVANXP_EVENT_AUTO_RESET);
    long parent_ready = event_create(SAVANXP_EVENT_AUTO_RESET);
    if (!expect_success(child_ready, "create child_ready") ||
        !expect_success(parent_ready, "create parent_ready")) {
        unmap_view(first_view);
        close((int)section);
        return 1;
    }

    long pid = fork();
    if (pid < 0) {
        eprintf("sectiontest: fork failed (%s)\n", result_error_string(pid));
        close((int)parent_ready);
        close((int)child_ready);
        unmap_view(first_view);
        close((int)section);
        return 1;
    }

    if (pid == 0) {
        if (shared[0] != 0x11) {
            eprintf("sectiontest: child initial byte mismatch (%u)\n", (unsigned)shared[0]);
            return 2;
        }

        shared[0] = 0x22;
        if (!expect_success(event_set((int)child_ready), "child signal ready")) {
            return 3;
        }
        if (!expect_success(wait_one((int)parent_ready, 1000), "child wait parent")) {
            return 4;
        }
        if (shared[0] != 0x33) {
            eprintf("sectiontest: child shared byte mismatch after parent write (%u)\n", (unsigned)shared[0]);
            return 5;
        }
        if (!expect_success(unmap_view((void*)shared), "child unmap view")) {
            return 6;
        }
        close((int)parent_ready);
        close((int)child_ready);
        close((int)section);
        return 0;
    }

    if (!expect_success(wait_one((int)child_ready, 1000), "parent wait child")) {
        return 1;
    }
    if (shared[0] != 0x22) {
        eprintf("sectiontest: parent shared byte mismatch after child write (%u)\n", (unsigned)shared[0]);
        return 1;
    }

    void* second_view = map_view((int)section, SAVANXP_SECTION_READ);
    if (!expect_pointer(second_view, "map second view")) {
        return 1;
    }
    if (((volatile unsigned char*)second_view)[0] != 0x22) {
        eprintf("sectiontest: second view mismatch (%u)\n", (unsigned)((volatile unsigned char*)second_view)[0]);
        return 1;
    }

    shared[0] = 0x33;
    if (((volatile unsigned char*)second_view)[0] != 0x33) {
        eprintf("sectiontest: second view did not reflect parent write (%u)\n", (unsigned)((volatile unsigned char*)second_view)[0]);
        return 1;
    }
    if (!expect_success(event_set((int)parent_ready), "parent signal child")) {
        return 1;
    }

    int status = -1;
    if (waitpid((int)pid, &status) < 0) {
        eprintf("sectiontest: waitpid failed\n");
        return 1;
    }
    if (status != 0) {
        eprintf("sectiontest: child status %d\n", status);
        return 1;
    }

    if (!expect_success(unmap_view(second_view), "parent unmap second view")) {
        return 1;
    }
    if (!expect_success(unmap_view(first_view), "parent unmap first view")) {
        return 1;
    }

    close((int)parent_ready);
    close((int)child_ready);
    close((int)section);
    return 0;
}
