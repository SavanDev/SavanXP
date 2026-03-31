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

static int text_contains(const char* text, const char* needle) {
    size_t needle_length = strlen(needle);
    if (needle_length == 0) {
        return 1;
    }
    for (size_t index = 0; text[index] != '\0'; ++index) {
        if (strncmp(text + index, needle, needle_length) == 0) {
            return 1;
        }
    }
    return 0;
}

static int validate_file_contains(const char* path, const char* needle) {
    char buffer[256] = {};
    sync();
    long fd = open(path);
    if (fd < 0) {
        printf("SMOKE FAIL missing %s\n", path);
        return 0;
    }
    if (read((int)fd, buffer, sizeof(buffer) - 1) <= 0) {
        close((int)fd);
        printf("SMOKE FAIL unable to read %s\n", path);
        return 0;
    }
    close((int)fd);
    if (!text_contains(buffer, needle)) {
        printf("SMOKE FAIL %s missing content '%s'\n", path, needle);
        return 0;
    }
    printf("smoke: validated %s\n", path);
    return 1;
}

static int file_missing(const char* path) {
    return !file_exists(path);
}

static int prepare_smoke_directory(void) {
    long existing = open("/disk/smoke");
    const char* mkdir_argv[] = {"/disk/bin/mkdir", "/disk/smoke", 0};
    if (existing >= 0) {
        close((int)existing);
        (void)unlink("/disk/smoke/sh.txt");
        (void)unlink("/disk/smoke/readme.copy");
        (void)unlink("/disk/smoke/readme.moved");
        printf("smoke: reusing /disk/smoke\n");
        return 1;
    }
    if (!run_and_expect("/disk/bin/mkdir", mkdir_argv, 2, 0)) {
        puts("SMOKE FAIL mkdir /disk/smoke\n");
        return 0;
    }
    printf("smoke: prepared /disk/smoke\n");
    return 1;
}

static void cleanup_smoke_directory(void) {
    (void)unlink("/disk/smoke/sh.txt");
    (void)unlink("/disk/smoke/readme.copy");
    (void)unlink("/disk/smoke/readme.moved");
    (void)rmdir("/disk/smoke");
}

int main(void) {
    const char* forktest_argv[] = {"/disk/bin/forktest", 0};
    const char* polltest_argv[] = {"/disk/bin/polltest", 0};
    const char* sigtest_argv[] = {"/disk/bin/sigtest", 0};
    const char* eventtest_argv[] = {"/disk/bin/eventtest", 0};
    const char* timertest_argv[] = {"/disk/bin/timertest", 0};
    const char* sectiontest_argv[] = {"/disk/bin/sectiontest", 0};
    const char* mmaptest_argv[] = {"/disk/bin/mmaptest", 0};
    const char* sh_argv[] = {"/bin/sh", "-c", "echo busybox-shell > /disk/smoke/sh.txt", 0};
    const char* echo_argv[] = {"/bin/echo", "busybox-echo", 0};
    const char* cat_argv[] = {"/bin/cat", "/disk/smoke/sh.txt", 0};
    const char* ls_argv[] = {"/bin/ls", "/disk/bin", 0};
    const char* cp_argv[] = {"/bin/cp", "/README", "/disk/smoke/readme.copy", 0};
    const char* mv_argv[] = {"/bin/mv", "/disk/smoke/readme.copy", "/disk/smoke/readme.moved", 0};
    const char* rm_argv[] = {"/bin/rm", "/disk/smoke/readme.moved", 0};
    const char* ps_argv[] = {"/bin/ps", 0};
    const char* gputest_argv[] = {"/disk/bin/gputest", "--smoke", 0};
    const char* audiotest_argv[] = {"/disk/bin/audiotest", "--smoke", 0};

    puts("SMOKE START\n");

    if (!file_exists("/disk/bin/forktest") ||
        !file_exists("/disk/bin/polltest") ||
        !file_exists("/disk/bin/sigtest") ||
        !file_exists("/disk/bin/eventtest") ||
        !file_exists("/disk/bin/timertest") ||
        !file_exists("/disk/bin/sectiontest") ||
        !file_exists("/disk/bin/mmaptest") ||
        !file_exists("/disk/bin/busybox") ||
        !file_exists("/disk/bin/sh") ||
        !file_exists("/disk/bin/ls") ||
        !file_exists("/disk/bin/cat") ||
        !file_exists("/disk/bin/echo") ||
        !file_exists("/disk/bin/mkdir") ||
        !file_exists("/disk/bin/rm") ||
        !file_exists("/disk/bin/mv") ||
        !file_exists("/disk/bin/cp") ||
        !file_exists("/disk/bin/ps") ||
        !file_exists("/disk/bin/gputest") ||
        !file_exists("/disk/bin/audiotest")) {
        puts("SMOKE FAIL missing binaries in /disk/bin\n");
        return 1;
    }

    if (!run_and_expect("/disk/bin/forktest", forktest_argv, 1, 0) ||
        !run_and_expect("/disk/bin/polltest", polltest_argv, 1, 0) ||
        !run_and_expect("/disk/bin/sigtest", sigtest_argv, 1, 0) ||
        !run_and_expect("/disk/bin/eventtest", eventtest_argv, 1, 0) ||
        !run_and_expect("/disk/bin/timertest", timertest_argv, 1, 0) ||
        !run_and_expect("/disk/bin/sectiontest", sectiontest_argv, 1, 0) ||
        !run_and_expect("/disk/bin/mmaptest", mmaptest_argv, 1, 0) ||
        !prepare_smoke_directory() ||
        !run_and_expect("/bin/sh", sh_argv, 3, 0) ||
        !validate_file_contains("/disk/smoke/sh.txt", "busybox-shell") ||
        !run_and_expect("/bin/echo", echo_argv, 2, 0) ||
        !run_and_expect("/bin/cat", cat_argv, 2, 0) ||
        !run_and_expect("/bin/ls", ls_argv, 2, 0) ||
        !run_and_expect("/bin/cp", cp_argv, 3, 0) ||
        !validate_file_contains("/disk/smoke/readme.copy", "SavanXP") ||
        !run_and_expect("/bin/mv", mv_argv, 3, 0) ||
        !file_exists("/disk/smoke/readme.moved") ||
        !run_and_expect("/bin/rm", rm_argv, 2, 0) ||
        !file_missing("/disk/smoke/readme.moved") ||
        !run_and_expect("/bin/ps", ps_argv, 1, 0) ||
        !run_and_expect("/disk/bin/gputest", gputest_argv, 2, 0) ||
        !run_and_expect("/disk/bin/audiotest", audiotest_argv, 2, 0)) {
        puts("SMOKE FAIL\n");
        return 1;
    }

    cleanup_smoke_directory();
    sync();
    sleep_ms(250);
    puts("SMOKE PASS\n");
    return 0;
}
