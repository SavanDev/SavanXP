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

#include "abi/savanxp_abi.h"
#include "abi/savanxp_native_abi.h"
#include "sxn/sxn.h"

#define MAX_ENVIRONMENT 64

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
        case SAVANXP_ENOENT: return "no such file in the guest tree";
        case SAVANXP_EACCES: return "permission denied";
        case SAVANXP_ENOEXEC: return "not a Linux x86-64 executable this subsystem can run";
        case SAVANXP_ENOMEM: return "out of memory";
        case SAVANXP_E2BIG: return "arguments or environment too long";
        case SAVANXP_EINVAL: return "invalid request";
        default: return "cannot start it";
    }
}

int main(int argc, char **argv) {
    char *environment[MAX_ENVIRONMENT];
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
    return status;
}
