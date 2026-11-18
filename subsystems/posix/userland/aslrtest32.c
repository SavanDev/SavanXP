#include "libc.h"
#include <stdint.h>

enum { PAGE = 4096, VIEW_BASE = 0x10000000, VIEW_END = 0x40000000 };
static const char path[] = "/bin/aslrtest32";

static uintptr_t map_first(void) {
    long fd = section_create(PAGE, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (fd < 0) return 0;
    void* view = map_view((int)fd, SAVANXP_SECTION_WRITE);
    savanxp_close((int)fd);
    return result_is_error((long)view) ? 0 : (uintptr_t)view;
}

static int check_new_images(void) {
    const char* args[] = {path, "sample", 0};
    uintptr_t first[2] = {0, 0};
    int varied[2] = {0, 0};
    for (int run = 0; run < 8; ++run) {
        int fds[2];
        if (savanxp_pipe(fds) < 0) return 0;
        long pid = spawn_fd(path, args, 2, 0, fds[1]);
        savanxp_close(fds[1]);
        if (pid < 0) return 0;
        for (int image = 0; image < 2; ++image) {
            uintptr_t base;
            if (savanxp_read(fds[0], &base, sizeof(base)) != sizeof(base) ||
                base < VIEW_BASE || base >= VIEW_BASE + (256u << 20) ||
                (base & (PAGE - 1))) return 0;
            if (run == 0) first[image] = base;
            else if (base != first[image]) varied[image] = 1;
        }
        savanxp_close(fds[0]);
        int status;
        if (savanxp_waitpid((int)pid, &status) != pid || status != 0) return 0;
    }
    return varied[0] && varied[1];
}

int main(int argc, char** argv) {
    uintptr_t base = map_first();
    if (!base) return 1;
    if (argc == 2) {
        if (savanxp_write(1, &base, sizeof(base)) != sizeof(base)) return 1;
        if (strcmp(argv[1], "exec") == 0) return 0;
        const char* args[] = {path, "exec", 0};
        exec(path, args, 2);
        return 1;
    }

    int status;
    long child = savanxp_fork();
    if (child == 0) return map_first() == base + PAGE ? 0 : 1;
    if (child < 0 || savanxp_waitpid((int)child, &status) != child || status != 0) return 1;
    const char* missing[] = {"/bin/aslrtest32-missing", 0};
    if (exec(missing[0], missing, 1) != -SAVANXP_ENOENT || map_first() != base + PAGE) return 1;
    if (unmap_view((void*)base) < 0 || unmap_view((void*)(base + PAGE)) < 0) return 1;

    long fd = section_create(PAGE, SAVANXP_SECTION_READ);
    if (fd < 0 || map_view_at((int)fd, VIEW_END - PAGE, SAVANXP_SECTION_READ) !=
        (void*)(VIEW_END - PAGE) || map_first() != VIEW_BASE ||
        unmap_view((void*)VIEW_BASE) < 0 || map_first() != VIEW_BASE + PAGE) return 1;
    if (!check_new_images()) return 1;
    puts_out("ASLR32 PASS\n");
    return 0;
}
