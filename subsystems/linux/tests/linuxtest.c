/*
 * linuxtest.sxe -- the linux-hello smoke scenario's runner (SSL's L1 and L2
 * gates, docs/userland/LINUX.md). A SUN parent that runs each Linux fixture
 * through the launcher, /system/linux.sxe, captures its standard output
 * through a pipe, and compares the bytes and the exit status with what Linux
 * gave for the same file (subsystems/linux/profile/expected, installed under
 * /system/linux/fixtures/expected).
 *
 * The static musl fixtures of L2 are built only where the L0 profile's
 * Alpine root exists (subsystems/linux/Linux.cmake); without them each is
 * reported as skipped, not passed and not failed.
 *
 * Then it asks the kernel to start images it must refuse, directly through
 * SXN_SPAWN_FLAG_SUBSYSTEM: a bad header, a truncated image and another
 * machine (damaged copies of hello the build makes), a missing file, a path out of the guest tree, a bad pointer in
 * the request and a missing handle. Each must fail with its error and leave
 * no process behind.
 *
 * Each case is a line of the system log; "LINUX TEST PASS" or
 * "LINUX TEST FAIL" ends the run.
 */
#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "abi/savanxp_native_abi.h"
#include "sxn/sxn.h"

#define LAUNCHER "/system/linux.sxe"
#define GUEST "/system/linux"
#define EXPECTED GUEST "/fixtures/expected"

static int g_failures;
static int g_skipped;

static uint32_t length_of(const char *text) {
    uint32_t n = 0;
    while (text[n] != 0) {
        ++n;
    }
    return n;
}

