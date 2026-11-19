#include "libc.h"
#include <stdint.h>

/* The kernel hands each new image a stack-protector guard in EDX and crt0
 * stores it here. Fork keeps the parent's guard; spawn and exec install fresh
 * ones. */
extern uintptr_t __stack_chk_guard;

static const char path[] = "/bin/guardtest32";

static int valid(uintptr_t guard) {
    return guard != 0 && (guard & 0xffu) != 0;
}

static int check_new_images(uintptr_t parent) {
    const char* args[] = {path, "sample", 0};
    int differs_from_parent = 0, exec_refreshed = 0, varied = 0;
    uintptr_t first = 0;
    for (int run = 0; run < 8; ++run) {
        int fds[2];
        if (savanxp_pipe(fds) < 0) return 0;
        long pid = spawn_fd(path, args, 2, 0, fds[1]);
        savanxp_close(fds[1]);
        if (pid < 0) return 0;
        uintptr_t spawned, executed;
        if (savanxp_read(fds[0], &spawned, sizeof(spawned)) != sizeof(spawned) ||
            savanxp_read(fds[0], &executed, sizeof(executed)) != sizeof(executed) ||
            !valid(spawned) || !valid(executed)) return 0;
        savanxp_close(fds[0]);
        int status;
        if (savanxp_waitpid((int)pid, &status) != pid || status != 0) return 0;
        if (spawned != parent) differs_from_parent = 1;
        if (executed != spawned) exec_refreshed = 1;
        if (run == 0) first = spawned;
        else if (spawned != first) varied = 1;
    }
    return differs_from_parent && exec_refreshed && varied;
}

int main(int argc, char** argv) {
    const uintptr_t guard = __stack_chk_guard;
    if (!valid(guard)) return 1;
    if (argc == 2) {
        if (savanxp_write(1, &guard, sizeof(guard)) != sizeof(guard)) return 1;
        if (strcmp(argv[1], "exec") == 0) return 0;
        const char* args[] = {path, "exec", 0};
        exec(path, args, 2);
        return 1;
    }

    int status;
    long child = savanxp_fork();
    if (child == 0) return __stack_chk_guard == guard ? 0 : 1;
    if (child < 0 || savanxp_waitpid((int)child, &status) != child || status != 0) return 1;
    if (!check_new_images(guard)) return 1;
    puts_out("GUARD32 PASS\n");
    return 0;
}
