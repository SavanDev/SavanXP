#include "libc.h"
#include <stdlib.h>
#include <string.h>

#define RW (SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE)
#define RX (SAVANXP_SECTION_READ | SAVANXP_SECTION_EXEC)
#define CHECK(condition) do { if (!(condition)) { eprintf("VM32 failed at line %d\n", __LINE__); return 1; } } while (0)

static struct savanxp_system_info snapshot(void) {
    struct savanxp_system_info info;
    memset(&info, 0, sizeof(info));
    system_info(&info);
    return info;
}
static int child_ok(long pid) {
    int status = -1;
    return pid > 0 && savanxp_waitpid((int)pid, &status) == pid && status == 0;
}
__attribute__((noinline)) static int stack_probe(void) {
    volatile unsigned char pages[256 * 1024];
    for (unsigned i = 0; i < sizeof(pages); i += 4096) pages[i] = (unsigned char)(i / 4096);
    for (unsigned i = 0; i < sizeof(pages); i += 4096)
        if (pages[i] != (unsigned char)(i / 4096)) return 1;
    return 0;
}
static int anonymous(void) {
    long fd = section_create(8191, RW | SAVANXP_SECTION_EXEC);
    CHECK(fd >= 0);
    void* fixed = map_view_at((int)fd, 0x09010000, RW);
    CHECK((unsigned long)fixed == 0x09010000);
    unsigned char* a = map_view((int)fd, RW);
    CHECK(!result_is_error((long)a) && (unsigned long)a >= 0x10000000);
    CHECK(unmap_view(fixed) == 0);
    for (unsigned i = 0; i < 8192; ++i) CHECK(a[i] == 0);
    a[4095] = 13; a[4096] = 27; a[8191] = 39;
    const unsigned char* b = map_view((int)fd, SAVANXP_SECTION_READ);
    CHECK(!result_is_error((long)b) && b[4095] == 13 && b[4096] == 27 && b[8191] == 39);
    CHECK((long)map_view((int)fd, RW | SAVANXP_SECTION_EXEC) == -SAVANXP_EINVAL);
    CHECK((long)map_view_at((int)fd, (unsigned long)a, RW) == -SAVANXP_ENOMEM);
    CHECK((long)map_view_at((int)fd, 0x10000001, RW) == -SAVANXP_EINVAL);
    CHECK((long)map_view_at((int)fd, 0xfffff000, RW) == -SAVANXP_ENOMEM);
    CHECK((long)map_view_at((int)fd, 0x0eeff000, RW) == -SAVANXP_ENOMEM);
    int duplicate = (int)savanxp_dup((int)fd);
    CHECK(duplicate >= 0 && savanxp_close((int)fd) == 0 && savanxp_close(duplicate) == 0);
    CHECK(a[4096] == 27 && unmap_view(a) == 0 && b[4096] == 27 && unmap_view((void*)b) == 0);
    fd = section_create(4096, SAVANXP_SECTION_READ);
    CHECK(fd >= 0 && (long)map_view((int)fd, RW) == -SAVANXP_EACCES);
    CHECK(savanxp_close((int)fd) == 0);
    // Copy machine code through a writable alias; execute through a distinct RX view.
    fd = section_create(4096, RW | SAVANXP_SECTION_EXEC);
    CHECK(fd >= 0);
    a = map_view((int)fd, RW);
    void* code = map_view((int)fd, RX);
    CHECK(!result_is_error((long)a) && !result_is_error((long)code));
    const unsigned char probe[] = {0xb8, 60, 0, 0, 0, 0xc3};
    memcpy(a, probe, sizeof(probe));
    CHECK(unmap_view(a) == 0 && ((int (*)(void))code)() == 60);
    CHECK(savanxp_close((int)fd) == 0 && unmap_view(code) == 0);
    puts_fd(1, "VM32 anonymous grants and execution PASS\n");
    return 0;
}
static int file_backing(void) {
    const char* path = "/disk/vm32-cache";
    int fd = (int)savanxp_open_mode(path, SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    CHECK(fd >= 0 && savanxp_write(fd, "first image", 11) == 11);
    long first = section_open(fd, SAVANXP_SECTION_READ);
    CHECK(first >= 0);
    const char* a = map_view((int)first, SAVANXP_SECTION_READ);
    CHECK(!result_is_error((long)a) && memcmp(a, "first image", 11) == 0 && a[11] == 0);
    struct savanxp_system_info before = snapshot();
    long same = section_open(fd, RX);
    CHECK(same >= 0 && snapshot().file_sections_live == before.file_sections_live);
    CHECK((long)map_view((int)first, RX) == -SAVANXP_EACCES);
    void* executable = map_view((int)same, RX);
    CHECK(!result_is_error((long)executable));
    CHECK(unmap_view(executable) == 0 && savanxp_close((int)same) == 0);
    CHECK(seek(fd, 0, SAVANXP_SEEK_SET) == 0 && savanxp_write(fd, "other image", 11) == 11);
    long second = section_open(fd, SAVANXP_SECTION_READ);
    CHECK(second >= 0 && snapshot().file_sections_live == before.file_sections_live + 1);
    const char* b = map_view((int)second, SAVANXP_SECTION_READ);
    CHECK(!result_is_error((long)b) && memcmp(b, "other image", 11) == 0 && memcmp(a, "first image", 11) == 0);
    long slice = section_open_range(fd, 6, 5, SAVANXP_SECTION_READ);
    CHECK(slice >= 0);
    const char* c = map_view((int)slice, SAVANXP_SECTION_READ);
    CHECK(!result_is_error((long)c) && memcmp(c, "image", 5) == 0 && c[5] == 0);
    CHECK(section_open(fd, RW) == -SAVANXP_EINVAL);
    CHECK(section_open_range(fd, 0xfffffff0, 32, 1) == -SAVANXP_ENOMEM);
    CHECK(savanxp_close((int)first) == 0 && savanxp_close((int)second) == 0 && savanxp_close((int)slice) == 0);
    CHECK(savanxp_close(fd) == 0 && savanxp_unlink(path) == 0);
    fd = (int)savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_READ | SAVANXP_OPEN_CREATE);
    CHECK(fd >= 0 && savanxp_write(fd, "third image", 11) == 11);
    long recreated = section_open(fd, SAVANXP_SECTION_READ);
    CHECK(recreated >= 0);
    const char* fresh = map_view((int)recreated, SAVANXP_SECTION_READ);
    CHECK(!result_is_error((long)fresh) && memcmp(fresh, "third image", 11) == 0 && memcmp(a, "first image", 11) == 0);
    CHECK(savanxp_close((int)recreated) == 0 && unmap_view((void*)fresh) == 0);
    CHECK(unmap_view((void*)a) == 0 && unmap_view((void*)b) == 0 && unmap_view((void*)c) == 0);
    CHECK(savanxp_close(fd) == 0 && savanxp_unlink(path) == 0);
    puts_fd(1, "VM32 file sharing, ranges and mutation snapshots PASS\n");
    return 0;
}
static int transfer(void) {
    int channels[2];
    CHECK(savanxp_pipe(channels) == 0);
    long section = section_create(4096, RW);
    CHECK(section >= 0);
    for (int i = 0; i < 4; ++i) CHECK(pipe_send_handle(channels[1], (int)section) == 0);
    CHECK(pipe_send_handle(channels[1], (int)section) == -SAVANXP_EAGAIN);
    CHECK(pipe_send_handle(channels[1], channels[0]) == -SAVANXP_EINVAL);
    CHECK(savanxp_close((int)section) == 0);
    int held[64], held_count = 0;
    for (;;) {
        long fd = savanxp_open("/disk/bin/true");
        if (fd < 0) break;
        held[held_count++] = (int)fd;
    }
    CHECK(held_count > 0 && pipe_receive_handle(channels[0]) == -SAVANXP_EBADF);
    CHECK(savanxp_close(held[--held_count]) == 0);
    long recovered = pipe_receive_handle(channels[0]);
    CHECK(recovered >= 0 && savanxp_close((int)recovered) == 0);
    while (held_count) CHECK(savanxp_close(held[--held_count]) == 0);
    for (int i = 0; i < 3; ++i) {
        long received = pipe_receive_handle(channels[0]);
        CHECK(received >= 0 && savanxp_close((int)received) == 0);
    }
    CHECK(pipe_receive_handle(channels[0]) == -SAVANXP_EAGAIN);
    section = section_create(4096, RW);
    CHECK(section >= 0 && pipe_send_handle(channels[1], (int)section) == 0);
    CHECK(savanxp_close(channels[0]) == 0 && pipe_send_handle(channels[1], (int)section) == -SAVANXP_EPIPE);
    CHECK(savanxp_close(channels[1]) == 0 && savanxp_close((int)section) == 0);
    puts_fd(1, "VM32 queued handle ownership PASS\n");
    return 0;
}
static int budgets(void) {
    long handle = section_create(4096, RW);
    CHECK(handle >= 0);
    volatile unsigned* barrier = map_view((int)handle, RW);
    CHECK(!result_is_error((long)barrier) && savanxp_close((int)handle) == 0);
    // Four address spaces can exhaust the global table while respecting the
    // six-process bring-up limit: children retain both views and bare handles.
    long children[3];
    for (unsigned id = 0; id < 3; ++id) {
        children[id] = savanxp_fork();
        CHECK(children[id] >= 0);
        if (!children[id]) {
            unsigned held = 0;
            for (unsigned i = 0; i < 60; ++i) {
                long section = section_create(4096, RW);
                if (section < 0) { if (section != -SAVANXP_ENOMEM) exit(2); break; }
                void* view = map_view((int)section, RW);
                savanxp_close((int)section);
                if (result_is_error((long)view)) exit(3);
                ++held;
            }
            for (unsigned i = 0; i < 40; ++i) {
                long section = section_create(4096, RW);
                if (section < 0) { if (section != -SAVANXP_ENOMEM) exit(4); break; }
                ++held;
            }
            barrier[id] = held;
            barrier[3 + id] = 1;
            while (!barrier[6]) sleep_ms(10);
            exit(0); // Deliberately abandon all views and handles.
        }
        while (!barrier[3 + id]) sleep_ms(10);
    }
    CHECK(snapshot().sections_live == 256 && section_create(4096, RW) == -SAVANXP_ENOMEM);
    CHECK(barrier[0] + barrier[1] + barrier[2] == 255);
    barrier[6] = 1;
    for (unsigned i = 0; i < 3; ++i) CHECK(child_ok(children[i]));
    CHECK(snapshot().sections_live == 1);
    CHECK(unmap_view((void*)barrier) == 0);
    handle = section_create(4096, RW);
    CHECK(handle >= 0);
    void* views[64];
    for (unsigned i = 0; i < 64; ++i) { views[i] = map_view((int)handle, RW); CHECK(!result_is_error((long)views[i])); }
    CHECK((long)map_view((int)handle, RW) == -SAVANXP_ENOMEM);
    CHECK(savanxp_close((int)handle) == 0);
    for (unsigned i = 0; i < 64; ++i) CHECK(unmap_view(views[i]) == 0);
    puts_fd(1, "VM32 global and per-process budgets PASS\n");
    return 0;
}
static int low_memory(void) {
    struct savanxp_system_info baseline = snapshot();
    if (baseline.memory_free_bytes >= 64u * 1024u * 1024u) return 0;
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(section_create(64u * 1024u * 1024u, RW) == -SAVANXP_ENOMEM);
        CHECK(snapshot().memory_free_bytes == baseline.memory_free_bytes);
        CHECK(snapshot().sections_live == baseline.sections_live);
    }
    unsigned bytes = (unsigned)(baseline.memory_free_bytes / 2 + 1024u * 1024u);
    long section = section_create(bytes, RW);
    CHECK(section >= 0);
    volatile unsigned char* data = map_view((int)section, RW | SAVANXP_VIEW_PRIVATE);
    CHECK(!result_is_error((long)data));
    CHECK(savanxp_close((int)section) == 0);
    data[0] = 41; data[bytes - 1] = 73;
    const struct savanxp_system_info held = snapshot();
    CHECK(savanxp_fork() == -SAVANXP_ENOMEM);
    CHECK(data[0] == 41 && data[bytes - 1] == 73 && snapshot().memory_free_bytes == held.memory_free_bytes);
    CHECK(snapshot().sections_live == held.sections_live && unmap_view((void*)data) == 0);
    CHECK(snapshot().memory_free_bytes == baseline.memory_free_bytes);
    puts_fd(1, "VM32 ENOMEM rollback PASS\n");
    return 0;
}
int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "after-exec") == 0) return snapshot().sections_live != 0;
    const struct savanxp_system_info baseline = snapshot();
    CHECK(anonymous() == 0 && file_backing() == 0 && transfer() == 0 && budgets() == 0 && low_memory() == 0);
    CHECK(stack_probe() == 0);
    long child = savanxp_fork();
    CHECK(child >= 0);
    if (!child) {
        long section = section_create(8192, RW);
        if (section < 0 || result_is_error((long)map_view((int)section, RW))) exit(1);
        savanxp_close((int)section);
        const char* args[] = {"vmtest32", "after-exec"};
        exec("/disk/bin/vmtest32", args, 2);
        exit(2);
    }
    CHECK(child_ok(child));
    const struct savanxp_system_info after = snapshot();
    CHECK(after.sections_live == baseline.sections_live && after.file_sections_live == baseline.file_sections_live);
    // The only permanent difference is the stack pages touched by stack_probe.
    CHECK(after.memory_free_bytes + 256 * 1024 >= baseline.memory_free_bytes);
    puts_fd(1, "VM32 stack demand and exec cleanup PASS\nVM32 PASS\n");
    return 0;
}
