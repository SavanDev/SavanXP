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
 * With --tools it runs the L3 BusyBox profile and the directory, path,
 * permission and transfer edge cases.
 *
 * With --restart it is the linux-restart scenario instead: the first boot
 * runs `files write`, makes the volume durable and logs
 * "LINUX RESTART READY", the host resets the machine, and the second boot,
 * finding the file, runs `files check` and `files remove`.
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

static int same(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
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
    if ((name[0] == 'b' && name[1] == 'b') || same(name, "jq-missing-lib") || same(name, "jq-missing-interp")) {
        expected_path(stdout_path, sizeof(stdout_path), name, ".stderr");
        long errors = read_file(stdout_path, expected + expected_size, sizeof(expected) - expected_size);
        if (errors < 0) { g_failures++; return; }
        expected_size += errors;
    }

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
    if ((name[0] == 'b' && name[1] == 'b') || same(name, "jq-missing-lib") || same(name, "jq-missing-interp")) request.std_handles[2] = pipe[1];
    struct sxn_identity user = {.uid = 1000, .euid = 1000, .gid = 1000};
    long child = same(name, "tools-denied") ? sxn_spawn_ex_as(&request, &user) : sxn_spawn_ex(&request);
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

    uint32_t output_start = 0;
    uint32_t expected_start = 0;
    if (same(name, "jq-missing-interp")) {
        /* chroot on Linux and the SUN launcher have different prefixes;
         * compare the complete error description and the 127 exit status. */
        for (uint32_t i = 0; i + 1 < total; ++i)
            if (output[i] == ':' && output[i + 1] == ' ') output_start = i + 2;
        for (uint32_t i = 0; (long)(i + 1) < expected_size; ++i)
            if (expected[i] == ':' && expected[i + 1] == ' ') expected_start = i + 2;
    }
    int same = (long)(total - output_start) == expected_size - expected_start;
    for (uint32_t i = 0; same && i < total - output_start; ++i) {
        same = output[output_start + i] == expected[expected_start + i];
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

/* The linux-restart scenario's two boots (see the top of the file). */
static int restart(void) {
    struct sxn_file_info info;
    if (!have_fixture("files")) {
        say("LINUX RESTART FAIL", "", "");
        return 1;
    }
    if (sxn_query_path(GUEST "/work/l2-files.dat", &info) != 0) {
        char *write[] = {"/fixtures/files", "write", "/work"};
        run("files-write", write, 3);
        /* Durable before the reset: flushing any file of the volume syncs
         * the whole of it, the fixture's file with it. */
        long marker = sxn_create_file(GUEST "/work/restart-marker", SXN_CREATE_ALWAYS);
        int durable = marker >= 0 && sxn_flush_file(marker) == 0;
        if (marker >= 0) {
            (void)sxn_close(marker);
        }
        if (g_failures != 0 || !durable) {
            say("LINUX RESTART FAIL", durable ? "" : ": the volume did not sync", "");
            return 1;
        }
        say("LINUX RESTART READY", "", "");
        long never = sxn_event_create(SXN_EVENT_MANUAL_RESET);
        for (;;) {
            (void)sxn_wait_one(never, -1);
        }
    }
    char *check[] = {"/fixtures/files", "check", "/work"};
    run("files-check", check, 3);
    char *remove[] = {"/fixtures/files", "remove", "/work"};
    run("files-remove", remove, 3);
    say(g_failures == 0 ? "LINUX RESTART PASS" : "LINUX RESTART FAIL", "", "");
    return g_failures == 0 ? 0 : 1;
}

static int tools(void) {
    struct sxn_file_info info;
    if (sxn_query_path(GUEST "/bin/busybox.static", &info) != 0) {
        say("LINUX TOOLS FAIL: pinned BusyBox missing", "", ""); return 1;
    }
    char *prepare[] = {"/bin/busybox.static", "mkdir", "-p", "/work/tree/a/b"};
    run("bb-mkdir", prepare, 4);
    const char *paths[] = {GUEST "/work/tree/a/f.txt", GUEST "/work/tree/z"};
    const char *data[] = {"one\ntwo three\n", "x\n"};
    for (int i = 0; i < 2; ++i) {
        long h = sxn_create_file(paths[i], SXN_CREATE_ALWAYS);
        if (h < 0 || sxn_write(h, data[i], length_of(data[i])) != (long)length_of(data[i])) g_failures++;
        if (h >= 0) (void)sxn_close(h);
    }
    char *echo[] = {"/bin/busybox.static", "echo", "hello", "world"}; run("bb-echo", echo, 4);
    char *cat[] = {"/bin/busybox.static", "cat", "/work/tree/a/f.txt"}; run("bb-cat", cat, 3);
    char *ls[] = {"/bin/busybox.static", "ls", "-1", "/work/tree", "/work/tree/a"}; run("bb-ls", ls, 5);
    char *pwd[] = {"/bin/busybox.static", "pwd"}; run("bb-pwd-root", pwd, 2);
    char *wc[] = {"/bin/busybox.static", "wc", "/work/tree/a/f.txt"}; run("bb-wc", wc, 3);
    char *mkdir[] = {"/bin/busybox.static", "mkdir", "-p", "/work/tree/new/deeper"}; run("bb-mkdir", mkdir, 4);
    char *missing[] = {"/bin/busybox.static", "cat", "/work/tree/missing"}; run("bb-cat-missing", missing, 3);
    char *edges[] = {"/fixtures/tools"}; run("tools", edges, 1);
    char *denied[] = {"/fixtures/tools", "denied"}; run("tools-denied", denied, 2);
    say(g_failures == 0 ? "LINUX TOOLS PASS" : "LINUX TOOLS FAIL", "", "");
    return g_failures != 0;
}

/* Platform policy assertions, separate from the byte-for-byte Linux cases.
 * SSL has no signal delivery yet: a protection #PF exits with 128 + 14. */
static void dynamic_policy(const char *mode, int wanted) {
    char *argv[] = {LAUNCHER, "/fixtures/dynamic", (char *)mode};
    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, LAUNCHER, argv, 3);
    long child = sxn_spawn_ex(&request);
    int status = -1;
    int ok = child > 0 && sxn_waitpid(child, &status) == child && status == wanted;
    say(ok ? "linuxtest: ok   dynamic policy " : "linuxtest: FAIL dynamic policy ", mode, "");
    if (!ok) g_failures++;
}

static int dynamic(const char *mode) {
    struct sxn_spawn_subsystem linux_abi = {.subsystem = SXN_SUBSYSTEM_LINUX, .envc = 0, .envp = 0};
    if (same(mode, "nointerp")) {
        /* Linux execve refuses the original jq with ENOENT when PT_INTERP
         * cannot be opened. There is no guest chroot wrapper in SSL. */
        refuse("a missing interpreter", "/usr/bin/jq", &linux_abi, -1, -SAVANXP_ENOENT);
        char *jq[] = {"/usr/bin/jq", "--version"};
        run("jq-missing-interp", jq, 2);
    } else if (same(mode, "nolib")) {
        char *jq[] = {"/usr/bin/jq", "--version"};
        run("jq-missing-lib", jq, 2);
    } else {
        refuse("duplicate PT_INTERP", "/fixtures/interp-duplicate", &linux_abi, -1, -SAVANXP_ENOEXEC);
        refuse("nested PT_INTERP", "/fixtures/interp-nested", &linux_abi, -1, -SAVANXP_ENOEXEC);
        refuse("unterminated PT_INTERP", "/fixtures/interp-unterminated", &linux_abi, -1, -SAVANXP_ENOEXEC);
        const uint64_t before = settled_free_memory();
        char *edges[] = {"/fixtures/dynamic"};
        run("dynamic", edges, 1);
        dynamic_policy("wx", 0);
        dynamic_policy("write", 128 + 14);
        dynamic_policy("execute", 128 + 14);
        const uint64_t after = settled_free_memory();
        if (before == 0 || after + (1u << 20) < before) {
            say("linuxtest: FAIL dynamic memory was not returned", "", "");
            g_failures++;
        }
        long file = sxn_create_file(GUEST "/work/in.json", SXN_CREATE_ALWAYS);
        const char input[] = "{\"a\":[1,2,3],\"s\":\"abbbc\"}\n";
        if (file < 0 || sxn_write(file, input, sizeof(input) - 1) != sizeof(input) - 1) g_failures++;
        if (file >= 0) (void)sxn_close(file);
        char *jq[] = {"-e", "HOME=/root", "/usr/bin/jq", "-c",
            "{d: (.a | map(. * 2)), m: (.s | test(\"ab+c\"))}", "/work/in.json"};
        run("jq", jq, 6);
    }
    say(g_failures == 0 ? "LINUX DYNAMIC PASS" : "LINUX DYNAMIC FAIL", "", "");
    return g_failures != 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && same(argv[1], "--dynamic")) return dynamic(argc > 2 ? argv[2] : "normal");
    if (argc > 1 && same(argv[1], "--tools")) return tools();
    if (argc > 1 && same(argv[1], "--restart")) {
        return restart();
    }

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
    if (have_fixture("clock")) {
        char *clock[] = {"/fixtures/clock"};
        run("clock", clock, 1);
    }
    if (have_fixture("files")) {
        char *write[] = {"/fixtures/files", "write", "/work"};
        run("files-write", write, 3);
        char *check[] = {"/fixtures/files", "check", "/work"};
        run("files-check", check, 3);
        char *remove[] = {"/fixtures/files", "remove", "/work"};
        run("files-remove", remove, 3);
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
