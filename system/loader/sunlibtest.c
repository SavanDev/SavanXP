/* A SUN program that maps a shared library: the shared loader's done
 * criterion (docs/kernel/SHARED_LIBRARIES.md, "One loader over SXN").
 *
 * It is linked LINK_PROFILE PIE with DEPENDS libmath, so its runtime carries no
 * math.c and sqrt can only come from /lib/libmath.so.0.4. crt0 runs the loader
 * before main through the same hook a POSIX program uses; nothing here calls
 * it. If the loader still needed POSIX, a native process would reach main with
 * an empty GOT and the first sqrt would jump nowhere.
 *
 * It also depends on libsxgl, SxGL as a library
 * (docs/roadmaps/open/SXGL_VERSIONS_ROADMAP.md, step 0): a SUN program reaches
 * OpenGL by name through sxgl_get_proc_address, from /lib/libsxgl.so.0.4 and
 * not from a copy of its own. The POSIX side, with dlopen, is sdk/glsmoke.
 *
 * Each case is a line of the system log; "SUN LIBRARY TEST PASS" or "SUN
 * LIBRARY TEST FAIL" ends it, and the exit status says the same. */

#include <stdint.h>

#include <savanxp/ldso.h>

#include "math.h"
#include "sxgfx/sxgl.h"
#include "sxn/sxn.h"

static int g_failures = 0;

static void check(int ok, const char* what) {
    if (!ok) {
        g_failures++;
        char line[160] = "sunlibtest: FAILED ";
        unsigned used = 19;
        for (const char* ch = what; *ch != '\0' && used + 1 < sizeof(line); ++ch) {
            line[used++] = *ch;
        }
        line[used] = '\0';
        (void)sxn_log(line);
    }
}

static int marker(void) {
    return 42;
}

/* A pointer in data to a function of this image: lld leaves it as an
 * R_X86_64_RELATIVE, which the loader has to apply for it to point anywhere. */
static int (*const g_marker_pointer)(void) = marker;

int main(void) {
    struct sxn_native_info info;

    /* 1 is the native subsystem (struct sxn_native_info). */
    check(sxn_info(&info) == 0 && info.subsystem_id == 1, "not running as a native process");
    check(ldso_missing() == 0, "a dependency did not load");
    /* The program and libmath, at least. */
    check(ldso_count() >= 2, "no library in the table");

    check(g_marker_pointer == marker && g_marker_pointer() == 42,
          "a RELATIVE relocation was not applied");

    const double root = sqrt(144.0);
    check(root > 11.999 && root < 12.001, "sqrt(144) through libmath is not 12");
    /* From the library, not from a copy in the executable: the build would
     * pass with a private math.c too, and nothing would be shared. */
    check(ldso_symbol_is_shared("sqrt"), "sqrt did not come from a shared library");
    /* Where the loader says sqrt is, the GOT went: otherwise 12 was luck. */
    check((const void*)ldso_lookup("sqrt") == (const void*)sqrt, "the loader and the GOT disagree on sqrt");

    check(ldso_symbol_is_shared("sxgl_get_proc_address"), "sxgl_get_proc_address did not come from libsxgl");
    check(ldso_symbol_is_shared("glClear"), "glClear did not come from a shared library");
    check(sxgl_get_proc_address("glClear") == (sxgl_proc)ldso_lookup("glClear"),
          "sxgl_get_proc_address and the loader disagree on glClear");
    check(sxgl_get_proc_address("glPointSize") == 0, "glPointSize resolves, and SxGL does not implement it");

    (void)sxn_log(g_failures == 0 ? "SUN LIBRARY TEST PASS" : "SUN LIBRARY TEST FAIL");
    return g_failures == 0 ? 0 : 1;
}