static void say(const char *a, const char *b, const char *c) {
    char line[200];
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

/* EXPECTED/<name><suffix>. */
static void expected_path(char *out, uint32_t capacity, const char *name, const char *suffix) {
    join(out, capacity, EXPECTED, name);
    uint32_t n = length_of(out);
    for (uint32_t i = 0; suffix[i] != 0 && n + 1u < capacity; ++i) {
        out[n++] = suffix[i];
    }
    out[n] = 0;
}

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

static int parse_status(const char *text, long size) {
    int value = 0;
    for (long i = 0; i < size && text[i] >= '0' && text[i] <= '9'; ++i) {
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

/* Free memory now, after a short pause: the pages an unmap freed while
 * another core still held the space are released on that core's next flush
 * (docs/kernel/MEMORY_REGIONS.md). */
static uint64_t settled_free_memory(void) {
    long pause = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    if (pause >= 0) {
        (void)sxn_wait_one(pause, 200);
        (void)sxn_close(pause);
    }
    struct savanxp_system_info info;
    if (sxn_system_info(&info) != 0) {
        return 0;
    }
    return info.memory_free_bytes;
}

static int live_processes(void) {
    struct savanxp_system_info info;
    if (sxn_system_info(&info) != 0) {
        return -1;
    }
    return (int)info.process_count;
}

/* One fixture through the launcher; argv[0] is the launcher's, the rest is
 * what `linux` receives. Compares with EXPECTED/<name>.stdout and .status. */
static void run(const char *name, char **arguments, int count) {
    static char output[65536];
    static char expected[65536];
    char status_text[16];
    char stdout_path[160];
    char status_path[160];

    expected_path(stdout_path, sizeof(stdout_path), name, ".stdout");
    expected_path(status_path, sizeof(status_path), name, ".status");
    long expected_size = read_file(stdout_path, expected, sizeof(expected));
    long status_size = read_file(status_path, status_text, sizeof(status_text));
    if (expected_size < 0 || status_size <= 0) {
        say("linuxtest: FAIL ", name, ": no expected output");
        g_failures++;
        return;
    }
    int expected_status = parse_status(status_text, status_size);

    int32_t pipe[2] = {-1, -1};
    if (sxn_pipe_create(pipe) != 0) {
        say("linuxtest: FAIL ", name, ": no pipe");
        g_failures++;
        return;
    }
    char *argv[16];
    int argc = 0;
    argv[argc++] = LAUNCHER;
    for (int i = 0; i < count && argc < 16; ++i) {
        argv[argc++] = arguments[i];
    }
    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, LAUNCHER, argv, argc);
    request.std_handles[1] = pipe[1];
    long child = sxn_spawn_ex(&request);
    (void)sxn_close(pipe[1]);
    if (child <= 0) {
        (void)sxn_close(pipe[0]);
        say("linuxtest: FAIL ", name, ": cannot start the launcher");
        g_failures++;
        return;
    }
    uint32_t total = 0;
    for (;;) {
        long got = sxn_read(pipe[0], output + total, sizeof(output) - total);
        if (got <= 0) {
            break;
        }
        total += (uint32_t)got;
    }
    (void)sxn_close(pipe[0]);
    int status = -1;
    long reaped = sxn_waitpid(child, &status);

    int same = (long)total == expected_size;
    for (uint32_t i = 0; same && i < total; ++i) {
        same = output[i] == expected[i];
    }
    if (!same) {
        uint32_t at = 0;
        uint32_t start = 0;
        while (at < total && (long)at < expected_size && output[at] == expected[at]) {
            if (output[at] == '\n') {
                start = at + 1u;
            }
            ++at;
        }
        char got_line[100];
        char wanted_line[100];
        uint32_t n = 0;
        for (uint32_t i = start; i < total && output[i] != '\n' && n + 1u < sizeof(got_line); ++i) {
            got_line[n++] = output[i];
        }
        got_line[n] = 0;
        n = 0;
        for (uint32_t i = start; (long)i < expected_size && expected[i] != '\n' && n + 1u < sizeof(wanted_line); ++i) {
            wanted_line[n++] = expected[i];
        }
        wanted_line[n] = 0;
        say("linuxtest: got    ", got_line, "");
        say("linuxtest: wanted ", wanted_line, "");
    }
    int ok = same && reaped == child && status == expected_status;
    say(ok ? "linuxtest: ok   " : "linuxtest: FAIL ", name,
        !same ? " (output differs)" : status != expected_status ? " (status differs)" : "");
    if (!ok) {
        g_failures++;
    }
}

/* A fixture that is only there when the build had the musl root. */
static int have_fixture(const char *file) {
    char path[160];
    struct sxn_file_info info;
    join(path, sizeof(path), GUEST "/fixtures", file);
    if (sxn_query_path(path, &info) == 0) {
        return 1;
    }
    say("linuxtest: skip ", file, " (not built: no musl root, see subsystems/linux/Linux.cmake)");
    g_skipped++;
    return 0;
}

/* A Linux spawn the kernel must refuse with `error`, leaving nothing. */
static void refuse(const char *what, const char *path, const struct sxn_spawn_subsystem *linux_abi,
                   int std_handle, long error) {
    char *argv[1] = {(char *)path};
    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, path, argv, 1);
    if (std_handle >= 0) {
        request.std_handles[1] = std_handle;
    }
    int before = live_processes();
    long result = sxn_spawn_ex_subsystem(&request, linux_abi);
    int after = live_processes();
    int ok = result == error && before == after && before > 0;
    if (result > 0) {
        int status = 0;
        (void)sxn_waitpid(result, &status);
    }
    say(ok ? "linuxtest: ok   refuses " : "linuxtest: FAIL refuses ", what, "");
    if (!ok) {
        g_failures++;
    }
}

static void test_refusals(void) {
    struct sxn_spawn_subsystem linux_abi = {.subsystem = SXN_SUBSYSTEM_LINUX, .envc = 0, .envp = 0};

    /* Damaged copies of hello, made by the build (subsystems/linux/profile/
     * damage.py) so they carry the fixture's permissions. */
    refuse("a bad header", "/fixtures/bad-header", &linux_abi, -1, -SAVANXP_ENOEXEC);
    refuse("a truncated image", "/fixtures/truncated", &linux_abi, -1, -SAVANXP_ENOEXEC);
    refuse("another machine", "/fixtures/other-machine", &linux_abi, -1, -SAVANXP_ENOEXEC);
    refuse("a missing file", "/fixtures/no-such-file", &linux_abi, -1, -SAVANXP_ENOENT);
    refuse("a path out of the guest tree", "/../../system/linux.sxe", &linux_abi, -1, -SAVANXP_ENOENT);
    refuse("a relative path", "fixtures/hello", &linux_abi, -1, -SAVANXP_ENOENT);
    refuse("a missing handle", "/fixtures/hello", &linux_abi, 77, -SAVANXP_EBADF);
    refuse("no subsystem block", "/fixtures/hello", (const struct sxn_spawn_subsystem *)0x1000, -1,
           -SAVANXP_EINVAL);
    struct sxn_spawn_subsystem bad_env = {.subsystem = SXN_SUBSYSTEM_LINUX, .envc = 1, .envp = 0x1000};
    refuse("a bad environment pointer", "/fixtures/hello", &bad_env, -1, -SAVANXP_EINVAL);
    struct sxn_spawn_subsystem bad_id = {.subsystem = 99, .envc = 0, .envp = 0};
    refuse("an unknown subsystem", "/fixtures/hello", &bad_id, -1, -SAVANXP_EINVAL);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    char *hello[] = {"/fixtures/hello"};
    run("hello", hello, 1);
    char *startup[] = {"-e", "A=1", "-e", "B=two", "/fixtures/startup", "x", "y z"};
    run("startup", startup, 7);
    char *errors[] = {"/fixtures/errors"};
    run("errors", errors, 1);

    /* L2: anonymous memory at its edges, libc-free, then static musl
     * programs. */
    char *regions[] = {"/fixtures/regions"};
    run("regions", regions, 1);
    if (have_fixture("args")) {
        char *args[] = {"-e", "A=1", "-e", "B=two", "/fixtures/args", "x", "y z"};
        run("args", args, 7);
    }
    if (have_fixture("tls")) {
        char *tls[] = {"/fixtures/tls"};
        run("tls", tls, 1);
    }
    if (have_fixture("heap")) {
        /* heap maps and unmaps some 25 MiB of blocks. Each unmapped page
         * must go back: one lost in munmap or mremap would not come back
         * even when the program exits. */
        const uint64_t before = settled_free_memory();
        char *heap[] = {"/fixtures/heap"};
        run("heap", heap, 1);
        const uint64_t after = settled_free_memory();
        const int returned = before != 0 && after + (1u << 20) >= before;
        say(returned ? "linuxtest: ok   " : "linuxtest: FAIL ", "heap gave its memory back", "");
        if (!returned) {
            g_failures++;
        }
    }
    test_refusals();

    if (g_skipped != 0) {
        say("linuxtest: some fixtures were skipped", "", "");
    }
    say(g_failures == 0 ? "LINUX TEST PASS" : "LINUX TEST FAIL", "", "");
    return g_failures == 0 ? 0 : 1;
}
