/* A position-independent program built outside the tree against the SDK.
 *
 *     ./tools/build-user.sh --source sdk/piesmoke --name piesmoke \
 *         --pie --needs libmath.so.0.4
 *
 * --pie links the loader into the program and --needs libmath.so.0.4 leaves
 * the SDK's own libm out of it, so sqrt and cos are undefined in the image
 * and the loader resolves them from /lib/libmath.so.0.4 (a DT_NEEDED). The
 * program also maps /lib/libsximg.so.0.4 itself, with dlopen and dlsym.
 *
 * Run headless from init: ./build.sh smoke pie-smoke. init prints
 * "PIE SMOKE PASS" when it exits with 0.
 */

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>

static int g_failures = 0;

static void report(const char* what, int ok) {
    printf("PIESMOKE: %s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        g_failures += 1;
    }
}

/* volatile, so the compiler cannot fold the calls into constants and the
 * program really calls into the library. */
static volatile double g_two = 2.0;
static volatile double g_zero = 0.0;

int main(void) {
    double root = sqrt(g_two);
    double one = cos(g_zero);
    void* image = 0;
    const char* (*error_string)(int) = 0;

    report("sqrt(2) from libmath.so.0.4", root > 1.41421 && root < 1.41422);
    report("cos(0) from libmath.so.0.4", one > 0.99999 && one < 1.00001);

    image = dlopen("libsximg.so.0.4", RTLD_NOW);
    report("dlopen libsximg.so.0.4", image != 0);
    if (image != 0) {
        error_string = (const char* (*)(int))dlsym(image, "sx_image_error_string");
        report("dlsym sx_image_error_string", error_string != 0);
        if (error_string != 0) {
            const char* text = error_string(0);
            report("call into libsximg.so.0.4", text != 0);
        }
    } else {
        printf("PIESMOKE: dlerror: %s\n", dlerror());
    }

    if (g_failures == 0) {
        printf("PIESMOKE: all ok\n");
        return 0;
    }
    printf("PIESMOKE: %d failures\n", g_failures);
    return 1;
}
