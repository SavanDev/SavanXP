/* tccsmoke: the TinyCC port's guest check (scenario tinycc-smoke).
 *
 * Writes a few C sources to /tmp, builds them with /bin/tcc, runs what it
 * built and checks the output and the exit status. Covers one-step builds,
 * -c plus a separate link, the SSE float path through printf, -run, a SUN
 * program, and a source that must be refused with a diagnostic. Ends with
 * TINYCC SMOKE PASS or TINYCC SMOKE FAIL on the log. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "abi/savanxp_native_abi.h"
#include "sxn/sxn.h"

static int g_failures;

static void fail(const char* what, const char* detail) {
    char line[256];

    snprintf(line, sizeof(line), "tccsmoke: FAIL %s: %s", what, detail);
    (void)sxn_log(line);
    ++g_failures;
}

static int write_source(const char* path, const char* text) {
    char detail[160];
    long result = sxn_write_file(path, text, strlen(text));

    if (result != 0) {
        snprintf(detail, sizeof(detail), "%s (%ld)", path, result);
        fail("write", detail);
        return 0;
    }
    return 1;
}

/* Runs `path` with argv, collecting stdout and stderr together in `out`.
 * Returns the exit status, or -1 when the program could not start. */
static int run(const char* path, char** argv, int argc, char* out, unsigned long capacity) {
    struct sxn_spawn_request request;
    int32_t output[2] = {-1, -1};
    unsigned long total = 0;
    int status = -1;
    long child;

    out[0] = '\0';
    if (sxn_pipe_create(output) != 0) {
        fail("pipe", path);
        return -1;
    }
    sxn_spawn_request_init(&request, path, argv, argc);
    request.std_handles[1] = output[1];
    request.std_handles[2] = output[1];
    child = sxn_spawn_ex(&request);
    (void)sxn_close(output[1]);
    if (child <= 0) {
        (void)sxn_close(output[0]);
        fail("spawn", path);
        return -1;
    }
    for (;;) {
        long count = sxn_read(output[0], out + total, capacity - 1u - total);
        if (count <= 0) {
            break;
        }
        total += (unsigned long)count;
        if (total + 1u >= capacity) {
            break;
        }
    }
    out[total] = '\0';
    (void)sxn_close(output[0]);
    if (sxn_waitpid(child, &status) != child) {
        fail("waitpid", path);
        return -1;
    }
    return status;
}

/* Puts a program's output on the log, a line at a time. */
static void log_output(const char* text) {
    char line[200];

    while (*text != '\0') {
        unsigned long length = strcspn(text, "\n");
        snprintf(line, sizeof(line), "tccsmoke: | %.*s", (int)(length < 180 ? length : 180), text);
        (void)sxn_log(line);
        text += length;
        if (*text == '\n') {
            ++text;
        }
    }
}

/* Runs tcc with the given arguments and expects it to succeed quietly. */
static int compile(char** argv, int argc, const char* what) {
    char out[1024];
    int status = run("/bin/tcc", argv, argc, out, sizeof(out));

    if (status != 0) {
        char detail[192];
        snprintf(detail, sizeof(detail), "tcc exited %d", status);
        fail(what, detail);
        log_output(out);
        return 0;
    }
    return 1;
}

static void expect_spawn(char** argv, int argc, const char* expected, int expected_status, const char* what) {
    char out[512];
    char detail[192];
    int status;

    status = run(argv[0], argv, argc, out, sizeof(out));
    if (status != expected_status || strcmp(out, expected) != 0) {
        snprintf(detail, sizeof(detail), "exit %d", status);
        fail(what, detail);
        log_output(out);
        return;
    }
    snprintf(detail, sizeof(detail), "tccsmoke: %s ok", what);
    (void)sxn_log(detail);
}

static void expect_run(const char* path, const char* expected, int expected_status, const char* what) {
    char* argv[1];

    argv[0] = (char*)path;
    expect_spawn(argv, 1, expected, expected_status, what);
}

static const char kHello[] =
    "#include <stdio.h>\n"
    "struct pair { long a; double b; };\n"
    "static double scale(struct pair p) { return p.a * p.b; }\n"
    "int main(void) {\n"
    "    struct pair p = { 6, 2.25 };\n"
    "    long long big = 1LL << 40;\n"
    "    printf(\"tcc hello %d %.3f %lld\\n\", 6 * 7, scale(p), big);\n"
    "    return 7;\n"
    "}\n";

static const char kUtil[] =
    "#include <stdlib.h>\n"
    "#include \"util.h\"\n"
    "static int compare(const void* a, const void* b) {\n"
    "    return *(const int*)a - *(const int*)b;\n"
    "}\n"
    "int* sorted_copy(const int* values, int count) {\n"
    "    int* copy = malloc(sizeof(int) * (unsigned long)count);\n"
    "    for (int i = 0; i < count; ++i) copy[i] = values[i];\n"
    "    qsort(copy, (unsigned long)count, sizeof(int), compare);\n"
    "    return copy;\n"
    "}\n";

static const char kUtilHeader[] = "int* sorted_copy(const int* values, int count);\n";

static const char kMain[] =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "#include \"util.h\"\n"
    "int main(int argc, char** argv) {\n"
    "    static const int values[] = { 8, 3, 5, 1, 2 };\n"
    "    int* sorted = sorted_copy(values, 5);\n"
    "    printf(\"sorted:\");\n"
    "    for (int i = 0; i < 5; ++i) printf(\" %d\", sorted[i]);\n"
    "    printf(\" argc=%d name=%s\\n\", argc, strrchr(argv[0], '/') + 1);\n"
    "    free(sorted);\n"
    "    return 0;\n"
    "}\n";

