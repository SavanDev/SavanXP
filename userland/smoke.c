#include "libc.h"

static int run_and_expect(const char* path, const char* const* argv, int argc, int expected_status) {
    int status = -1;
    long pid = spawn(path, argv, argc);
    if (pid < 0) {
        printf("SMOKE FAIL spawn %s (%s)\n", path, result_error_string(pid));
        return 0;
    }
    if (waitpid((int)pid, &status) < 0) {
        printf("SMOKE FAIL waitpid %s\n", path);
        return 0;
    }
    if (status != expected_status) {
        printf("SMOKE FAIL status %s expected=%d got=%d\n", path, expected_status, status);
        return 0;
    }
    printf("smoke: ok %s\n", path);
    return 1;
}

static int file_exists(const char* path) {
    long fd = open(path);
    if (fd < 0) {
        return 0;
    }
    close((int)fd);
    return 1;
}

static int validate_copied_readme(void) {
    char buffer[16] = {};
    sync();
    long fd = open("/disk/smoke/readme.copy");
    if (fd < 0) {
        puts("SMOKE FAIL missing copied README\n");
        return 0;
    }
    {
        long size = seek((int)fd, 0, SAVANXP_SEEK_END);
        printf("smoke: copied README size=%d\n", (int)size);
        (void)seek((int)fd, 0, SAVANXP_SEEK_SET);
    }
    if (read((int)fd, buffer, sizeof(buffer) - 1) <= 0) {
        close((int)fd);
        puts("SMOKE FAIL unable to read copied README\n");
        return 0;
    }
    close((int)fd);
    puts("smoke: copied README readable\n");
    return 1;
}

static int write_readme_copy(void) {
    char buffer[256];
    long total = 0;
    long source = open("/README");
    if (source < 0) {
        puts("SMOKE FAIL missing /README\n");
        return 0;
    }

    if (mkdir("/disk/smoke") < 0) {
        close((int)source);
        puts("SMOKE FAIL mkdir /disk/smoke\n");
        return 0;
    }

    long destination = open_mode("/disk/smoke/readme.copy", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (destination < 0) {
        close((int)source);
        puts("SMOKE FAIL open destination on /disk\n");
        return 0;
    }

    for (;;) {
        long count = read((int)source, buffer, sizeof(buffer));
        if (count < 0) {
            close((int)source);
            close((int)destination);
            puts("SMOKE FAIL read /README\n");
            return 0;
        }
        if (count == 0) {
            break;
        }
        if (write((int)destination, buffer, (size_t)count) != count) {
            close((int)source);
            close((int)destination);
            puts("SMOKE FAIL write /disk/smoke/readme.copy\n");
            return 0;
        }
        total += count;
    }

    close((int)source);
    close((int)destination);
    printf("smoke: wrote README copy to /disk bytes=%d\n", (int)total);
    return 1;
}

int main(void) {
    const char* forktest_argv[] = {"/disk/bin/forktest", 0};
    const char* polltest_argv[] = {"/disk/bin/polltest", 0};
    const char* sigtest_argv[] = {"/disk/bin/sigtest", 0};
    const char* bb_ls_argv[] = {"/disk/bin/busybox", "ls", "/disk/bin", 0};
    const char* bb_ps_argv[] = {"/disk/bin/busybox", "ps", 0};

    puts("SMOKE START\n");

    if (!file_exists("/disk/bin/forktest") ||
        !file_exists("/disk/bin/polltest") ||
        !file_exists("/disk/bin/sigtest") ||
        !file_exists("/disk/bin/busybox")) {
        puts("SMOKE FAIL missing binaries in /disk/bin\n");
        return 1;
    }

    if (!run_and_expect("/disk/bin/forktest", forktest_argv, 1, 0) ||
        !run_and_expect("/disk/bin/polltest", polltest_argv, 1, 0) ||
        !run_and_expect("/disk/bin/sigtest", sigtest_argv, 1, 0) ||
        !run_and_expect("/disk/bin/busybox", bb_ls_argv, 3, 0) ||
        !run_and_expect("/disk/bin/busybox", bb_ps_argv, 2, 0) ||
        !write_readme_copy() ||
        !validate_copied_readme()) {
        puts("SMOKE FAIL\n");
        return 1;
    }

    puts("SMOKE PASS\n");
    return 0;
}
