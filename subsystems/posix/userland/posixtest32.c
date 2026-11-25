#include "libc.h"
#include <string.h>

static const char* kSelf = "/disk/bin/posixtest32";
static const char* kDirectory = "/disk/posix-regression";
static const char* kFile = "/disk/posix-regression/data";
static unsigned char g_payload[24576];
static unsigned char g_readback[24576];
static volatile unsigned int g_marker;

static int fail(const char* message) {
    puts_out("POSIX FAIL "); puts_out(message); puts_out("\n"); return 1;
}
#define CHECK(condition, message) do { if (!(condition)) return fail(message); } while (0)

static int collect(long pid, int expected) {
    int status = -1;
    return pid > 0 && savanxp_waitpid((int)pid, &status) == pid && status == expected;
}

static long child(const char* mode, int input, int output) {
    const char* args[] = {kSelf, mode};
    return spawn_fd(kSelf, args, 2, input, output);
}

static long raw_spawn_fd(const char* path, const char* const* args, unsigned int count, int input, int output) {
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "a"(SAVANXP_SYS_SPAWN_FD), "D"(path),
        "S"(args), "d"(count), "c"(input), "b"(output) : "memory");
    return result;
}

static unsigned int decimal(const char* text) {
    unsigned int result = 0;
    while (*text >= '0' && *text <= '9') result = result * 10 + (unsigned int)(*text++ - '0');
    return result;
}

