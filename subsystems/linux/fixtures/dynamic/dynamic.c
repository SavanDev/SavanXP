/* L4: execute unchanged on Linux against the same guest files. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>

extern int l4_tls(void);
#define PAGE 4096
static void result(const char *name, long value) {
    printf("%s=%ld\n", name, value < 0 ? -(long)errno : value);
}
int main(int argc, char **argv) {
    if (argc > 1) {
        char *page = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) return 98;
        page[0] = (char)0xc3;
        if (!strcmp(argv[1], "wx")) {
            /* SSL's explicit W^X policy differs from Linux. */
            int status = mprotect(page, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC);
            return status < 0 && errno == EACCES ? 0 : 97;
        }
        if (mprotect(page, PAGE, PROT_READ) != 0) return 96;
        if (!strcmp(argv[1], "write")) *(volatile char *)page = 0;
        else ((void (*)(void))page)();
        return 95; /* A write to read-only memory, or NX execution, escaped. */
    }
    result("shared-tls", l4_tls());
    unsigned char random[64];
    result("random", getrandom(random, sizeof(random), 0));
    result("random-zero", getrandom((void *)1, 0, 0));
    result("random-bad", getrandom((void *)1, 4, 0));
    result("random-flags", getrandom(random, 4, 128));
    char *pages = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) return 1;
    pages[PAGE] = 77;
    result("protect-none", mprotect(pages + PAGE, PAGE, PROT_NONE));
    result("none-copy", write(1, pages + PAGE, 1));
    result("protect-restore", mprotect(pages + PAGE, PAGE, PROT_READ | PROT_WRITE));
    result("contents-retained", pages[PAGE] == 77);
    result("protect-read", mprotect(pages + PAGE, PAGE, PROT_READ));
    int fd = open("/work/l4-map", O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) return 2;
    static unsigned char data[2 * PAGE + 17];
    memset(data, 0x3d, sizeof(data));
    /* mov $42,%eax; ret: prove that file-backed executable pages execute. */
    const unsigned char code[] = {0xb8, 42, 0, 0, 0, 0xc3};
    memcpy(data, code, sizeof(code));
    result("file-write", write(fd, data, sizeof(data)));
    lseek(fd, 0, SEEK_SET);
    result("read-into-readonly", read(fd, pages + PAGE, 1));
    result("protect-split", mprotect(pages, 3 * PAGE, PROT_READ));
    result("protect-unaligned", mprotect(pages + 1, PAGE, PROT_READ));
    munmap(pages + PAGE, PAGE);
    result("protect-hole", mprotect(pages, 3 * PAGE, PROT_READ));
    munmap(pages, 3 * PAGE);
    char *file = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) return 3;
    result("file-bytes", !memcmp(file, data, sizeof(data)));
    result("file-tail-zero", file[2 * PAGE + 17] == 0 && file[3 * PAGE - 1] == 0);
    result("past-eof-copy", write(1, file + 3 * PAGE, 1));
    file[PAGE] = 55;
    unsigned char original = 0;
    lseek(fd, PAGE, SEEK_SET);
    read(fd, &original, 1);
    result("private-write", original == 0x3d);
    char *hidden = mmap(NULL, PAGE, PROT_NONE, MAP_PRIVATE, fd, PAGE);
    if (hidden == MAP_FAILED) return 4;
    result("file-none-copy", write(1, hidden, 1));
    result("file-none-restore", mprotect(hidden, PAGE, PROT_READ));
    result("file-offset", hidden[0] == 0x3d);
    munmap(hidden, PAGE);
    void *exec = mmap(NULL, PAGE, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    if (exec == MAP_FAILED) return 5;
    result("file-execute", ((int (*)(void))exec)());
    munmap(exec, PAGE);
    result("file-offset-unaligned", mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, 1) == MAP_FAILED ? -1 : 0);
    char *fixed = mmap(file + PAGE, PAGE, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
    result("file-fixed", fixed == file + PAGE && !memcmp(fixed, data, sizeof(code)));
    enum { MAPS = 320 };
    char *many[MAPS];
    int created = 0;
    for (; created < MAPS; ++created) {
        many[created] = mmap(NULL, 3 * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (many[created] == MAP_FAILED) break;
        if (mmap(many[created] + PAGE, PAGE, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, PAGE) == MAP_FAILED) break;
        if (many[created][PAGE] != 0x3d) break;
    }
    result("many-mappings", created);
    while (created) munmap(many[--created], 3 * PAGE);
    close(fd);
    unlink("/work/l4-map");
    result("file-after-close-unlink", file[PAGE] == (char)0xb8);
    munmap(file, 4 * PAGE);
    char link[64];
    result("readlink-directory", readlink("/work", link, sizeof(link)));
    result("readlink-missing", readlink("/work/no-such-link", link, sizeof(link)));
    return 0;
}
