/* L3 edge cases. Run unchanged on Linux in the same prepared guest tree. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <unistd.h>

static void result(const char *name, long value) {
    printf("%s=%ld\n", name, value < 0 ? -(long)errno : value);
}
int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        result("denied", chdir("/work/l3-private"));
        return 0;
    }
    char cwd[1024], bytes[1024];
    result("chdir", chdir("/work/tree/a"));
    result("cwd", getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/work/tree/a"));
    result("cwd-small", syscall(SYS_getcwd, cwd, 1));
    int fd = open("f.txt", O_RDONLY);
    result("relative", fd >= 0);
    result("setfd", fcntl(fd, F_SETFD, FD_CLOEXEC));
    result("getfd", fcntl(fd, F_GETFD));
    result("directory-file", open("f.txt", O_DIRECTORY));
    result("dents-file", syscall(SYS_getdents64, fd, bytes, sizeof(bytes)));
    close(fd);
    fd = open(".", O_RDONLY | O_DIRECTORY);
    result("dents-small", syscall(SYS_getdents64, fd, bytes, 1));
    result("dents-retry", syscall(SYS_getdents64, fd, bytes, sizeof(bytes)) > 0);
    result("dents-end", syscall(SYS_getdents64, fd, bytes, sizeof(bytes)));
    close(fd);
    result("missing", chdir("missing"));
    result("not-directory", chdir("f.txt"));
    result("umask", umask(0077));
    fd = open("/work/l3-mode", O_CREAT | O_WRONLY | O_TRUNC, 0666);
    struct stat st;
    result("created", fd >= 0);
    result("fstat", fstat(fd, &st));
    result("mode", st.st_mode & 0777);
    close(fd);
    unlink("/work/l3-mode");
    fd = open("/work/l3-transfer", O_CREAT | O_RDWR | O_TRUNC, 0600);
    static char payload[20000];
    memset(payload, 'x', sizeof(payload));
    payload[sizeof(payload) - 1] = '\n';
    result("transfer-file", write(fd, payload, sizeof(payload)));
    lseek(fd, 0, SEEK_SET);
    fflush(stdout);
    long total = 0, moved;
    while ((moved = sendfile(1, fd, NULL, sizeof(payload))) > 0) total += moved;
    result("transfer-total", total);
    result("transfer-eof", moved);
    close(fd);
    unlink("/work/l3-transfer");
    result("mkdir-private", mkdir("/work/l3-private", 0000));
    result("mkdir-existing", mkdir("/work/l3-private", 0700));
    result("system-search", chdir("/work/l3-private"));
    result("root", chdir("/../../.."));
    result("root-cwd", getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/"));
    return 0;
}