static void number(char* text, unsigned int value) {
    char reversed[12]; unsigned int count = 0, index = 0;
    do { reversed[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) text[index++] = reversed[--count];
    text[index] = 0;
}

static int worker(int argc, char** argv) {
    /* The v0.4.0 ES layout has no key for '<' or '>'. The console transcript
     * selects EN through the reference ioctl, as the taskbar selector does. */
    if (strcmp(argv[1], "layout") == 0 && argc == 3 &&
        (strcmp(argv[2], "en") == 0 || strcmp(argv[2], "es") == 0)) {
        const int english = strcmp(argv[2], "en") == 0;
        long input = savanxp_open_mode("/dev/input0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
        if (input < 0 || input_set_layout((int)input, english ? SAVANXP_KEYBOARD_LAYOUT_EN
                                                              : SAVANXP_KEYBOARD_LAYOUT_ES) < 0) return 1;
        savanxp_close((int)input);
        puts_out(english ? "POSIX layout en\n" : "POSIX layout es\n");
        return 0;
    }
    if (strcmp(argv[1], "persist-write") == 0 || strcmp(argv[1], "persist-read") == 0) {
        const char* path = "/disk/posix-persistent";
        const char* text = "SavanXP i386 persistent file\n";
        const unsigned int length = (unsigned int)strlen(text);
        if (strcmp(argv[1], "persist-write") == 0) {
            long fd = savanxp_open_mode("/disk/posix-persistent-tmp",
                SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
            CHECK(fd >= 0 && savanxp_write((int)fd, text, length) == (long)length &&
                  savanxp_close((int)fd) == 0, "persistent write");
            savanxp_unlink(path);
            CHECK(savanxp_rename("/disk/posix-persistent-tmp", path) == 0 && savanxp_sync() == 0,
                  "persistent rename and sync");
            puts_out("POSIX PERSIST WRITE PASS\n");
        } else {
            long fd = savanxp_open(path);
            struct savanxp_stat info;
            CHECK(fd >= 0 && savanxp_fstat((int)fd, &info) == 0 && info.st_size == length &&
                  savanxp_read((int)fd, g_readback, sizeof(g_readback)) == (long)length &&
                  memcmp(g_readback, text, length) == 0 && savanxp_read((int)fd, g_readback, 1) == 0 &&
                  savanxp_close((int)fd) == 0, "persistent cold-boot read");
            puts_out("POSIX PERSIST READ PASS\n");
        }
        return 0;
    }
    if (strcmp(argv[1], "audit") == 0) {
        struct savanxp_process_info info;
        unsigned int count = 0, idles = 0;
        for (unsigned long index = 0; proc_info(index, &info) > 0; ++index) {
            CHECK(info.state != SAVANXP_PROC_ZOMBIE, "shell left a zombie");
            if (info.flags & SAVANXP_PROC_FLAG_IDLE) {
                CHECK(info.handle_count == 0, "idle acquired descriptors");
                ++idles;
                continue;
            }
            CHECK(info.handle_count == 3, "shell leaked descriptors");
            ++count;
        }
        CHECK(count == 3, "shell left a process");
        struct savanxp_system_info memory;
        char bytes[12];
        CHECK(system_info(&memory) == 0, "shell cleanup memory sample");
        CHECK(idles == memory.cpu_online, "shell cleanup idle count");
        number(bytes, (unsigned int)memory.memory_free_bytes);
        puts_out("POSIX audit free bytes "); puts_out(bytes); puts_out("\n");
        puts_out("POSIX shell cleanup PASS\n"); return 0;
    }
    if (strcmp(argv[1], "produce") == 0) {
        for (unsigned int index = 0; index < sizeof(g_payload); ++index)
            g_payload[index] = (unsigned char)(index * 17 + 31);
        return savanxp_write(1, g_payload, sizeof(g_payload)) == (long)sizeof(g_payload) ? 43 : 50;
    }
    if (strcmp(argv[1], "copy") == 0) {
        unsigned char buffer[257]; long count;
        while ((count = savanxp_read(0, buffer, sizeof(buffer))) > 0) {
            if (savanxp_write(1, buffer, (size_t)count) != count) return 51;
        }
        return count == 0 ? 41 : 52;
    }
    if (strcmp(argv[1], "inherit") == 0) {
        struct savanxp_stat info;
        char text[4];
        if (savanxp_fstat(0, &info) || (info.st_mode & SAVANXP_S_IFMT) != SAVANXP_S_IFREG ||
            savanxp_read(0, text, 4) != 4 || memcmp(text, "efgh", 4) != 0) return 53;
        for (int fd = 3; fd < 64; ++fd) if (savanxp_fstat(fd, &info) != -SAVANXP_EBADF) return 54;
        if (savanxp_write(1, "child", 5) != 5 || savanxp_write(2, "error", 5) != 5) return 55;
        return 42;
    }
    if (strcmp(argv[1], "fault") == 0) {
        // Deliberately leave several live objects for exceptional-exit cleanup.
        if (savanxp_open(kFile) < 0) return 56;
        int fds[2]; if (savanxp_pipe(fds) < 0) return 57;
        __asm__ volatile("ud2");
    }
    if (strcmp(argv[1], "exec") == 0) {
        CHECK(savanxp_chdir(kDirectory) == 0, "exec cwd setup");
        long fd = savanxp_open_mode("exec-data", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
        CHECK(fd == 3, "exec descriptor setup");
        char pid[12]; number(pid, (unsigned int)savanxp_getpid());
        const char* args[] = {kSelf, "exec-done", pid};
        g_marker = 0x12345678;
        CHECK(exec("/disk/bin/missing-exec", args, 3) == -SAVANXP_ENOENT && g_marker == 0x12345678,
              "failed exec preserves image");
        CHECK(exec("/disk/bin", args, 3) == -SAVANXP_ENOEXEC && g_marker == 0x12345678,
              "bad exec preserves image");
        exec(kSelf, args, 3);
        return fail("exec returned after success");
    }
    if (strcmp(argv[1], "exec-done") == 0 && argc == 3) {
        char cwd[256];
        CHECK((unsigned int)savanxp_getpid() == decimal(argv[2]), "exec PID preservation");
        CHECK(savanxp_getcwd(cwd, sizeof(cwd)) > 0 && strcmp(cwd, kDirectory) == 0, "exec cwd preservation");
        CHECK(savanxp_write(3, "exec survives", 13) == 13 && g_marker == 0, "exec FD and fresh BSS");
        return 67;
    }
    return 58;
}

static int files(void) {
    CHECK(savanxp_mkdir(kDirectory) == 0 || savanxp_stat(kDirectory, (struct savanxp_stat*)g_readback) == 0,
          "test directory");
    const char* old[] = {"inherited", "error", "exec-data", "stream", "renamed"};
    for (unsigned int index = 0; index < sizeof(old) / sizeof(old[0]); ++index) {
        char path[256]; strcpy(path, kDirectory); strcat(path, "/"); strcat(path, old[index]);
        savanxp_unlink(path);
    }
    long fd = savanxp_open_mode(kFile, SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    CHECK(fd == 3 && savanxp_write((int)fd, "abcdefghijklmnop", 16) == 16, "regular write");
    CHECK(seek((int)fd, 0, SAVANXP_SEEK_SET) == 0, "seek set");
    long shared = savanxp_dup((int)fd);
    CHECK(shared == 4 && savanxp_read((int)fd, g_readback, 4) == 4 && memcmp(g_readback, "abcd", 4) == 0,
          "first shared read");
    CHECK(savanxp_read((int)shared, g_readback, 4) == 4 && memcmp(g_readback, "efgh", 4) == 0,
          "dup shared offset");
    CHECK(savanxp_dup2((int)fd, 10) == 10 && seek(10, -4, SAVANXP_SEEK_CUR) == 4 &&
          seek((int)fd, 0, SAVANXP_SEEK_CUR) == 4, "dup2 shared offset");
    CHECK(savanxp_dup2(10, 10) == 10 && savanxp_close(10) == 0 && savanxp_close((int)shared) == 0,
          "dup2 same descriptor");
    long independent = savanxp_open(kFile);
    CHECK(independent == 4 && savanxp_read((int)independent, g_readback, 4) == 4 &&
          memcmp(g_readback, "abcd", 4) == 0 && seek((int)fd, 0, SAVANXP_SEEK_CUR) == 4,
          "independent open offset");
    CHECK(savanxp_write((int)independent, "x", 1) == -SAVANXP_EBADF &&
          seek((int)fd, -17, SAVANXP_SEEK_END) == -SAVANXP_EINVAL &&
          seek((int)fd, 1, 99) == -SAVANXP_EINVAL, "file access and seek validation");
    struct savanxp_stat info;
    CHECK(savanxp_stat(kFile, &info) == 0 && info.st_size == 16 &&
          (info.st_mode & SAVANXP_S_IFMT) == SAVANXP_S_IFREG && savanxp_fstat((int)fd, &info) == 0,
          "stat and fstat");
    CHECK(savanxp_unlink(kFile) == -SAVANXP_EBUSY && savanxp_close((int)independent) == 0 &&
          savanxp_close((int)fd) == 0, "open-file ownership");
    fd = savanxp_open_mode(kFile, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_APPEND);
    CHECK(fd == 3 && savanxp_write((int)fd, "xyz", 3) == 3 && savanxp_close((int)fd) == 0,
          "append");
    CHECK(savanxp_stat(kFile, &info) == 0 && info.st_size == 19 && savanxp_truncate(kFile, 10) == 0,
          "truncate");
    CHECK(savanxp_rename(kFile, "/disk/posix-regression/renamed") == 0 &&
          savanxp_stat(kFile, &info) == -SAVANXP_ENOENT &&
          savanxp_rename("/disk/posix-regression/renamed", kFile) == 0, "rename");
    char cwd[256];
    CHECK(savanxp_chdir(kDirectory) == 0 && savanxp_getcwd(cwd, sizeof(cwd)) > 0 &&
          strcmp(cwd, kDirectory) == 0 && savanxp_stat("../posix-regression/./data", &info) == 0,
          "relative paths");
    CHECK(savanxp_chdir("data") == -SAVANXP_ENOTDIR && savanxp_chdir("/") == 0, "chdir validation");
    fd = savanxp_open(kDirectory);
    CHECK(fd == 3 && savanxp_readdir((int)fd, (char*)g_readback, sizeof(g_readback)) > 0 &&
          strcmp((char*)g_readback, "data") == 0 &&
          savanxp_readdir((int)fd, (char*)g_readback, sizeof(g_readback)) == 0 &&
          savanxp_rmdir(kDirectory) == -SAVANXP_EBUSY && savanxp_close((int)fd) == 0,
          "directory iteration and ownership");
    CHECK(savanxp_rmdir(kDirectory) == -SAVANXP_ENOTEMPTY, "nonempty directory");
    CHECK(savanxp_mkdir("/disk/tmp") == 0 || savanxp_stat("/disk/tmp", &info) == 0,
          "reference diagnostic directory");
    CHECK(savanxp_open((const char*)1) == -SAVANXP_EFAULT &&
          savanxp_stat(kFile, (struct savanxp_stat*)"readonly") == -SAVANXP_EFAULT &&
          savanxp_pipe((int*)"readonly") == -SAVANXP_EFAULT &&
          savanxp_close(-1) == -SAVANXP_EBADF && savanxp_dup2(1, -1) == -SAVANXP_EBADF,
          "invalid descriptors and pointers");
    puts_out("POSIX files and paths PASS\n");
    return 0;
}

static int inheritance(void) {
    long input = savanxp_open(kFile);
    long output = savanxp_open_mode("/disk/posix-regression/inherited", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    long error = savanxp_open_mode("/disk/posix-regression/error", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    CHECK(input == 3 && output == 4 && error == 5 && seek((int)input, 4, SAVANXP_SEEK_SET) == 4,
          "inheritance setup");
    const char* args[] = {kSelf, "inherit"};
    CHECK(collect(spawn_fds(kSelf, args, 2, (int)input, (int)output, (int)error), 42),
          "stdio inheritance without unrelated handles");
    CHECK(seek((int)input, 0, SAVANXP_SEEK_CUR) == 8 && seek((int)output, 0, SAVANXP_SEEK_CUR) == 5 &&
          seek((int)error, 0, SAVANXP_SEEK_CUR) == 5, "inherited shared offsets");
    const char* copy_args[] = {kSelf, "copy"};
    CHECK(raw_spawn_fd(kSelf, copy_args, 2, 63, (int)output) == -SAVANXP_EBADF &&
          seek((int)input, 0, SAVANXP_SEEK_SET) == 0 &&
          collect(raw_spawn_fd(kSelf, copy_args, 2, (int)input, (int)output), 41) &&
          seek((int)input, 0, SAVANXP_SEEK_CUR) == 10 && seek((int)output, 0, SAVANXP_SEEK_CUR) == 15,
          "i386 five-register spawn FD ABI and rollback");
    CHECK(savanxp_close((int)input) == 0 && savanxp_close((int)output) == 0 && savanxp_close((int)error) == 0,
          "inheritance cleanup");
    CHECK(collect(child("exec", 0, 1), 67), "exec lifecycle");
    puts_out("POSIX inheritance and exec PASS\n");
    return 0;
}

static int pipes(void) {
    for (unsigned int index = 0; index < sizeof(g_payload); ++index) g_payload[index] = (unsigned char)(index * 17 + 31);
    int fds[2];
    CHECK(savanxp_pipe(fds) == 0, "pipe create");
    CHECK(savanxp_fcntl(fds[0], SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) == 0 &&
          savanxp_read(fds[0], g_readback, 1) == -SAVANXP_EAGAIN, "empty nonblocking read");
    CHECK(savanxp_fcntl(fds[1], SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) == 0 &&
          savanxp_write(fds[1], g_payload, 8190) == 8190 &&
          savanxp_write(fds[1], g_payload, 4) == -SAVANXP_EAGAIN &&
          savanxp_read(fds[0], g_readback, 8192) == 8190 && memcmp(g_payload, g_readback, 8190) == 0,
          "atomic nonblocking pipe write");
    CHECK(savanxp_write(fds[1], g_payload, 6000) == 6000 && savanxp_read(fds[0], g_readback, 5000) == 5000 &&
          savanxp_write(fds[1], g_payload, 6000) == 6000 && savanxp_read(fds[0], g_readback, 7000) == 7000 &&
          memcmp(g_readback, g_payload + 5000, 1000) == 0 && memcmp(g_readback + 1000, g_payload, 6000) == 0,
          "pipe ring wraparound");
    struct savanxp_pollfd entry = {fds[0], SAVANXP_POLLIN, 0};
    unsigned long began = uptime_ms();
    CHECK(savanxp_poll(&entry, 1, 20) == 0 && uptime_ms() - began >= 20, "poll timeout");
    CHECK(savanxp_close(fds[1]) == 0 && savanxp_poll(&entry, 1, 0) == 1 &&
          (entry.revents & SAVANXP_POLLHUP) && savanxp_read(fds[0], g_readback, 1) == 0 &&
          savanxp_close(fds[0]) == 0, "pipe EOF and poll HUP");
    CHECK(savanxp_pipe(fds) == 0, "blocking pipe create");
    int output[2];
    CHECK(savanxp_pipe(output) == 0, "output pipe create");
    long pid = child("copy", fds[0], output[1]);
    long producer = child("produce", 0, fds[1]);
    CHECK(pid > 0 && producer > 0 && savanxp_close(fds[0]) == 0 &&
          savanxp_close(fds[1]) == 0 && savanxp_close(output[1]) == 0, "blocking pipe inheritance");
    unsigned int received = 0;
    long bytes = 0;
    while (received < sizeof(g_readback) &&
           (bytes = savanxp_read(output[0], g_readback + received, sizeof(g_readback) - received)) > 0)
        received += (unsigned int)bytes;
    CHECK(received == sizeof(g_readback) && memcmp(g_readback, g_payload, sizeof(g_payload)) == 0 &&
          savanxp_read(output[0], g_readback, 1) == 0 && savanxp_close(output[0]) == 0 &&
          collect(producer, 43) && collect(pid, 41), "blocking pipe progress, contents and EOF");
    for (int round = 0; round < 96; ++round) {
        CHECK(savanxp_pipe(fds) == 0, "fault pipe reuse");
        pid = child("fault", fds[0], 1);
        CHECK(pid > 0 && savanxp_close(fds[0]) == 0 && collect(pid, 134), "fault closes reader");
        entry.fd = fds[1]; entry.events = SAVANXP_POLLOUT;
        CHECK(savanxp_poll(&entry, 1, 0) == 1 && (entry.revents & SAVANXP_POLLERR) &&
              savanxp_write(fds[1], "x", 1) == -SAVANXP_EPIPE && savanxp_close(fds[1]) == 0,
              "reader fault EPIPE and cleanup");
    }
    puts_out("POSIX pipes and poll PASS\n");
    return 0;
}

static int applet(const char* path, const char* const* args, int count, int status) {
    return collect(spawn(path, args, count), status);
}

static int applets(void) {
    const char* banner[] = {"/bin/busybox"};
    CHECK(applet(banner[0], banner, 1, 0), "BusyBox multicall banner");
    const char* list[] = {"/bin/ls", kDirectory};
    const char* ps[] = {"/bin/ps"};
    const char* truth[] = {"/bin/true"};
    const char* falsity[] = {"/bin/false"};
    const char* sleep[] = {"/bin/sleep", "0"};
    CHECK(applet(list[0], list, 2, 0) && applet(ps[0], ps, 1, 0) &&
          applet(truth[0], truth, 1, 0) && applet(falsity[0], falsity, 1, 1) &&
          applet(sleep[0], sleep, 2, 0), "BusyBox listing, processes and exit status");
    const char* mkdir[] = {"/bin/mkdir", "-p", "/disk/posix-regression/applets"};
    CHECK(applet(mkdir[0], mkdir, 3, 0), "BusyBox mkdir");
    const char* source = "/disk/posix-regression/applets/source";
    const char* copy = "/disk/posix-regression/applets/copy";
    const char* moved = "/disk/posix-regression/applets/moved";
    long output = savanxp_open_mode(source, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    const char* echo[] = {"/bin/echo", "busybox-data"};
    CHECK(output >= 0 && collect(spawn_fd(echo[0], echo, 2, 0, (int)output), 0) &&
          savanxp_close((int)output) == 0, "BusyBox echo redirection");
    const char* cp[] = {"/bin/cp", source, copy};
    const char* mv[] = {"/bin/mv", copy, moved};
    const char* cat[] = {"/bin/cat", moved};
    CHECK(applet(cp[0], cp, 3, 0) && applet(mv[0], mv, 3, 0) && applet(cat[0], cat, 2, 0),
          "BusyBox cp, mv and cat");
    output = savanxp_open(moved);
    CHECK(output >= 0 && savanxp_read((int)output, g_readback, sizeof(g_readback)) == 13 &&
          memcmp(g_readback, "busybox-data\n", 13) == 0 && savanxp_close((int)output) == 0,
          "BusyBox copied bytes");
    const char* rm[] = {"/bin/rm", source, moved};
    CHECK(applet(rm[0], rm, 3, 0) && savanxp_rmdir(mkdir[2]) == 0, "BusyBox rm and directory cleanup");
    puts_out("POSIX BusyBox applets PASS\n");
    return 0;
}

static int exhaustion(void) {
    int fds[64], count = 0;
    while (count < 64) {
        long fd = savanxp_open("/README");
        if (fd < 0) { CHECK(fd == -SAVANXP_EBADF, "descriptor exhaustion errno"); break; }
        fds[count++] = (int)fd;
    }
    CHECK(count == 61 && savanxp_close(fds[--count]) == 0, "bounded descriptor table");
    for (int attempt = 0; attempt < 80; ++attempt) {
        int values[2];
        CHECK(savanxp_pipe(values) == -SAVANXP_EBADF, "partial pipe allocation rollback");
    }
    while (count) CHECK(savanxp_close(fds[--count]) == 0, "descriptor cleanup");
    for (int attempt = 0; attempt < 300; ++attempt) {
        int values[2];
        CHECK(savanxp_pipe(values) == 0 && savanxp_close(values[0]) == 0 && savanxp_close(values[1]) == 0,
              "pipe/object reuse");
    }
    puts_out("POSIX descriptor exhaustion PASS\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return worker(argc, argv);
    struct savanxp_system_info before, after;
    CHECK(system_info(&before) == 0, "baseline memory");
    if (files() || inheritance() || pipes() || applets() || exhaustion()) return 1;
    CHECK(system_info(&after) == 0 && after.memory_free_bytes == before.memory_free_bytes &&
          after.process_count == before.process_count, "process or page leak");
    CHECK(savanxp_sync() == 0, "persistent sync");
    char bytes[12]; number(bytes, (unsigned int)after.memory_free_bytes);
    puts_out("POSIX baseline free bytes "); puts_out(bytes); puts_out("\n");
    puts_out("POSIX PASS\n");
    return 0;
}
