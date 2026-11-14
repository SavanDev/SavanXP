#include "libc.h"

// Process policy belongs to userland. The shell can launch and wait for any
// supported ELF32; exiting it returns here rather than selecting a kernel stage.
static int launch(const char* path) {
    const char* arguments[] = {path};
    long pid = spawn(path, arguments, 1);
    int status = 0;
    if (pid < 0 || savanxp_waitpid((int)pid, &status) != pid) {
        puts_out("init32: launch/wait failed\n");
        return 1;
    }
    return status;
}

int main(void) {
    puts_out("init32: ready\n");
    launch("/disk/bin/sysinfo");
    for (;;) {
        launch("/disk/bin/sh32");
        sleep_ms(100);
    }
}
