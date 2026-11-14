/* L2 fixture "files": the file calls of a static program.
 *
 *   files write DIR   create DIR/l2-files.dat, check it, and leave it there
 *   files check DIR   read back what "write" left, after a restart
 *   files remove DIR  delete it
 *
 * Covers open, read, write, writev, lseek, fstat, stat, close, short reads,
 * the end of file, and the errors ENOENT, EBADF and ENOTTY.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <termios.h>
#include <unistd.h>

#define SIZE 10000

static int fail;

static void expect(int ok, const char *what)
{
    printf("%s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        fail = 1;
    }
}

static unsigned char byte_at(int i)
{
    return (unsigned char)((i * 13 + 5) & 0xff);
}

static int check_contents(const char *path)
{
    unsigned char buf[200];
    struct stat st;
    int fd = open(path, O_RDONLY);
    expect(fd >= 0, "open for reading");
    if (fd < 0) {
        return 1;
    }
    expect(fstat(fd, &st) == 0 && st.st_size == SIZE && S_ISREG(st.st_mode), "fstat size");
    expect(lseek(fd, 5000, SEEK_SET) == 5000, "lseek set");
    int ok = read(fd, buf, 100) == 100;
    for (int i = 0; ok && i < 100; i++) {
        ok = buf[i] == byte_at(5000 + i);
    }
    expect(ok, "read middle");
    expect(lseek(fd, -10, SEEK_END) == SIZE - 10, "lseek end");
    ssize_t n = read(fd, buf, sizeof buf);
    expect(n == 10 && buf[9] == byte_at(SIZE - 1), "short read at end");
    expect(read(fd, buf, sizeof buf) == 0, "read at end of file");
    expect(lseek(fd, 0, SEEK_CUR) == SIZE, "lseek cur");
    errno = 0;
    expect(write(fd, buf, 1) == -1 && errno == EBADF, "write to read-only EBADF");
    expect(close(fd) == 0, "close");
    errno = 0;
    expect(read(fd, buf, 1) == -1 && errno == EBADF, "read closed EBADF");
    expect(stat(path, &st) == 0 && st.st_size == SIZE, "stat path");
    return 0;
}

int main(int argc, char **argv)
{
    char path[512];
    if (argc != 3) {
        fprintf(stderr, "usage: files write|check|remove DIR\n");
        return 2;
    }
    snprintf(path, sizeof path, "%s/l2-files.dat", argv[2]);

    if (strcmp(argv[1], "write") == 0) {
        unsigned char data[SIZE];
        for (int i = 0; i < SIZE; i++) {
            data[i] = byte_at(i);
        }
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        expect(fd >= 0, "create");
        if (fd < 0) {
            return 1;
        }
        expect(write(fd, data, 4000) == 4000, "write");
        struct iovec iov[2] = {{data + 4000, 3000}, {data + 7000, 3000}};
        expect(writev(fd, iov, 2) == 6000, "writev");
        expect(close(fd) == 0, "close after write");
        check_contents(path);
        errno = 0;
        char missing[600];
        snprintf(missing, sizeof missing, "%s/no-such-file", argv[2]);
        expect(open(missing, O_RDONLY) == -1 && errno == ENOENT, "open missing ENOENT");
        struct termios t;
        fd = open(path, O_RDONLY);
        errno = 0;
        expect(tcgetattr(fd, &t) == -1 && errno == ENOTTY, "tcgetattr on file ENOTTY");
        expect(!isatty(fd), "isatty on file");
        close(fd);
    } else if (strcmp(argv[1], "check") == 0) {
        check_contents(path);
    } else if (strcmp(argv[1], "remove") == 0) {
        expect(unlink(path) == 0, "unlink");
        errno = 0;
        struct stat st;
        expect(stat(path, &st) == -1 && errno == ENOENT, "stat after unlink ENOENT");
    } else {
        return 2;
    }
    printf("files %s %s\n", argv[1], fail ? "FAILED" : "ok");
    return fail;
}
