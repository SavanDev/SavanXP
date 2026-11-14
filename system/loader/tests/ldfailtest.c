/* The loader's failure paths (docs/kernel/SHARED_LIBRARIES.md, "The loader's
 * failure paths"): ldso_load on a file made wrong in exactly one way, for each
 * step that checks one. tools/make_loader_fixtures.py writes them to
 * /system/loader-fixtures.
 *
 * Each must fail at its own step, consume no slot -- except -8, where the
 * library itself loaded and only its dependency did not -- and leave a line
 * in the system log saying why. One more case is the opposite: a weak
 * reference nothing defines must not fail a load. "LOADER FAIL TEST PASS" or "LOADER FAIL TEST
 * FAIL" ends it, and the exit status says the same. */

#include <savanxp/ldso.h>

#include "loglines.h"
#include "sxn/sxn.h"

static int g_failures = 0;

static void check(int ok, const char* what) {
    if (!ok) {
        g_failures++;
        char line[160] = "ldfailtest: FAILED ";
        unsigned used = 19;
        for (const char* ch = what; *ch != '\0' && used + 1 < sizeof(line); ++ch) {
            line[used++] = *ch;
        }
        line[used] = '\0';
        (void)sxn_log(line);
    }
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
    static const struct {
        const char* path;
        int step;
        const char* logged;
        const char* what;
    } cases[] = {
        {"/system/loader-fixtures/empty.so", -3,
         "loader: cannot map /system/loader-fixtures/empty.so as an executable section (step 3)",
         "an empty file is not step -3"},
        {"/system/loader-fixtures/short.so", -4, 0, "a file shorter than a header is not step -4"},
        {"/system/loader-fixtures/notelf.so", -5,
         "loader: /system/loader-fixtures/notelf.so does not start with the ELF magic",
         "a file without the ELF magic is not step -5"},
        {"/system/loader-fixtures/exec.so", -5,
         "loader: /system/loader-fixtures/exec.so is an ELF that cannot be a library",
         "an executable's ELF is not step -5"},
        {"/system/loader-fixtures/badsegment.so", -6, 0, "a segment past the file's end is not step -6"},
        {"/system/loader-fixtures/nodynamic.so", -7, 0, "a library with no PT_DYNAMIC is not step -7"},
        {"/system/loader-fixtures/needsgone.so", -8,
         "loader: cannot open /lib/libldfail_gone.so.0.4, and /lib has no newer minor of it (step 2)",
         "a library whose dependency is missing is not step -8"},
    };

    /* Not a failure: a weak reference nothing defines is zero, and the
     * library loads. It goes first: after -8 a dependency is missing, the
     * loader relocates tolerantly, and an unresolved symbol would read zero
     * whether or not weak references are honoured. */
    {
        const int before = ldso_count();
        check(ldso_missing() == 0, "a dependency was missing before the weak case");
        check(ldso_load("/lib/libweakref.so.0.4") == 0, "a weak reference nothing defines failed the load");
        int (*probe)(void) = (int (*)(void))ldso_lookup("weakref_probe");
        check(ldso_count() == before + 1 && probe != 0 && probe() == 1,
              "the weak reference did not resolve to zero");
    }

    for (unsigned index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const int before = ldso_count();
        const int step = ldso_load(cases[index].path);
        check(step == cases[index].step, cases[index].what);
        /* A failed load gives its slot back; at -8 the library itself loaded
         * and stays, as at startup, where the program goes on without its
         * dependency. */
        check(ldso_count() == before + (cases[index].step == -8 ? 1 : 0),
              "a failed load kept a slot, or -8 did not keep its library");
        if (cases[index].logged != 0) {
            check(log_count(cases[index].logged) == 1, "the failure's reason is not in the system log");
        }
    }
    /* The dependency that was missing is named, as for a program's own. */
    check(same(ldso_missing(), "libldfail_gone.so.0.4"), "ldso_missing does not name the missing dependency");

    (void)sxn_log(g_failures == 0 ? "LOADER FAIL TEST PASS" : "LOADER FAIL TEST FAIL");
    return g_failures == 0 ? 0 : 1;
}
