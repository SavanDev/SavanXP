/* sxnlibrary: SUN's run-time loading (include/sxn/sxnlibrary.h), over the
 * shared loader's interface (include/loader/loader.h).
 *
 * The loader is platform code; what is SUN's lives here: the flags, negative
 * statuses instead of null and a held message, and the rule that a name is
 * bare or absolute, since SXN has no working directory. Compiled into every
 * SUN PIE program, next to the loader, as dlfcn.c is into every POSIX one, and
 * into no STATIC one, where a call is a link error. Like the loader it calls no
 * C library. */

#include "sxn/sxnlibrary.h"

#include "loader/loader.h"

/* The loader's path buffers are this size too (subsystems/posix/userland/dlfcn.c). */
#define SXN_LIBRARY_PATH_MAX 128u

static unsigned long text_length(const char* text) {
    unsigned long length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

/* `error` = first second, cut to fit; nothing when there is no buffer. */
static unsigned long append(char* error, unsigned long size, unsigned long used, const char* text) {
    while (text != 0 && *text != '\0' && used + 1 < size) {
        error[used++] = *text++;
    }
    return used;
}

static void set_error(char* error, unsigned long size, const char* first, const char* second) {
    unsigned long used;
    if (error == 0 || size == 0) {
        return;
    }
    used = append(error, size, 0, first);
    used = append(error, size, used, second);
    error[used] = '\0';
}

long sxn_library_open(const char* name, unsigned flags, struct sxn_library** out, char* error,
                      unsigned long error_size) {
    char path[SXN_LIBRARY_PATH_MAX];
    int has_slash = 0;
    unsigned long length;
    void* image;

    if (error != 0 && error_size != 0) {
        error[0] = '\0';
    }
    if (out == 0) {
        return -SX_EINVAL;
    }
    *out = 0;
    if (name == 0 || name[0] == '\0' || (flags & ~(SXN_LIBRARY_GLOBAL | SXN_LIBRARY_NOLOAD)) != 0) {
        set_error(error, error_size, "sxn_library_open: no name, or an unknown flag", 0);
        return -SX_EINVAL;
    }
    for (const char* ch = name; *ch != '\0'; ++ch) {
        if (*ch == '/') {
            has_slash = 1;
            break;
        }
    }
    length = text_length(name);
    if (has_slash) {
        /* SXN resolves no relative path, and neither does this. */
        if (name[0] != '/') {
            set_error(error, error_size, "sxn_library_open: a relative path: ", name);
            return -SX_EINVAL;
        }
        if (length >= sizeof(path)) {
            set_error(error, error_size, "sxn_library_open: path too long: ", name);
            return -SX_ENAMETOOLONG;
        }
        for (unsigned long index = 0; index <= length; ++index) {
            path[index] = name[index];
        }
    } else {
        /* A bare name is looked up where a DT_NEEDED is, and resolved the
         * same way: "/lib/" plus the name, or another version of it. */
        if (length + 5u >= sizeof(path)) {
            set_error(error, error_size, "sxn_library_open: name too long: ", name);
            return -SX_ENAMETOOLONG;
        }
        (void)sx_loader_resolve(name, path, sizeof(path));
    }

    image = sx_loader_find(path);
    if (image != 0) {
        if ((flags & SXN_LIBRARY_GLOBAL) != 0) {
            sx_loader_make_global(image);
        }
        sx_loader_retain(image);
        *out = (struct sxn_library*)image;
        return 0;
    }
    if ((flags & SXN_LIBRARY_NOLOAD) != 0) {
        set_error(error, error_size, "sxn_library_open: not loaded: ", path);
        return -SX_ENOENT;
    }
    image = sx_loader_open(path, (flags & SXN_LIBRARY_GLOBAL) != 0, error, error_size);
    if (image == 0) {
        return -SX_ENOEXEC;
    }
    *out = (struct sxn_library*)image;
    return 0;
}

long sxn_library_symbol(struct sxn_library* library, const char* name, void** address) {
    void* found = 0;
    int status;

    if (address == 0) {
        return -SX_EINVAL;
    }
    *address = 0;
    if (name == 0) {
        return -SX_EINVAL;
    }
    status = sx_loader_symbol(library, name, &found);
    if (status == -1) {
        return -SX_EBADF;
    }
    if (status != 0) {
        return -SX_ENOENT;
    }
    *address = found;
    return 0;
}

long sxn_library_close(struct sxn_library* library) {
    if (library == 0 || sx_loader_release(library) != 0) {
        return -SX_EBADF;
    }
    return 0;
}

long sxn_library_address(const void* address, struct sxn_library_address_info* info) {
    const char* path = 0;
    void* base = 0;
    const char* symbol = 0;
    void* symbol_address = 0;

    if (info == 0) {
        return -SX_EINVAL;
    }
    if (!sx_loader_address(address, &path, &base, &symbol, &symbol_address)) {
        return -SX_ENOENT;
    }
    info->path = path;
    info->base = base;
    info->symbol = symbol;
    info->symbol_address = symbol_address;
    return 0;
}
