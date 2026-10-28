/* Run-time loading of shared libraries by name.
 *
 * Implemented by the loader (ldso.c), which is compiled into every PIE program.
 * A STATIC program has no loader, so using these is a link error there rather
 * than a call that silently does nothing.
 *
 * What differs from glibc, on purpose (docs/kernel/SHARED_LIBRARIES.md#dlopen):
 *
 * - Binding is always immediate: RTLD_LAZY behaves as RTLD_NOW.
 * - dlclose never unmaps. It drops the reference and returns 0; a later dlopen
 *   of the same path returns the same image with its data as it was left. musl
 *   behaves the same way.
 * - A name without a '/' is looked up in /lib, the same place DT_NEEDED is.
 * - dlsym(RTLD_DEFAULT, ...) and dlsym(dlopen(NULL, ...), ...) search the global
 *   scope newest library first, executable last: the order the startup loader
 *   has always used. */
#ifndef SAVANXP_DLFCN_H
#define SAVANXP_DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY 0x0001
#define RTLD_NOW 0x0002
#define RTLD_NOLOAD 0x0004
#define RTLD_GLOBAL 0x0100
#define RTLD_LOCAL 0x0000
#define RTLD_NODELETE 0x1000

#define RTLD_DEFAULT ((void*)0)

typedef struct {
    const char* dli_fname; /* path the image was loaded from; "" for the program */
    void* dli_fbase;       /* address the image starts at */
    const char* dli_sname; /* nearest symbol at or below the address, or 0 */
    void* dli_saddr;       /* that symbol's address, or 0 */
} Dl_info;

void* dlopen(const char* path, int mode);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);
char* dlerror(void);
int dladdr(const void* address, Dl_info* info);

#ifdef __cplusplus
}
#endif

#endif
