#pragma once

/* sxnlibrary: opening shared libraries at run time, for a SUN program
 * (docs/kernel/SHARED_LIBRARIES.md, "Run-time loading in SUN").
 *
 * SUN's layer over the shared loader's interface (include/loader/loader.h),
 * as <dlfcn.h> is POSIX's: the same loader, the same scopes, the same
 * rollback of a failed load. What is SUN's is the shape. Every call returns 0
 * or a negative status, and the reason a load failed goes into a buffer the
 * caller passes rather than into a message held for later. A name is a bare
 * library name, resolved in /lib as a DT_NEEDED is, or an absolute path: SXN
 * has no working directory to resolve a relative one against.
 *
 * Only a PIE program has the loader, so only a PIE program has these calls.
 * A library opened at run time asks the program for its C library, and a SUN
 * PIE exports nothing it was not linked against, so a program that uses them
 * is linked to export its runtime: DLOPEN in savanxp_program, --export-dynamic
 * with tools/build-user.sh. Without it a load that needs one of those symbols
 * fails and names it.
 */

#include <stdint.h>

#include "abi/savanxp_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An image the loader has mapped: a library, its dependencies with it. */
struct sxn_library;

/* sxn_library_open flags. GLOBAL puts the library and its whole tree in the
 * global scope, the one sxn_library_symbol(0, ...) and every later image
 * search; without it the library answers only for itself and for images whose
 * tree contains it. NOLOAD returns a library already loaded and loads none
 * (with GLOBAL, it makes that library global). */
#define SXN_LIBRARY_GLOBAL 0x1u
#define SXN_LIBRARY_NOLOAD 0x2u

/* Opens a library and the libraries it needs, binds it and runs its
 * constructors, dependencies first. A library already loaded is returned
 * again with one more reference. 0 and *out; or -SX_EINVAL (no name, a
 * relative path, an unknown flag), -SX_ENAMETOOLONG, -SX_ENOENT (NOLOAD and
 * not loaded), -SX_ENOEXEC (the load failed: a missing file, library or
 * symbol, which `error` names when it is not 0). On failure nothing the call
 * loaded stays mapped. */
long sxn_library_open(const char* name, unsigned flags, struct sxn_library** out, char* error,
                      unsigned long error_size);

/* A symbol's address. With a library: the library, then its dependency tree
 * in load order. With 0: the global scope, newest image first and the program
 * last. 0 and *address; -SX_EINVAL, -SX_EBADF (not a library this loader
 * opened), -SX_ENOENT (not found). */
long sxn_library_symbol(struct sxn_library* library, const char* name, void** address);

/* One reference less. Nothing is ever unmapped, so a later open of the same
 * name returns the same image with its data as it was left. 0, or -SX_EBADF
 * when it is not a library this loader opened or it has no reference left. */
long sxn_library_close(struct sxn_library* library);

/* Which image holds an address. */
struct sxn_library_address_info {
    const char* path; /* the image's path; the program's for its own code */
    void* base; /* where it is loaded */
    const char* symbol; /* nearest function or object at or below, or 0 */
    void* symbol_address;
};

/* 0 and *info when an image the loader knows holds the address; -SX_EINVAL,
 * or -SX_ENOENT when none does. */
long sxn_library_address(const void* address, struct sxn_library_address_info* info);

#ifdef __cplusplus
}
#endif
