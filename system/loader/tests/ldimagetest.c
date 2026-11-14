/* Step -10 (docs/kernel/SHARED_LIBRARIES.md, "The loader's failure paths"): the
 * loader cannot register the program's own image. This program is built with
 * SAVANXP_LD_MAX_PROGRAM_HEADERS=1, and every PIE has more than one program
 * header, so adopt_executable refuses the image crt0 hands it and ldso_start
 * returns -10. The program still reaches main -- the loader is never fatal --
 * and checks that its own line is in the system log.
 *
 * "LOADER IMAGE TEST PASS" or "LOADER IMAGE TEST FAIL" ends it. */

#include <savanxp/ldso.h>

#include "loglines.h"
#include "sxn/sxn.h"

int main(void) {
    const int said = log_count("loader: the program's own image could not be registered (step 10)") >= 1 &&
                     log_count("sx_start_dynamic: the interpreter failed (step 10)") >= 1;
    /* Nothing was registered: not even the program counts as an image. */
    const int empty = ldso_count() == 0;
    if (!said) {
        (void)sxn_log("ldimagetest: FAILED step -10 is not in the system log");
    }
    if (!empty) {
        (void)sxn_log("ldimagetest: FAILED an image was registered");
    }
    (void)sxn_log(said && empty ? "LOADER IMAGE TEST PASS" : "LOADER IMAGE TEST FAIL");
    return said && empty ? 0 : 1;
}
