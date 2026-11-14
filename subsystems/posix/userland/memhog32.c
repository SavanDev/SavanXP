#include "libc.h"

// A sparse-on-disk ELF with enough resident BSS to force partial-load failure
// on the 32 MiB regression machine. The normal process table still bounds it.
static volatile unsigned char g_memory[12 * 1024 * 1024];

int main(int argc, char** argv) {
    for (unsigned int index = 0; index < sizeof(g_memory); index += 4096)
        if (g_memory[index] != 0) return 1;
    g_memory[0] = 19;
    g_memory[sizeof(g_memory) - 1] = 37;
    if (argc == 2 && strcmp(argv[1], "crosspage") == 0) {
        // Crossing a 4 MiB boundary crosses page tables: the new table occupies
        // a physical frame between the two user pages. An alias memcpy corrupts
        // that table; page-by-page result copying preserves both mappings.
        unsigned int boundary = ((unsigned int)g_memory + 0x400000) & ~0x3fffffu;
        int* status = (int*)(boundary - 2);
        const char* args[] = {"/disk/bin/multitest32", "short"};
        for (int attempt = 0; attempt < 2; ++attempt) {
            long pid = spawn(args[0], args, 2);
            if (pid < 0) return 3;
            if (attempt == 0) sleep_ms(50);
            if (savanxp_waitpid((int)pid, status) != pid || *status != 23) return 4;
        }
    } else {
        sleep_ms(5000);
    }
    return g_memory[0] == 19 && g_memory[sizeof(g_memory) - 1] == 37 ? 43 : 2;
}
