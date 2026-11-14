/* <dlfcn.h>: POSIX's run-time loading, over the shared loader's interface
 * (loader/loader.h). The loader is platform code and knows nothing of POSIX;
 * what is POSIX lives here: the RTLD_* modes, dlerror's one-shot message, and
 * the working directory, since the loader opens absolute paths only.
 *
 * It was built for Xash3D FWGS, whose engine loads its renderer, menu, client
 * and game logic by name; what it deliberately does not do and why is in
 * docs/kernel/SHARED_LIBRARIES.md#dlopen. Compiled into every POSIX PIE
 * program, next to the loader, and into no STATIC one -- where a call is a link
 * error rather than a silent no-op -- and into no SUN program, which names its
 * own. */

#include <dlfcn.h>
#include <stddef.h>

#include <savanxp/libc.h>

#include "loader/loader.h"

/* The last failure, until dlerror() hands it out once. */
static char g_dl_error[256];
static int g_dl_error_pending = 0;

static void dl_error_append(size_t* used, const char* text) {
    while (text != 0 && *text != '\0' && *used < sizeof(g_dl_error) - 1) {
        g_dl_error[(*used)++] = *text++;
    }
    g_dl_error[*used] = '\0';
}

static void dl_fail(const char* first, const char* second, const char* third) {
    size_t used = 0;
    dl_error_append(&used, first);
    dl_error_append(&used, second);
    dl_error_append(&used, third);
    g_dl_error_pending = 1;
}

char* dlerror(void) {
    if (!g_dl_error_pending) {
        return 0;
    }
    g_dl_error_pending = 0;
    return g_dl_error;
}

void* dlopen(const char* path, int mode) {
    if (path == 0) {
        void* program = sx_loader_program();
        if (program == 0) {
            dl_fail("dlopen: the program image is not registered", 0, 0);
            return 0;
        }
        sx_loader_retain(program);
        return program;
    }

    /* A bare name is looked up where DT_NEEDED is; anything with a '/' is a
     * path, relative ones included, and a relative one is the working
     * directory's. */
    char full[128];
    char cwd[128];
    size_t used = 0;
    int has_slash = 0;
    for (const char* ch = path; *ch != '\0'; ++ch) {
        if (*ch == '/') {
            has_slash = 1;
            break;
        }
    }
    const char* parts[3] = {has_slash ? "" : "/lib/", "", path};
    if (has_slash && path[0] != '/') {
        if (savanxp_getcwd(cwd, sizeof(cwd)) < 0 || cwd[0] != '/') {
            dl_fail("dlopen: no working directory for ", path, 0);
            return 0;
        }
        size_t length = 0;
        while (cwd[length] != '\0') {
            length++;
        }
        parts[0] = cwd;
        parts[1] = cwd[length - 1] == '/' ? "" : "/";
    }
    for (int part = 0; part < 3; ++part) {
        for (const char* ch = parts[part]; *ch != '\0'; ++ch) {
            if (used >= sizeof(full) - 1) {
                dl_fail("dlopen: path too long: ", path, 0);
                return 0;
            }
            full[used++] = *ch;
        }
    }
    full[used] = '\0';

    void* image = sx_loader_find(full);
    if (image != 0) {
        if ((mode & RTLD_GLOBAL) != 0) {
            sx_loader_make_global(image);
        }
        sx_loader_retain(image);
        return image;
    }
    if ((mode & RTLD_NOLOAD) != 0) {
        return 0;
    }
    image = sx_loader_open(full, (mode & RTLD_GLOBAL) != 0, g_dl_error, sizeof(g_dl_error));
    if (image == 0) {
        g_dl_error_pending = 1;
    }
    return image;
}

void* dlsym(void* handle, const char* name) {
    if (name == 0) {
        dl_fail("dlsym: no symbol name", 0, 0);
        return 0;
    }
    void* address = 0;
    const int found = sx_loader_symbol(handle == RTLD_DEFAULT ? 0 : handle, name, &address);
    if (found == -1) {
        dl_fail("dlsym: invalid handle", 0, 0);
        return 0;
    }
    if (found != 0) {
        dl_fail("dlsym: symbol not found: ", name, 0);
        return 0;
    }
    return address;
}

int dlclose(void* handle) {
    if (sx_loader_release(handle) != 0) {
        dl_fail("dlclose: handle is not open", 0, 0);
        return -1;
    }
    return 0;
}

int dladdr(const void* address, Dl_info* info) {
    if (info == 0) {
        return 0;
    }
    const char* path = 0;
    void* base = 0;
    const char* symbol = 0;
    void* symbol_address = 0;
    if (!sx_loader_address(address, &path, &base, &symbol, &symbol_address)) {
        return 0;
    }
    info->dli_fname = path;
    info->dli_fbase = base;
    info->dli_sname = symbol;
    info->dli_saddr = symbol_address;
    return 1;
}
