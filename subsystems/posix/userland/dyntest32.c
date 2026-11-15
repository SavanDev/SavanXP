#include "libc.h"
#include <savanxp/ldso.h>
#define CHECK(x) do { if (!(x)) { eprintf("DYN32 failed at line %d\n", __LINE__); return 1; } } while (0)
extern int elf32_probe(void);
extern int elf32_data[2];
extern int* elf32_second;
extern int elf32_pc;
int elf32_callback(void) { return 100; }
static struct savanxp_system_info info(void) {
    struct savanxp_system_info result;
    memset(&result, 0, sizeof(result)); system_info(&result); return result;
}
static int rejected_images(void) {
    // A valid first segment followed by an invalid file range forces rollback
    // after actual views were installed, rather than only testing header rejection.
    uint32_t image[64] = {0};
    image[0] = 0x464c457f; image[1] = 0x00010101;
    image[4] = 3 | (3u << 16); image[5] = 1; image[7] = 52;
    image[10] = 52 | (32u << 16); image[11] = 2;
    image[13] = 1; image[17] = sizeof(image); image[18] = sizeof(image);
    image[19] = 4; image[20] = 4096;
    image[21] = 1; image[22] = 0x100000; image[23] = 4096;
    image[25] = 1; image[26] = 1; image[27] = 5; image[28] = 4096;
    const char* path = "/disk/dyn32-rejected.so";
    int file = (int)savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    CHECK(file >= 0 && savanxp_write(file, image, sizeof(image)) == sizeof(image));
    CHECK(savanxp_close(file) == 0);
    const struct savanxp_system_info before = info();
    const int images = ldso_count();
    int saved = (int)savanxp_dup(0);
    CHECK(saved >= 0 && savanxp_close(0) == 0);
    for (unsigned attempt = 0; attempt < 12; ++attempt) {
        CHECK(ldso_load(path) == -5 && ldso_count() == images);
        CHECK(info().sections_live == before.sections_live && info().memory_free_bytes == before.memory_free_bytes);
    }
    CHECK(savanxp_dup2(saved, 0) == 0 && savanxp_close(saved) == 0 && savanxp_unlink(path) == 0);
    return 0;
}
int main(int argc, char** argv) {
    (void)argv;
    CHECK(!ldso_missing() && ldso_symbol_is_shared("elf32_probe"));
    CHECK(elf32_second == elf32_data + 1 && *elf32_second == 37 && elf32_probe() == 101);
    CHECK((char*)&elf32_pc + elf32_pc == (char*)(void*)elf32_callback);
    if (argc > 1) {
        char ready = 'R', release;
        CHECK(savanxp_write(1, &ready, 1) == 1 && savanxp_read(0, &release, 1) == 1);
        return 0;
    }
    CHECK(rejected_images() == 0);
    const unsigned long file_sections = info().file_sections_live;
    int down[2], up[2];
    CHECK(savanxp_pipe(down) == 0 && savanxp_pipe(up) == 0);
    const char* args[] = {"dyntest32", "child"};
    long child = spawn_fds("/disk/bin/dyntest32", args, 2, down[0], up[1], 2);
    CHECK(child > 0);
    savanxp_close(down[0]); savanxp_close(up[1]);
    char signal;
    CHECK(savanxp_read(up[0], &signal, 1) == 1 && signal == 'R');
    CHECK(info().file_sections_live == file_sections);
    CHECK(elf32_probe() == 102); // The spawned process started with its own counter.
    CHECK(savanxp_write(down[1], "X", 1) == 1);
    savanxp_close(down[1]); savanxp_close(up[0]);
    int status;
    CHECK(savanxp_waitpid((int)child, &status) == child && status == 0);
    child = savanxp_fork();
    CHECK(child >= 0);
    if (!child) return elf32_probe() != 103;
    CHECK(savanxp_waitpid((int)child, &status) == child && status == 0 && elf32_probe() == 103);
    // Loading must return all temporary file and section descriptors.
    int opened[61];
    for (int i = 0; i < 61; ++i) { opened[i] = (int)savanxp_open("/disk/bin/true"); CHECK(opened[i] == i + 3); }
    for (int i = 0; i < 61; ++i) savanxp_close(opened[i]);
    puts_fd(1, "DYN32 relocations, shared text and private data PASS\n");
    return 0;
}
