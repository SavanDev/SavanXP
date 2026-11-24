/* Launch the client in a window and expose its console to QEMU's serial log. */
#include <fcntl.h>
#include <unistd.h>

int main(void)
{
    int serial = open("/dev/serial", O_WRONLY);
    if (serial < 0 || dup2(serial, 1) < 0 || dup2(serial, 2) < 0)
        return 1;
    close(serial);
    char *argv[] = {
        "/bin/xash3d-client", "-basedir", "/games/xash3d-client-smoke",
        "-windowed", "-width", "640", "-height", "480", "-dev", "2",
        "+sv_lan", "1", "+map", "c0a0", 0,
    };
    return execv(argv[0], argv);
}
