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
 * Last, it opens libraries at run time through sxn/sxnlibrary.h, SUN's
 * counterpart of <dlfcn.h>: dltest's libdlmod_* for the scopes, and
 * libsxmidi, which asks the program for calloc and realloc that no library
 * on its link line asks for, so it loads only because DLOPEN exports the
 * runtime.
 *
 * Each case is a line of the system log; "SUN LIBRARY TEST PASS" or "SUN
 * LIBRARY TEST FAIL" ends it, and the exit status says the same. */

#include <stdint.h>

#include <savanxp/ldso.h>

#include "math.h"
#include "sxgfx/sxgl.h"
#include "sxn/sxn.h"
#include "sxn/sxnlibrary.h"

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

static int text_contains(const char* text, const char* part) {
    for (; text != 0 && *text != '\0'; ++text) {
        const char* a = text;
        const char* b = part;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return 1;
        }
    }
    return 0;
}

typedef int (*probe_fn)(void);

/* sxn/sxnlibrary.h, case by case, after dltest's (subsystems/posix/userland/dltest.c). */
static void check_runtime_loading(void) {
    char error[160];
    struct sxn_library* base = 0;
    struct sxn_library* one = 0;
    struct sxn_library* two = 0;
    struct sxn_library* again = 0;
    struct sxn_library* midi = 0;
    void* address = 0;

    check(sxn_library_open("libdlmod_nothere.so.0.4", 0, &again, error, sizeof(error)) == -SX_ENOEXEC &&
              again == 0,
          "opening a missing library did not fail with -SX_ENOEXEC");
    check(text_contains(error, "libdlmod_nothere"), "the error does not name the missing library");
    check(sxn_library_open("lib/libdlmod_one.so.0.4", 0, &again, error, sizeof(error)) == -SX_EINVAL,
          "a relative path was not refused");
    check(sxn_library_open("libdlmod_base.so.0.4", SXN_LIBRARY_NOLOAD, &again, 0, 0) == -SX_ENOENT,
          "NOLOAD of a library not loaded did not answer -SX_ENOENT");
    check(sxn_library_open("libdlmod_base.so.0.4", 0x80u, &again, 0, 0) == -SX_EINVAL,
          "an unknown flag was not refused");

    check(sxn_library_open("libdlmod_base.so.0.4", 0, &base, error, sizeof(error)) == 0 && base != 0,
          "libdlmod_base did not open by name");
    check(sxn_library_open("/lib/libdlmod_one.so.0.4", 0, &one, error, sizeof(error)) == 0 && one != 0,
          "libdlmod_one did not open by absolute path");
    if (base == 0 || one == 0) {
        return;
    }

    probe_fn constructed = 0;
    probe_fn base_probe = 0;
    probe_fn one_probe = 0;
    check(sxn_library_symbol(base, "dl_base_constructed", (void**)&constructed) == 0 && constructed != 0 &&
              constructed() == 1,
          "opening base did not run its constructor");
    check(sxn_library_symbol(base, "dl_probe", (void**)&base_probe) == 0 &&
              sxn_library_symbol(one, "dl_probe", (void**)&one_probe) == 0 && base_probe != 0 &&
              one_probe != 0 && base_probe() == 100 && one_probe() == 1,
          "the two dl_probe definitions were not kept apart");
    check(sxn_library_symbol(0, "dl_probe", &address) == -SX_ENOENT,
          "a library opened without GLOBAL answered the global scope");

    check(sxn_library_open("libdlmod_two.so.0.4", 0, &two, error, sizeof(error)) == 0 && two != 0,
          "libdlmod_two did not open");
    if (two != 0) {
        probe_fn two_probe = 0;
        probe_fn through = 0;
        check(sxn_library_symbol(two, "dl_two_probe", (void**)&two_probe) == 0 && two_probe != 0 &&
                  two_probe() == 100,
              "two did not bind to base, its DT_NEEDED, over the newer one");
        check(sxn_library_symbol(two, "dl_probe", (void**)&through) == 0 && through == base_probe,
              "a symbol lookup on two did not reach base through its tree");
        check(sxn_library_symbol(two, "dl_one_only", &address) == -SX_ENOENT,
              "a symbol lookup on two found one outside its tree");
    }

    check(sxn_library_open("libdlmod_one.so.0.4", 0, &again, 0, 0) == 0 && again == one,
          "a second open of one gave another image");
    check(sxn_library_close(again) == 0, "closing the second reference failed");
    check(sxn_library_open("libdlmod_base.so.0.4", SXN_LIBRARY_NOLOAD | SXN_LIBRARY_GLOBAL, &again, 0, 0) == 0 &&
              again == base,
          "NOLOAD | GLOBAL did not return base");
    check(sxn_library_symbol(0, "dl_probe", &address) == 0 && address == (void*)base_probe,
          "base made global did not answer the global scope");
    check(sxn_library_symbol((struct sxn_library*)(uintptr_t)0x1000u, "dl_probe", &address) == -SX_EBADF,
          "a handle the loader never gave out was accepted");

    struct sxn_library_address_info info;
    check(sxn_library_address((const void*)base_probe, &info) == 0 && text_contains(info.path, "libdlmod_base") &&
              info.symbol != 0 && text_contains(info.symbol, "dl_probe") && info.symbol_address == (void*)base_probe,
          "sxn_library_address did not name base and dl_probe");
    check(sxn_library_address((const void*)marker, &info) == 0 && info.path != 0,
          "sxn_library_address did not place the program's own code");

    /* calloc and realloc come from the program, exported by DLOPEN. */
    check(sxn_library_open("libsxmidi.so.0.4", 0, &midi, error, sizeof(error)) == 0 && midi != 0,
          "libsxmidi did not load: the program did not export its runtime");
    if (midi == 0) {
        (void)sxn_log(error);
    }

    check(sxn_library_close(two) == 0 && sxn_library_close(one) == 0, "closing a library failed");
    check(sxn_library_close(0) == -SX_EBADF, "closing no library was accepted");
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

    check_runtime_loading();

    (void)sxn_log(g_failures == 0 ? "SUN LIBRARY TEST PASS" : "SUN LIBRARY TEST FAIL");
    return g_failures == 0 ? 0 : 1;
}
