/* Step -11 (docs/kernel/SHARED_LIBRARIES.md, "The loader's failure paths"): a
 * program linked STATIC -- an ET_EXEC, with no dynamic symbol table -- that
 * links the loader anyway. crt0 runs the loader before main; it finds nothing
 * to resolve against and has to say so in words, because such a program starts
 * and runs and finds none of its libraries.
 *
 * This program checks that its own line is in the system log. "LOADER STATIC
 * TEST PASS" or "LOADER STATIC TEST FAIL" ends it. */

#include "loglines.h"
#include "sxn/sxn.h"

int main(void) {
    const int said = log_count("loader: the program has no dynamic symbol table; link it LINK_PROFILE PIE") >= 1 &&
                     log_count("sx_start_dynamic: the interpreter failed (step 11)") >= 1;
    if (!said) {
        (void)sxn_log("ldstatictest: FAILED the non-PIE program's loader did not say why it cannot work");
    }
    (void)sxn_log(said ? "LOADER STATIC TEST PASS" : "LOADER STATIC TEST FAIL");
    return said ? 0 : 1;
}
