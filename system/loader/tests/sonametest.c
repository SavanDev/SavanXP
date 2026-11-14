/* How the loader resolves a DT_NEEDED (docs/kernel/SHARED_LIBRARIES.md,
 * "DT_SONAME resolution"): the exact file when it is there, and otherwise the
 * highest newer minor of the same ABI, never an older one.
 *
 * The program needs libsoname.so.0.4, libsonameuser.so.0.4 (which needs
 * libsoname.so.0.4 too) and libsonamelow.so.0.4. /lib also holds
 * libsoname.so.0.3, 0.5 and 0.6, and libsonamelow.so.0.3. The sonametest
 * scenario runs it on the full image; sonametest-newer removes both 0.4 files
 * first. The program looks at /lib itself to know which of the two it is in.
 *
 * Each case is a line of the system log; "SONAME TEST PASS" or "SONAME TEST
 * FAIL" ends it, and the exit status says the same. */

#include <savanxp/ldso.h>

#include "sxn/sxn.h"

int soname_version(void);
int soname_user_version(void);
int soname_low_version(void);

static int g_failures = 0;

static void check(int ok, const char* what) {
    if (!ok) {
        g_failures++;
        char line[160] = "sonametest: FAILED ";
        unsigned used = 19;
        for (const char* ch = what; *ch != '\0' && used + 1 < sizeof(line); ++ch) {
            line[used++] = *ch;
        }
        line[used] = '\0';
        (void)sxn_log(line);
    }
}

static int exists(const char* path) {
    const long handle = sxn_open_file(path, 0);
    if (handle < 0) {
        return 0;
    }
    (void)sxn_close(handle);
    return 1;
}

static int same(const char* a, const char* b) {
    if (a == 0 || b == 0) {
        return a == b;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(void) {
    const int exact = exists("/lib/libsoname.so.0.4");
    const int low_exact = exists("/lib/libsonamelow.so.0.4");

    (void)sxn_log(exact ? "sonametest: exact files present" : "sonametest: exact files removed");
    check(exact == low_exact, "the scenario removed one 0.4 file and not the other");

    /* The exact file wins over newer ones; without it, the highest newer
     * minor -- 0.6, not 0.5, and never 0.3. */
    check(soname_version() == (exact ? 4 : 6),
          exact ? "the exact 0.4 did not win over 0.5 and 0.6" : "0.4 missing did not resolve to 0.6");
    /* libsonameuser needs 0.4 as well: it binds to the same image. */
    check(soname_user_version() == soname_version(), "libsonameuser bound to another libsoname");

    if (low_exact) {
        check(ldso_missing() == 0, "a dependency did not load with every file present");
        check(soname_low_version() == 4, "libsonamelow is not the 0.4");
        /* The program, libsoname, libsonameuser, libsonamelow: no image twice. */
        check(ldso_count() == 4, "an image was loaded twice, or one is missing");
    } else {
        /* Only 0.3 is there, which may lack what a 0.4 program calls. */
        check(same(ldso_missing(), "libsonamelow.so.0.4"), "libsonamelow.so.0.4 was not named as missing");
        check(ldso_lookup("soname_low_version") == 0, "an older libsonamelow stood in for 0.4");
        check(ldso_count() == 3, "an image was loaded twice, or an older one taken");
    }

    (void)sxn_log(g_failures == 0 ? "SONAME TEST PASS" : "SONAME TEST FAIL");
    return g_failures == 0 ? 0 : 1;
}
