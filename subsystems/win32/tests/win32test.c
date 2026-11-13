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
 *
 * With --window it runs the win32-window scenario instead: it starts
 * compositord and windowd with no desktop, then runs
 * window.exe and logs "WIN32 WINDOW READY". The host plays the fixture's
 * input script over QMP and checks the pixels (tools/win32_window_smoke.py);
 * here the output and the status are compared with Windows XP's.
 *
 * With --dialog it runs dialog.exe the same way for win32-dialog, logging
 * "WIN32 DIALOG READY" (tools/win32_dialog_smoke.py).
 *
 * With --cards it runs cards.exe, which draws every card with the user's own
 * cards.dll; the win32-cards scenario copies that file next to it.
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
    const char *ready;  /* logged once it runs, or 0 */
} fixture;

#define WINDOW_READY "WIN32 WINDOW READY"
#define DIALOG_READY "WIN32 DIALOG READY"

static const fixture k_fixtures[] = {
    {"hello.exe", "hello.out", 42u, 0, 0, 0},
    {"initorder.exe", "initorder.out", 7u, 0, 0, 0},
    {"missingdll.exe", 0, 0xc0000135u, 0, 0, 0},
    {"missingsym.exe", 0, 0xc0000139u, 0, 0, 0},
    {"startup.exe", "startup-1.out", 5u, 1, 1, 0},
    {"startup.exe", "startup-2.out", 5u, 1, 1, 0},
    {"resource.exe", "resource.out", 0u, 0, 0, 0},
};

static const fixture k_window = {"window.exe", "window.out", 3u, 0, 0, WINDOW_READY};
static const fixture k_dialog = {"dialog.exe", "dialog.out", 5u, 0, 0, DIALOG_READY};
/* Needs the user's cards.dll next to it, which only the win32-cards
 * scenario puts there. */
static const fixture k_cards = {"cards.exe", "cards.out", 0u, 0, 0, 0};

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
    static char output[32768];
    static char expected[32768];
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
    if (f->ready != 0) {
        (void)sxn_log(f->ready);
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
    if (!same) {
        /* The first line that differs, both ways, for whoever reads the log. */
        uint32_t at = 0;
        uint32_t start = 0;
        while (at < total && (long)at < expected_size && output[at] == expected[at]) {
            if (output[at] == '\n') {
                start = at + 1u;
            }
            ++at;
        }
        char got[100];
        char wanted[100];
        uint32_t n = 0;
        for (uint32_t i = start; i < total && output[i] != '\n' && n + 1u < sizeof(got); ++i) {
            got[n++] = output[i];
        }
        got[n] = 0;
        n = 0;
        for (uint32_t i = start; (long)i < expected_size && expected[i] != '\n' && n + 1u < sizeof(wanted); ++i) {
            wanted[n++] = expected[i];
        }
        wanted[n] = 0;
        say("win32test: got    ", got, "");
        say("win32test: wanted ", wanted, "");
    }
    int ok = same && reaped == child && (uint32_t)status == f->status;
    say(ok ? "win32test: ok   " : "win32test: FAIL ", f->expected != 0 ? f->expected : f->name,
        !same ? " (output differs)" : (uint32_t)status != f->status ? " (status differs)" : "");
    return ok;
}

static int start(char *path) {
    char *argv[1] = {path};
    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, path, argv, 1);
    return sxn_spawn_ex(&request) > 0;
}

/* compositord and windowd as sessiond starts them, with no desktop. windowd
 * gives up when compositord's port is not there yet (under TCG it can take a
 * few seconds), so it is started again until its own port takes a
 * connection, as sessiond restarts it. */
static int start_windowd(void) {
    if (!start("/system/compositord.sxe")) {
        return 0;
    }
    long idle = sxn_event_create(0);
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (attempt % 30 == 0 && !start("/system/windowd.sxe")) {
            break;
        }
        int32_t handles[2] = {-1, -1};
        if (sxn_port_connect("windowd", handles) == 0) {
            (void)sxn_close(handles[0]);
            (void)sxn_close(handles[1]);
            (void)sxn_close(idle);
            return 1;
        }
        (void)sxn_wait_one(idle, 100);
    }
    (void)sxn_close(idle);
    return 0;
}

static int run_window(const fixture *f) {
    if (!start_windowd()) {
        say("win32test: FAIL ", "windowd", ": did not start");
        return 0;
    }
    return run(f);
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 'w') {
        sxn_exit(run_window(&k_window) ? 0 : 1);
    }
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 'd') {
        sxn_exit(run_window(&k_dialog) ? 0 : 1);
    }
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 'c') {
        sxn_exit(run(&k_cards) ? 0 : 1);
    }
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
