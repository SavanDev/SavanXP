/* <dlfcn.h> as Xash3D FWGS uses it: open by name, look up through the handle's
 * dependency tree, keep RTLD_LOCAL libraries apart, run constructors, name a
 * function for save/restore, and fail a broken library without leaving it
 * behind.
 *
 * None of the libraries is a DEPENDS of this program: they are opened by name at
 * run time, which is the feature under test. */
#include "libc.h"
#include <dlfcn.h>
#include <savanxp/ldso.h>

typedef int (*probe_fn)(void);

/* Exported by the program (PIE, --export-dynamic), for the global-scope and
 * dladdr checks. */
int dltest_marker(void)
{
    return 7;
}

static int fail(const char* what)
{
    eprintf("dltest: %s\n", what);
    return 1;
}

static int contains(const char* text, const char* part)
{
    if (text == 0) {
        return 0;
    }
    const size_t length = strlen(part);
    for (; *text != '\0'; ++text) {
        if (strncmp(text, part, length) == 0) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    const int images_at_start = ldso_count();

    /* A missing file: NULL, a reason that names it, and the error read once. */
    if (dlopen("libdlmod_nothere.so.0.4", RTLD_NOW) != 0) {
        return fail("a library that does not exist opened");
    }
    const char* error = dlerror();
    if (!contains(error, "libdlmod_nothere.so.0.4")) {
        return fail("dlerror does not name the missing file");
    }
    if (dlerror() != 0) {
        return fail("dlerror did not clear after being read");
    }

    /* RTLD_NOLOAD must not load. */
    if (dlopen("libdlmod_base.so.0.4", RTLD_NOW | RTLD_NOLOAD) != 0) {
        return fail("RTLD_NOLOAD loaded a library");
    }

    void* base = dlopen("libdlmod_base.so.0.4", RTLD_NOW);
    void* one = dlopen("/lib/libdlmod_one.so.0.4", RTLD_NOW);
    if (base == 0 || one == 0) {
        eprintf("dltest: %s\n", dlerror());
        return fail("base or one did not open");
    }

    /* dlopen ran base's constructor before returning. */
    probe_fn constructed = (probe_fn)dlsym(base, "dl_base_constructed");
    if (constructed == 0 || constructed() != 1) {
        return fail("dlopen did not run the library's constructor");
    }

    /* Same name, two definitions, two handles: each answers for itself. */
    probe_fn base_probe = (probe_fn)dlsym(base, "dl_probe");
    probe_fn one_probe = (probe_fn)dlsym(one, "dl_probe");
    if (base_probe == 0 || one_probe == 0 || base_probe() != 100 || one_probe() != 1) {
        return fail("dlsym did not keep the two dl_probe definitions apart");
    }

    /* Both are RTLD_LOCAL, so neither is in the global scope. */
    if (dlsym(RTLD_DEFAULT, "dl_probe") != 0) {
        return fail("an RTLD_LOCAL library answered dlsym(RTLD_DEFAULT)");
    }
    (void)dlerror();

    /* two needs base. Loaded after one, it must still bind dl_probe to base:
     * one is newer but not in two's scope. The loader before dlopen searched
     * every image newest first and would have bound it to one. */
    void* two = dlopen("libdlmod_two.so.0.4", RTLD_NOW);
    if (two == 0) {
        eprintf("dltest: %s\n", dlerror());
        return fail("two did not open");
    }
    probe_fn two_probe = (probe_fn)dlsym(two, "dl_two_probe");
    if (two_probe == 0 || two_probe() != 100) {
        return fail("two's call to dl_probe bound to the wrong library");
    }

    /* dlsym walks the handle's dependency tree: two does not define dl_probe,
     * its dependency does. */
    if ((probe_fn)dlsym(two, "dl_probe") != base_probe) {
        return fail("dlsym on two did not reach base through DT_NEEDED");
    }
    /* ...and only that tree. */
    if (dlsym(two, "dl_one_only") != 0) {
        return fail("dlsym on two found a symbol outside its tree");
    }
    (void)dlerror();

    /* The same path twice is the same image, and each open needs its close. */
    if (dlopen("libdlmod_one.so.0.4", RTLD_NOW) != one) {
        return fail("a second dlopen of one gave a different handle");
    }
    if (dlclose(one) != 0 || dlclose(one) != 0) {
        return fail("dlclose of an open handle failed");
    }
    if (dlclose(one) == 0) {
        return fail("dlclose past the last reference succeeded");
    }
    (void)dlerror();

    /* RTLD_GLOBAL on an image already open promotes it. */
    if (dlopen("libdlmod_base.so.0.4", RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL) != base) {
        return fail("RTLD_NOLOAD did not return the open image");
    }
    if ((probe_fn)dlsym(RTLD_DEFAULT, "dl_probe") != base_probe) {
        return fail("RTLD_GLOBAL did not put base in the global scope");
    }

    /* dlopen(NULL) is the program, and the program's exports are global. */
    void* self = dlopen(0, RTLD_NOW);
    if (self == 0 || (probe_fn)dlsym(self, "dltest_marker") != dltest_marker) {
        return fail("dlopen(NULL) did not reach the program's own exports");
    }

    /* dladdr: what save/restore needs, a function pointer back to its name. */
    Dl_info info;
    if (dladdr((const void*)one_probe, &info) == 0 || info.dli_sname == 0 ||
        strcmp(info.dli_sname, "dl_probe") != 0 || info.dli_saddr != (void*)one_probe ||
        !contains(info.dli_fname, "libdlmod_one.so.0.4")) {
        return fail("dladdr did not name one's dl_probe");
    }
    /* An address inside the function, not at its start, is still that function. */
    if (dladdr((const char*)two_probe + 1, &info) == 0 || info.dli_sname == 0 ||
        strcmp(info.dli_sname, "dl_two_probe") != 0) {
        return fail("dladdr did not name a function from inside it");
    }
    if (dladdr((const void*)dltest_marker, &info) == 0 || info.dli_sname == 0 ||
        strcmp(info.dli_sname, "dltest_marker") != 0) {
        return fail("dladdr did not name a function of the program");
    }

    /* A broken library: NULL, the symbol in dlerror, and nothing left behind. */
    const int images_before_broken = ldso_count();
    if (dlopen("libbroken.so.0.4", RTLD_NOW) != 0) {
        return fail("the broken library opened");
    }
    error = dlerror();
    if (!contains(error, "sx_missing_symbol_nobody_defines")) {
        eprintf("dltest: dlerror said \"%s\"\n", error != 0 ? error : "(null)");
        return fail("dlerror does not name the unresolved symbol");
    }
    if (ldso_count() != images_before_broken) {
        return fail("a failed dlopen left an image in the table");
    }
    if (dlsym(RTLD_DEFAULT, "broken_missing") != 0) {
        return fail("a failed dlopen left its symbols visible");
    }
    (void)dlerror();
    if (ldso_missing() != 0) {
        return fail("a failed dlopen changed ldso_missing()");
    }

    /* base, one, two: three images, loaded once each. */
    if (ldso_count() != images_at_start + 3) {
        eprintf("dltest: %d images, expected %d\n", ldso_count(), images_at_start + 3);
        return fail("the image count is wrong");
    }

    puts_fd(1, "DLTEST RESOLVED\n");
    return 0;
}
