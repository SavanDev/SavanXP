/*
 * linux.sxe -- start a Linux program (docs/userland/LINUX.md, "The launcher").
 *
 *   linux [-e NAME=VALUE]... GUEST_PATH [ARGUMENT]...
 *
 * GUEST_PATH is a path in the Linux guest tree: /fixtures/hello names
 * /system/linux/fixtures/hello. The program receives GUEST_PATH as its
 * argv[0], the arguments after it, and exactly the environment the -e options
 * give, in order, nothing inherited. It shares this process's standard
 * handles. The launcher waits for it and exits with its status; when it
 * cannot be started, it says why on standard error and exits with 127.
 *
 * A SUN program: it asks the kernel to run the image as Linux through
 * SXN_SPAWN_FLAG_SUBSYSTEM. Nothing is guessed from the file.
 */
#include <stdint.h>
#include <string.h>

#include "abi/savanxp_abi.h"
#include "abi/savanxp_native_abi.h"
#include "sxn/sxn.h"

#define MAX_ENVIRONMENT 64

static int32_t display_lease[2] = {-1, -1};

static uint32_t length_of(const char *text) {
    uint32_t n = 0;
    while (text[n] != 0) {
        ++n;
    }
    return n;
}

static void put(const char *text) {
    (void)sxn_write(2, text, length_of(text));
}

static const char *reason(long error) {
    switch (-error) {
        case SAVANXP_ENOENT: return "No such file or directory";
        case SAVANXP_EACCES: return "permission denied";
        case SAVANXP_ENOEXEC: return "not a Linux x86-64 executable this subsystem can run";
        case SAVANXP_ENOMEM: return "out of memory";
        case SAVANXP_E2BIG: return "arguments or environment too long";
        case SAVANXP_EINVAL: return "invalid request";
        default: return "cannot start it";
    }
}

/* Start the display service only for a requested local display. A readiness
 * pipe distinguishes a usable listener from merely having spawned a process.
 * Racing launchers are safe: the port name has one creator. */
static long start_display(char** environment, int count) {
    int display = -1;
    for (int i = 0; i < count; ++i) {
        const char* value = environment[i];
        if (value[0] == 'D' && value[1] == 'I' && value[2] == 'S' && value[3] == 'P' &&
            value[4] == 'L' && value[5] == 'A' && value[6] == 'Y' && value[7] == '=') {
            value += 8;
            if (value[0] == ':' && (value[1] == '0' || value[1] == '1') &&
                (!value[2] || (value[2] == '.' && value[3] == '0' && !value[4]))) display = value[1] - '0';
            break;
        }
    }
    if (display < 0) return 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        int32_t ready[2];
        if (sxn_pipe_create(ready)) return -1;
        char* args[] = {"/system/x11d.sxe", display ? ":1" : ":0"};
        struct sxn_spawn_request request; sxn_spawn_request_init(&request, args[0], args, 2);
        request.std_handles[1] = ready[1];
        const long service = sxn_spawn_ex(&request);
        (void)sxn_close(ready[1]);
        unsigned char byte = 0;
        int ok = service > 0 && sxn_wait_one(ready[0], 5000) >= 0 && sxn_read(ready[0], &byte, 1) == 1 && byte == 1;
        (void)sxn_close(ready[0]);
        if (ok && sxn_port_connect(display ? "x11d.1" : "x11d.0", display_lease) == 0) {
            const unsigned char lease = 'L';
            ok = sxn_write(display_lease[0], &lease, 1) == 1 && sxn_wait_one(display_lease[1], 5000) >= 0 &&
                sxn_read(display_lease[1], &byte, 1) == 1 && byte == 1;
            if (ok) return service;
            (void)sxn_close(display_lease[0]); (void)sxn_close(display_lease[1]);
            display_lease[0] = display_lease[1] = -1;
        }
        if (service > 0) {
            struct sxn_process_query query;
            if (sxn_process_query(service, &query) == 0 && query.state == SXN_PROCESS_ENDED) {
                int status; (void)sxn_waitpid(service, &status);
            }
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    char *environment[MAX_ENVIRONMENT + 2];
    int envc = 0;
    int first = 1;

    while (first + 1 < argc && argv[first][0] == '-' && argv[first][1] == 'e' && argv[first][2] == 0) {
        if (envc == MAX_ENVIRONMENT) {
            put("linux: too many -e options\n");
            sxn_exit(127);
        }
        environment[envc++] = argv[first + 1];
        first += 2;
    }
    if (first >= argc || argv[first][0] != '/') {
        put("usage: linux [-e NAME=VALUE]... GUEST_PATH [ARGUMENT]...\n");
        sxn_exit(127);
    }

    // Files and Start explicitly select SSL for files inside its guest tree.
    // This spelling is a physical desktop path; the CLI's /usr/... spelling
    // remains a guest path with only the environment supplied by -e.
    if (!strncmp(argv[first], "/system/linux/", 14)) {
        argv[first] += 13;
        int have_display = 0, have_home = 0;
        for (int i = 0; i < envc; ++i) {
            if (!strncmp(environment[i], "DISPLAY=", 8)) have_display = 1;
            if (!strncmp(environment[i], "HOME=", 5)) have_home = 1;
        }
        if (!have_display) environment[envc++] = "DISPLAY=:0";
        if (!have_home) environment[envc++] = "HOME=/root";
    }

    const long display = start_display(environment, envc);
    if (display < 0) { put("linux: cannot start the local X11 display\n"); return 127; }

    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, argv[first], argv + first, argc - first);
    struct sxn_spawn_subsystem linux_abi = {
        .subsystem = SXN_SUBSYSTEM_LINUX,
        .envc = envc,
        .envp = (uint64_t)(uintptr_t)environment,
    };
    long child = sxn_spawn_ex_subsystem(&request, &linux_abi);
    if (child <= 0) {
        put("linux: ");
        put(argv[first]);
        put(": ");
        put(reason(child));
        put("\n");
        sxn_exit(127);
    }
    int status = 0;
    if (sxn_waitpid(child, &status) != child) {
        put("linux: lost the program\n");
        sxn_exit(127);
    }
    if (display_lease[0] >= 0) (void)sxn_close(display_lease[0]);
    if (display_lease[1] >= 0) (void)sxn_close(display_lease[1]);
    // Reap an already ended readiness peer. A shared live display is never
    // terminated just because this particular client ended.
    struct sxn_process_query query;
    if (display > 0 && sxn_process_query(display, &query) == 0 && query.state == SXN_PROCESS_ENDED) {
        int service_status; (void)sxn_waitpid(display, &service_status);
    }
    return status;
}