/* For -run: arguments, errno through the libc tcc itself runs on, and a
 * callback from that libc back into the generated code. */
static const char kRun[] =
    "#include <errno.h>\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "static int compare(const void* a, const void* b) {\n"
    "    return strcmp(*(char* const*)a, *(char* const*)b);\n"
    "}\n"
    "int main(int argc, char** argv) {\n"
    "    FILE* missing = fopen(\"/tmp/tccsmoke-not-there\", \"r\");\n"
    "    qsort(argv + 1, (unsigned long)(argc - 1), sizeof(char*), compare);\n"
    "    printf(\"run %d %s %s %s\\n\", argc, argv[1], argv[2],\n"
    "           missing == 0 && errno == ENOENT ? \"enoent\" : \"no-errno\");\n"
    "    return 5;\n"
    "}\n";

/* A SUN program: -o <name>.sxe. sxn_info proves the kernel runs it as
 * native, the 4 MiB block comes from SUN's heap page source (a native
 * process has no POSIX mmap), and the line goes out through its handle 1. */
static const char kSun[] =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "#include \"sxn/sxn.h\"\n"
    "int main(int argc, char** argv) {\n"
    "    struct sxn_native_info info;\n"
    "    char line[96];\n"
    "    char* block = malloc(4u << 20);\n"
    "    int length;\n"
    "    if (sxn_info(&info) != 0 || block == 0) return 1;\n"
    "    strcpy(block + (4u << 20) - 16u, argc > 1 ? argv[1] : \"none\");\n"
    "    length = snprintf(line, sizeof(line), \"sun %u %s\\n\", info.subsystem_id,\n"
    "                      block + (4u << 20) - 16u);\n"
    "    free(block);\n"
    "    return sxn_write(1, line, (unsigned long)length) == length ? 6 : 2;\n"
    "}\n";

static const char kBroken[] =
    "int main(void) {\n"
    "    return undeclared_name;\n"
    "}\n";

int main(void) {
    char out[1024];

    (void)sxn_log("tccsmoke: start");

    /* tcc runs at all and is the build the port pinned. */
    {
        char* argv[] = {"/bin/tcc", "-v"};
        int status = run("/bin/tcc", argv, 2, out, sizeof(out));
        if (status != 0 || strstr(out, "tcc version 0.9.28rc") == 0) {
            char detail[192];
            snprintf(detail, sizeof(detail), "exit %d, output '%.120s'", status, out);
            fail("tcc -v", detail);
        }
    }

    /* One step: source to executable. */
    if (write_source("/tmp/tccsmoke-hello.c", kHello)) {
        char* argv[] = {"/bin/tcc", "/tmp/tccsmoke-hello.c", "-o", "/tmp/tccsmoke-hello"};
        if (compile(argv, 4, "build hello")) {
            expect_run("/tmp/tccsmoke-hello", "tcc hello 42 13.500 1099511627776\n", 7, "hello");
        }
    }

    /* Two steps: an object with -c, then a link that names it, with the
     * header found next to the source. */
    if (write_source("/tmp/util.h", kUtilHeader) && write_source("/tmp/tccsmoke-util.c", kUtil) &&
        write_source("/tmp/tccsmoke-main.c", kMain)) {
        char* object_argv[] = {"/bin/tcc", "-c", "/tmp/tccsmoke-util.c", "-o", "/tmp/tccsmoke-util.o"};
        char* link_argv[] = {"/bin/tcc", "-O1", "/tmp/tccsmoke-main.c", "/tmp/tccsmoke-util.o", "-o",
                             "/tmp/tccsmoke-sort"};
        if (compile(object_argv, 5, "build util.o") && compile(link_argv, 6, "link sort")) {
            expect_run("/tmp/tccsmoke-sort", "sorted: 1 2 3 5 8 argc=1 name=tccsmoke-sort\n", 0, "sort");
        }
    }

    /* -run: compiled into memory and called in this tcc's process. */
    if (write_source("/tmp/tccsmoke-run.c", kRun)) {
        char* argv[] = {"/bin/tcc", "-run", "/tmp/tccsmoke-run.c", "zeta", "alpha"};
        expect_spawn(argv, 5, "run 3 alpha zeta enoent\n", 5, "-run");
    }

    /* A SUN program, built the same way under a .sxe name. */
    if (write_source("/tmp/tccsmoke-sun.c", kSun)) {
        char* argv[] = {"/bin/tcc", "/tmp/tccsmoke-sun.c", "-o", "/tmp/tccsmoke-sun.sxe"};
        if (compile(argv, 4, "build sun")) {
            char* run_argv[] = {"/tmp/tccsmoke-sun.sxe", "tcc"};
            expect_spawn(run_argv, 2, "sun 1 tcc\n", 6, "sun");
        }
    }

    /* A wrong source is refused with a diagnostic naming the line. */
    if (write_source("/tmp/tccsmoke-broken.c", kBroken)) {
        char* argv[] = {"/bin/tcc", "/tmp/tccsmoke-broken.c", "-o", "/tmp/tccsmoke-broken"};
        int status = run("/bin/tcc", argv, 4, out, sizeof(out));
        if (status == 0 || strstr(out, "tccsmoke-broken.c:2: error:") == 0) {
            char detail[192];
            snprintf(detail, sizeof(detail), "exit %d, output '%.120s'", status, out);
            fail("refuse broken", detail);
        } else {
            (void)sxn_log("tccsmoke: refuse broken ok");
        }
    }

    (void)sxn_log(g_failures ? "TINYCC SMOKE FAIL" : "TINYCC SMOKE PASS");
    return g_failures ? 1 : 0;
}
