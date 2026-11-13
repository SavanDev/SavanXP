/*
 * win32test.sxe -- The win32-hello smoke scenario's runner: a SUN parent that
 * runs each W1 fixture through winhost, captures its standard output through
 * a pipe and compares the bytes and the exit status with what Windows XP
 * produced for the same files (docs/userland/WIN32.md, "Fixtures"; the host
 * harness checks the same expectations in tests/host/win32_harness_test.py).
 * Each case is reported as a line of the system log, which reaches the
 * serial port; exits 0 when every case matches.
 */
#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "abi/savanxp_native_abi.h"
#include "sxn/sxn.h"

#define WINHOST "/system/winhost.sxe"
#define FIXTURES "/system/win/drive_c/tests"

typedef struct fixture {
    const char *name;
    const char *output;
    uint32_t status;
} fixture;

static const fixture k_fixtures[] = {
    {"hello.exe", "Hello from a PE32 executable.\r\n", 42u},
    {"initorder.exe",
     "beta: attach, static load, pointers agree\r\n"
     "alpha: attach, beta says 7, pointers agree\r\n"
     "initorder: TLS callback 1, reason 1\r\n"
     "initorder: TLS callback 2, reason 1\r\n"
     "initorder: entry, thread value 7, alpha says 3, beta says 7\r\n"
     "alpha: detach, process ending\r\n"
     "beta: detach, process ending\r\n"
     "initorder: TLS callback 1, reason 0\r\n"
     "initorder: TLS callback 2, reason 0\r\n",
     7u},
    {"missingdll.exe", "", 0xc0000135u},
    {"missingsym.exe", "", 0xc0000139u},
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

/* Runs one fixture; 1 when its output and status match. */
static int run(const fixture *f) {
    static char output[4096];
    char path[128];
    uint32_t n = 0;
    for (const char *p = FIXTURES "/"; *p != 0; ++p) {
        path[n++] = *p;
    }
    for (const char *p = f->name; *p != 0 && n + 1u < sizeof(path); ++p) {
        path[n++] = *p;
    }
    path[n] = 0;

    int32_t pipe[2] = {-1, -1};
    if (sxn_pipe_create(pipe) != 0) {
        say("win32test: ", f->name, ": no pipe\n");
        return 0;
    }
    char *argv[2] = {WINHOST, path};
    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, WINHOST, argv, 2);
    request.std_handles[1] = pipe[1];
    long child = sxn_spawn_ex(&request);
    (void)sxn_close(pipe[1]);
    if (child <= 0) {
        (void)sxn_close(pipe[0]);
        say("win32test: ", f->name, ": cannot start winhost\n");
        return 0;
    }
    uint32_t total = 0;
    for (;;) {
        long count = sxn_read(pipe[0], output + total, sizeof(output) - 1u - total);
        if (count <= 0) {
            break;
        }
        total += (uint32_t)count;
    }
    (void)sxn_close(pipe[0]);
    int status = -1;
    long reaped = sxn_waitpid(child, &status);

    uint32_t expected = length_of(f->output);
    int same = total == expected;
    for (uint32_t i = 0; same && i < expected; ++i) {
        same = output[i] == f->output[i];
    }
    int ok = same && reaped == child && (uint32_t)status == f->status;
    say(ok ? "win32test: ok   " : "win32test: FAIL ", f->name,
        !same ? " (output differs)\n" : (uint32_t)status != f->status ? " (status differs)\n" : "\n");
    return ok;
}

int main(void) {
    int failures = 0;
    for (uint32_t i = 0; i < sizeof(k_fixtures) / sizeof(k_fixtures[0]); ++i) {
        failures += !run(&k_fixtures[i]);
    }
    sxn_exit(failures == 0 ? 0 : 1);
}
