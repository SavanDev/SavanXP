/* L5's real Unix-stream contract, unchanged on Linux and SSL. The native
 * parent and the host fixture server send the same eight bytes, then wait
 * for write shutdown before sending the final byte and orderly EOF. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { printf("transport FAIL %d errno=%d\n", __LINE__, errno); return 1; } } while (0)
int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/tmp/.X11-unix/X1";
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    CHECK(strlen(path) < sizeof(address.sun_path)); strcpy(address.sun_path, path);
    const socklen_t address_size = offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); CHECK(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    CHECK((fcntl(fd, F_GETFL) & (O_ACCMODE | O_NONBLOCK)) == (O_RDWR | O_NONBLOCK));
    CHECK(connect(fd, (struct sockaddr*)&address, address_size) == 0);
    struct sockaddr_un peer = {0}; socklen_t size = sizeof(peer);
    CHECK(getpeername(fd, (struct sockaddr*)&peer, &size) == 0 && size == address_size);
    CHECK(peer.sun_family == AF_UNIX && !strcmp(peer.sun_path, path));
    size = 2; memset(&peer, 0, sizeof(peer));
    CHECK(getpeername(fd, (struct sockaddr*)&peer, &size) == 0 && size == address_size && peer.sun_family == AF_UNIX);
    int option = 0; size = sizeof(option);
    CHECK(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &option, &size) == 0 && option > 0 && size == 4);
    CHECK(getsockopt(fd, SOL_SOCKET, SO_TYPE, &option, &size) == 0 && option == SOCK_STREAM);
    CHECK(getsockopt(fd, SOL_SOCKET, SO_ERROR, &option, &size) == 0 && option == 0);
    struct stat status; CHECK(fstat(fd, &status) == 0 && S_ISSOCK(status.st_mode));
    CHECK(lseek(fd, 0, SEEK_SET) == -1 && errno == ESPIPE);
    char bytes[16] = {0}; CHECK(recv(fd, bytes, sizeof(bytes), 0) == -1 && errno == EAGAIN);
    CHECK(write(fd, "N", 1) == 1);
    struct pollfd input = {fd, POLLIN, 0}; CHECK(poll(&input, 1, 1000) == 1 && (input.revents & POLLIN));
    struct iovec bad = {(void*)1, 4};
    struct msghdr message = {.msg_iov = &bad, .msg_iovlen = 1};
    CHECK(recvmsg(fd, &message, 0) == -1 && errno == EFAULT);
    struct iovec vectors[3] = {{bytes, 2}, {0, 0}, {bytes + 2, 6}};
    message.msg_iov = vectors; message.msg_iovlen = 3;
    CHECK(recvmsg(fd, &message, 0) == 8 && !memcmp(bytes, "abcdEFGH", 8));
    CHECK(message.msg_flags == 0 && message.msg_namelen == 0 && message.msg_controllen == 0);
    CHECK(recv(fd, bytes, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    struct pollfd invalid[2] = {{255, POLLIN, 0}, {-1, POLLIN | POLLOUT, 0}};
    CHECK(poll(invalid, 2, 0) == 1 && invalid[0].revents == POLLNVAL && invalid[1].revents == 0);
    CHECK(poll(&input, 1, 20) == 0 && input.revents == 0);
    CHECK(fcntl(fd, F_SETFL, O_RDWR) == 0 && !(fcntl(fd, F_GETFL) & O_NONBLOCK));
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    CHECK(shutdown(fd, SHUT_WR) == 0);
    CHECK(recv(fd, bytes, 1, 0) == 1 && bytes[0] == 'Z');
    CHECK(recv(fd, bytes, 1, 0) == 0);
    CHECK(poll(&input, 1, 0) == 1 && (input.revents & POLLIN));
    CHECK(shutdown(fd, SHUT_RD) == 0 && read(fd, bytes, 1) == 0);
    CHECK(close(fd) == 0 && close(fd) == -1 && errno == EBADF);
    struct utsname name; CHECK(uname(&name) == 0 && !strcmp(name.sysname, "Linux") && !strcmp(name.machine, "x86_64"));
    puts("transport ok"); return 0;
}
