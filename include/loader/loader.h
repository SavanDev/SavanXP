/* The shared-library loader's interface for subsystem layers.
 *
 * The loader itself (system/loader/ldso.c) is platform code: it speaks SXN,
 * calls no C library, and is compiled into every program that maps a library,
 * SUN or POSIX alike (docs/kernel/SHARED_LIBRARIES.md, "One loader over SXN").
 * What a program sees of it at startup -- ldso_start, ldso_missing and the rest
 * -- is <savanxp/ldso.h>. This header is the other side: what a subsystem builds
 * its own run-time loading interface on. POSIX builds <dlfcn.h> on it
 * (subsystems/posix/userland/dlfcn.c), SUN builds sxn/sxnlibrary.h
 * (system/runtime/sxnlibrary.c).
 *
 * A handle is an image: the program, or a library. Paths are absolute -- the
 * loader opens files through SXN, which knows no working directory; turning a
 * relative path into an absolute one is the subsystem's business. */
#ifndef SAVANXP_LOADER_H
#define SAVANXP_LOADER_H

/* The /lib path a bare library name resolves to, by the rule a DT_NEEDED
 * follows (docs/kernel/SHARED_LIBRARIES.md, "DT_SONAME resolution"): a full
 * version (libfoo.so.0.4) is the exact file, or its highest newer minor when
 * that is missing; a name without one (libfoo.so, libfoo.so.0) is the latest
 * version /lib has, or the name itself when it has none. Writes the path and
 * returns 1 when another version was chosen, 0 when it is the name as given. */
int sx_loader_resolve(const char* name, char* path, unsigned long size);

/* The program's own image, which stands for the global scope, or 0 when it
 * could not be registered. */
void* sx_loader_program(void);

/* The image loaded from this absolute path, or 0. Identity is the path string:
 * two spellings of one file are two images. */
void* sx_loader_find(const char* path);

/* Loads an absolute path and the libraries it needs, binds it, and runs its
 * constructors, dependencies first. `global` puts the image and its whole tree
 * in the global scope; otherwise it answers only for itself and for images
 * whose tree contains it. Strict: a symbol that does not resolve fails the
 * load. On failure nothing the call loaded stays mapped, and `error` (when not
 * 0) holds why, naming the file and the missing library or symbol. Returns the
 * image with one reference, or 0. */
void* sx_loader_open(const char* path, int global, char* error, unsigned long error_size);

/* Puts an image and its whole tree in the global scope. */
void sx_loader_make_global(void* image);

/* A reference more on an image from sx_loader_program, _find or _open. */
void sx_loader_retain(void* image);

/* A reference less. 0, or -1 when the image is not one this loader gave out or
 * has no reference left. Nothing is ever unmapped. */
int sx_loader_release(void* image);

/* A symbol's address. Image 0 or the program: the global scope, newest image
 * first and the program last. A library: itself, then its dependency tree in
 * load order. 0 found, -1 not an image, -2 not found. */
int sx_loader_symbol(void* image, const char* name, void** address);

/* Which image holds an address, and the nearest defined function or object at
 * or below it (0 when the image has no symbol count to scan safely). 1 when an
 * image holds it, 0 otherwise. */
int sx_loader_address(const void* address, const char** path, void** base,
                      const char** symbol, void** symbol_address);

#endif
