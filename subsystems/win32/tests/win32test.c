/*
 * win32test.sxe -- The win32-hello smoke scenario's runner: a SUN parent that
 * runs each fixture through winhost, captures its standard output through a
 * pipe and compares the bytes and the exit status with what Windows XP
 * produced for the same files. The expected bytes are the files under
 * /system/win/drive_c/tests/expected, the same ones the host harness test
 * reads (docs/userland/WIN32.md, "Fixtures"). startup.exe runs twice on one
 * registry file: the second run reads back what the first wrote.
 *
 * Each case is reported as a line of the system log, which reaches the
 * serial port; exits 0 when every case matches.
 */
#include <stdint.h>
#include <stdlib.h>

#include "abi/savanxp_abi.h"
#include "abi/savanxp_native_abi.h"
#include "sxn/sxn.h"

#define WINHOST "/system/winhost.sxe"
#define TESTS "/system/win/drive_c/tests"
#define EXPECTED TESTS "/expected"
#define REGISTRY TESTS "/registry.reg"

typedef struct fixture {
    const char *name;
    const char *expected; /* a file under EXPECTED, or 0 for no output */
    uint32_t status;
    int registry;
    int with_arguments; /* alpha "two words" 3 */
} fixture;

static const fixture k_fixtures[] = {
    {"hello.exe", "hello.out", 42u, 0, 0},
    {"initorder.exe", "initorder.out", 7u, 0, 0},
    {"missingdll.exe", 0, 0xc0000135u, 0, 0},
    {"missingsym.exe", 0, 0xc0000139u, 0, 0},
    {"startup.exe", "startup-1.out", 5u, 1, 1},
    {"startup.exe", "startup-2.out", 5u, 1, 1},
};

static uint32_t length_of(const char *text) {
    uint32_t n = 0;
    while (text[n] != 0) {
        ++n;
    }
    return n;
}

static void say(const char *a, const char *b, const char *c) {
    char line[160];
    uint32_t n = 0;
    const char *parts[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        for (const char *p = parts[i]; *p != 0 && *p != '\n' && n + 1u < sizeof(line); ++p) {
            line[n++] = *p;
        }
    }
    line[n] = 0;
    (void)sxn_log(line);
}

static void join(char *out, uint32_t capacity, const char *dir, const char *name) {
    uint32_t n = 0;
    for (const char *p = dir; *p != 0 && n + 2u < capacity; ++p) {
        out[n++] = *p;
    }
    out[n++] = '/';
    for (const char *p = name; *p != 0 && n + 1u < capacity; ++p) {
        out[n++] = *p;
    }
    out[n] = 0;
}

/* A whole small file into buffer; its size, or -1. */
static long read_file(const char *path, char *buffer, uint32_t capacity) {
    long handle = sxn_open_file(path, SXN_FILE_READ);
    if (handle < 0) {
        return -1;
    }
    uint32_t total = 0;
    for (;;) {
        long count = sxn_read(handle, buffer + total, capacity - total);
        if (count <= 0) {
            break;
        }
        total += (uint32_t)count;
    }
    (void)sxn_close(handle);
    return (long)total;
}

/* Runs one fixture; 1 when its output and status match. */
static int run(const fixture *f) {
    static char output[8192];
    static char expected[8192];
    char path[128];
    join(path, sizeof(path), TESTS, f->name);

    long expected_size = 0;
    if (f->expected != 0) {
        char expected_path[128];
        join(expected_path, sizeof(expected_path), EXPECTED, f->expected);
        expected_size = read_file(expected_path, expected, sizeof(expected));
        if (expected_size < 0) {
            say("win32test: FAIL ", f->name, ": no expected output file");
            return 0;
        }
    }

    int32_t pipe[2] = {-1, -1};
    if (sxn_pipe_create(pipe) != 0) {
        say("win32test: FAIL ", f->name, ": no pipe");
        return 0;
    }
    char *argv[9];
    int argc = 0;
    argv[argc++] = WINHOST;
    if (f->registry) {
        argv[argc++] = "--registry";
        argv[argc++] = REGISTRY;
    }
    argv[argc++] = path;
    if (f->with_arguments) {
        argv[argc++] = "alpha";
        argv[argc++] = "two words";
        argv[argc++] = "3";
    }
    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, WINHOST, argv, argc);
    request.std_handles[1] = pipe[1];
    long child = sxn_spawn_ex(&request);
    (void)sxn_close(pipe[1]);
    if (child <= 0) {
        (void)sxn_close(pipe[0]);
        say("win32test: FAIL ", f->name, ": cannot start winhost");
        return 0;
    }
    uint32_t total = 0;
    for (;;) {
        long count = sxn_read(pipe[0], output + total, sizeof(output) - total);
        if (count <= 0) {
            break;
        }
        total += (uint32_t)count;
    }
    (void)sxn_close(pipe[0]);
    int status = -1;
    long reaped = sxn_waitpid(child, &status);

    int same = (long)total == expected_size;
    for (uint32_t i = 0; same && i < total; ++i) {
        same = output[i] == expected[i];
    }
    int ok = same && reaped == child && (uint32_t)status == f->status;
    say(ok ? "win32test: ok   " : "win32test: FAIL ", f->expected != 0 ? f->expected : f->name,
        !same ? " (output differs)" : (uint32_t)status != f->status ? " (status differs)" : "");
    return ok;
}

int main(void) {
    /* A fresh registry, so the counter starts where Windows XP's did. */
    static const char empty[] = "SavanXP registry 1\n";
    if (sxn_write_file(REGISTRY, empty, length_of(empty)) != 0) {
        say("win32test: FAIL ", REGISTRY, ": cannot be written");
        sxn_exit(1);
    }
    int failures = 0;
    for (uint32_t i = 0; i < sizeof(k_fixtures) / sizeof(k_fixtures[0]); ++i) {
        failures += !run(&k_fixtures[i]);
    }
    sxn_exit(failures == 0 ? 0 : 1);
}
