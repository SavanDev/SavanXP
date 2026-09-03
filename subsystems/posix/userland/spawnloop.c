#include "libc.h"

int main(void) {
    const char* argv[] = {"/bin/true", 0};

    for (int index = 0; index < 16; ++index) {
        int status = 0;
        long pid = spawn("/bin/true", argv, 1);
        if (pid < 0) {
            puts_out("spawnloop: spawn failed\n");
            return 1;
        }
        if (savanxp_waitpid((int)pid, &status) < 0 || status != 0) {
            puts_out("spawnloop: wait failed\n");
            return 1;
        }
    }

    puts_out("spawnloop: ok\n");
    return 0;
}
