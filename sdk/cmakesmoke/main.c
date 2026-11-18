/* A program built with CMake and the SDK's toolchain file (sdk/cmakesmoke).
 *
 * Its build goes through sxcc, which links crt0, the runtime archive and the
 * linker script on its own; libm comes from -lm. Run headless from init:
 * ./build.sh smoke cmake-smoke. init prints "CMAKE SMOKE PASS" when it exits
 * with 0.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void report(const char* what, int ok) {
    printf("CMAKESMOKE: %s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        g_failures += 1;
    }
}

/* volatile, so the compiler calls libm instead of folding the result. */
static volatile double g_two = 2.0;

int main(int argc, char** argv) {
    char text[32];
    char* copy = 0;
    double root = sqrt(g_two);

    report("argv[0]", argc >= 1 && argv[0] != 0);
    report("sqrt(2) from -lm", root > 1.41421 && root < 1.41422);
    snprintf(text, sizeof(text), "%.3f", root);
    report("printf %.3f", strcmp(text, "1.414") == 0);
    copy = malloc(64);
    report("malloc", copy != 0);
    if (copy != 0) {
        strcpy(copy, "built with CMake");
        report("heap write", strcmp(copy, "built with CMake") == 0);
        free(copy);
    }

    if (g_failures == 0) {
        printf("CMAKESMOKE: all ok\n");
        return 0;
    }
    printf("CMAKESMOKE: %d failures\n", g_failures);
    return 1;
}
