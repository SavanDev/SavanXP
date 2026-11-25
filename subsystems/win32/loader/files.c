/*
 * files.c -- The handle table and the file services (sxw_abi.h, SXW_SVC_CLOSE
 * to SXW_SVC_FIND_NEXT), over the platform's sxw_file_platform.
 *
 * Portable C, no library dependency. The guest's paths are full Windows
 * paths; C:\ is the platform's drive_c folder and Z:\ the host's root, and
 * every component is found without case, as NTFS finds it, so a program may
 * spell a name as it likes. Share modes are checked between the opens of one
 * process, which is all a Windows program here shares a file with. See
 * docs/userland/WIN32.md, "Files".
 */
#include "services.h"

#include "guest.h"

/* CreateFileW's values. */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_ALL 0x10000000u
#define FILE_READ_DATA 0x0001u
#define FILE_WRITE_DATA 0x0002u
#define FILE_APPEND_DATA 0x0004u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u

#define ATTRIBUTE_READONLY 0x01u
#define ATTRIBUTE_DIRECTORY 0x10u
#define ATTRIBUTE_ARCHIVE 0x20u

/* ---- Strings ---------------------------------------------------------------- */

static uint32_t text_length(const char *s) {
    uint32_t n = 0;
    while (s[n] != 0) {
        ++n;
    }
    return n;
}

static int append(char *out, uint32_t *at, uint32_t capacity, const char *text, uint32_t length) {
    if (*at + length + 1u > capacity) {
        return 0;
    }
    for (uint32_t i = 0; i < length; ++i) {
        out[(*at)++] = text[i];
    }
    out[*at] = 0;
    return 1;
}

/* The next code point of UTF-8 text, upper-cased as NTFS's table does for
 * ASCII and Latin-1 (the code pages the profiles use); anything malformed
 * is taken byte by byte. */
static uint32_t next_upper(const char **text) {
    const uint8_t *s = (const uint8_t *)*text;
    uint32_t c = *s++;
    if (c >= 0xc0u && c < 0xe0u && (*s & 0xc0u) == 0x80u) {
        c = ((c & 0x1fu) << 6) | (*s++ & 0x3fu);
    } else if (c >= 0xe0u && c < 0xf0u && (s[0] & 0xc0u) == 0x80u && (s[1] & 0xc0u) == 0x80u) {
        c = ((c & 0x0fu) << 12) | ((uint32_t)(s[0] & 0x3fu) << 6) | (s[1] & 0x3fu);
        s += 2;
    }
    *text = (const char *)s;
    if ((c >= 'a' && c <= 'z') || (c >= 0xe0u && c <= 0xfeu && c != 0xf7u)) {
        c -= 0x20u;
    } else if (c == 0xffu) {
        c = 0x178u;
    }
    return c;
}

/* NTFS's order: upper-cased code points compared one by one. */
static int compare_names(const char *a, const char *b) {
    for (;;) {
        if (*a == 0 || *b == 0) {
            return *a == 0 ? (*b == 0 ? 0 : -1) : 1;
        }
        const uint32_t x = next_upper(&a);
        const uint32_t y = next_upper(&b);
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
}

/* A search mask against a name, without case: '*' any run, '?' one code
 * point. As FindFirstFile has it, "*.*" is everything and a mask ending in
 * ".*" also takes a name with no dot. */
static int matches(const char *mask, const char *name) {
    if (*mask == 0) {
        return *name == 0;
    }
    if (*mask == '*') {
        while (*mask == '*') {
            ++mask;
        }
        if (*mask == 0) {
            return 1;
        }
        for (const char *n = name;; ) {
            if (matches(mask, n)) {
                return 1;
            }
            if (*n == 0) {
                return 0;
            }
            (void)next_upper(&n);
        }
    }
    if (*name == 0) {
        /* ".*" at the end of the mask matches the end of a name with no dot. */
        return mask[0] == '.' && mask[1] == '*' && mask[2] == 0;
    }
    const char *m = mask;
    const uint32_t want = next_upper(&m);
    const uint32_t have = next_upper(&name);
    if (want != '?' && want != have) {
        return 0;
    }
    return matches(m, name);
}

static int mask_matches(const char *mask, const char *name) {
    if (mask[0] == '*' && mask[1] == '.' && mask[2] == '*' && mask[3] == 0) {
        return 1;
    }
    return matches(mask, name);
}

/* ---- Errors -------------------------------------------------------------------- */

static uint32_t win32_error(int status) {
    switch (status) {
    case SXW_FS_OK: return SXW_ERROR_SUCCESS;
    case SXW_FS_NOT_FOUND: return SXW_ERROR_FILE_NOT_FOUND;
    case SXW_FS_EXISTS: return SXW_ERROR_ALREADY_EXISTS;
    case SXW_FS_DENIED: return SXW_ERROR_ACCESS_DENIED;
    case SXW_FS_NOT_DIRECTORY: return SXW_ERROR_DIRECTORY;
    case SXW_FS_IS_DIRECTORY: return SXW_ERROR_ACCESS_DENIED;
    case SXW_FS_NOT_EMPTY: return SXW_ERROR_DIR_NOT_EMPTY;
    case SXW_FS_BUSY: return SXW_ERROR_SHARING_VIOLATION;
    case SXW_FS_FULL: return SXW_ERROR_HANDLE_DISK_FULL;
    case SXW_FS_INVALID: return SXW_ERROR_INVALID_PARAMETER;
    default: return SXW_ERROR_WRITE_FAULT;
    }
}

/* ---- Paths ----------------------------------------------------------------------- */

/* The host path of a full Windows path, each component found without case.
 * The last component may be missing (*last_missing is then 1, and it keeps
 * the guest's spelling); a missing or non-directory component before it is
 * ERROR_PATH_NOT_FOUND. root is set for a drive's root itself. */
static uint32_t host_path(sxw_services *s, const char *windows, char *out, int *last_missing, int *root) {
    const sxw_file_platform *f = s->files;
    *last_missing = 0;
    *root = 0;
    uint32_t at = 0;
    out[0] = 0;
    const char drive = windows[0] >= 'a' && windows[0] <= 'z' ? (char)(windows[0] - 0x20) : windows[0];
    if (f == 0 || windows[1] != ':' || windows[2] != '\\') {
        return SXW_ERROR_PATH_NOT_FOUND;
    }
    if (drive == 'C' && s->drive_c != 0) {
        if (!append(out, &at, SXW_HOST_PATH, s->drive_c, text_length(s->drive_c))) {
            return SXW_ERROR_PATH_NOT_FOUND;
        }
    } else if (drive != 'Z') {
        return SXW_ERROR_PATH_NOT_FOUND;
    }
    const char *p = windows + 3;
    if (*p == 0) {
        *root = 1;
        if (at == 0u) {
            out[at++] = '/';
            out[at] = 0;
        }
        return SXW_ERROR_SUCCESS;
    }
    while (*p != 0) {
        const char *start = p;
        while (*p != 0 && *p != '\\') {
            ++p;
        }
        const uint32_t length = (uint32_t)(p - start);
        const int last = *p == 0 || p[1] == 0;
        if (*p == '\\') {
            ++p;
        }
        if (length == 0u || length > 255u) {
            return SXW_ERROR_PATH_NOT_FOUND;
        }
        char name[256];
        for (uint32_t i = 0; i < length; ++i) {
            name[i] = start[i];
        }
        name[length] = 0;
        const uint32_t parent_end = at;
        if (!append(out, &at, SXW_HOST_PATH, "/", 1) || !append(out, &at, SXW_HOST_PATH, name, length)) {
            return SXW_ERROR_PATH_NOT_FOUND;
        }
        sxw_host_info info;
        int status = f->stat(s->context, out, &info);
        if (status == SXW_FS_NOT_FOUND) {
            /* Not with this spelling: the parent's entry with any case. */
            char parent[SXW_HOST_PATH];
            for (uint32_t i = 0; i < parent_end; ++i) {
                parent[i] = out[i];
            }
            parent[parent_end] = 0;
            if (parent_end == 0u) {
                parent[0] = '/';
                parent[1] = 0;
            }
            long list;
            char found[256];
            int have = 0;
            if (f->list_open(s->context, parent, &list) == SXW_FS_OK) {
                char entry[256];
                sxw_host_info entry_info;
                while (!have && f->list_next(s->context, list, entry, &entry_info) == 1) {
                    if (compare_names(entry, name) == 0) {
                        for (uint32_t i = 0; i <= text_length(entry); ++i) {
                            found[i] = entry[i];
                        }
                        have = 1;
                    }
                }
                f->list_close(s->context, list);
            }
            if (have) {
                at = parent_end;
                if (!append(out, &at, SXW_HOST_PATH, "/", 1) ||
                    !append(out, &at, SXW_HOST_PATH, found, text_length(found))) {
                    return SXW_ERROR_PATH_NOT_FOUND;
                }
                status = f->stat(s->context, out, &info);
            }
        }
        if (status != SXW_FS_OK) {
            if (last && status == SXW_FS_NOT_FOUND) {
                *last_missing = 1;
                return SXW_ERROR_SUCCESS;
            }
            return status == SXW_FS_DENIED ? SXW_ERROR_ACCESS_DENIED : SXW_ERROR_PATH_NOT_FOUND;
        }
        if (!last && info.type != SXW_HOST_DIRECTORY) {
            return SXW_ERROR_PATH_NOT_FOUND;
        }
    }
    return SXW_ERROR_SUCCESS;
}

/* A guest path argument as a host path. */
static uint32_t guest_path(sxw_services *s, uint32_t address, char *out, int *last_missing, int *root) {
    char windows[SXW_HOST_PATH];
    if (!sxw_read_utf16(s->process, address, windows, sizeof(windows))) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    return host_path(s, windows, out, last_missing, root);
}

/* ---- Handles ---------------------------------------------------------------------- */

sxw_object *sxw_object_of(sxw_services *s, uint32_t handle, uint32_t kind) {
    if (handle < SXW_HANDLE_FIRST || (handle & 3u) != 0u) {
        return 0;
    }
    const uint32_t slot = (handle - SXW_HANDLE_FIRST) / 4u;
    if (slot >= SXW_MAX_HANDLES || s->handles[slot] == 0u) {
        return 0;
    }
    sxw_object *o = &s->objects[s->handles[slot] - 1u];
    return kind == SXW_OBJECT_NONE || o->kind == kind ? o : 0;
}

sxw_object *sxw_new_object(sxw_services *s, uint32_t kind) {
    for (uint32_t i = 0; i < SXW_MAX_OBJECTS; ++i) {
        if (s->objects[i].kind == SXW_OBJECT_NONE) {
            s->objects[i].kind = kind;
            s->objects[i].references = 0;
            s->objects[i].name[0] = 0;
            return &s->objects[i];
        }
    }
    return 0;
}

/* A new handle to o, or 0 when the table is full. */
uint32_t sxw_new_handle(sxw_services *s, sxw_object *o) {
    for (uint32_t slot = 0; slot < SXW_MAX_HANDLES; ++slot) {
        if (s->handles[slot] == 0u) {
            s->handles[slot] = (uint8_t)(o - s->objects + 1);
            ++o->references;
            return SXW_HANDLE_FIRST + 4u * slot;
        }
    }
    return 0;
}

static void release_object(sxw_services *s, sxw_object *o) {
    if (--o->references != 0u) {
        return;
    }
    if (o->kind == SXW_OBJECT_FILE) {
        s->files->close(s->context, o->u.file.id);
    }
    o->kind = SXW_OBJECT_NONE;
}

uint32_t sxw_close_handle(sxw_services *s, uint32_t handle) {
    sxw_object *o = sxw_object_of(s, handle, SXW_OBJECT_NONE);
    if (o == 0) {
        return SXW_ERROR_INVALID_HANDLE;
    }
    s->handles[(handle - SXW_HANDLE_FIRST) / 4u] = 0;
    release_object(s, o);
    return SXW_ERROR_SUCCESS;
}

/* The open of path that keeps another open, or deleting it, from happening:
 * 1 when the share modes allow it. */
static int share_allows(sxw_services *s, const char *path, uint32_t read, uint32_t write, uint32_t share,
                        int deleting) {
    for (uint32_t i = 0; i < SXW_MAX_OBJECTS; ++i) {
        const sxw_object *o = &s->objects[i];
        if (o->kind != SXW_OBJECT_FILE || compare_names(o->u.file.path, path) != 0) {
            continue;
        }
        if (deleting) {
            if ((o->u.file.share & FILE_SHARE_DELETE) == 0u) {
                return 0;
            }
            continue;
        }
        if ((read && (o->u.file.share & FILE_SHARE_READ) == 0u) ||
            (write && (o->u.file.share & FILE_SHARE_WRITE) == 0u) ||
            (o->u.file.read && (share & FILE_SHARE_READ) == 0u) ||
            (o->u.file.write && (share & FILE_SHARE_WRITE) == 0u)) {
            return 0;
        }
    }
    return 1;
}

/* ---- Files ------------------------------------------------------------------------- */

static void entry_header(const sxw_host_info *info, uint32_t out[4]) {
    out[0] = info->type == SXW_HOST_DIRECTORY ? ATTRIBUTE_DIRECTORY : ATTRIBUTE_ARCHIVE;
    if (!info->writable) {
        out[0] |= ATTRIBUTE_READONLY;
    }
    out[1] = info->type == SXW_HOST_DIRECTORY ? 0u : (uint32_t)info->size;
    out[2] = info->type == SXW_HOST_DIRECTORY ? 0u : (uint32_t)(info->size >> 32);
    out[3] = (uint32_t)info->mtime;
}

static uint32_t write_header(sxw_services *s, uint32_t address, const sxw_host_info *info) {
    uint32_t words[4];
    entry_header(info, words);
    if (!sxw_guest_range(s->process, address, SXW_FILE_ENTRY_HEADER)) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < 4u; ++i) {
        sxw_write_word(s->process, address + 4u * i, words[i]);
    }
    return SXW_ERROR_SUCCESS;
}

static uint32_t file_open(sxw_services *s, const uint32_t *a) {
    const sxw_file_platform *f = s->files;
    const uint32_t access = a[1];
    const uint32_t share = a[2] & (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    const uint32_t disposition = a[3];
    if (!sxw_guest_range(s->process, a[4], 8u) || disposition < CREATE_NEW || disposition > TRUNCATE_EXISTING) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    char path[SXW_HOST_PATH];
    int missing;
    int root;
    uint32_t error = guest_path(s, a[0], path, &missing, &root);
    if (error != SXW_ERROR_SUCCESS) {
        return error;
    }
    const uint32_t read = (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) != 0u;
    const uint32_t write = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0u;
    if (root) {
        return SXW_ERROR_ACCESS_DENIED;
    }
    sxw_host_info info;
    info.type = SXW_HOST_NONE;
    if (!missing) {
        const int status = f->stat(s->context, path, &info);
        if (status != SXW_FS_OK) {
            return win32_error(status);
        }
        if (info.type == SXW_HOST_DIRECTORY) {
            return SXW_ERROR_ACCESS_DENIED;
        }
        if (info.type != SXW_HOST_FILE) {
            return SXW_ERROR_ACCESS_DENIED;
        }
    }
    if (missing && (disposition == OPEN_EXISTING || disposition == TRUNCATE_EXISTING)) {
        return SXW_ERROR_FILE_NOT_FOUND;
    }
    if (!missing && disposition == CREATE_NEW) {
        return SXW_ERROR_FILE_EXISTS;
    }
    if (disposition == TRUNCATE_EXISTING && !write) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    if (!share_allows(s, path, read, write, share, 0)) {
        return SXW_ERROR_SHARING_VIOLATION;
    }
    const int empties = !missing && (disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING);
    if (!missing && (write || empties) && !info.writable) {
        return SXW_ERROR_ACCESS_DENIED;
    }
    sxw_object *o = sxw_new_object(s, SXW_OBJECT_FILE);
    if (o == 0) {
        return SXW_ERROR_TOO_MANY_OPEN_FILES;
    }
    uint32_t mode = (read ? SXW_OPEN_READ : 0u) | (write ? SXW_OPEN_WRITE : 0u);
    if (missing) {
        mode |= SXW_OPEN_CREATE | SXW_OPEN_NEW;
    } else if (empties) {
        mode |= SXW_OPEN_CREATE;
    }
    long id = -1;
    const int status = f->open(s->context, path, mode, &id);
    if (status != SXW_FS_OK) {
        o->kind = SXW_OBJECT_NONE;
        return win32_error(status);
    }
    o->u.file.id = id;
    o->u.file.position = 0;
    o->u.file.read = read;
    o->u.file.write = write;
    o->u.file.share = share;
    uint32_t at = 0;
    (void)append(o->u.file.path, &at, SXW_HOST_PATH, path, text_length(path));
    const uint32_t handle = sxw_new_handle(s, o);
    if (handle == 0u) {
        f->close(s->context, id);
        o->kind = SXW_OBJECT_NONE;
        return SXW_ERROR_TOO_MANY_OPEN_FILES;
    }
    sxw_write_word(s->process, a[4], handle);
    sxw_write_word(s->process, a[4] + 4u, missing ? 0u : 1u);
    return SXW_ERROR_SUCCESS;
}

static uint32_t file_transfer(sxw_services *s, const uint32_t *a, int writing) {
    sxw_object *o = sxw_object_of(s, a[0], SXW_OBJECT_FILE);
    if (o == 0) {
        return SXW_ERROR_INVALID_HANDLE;
    }
    if (!sxw_guest_range(s->process, a[3], 4u) || (a[2] != 0u && !sxw_guest_range(s->process, a[1], a[2]))) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    sxw_write_word(s->process, a[3], 0);
    if (writing ? !o->u.file.write : !o->u.file.read) {
        return SXW_ERROR_ACCESS_DENIED;
    }
    uint32_t done = 0;
    int status = SXW_FS_OK;
    if (a[2] != 0u) {
        status = writing ? s->files->write(s->context, o->u.file.id, o->u.file.position,
                                           (const void *)(uintptr_t)a[1], a[2], &done)
                         : s->files->read(s->context, o->u.file.id, o->u.file.position, (void *)(uintptr_t)a[1],
                                          a[2], &done);
    }
    o->u.file.position += done;
    sxw_write_word(s->process, a[3], done);
    if (status != SXW_FS_OK) {
        return status == SXW_FS_FULL ? SXW_ERROR_HANDLE_DISK_FULL
               : writing             ? SXW_ERROR_WRITE_FAULT
                                     : SXW_ERROR_READ_FAULT;
    }
    return SXW_ERROR_SUCCESS;
}

static uint32_t file_seek(sxw_services *s, const uint32_t *a) {
    sxw_object *o = sxw_object_of(s, a[0], SXW_OBJECT_FILE);
    if (o == 0) {
        return SXW_ERROR_INVALID_HANDLE;
    }
    if (!sxw_guest_range(s->process, a[4], 8u)) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    const int64_t distance = (int64_t)(((uint64_t)a[2] << 32) | a[1]);
    int64_t base = 0;
    switch (a[3]) {
    case 0u:
        break;
    case 1u:
        base = (int64_t)o->u.file.position;
        break;
    case 2u: {
        sxw_host_info info;
        const int status = s->files->query(s->context, o->u.file.id, &info);
        if (status != SXW_FS_OK) {
            return win32_error(status);
        }
        base = (int64_t)info.size;
        break;
    }
    default:
        return SXW_ERROR_INVALID_PARAMETER;
    }
    const int64_t target = base + distance;
    if (target < 0) {
        return SXW_ERROR_NEGATIVE_SEEK;
    }
    o->u.file.position = (uint64_t)target;
    sxw_write_word(s->process, a[4], (uint32_t)target);
    sxw_write_word(s->process, a[4] + 4u, (uint32_t)((uint64_t)target >> 32));
    return SXW_ERROR_SUCCESS;
}

/* ---- Searches ------------------------------------------------------------------------ */

/* The next entry of a search after o->u.find.last, in NTFS's order. 1 with
 * name and info, 0 at the end, or -1 when the directory cannot be read. */
static int next_entry(sxw_services *s, sxw_object *o, char *name, sxw_host_info *info) {
    const sxw_file_platform *f = s->files;
    if (!o->u.find.root && o->u.find.state < 2u && mask_matches(o->u.find.mask, ".")) {
        const char *dots = o->u.find.state == 0u ? "." : "..";
        const int status = f->stat(s->context, o->u.find.directory, info);
        if (status != SXW_FS_OK) {
            return -1;
        }
        ++o->u.find.state;
        name[0] = '.';
        name[1] = dots[1];
        name[2] = 0;
        return 1;
    }
    long list;
    if (f->list_open(s->context, o->u.find.directory, &list) != SXW_FS_OK) {
        return -1;
    }
    int have = 0;
    char entry[256];
    sxw_host_info entry_info;
    while (f->list_next(s->context, list, entry, &entry_info) == 1) {
        if (!mask_matches(o->u.find.mask, entry) ||
            (o->u.find.state == 3u && compare_names(entry, o->u.find.last) <= 0) ||
            (have && compare_names(entry, name) >= 0)) {
            continue;
        }
        for (uint32_t i = 0; i <= text_length(entry); ++i) {
            name[i] = entry[i];
        }
        *info = entry_info;
        have = 1;
    }
    f->list_close(s->context, list);
    if (!have) {
        o->u.find.state = 3u;
        return 0;
    }
    for (uint32_t i = 0; i <= text_length(name); ++i) {
        o->u.find.last[i] = name[i];
    }
    o->u.find.state = 3u;
    return 1;
}

static uint32_t write_entry(sxw_services *s, uint32_t address, const char *name, const sxw_host_info *info) {
    if (!sxw_guest_range(s->process, address, (uint32_t)sizeof(sxw_file_entry))) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    write_header(s, address, info);
    sxw_write_utf16(s->process, address + SXW_FILE_ENTRY_HEADER, SXW_FILE_NAME_UNITS, name);
    return SXW_ERROR_SUCCESS;
}

static uint32_t find_first(sxw_services *s, const uint32_t *a) {
    if (!sxw_guest_range(s->process, a[2], 4u) ||
        !sxw_guest_range(s->process, a[3], (uint32_t)sizeof(sxw_file_entry))) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    char directory[SXW_HOST_PATH];
    char mask[256];
    int missing;
    int root;
    uint32_t error = guest_path(s, a[0], directory, &missing, &root);
    if (error != SXW_ERROR_SUCCESS) {
        return error;
    }
    sxw_host_info info;
    if (missing || s->files->stat(s->context, directory, &info) != SXW_FS_OK || info.type != SXW_HOST_DIRECTORY) {
        return SXW_ERROR_PATH_NOT_FOUND;
    }
    if (!sxw_read_utf16(s->process, a[1], mask, sizeof(mask)) || mask[0] == 0) {
        return SXW_ERROR_FILE_NOT_FOUND;
    }
    sxw_object *o = sxw_new_object(s, SXW_OBJECT_FIND);
    if (o == 0) {
        return SXW_ERROR_TOO_MANY_OPEN_FILES;
    }
    uint32_t at = 0;
    (void)append(o->u.find.directory, &at, SXW_HOST_PATH, directory, text_length(directory));
    at = 0;
    (void)append(o->u.find.mask, &at, sizeof(o->u.find.mask), mask, text_length(mask));
    o->u.find.last[0] = 0;
    o->u.find.state = 0;
    o->u.find.root = (uint32_t)root;
    char name[256];
    const int found = next_entry(s, o, name, &info);
    if (found != 1) {
        o->kind = SXW_OBJECT_NONE;
        return found == 0 ? SXW_ERROR_FILE_NOT_FOUND : SXW_ERROR_PATH_NOT_FOUND;
    }
    const uint32_t handle = sxw_new_handle(s, o);
    if (handle == 0u) {
        o->kind = SXW_OBJECT_NONE;
        return SXW_ERROR_TOO_MANY_OPEN_FILES;
    }
    sxw_write_word(s->process, a[2], handle);
    return write_entry(s, a[3], name, &info);
}

static uint32_t find_next(sxw_services *s, const uint32_t *a) {
    sxw_object *o = sxw_object_of(s, a[0], SXW_OBJECT_FIND);
    if (o == 0) {
        return SXW_ERROR_INVALID_HANDLE;
    }
    char name[256];
    sxw_host_info info;
    const int found = next_entry(s, o, name, &info);
    if (found != 1) {
        return SXW_ERROR_NO_MORE_FILES;
    }
    return write_entry(s, a[1], name, &info);
}

/* ---- Paths ------------------------------------------------------------------------------ */

static uint32_t path_action(sxw_services *s, const uint32_t *a) {
    const sxw_file_platform *f = s->files;
    char path[SXW_HOST_PATH];
    int missing;
    int root;
    uint32_t error = guest_path(s, a[1], path, &missing, &root);
    if (error != SXW_ERROR_SUCCESS) {
        return error;
    }
    sxw_host_info info;
    info.type = SXW_HOST_NONE;
    if (!missing && f->stat(s->context, path, &info) != SXW_FS_OK) {
        missing = 1;
    }
    switch (a[0]) {
    case SXW_PATH_DELETE_FILE:
        if (missing) {
            return SXW_ERROR_FILE_NOT_FOUND;
        }
        if (info.type == SXW_HOST_DIRECTORY || root) {
            return SXW_ERROR_ACCESS_DENIED;
        }
        if (!share_allows(s, path, 0, 0, 0, 1)) {
            return SXW_ERROR_SHARING_VIOLATION;
        }
        return win32_error(f->remove_file(s->context, path));
    case SXW_PATH_MAKE_DIRECTORY:
        if (!missing || root) {
            return SXW_ERROR_ALREADY_EXISTS;
        }
        return win32_error(f->make_directory(s->context, path));
    case SXW_PATH_REMOVE_DIRECTORY: {
        if (missing) {
            return SXW_ERROR_FILE_NOT_FOUND;
        }
        if (info.type != SXW_HOST_DIRECTORY) {
            return SXW_ERROR_DIRECTORY;
        }
        if (root) {
            return SXW_ERROR_ACCESS_DENIED;
        }
        const int status = f->remove_directory(s->context, path);
        return status == SXW_FS_BUSY ? SXW_ERROR_SHARING_VIOLATION : win32_error(status);
    }
    case SXW_PATH_MOVE: {
        char target[SXW_HOST_PATH];
        int target_missing;
        int target_root;
        if (missing) {
            return SXW_ERROR_FILE_NOT_FOUND;
        }
        error = guest_path(s, a[2], target, &target_missing, &target_root);
        if (error != SXW_ERROR_SUCCESS) {
            return error;
        }
        if (!target_missing || target_root) {
            return SXW_ERROR_ALREADY_EXISTS;
        }
        if (info.type == SXW_HOST_FILE && !share_allows(s, path, 0, 0, 0, 1)) {
            return SXW_ERROR_SHARING_VIOLATION;
        }
        return win32_error(f->rename(s->context, path, target));
    }
    default:
        return SXW_ERROR_INVALID_PARAMETER;
    }
}

/* ---- The services ------------------------------------------------------------------------- */

uint32_t sxw_file_service(sxw_services *s, uint32_t service, const uint32_t *a) {
    if (s->files == 0 && service != SXW_SVC_CLOSE && service != SXW_SVC_DUPLICATE) {
        return SXW_ERROR_PATH_NOT_FOUND;
    }
    switch (service) {
    case SXW_SVC_CLOSE:
        return sxw_close_handle(s, a[0]);
    case SXW_SVC_DUPLICATE: {
        sxw_object *o = sxw_object_of(s, a[0], SXW_OBJECT_NONE);
        if (o == 0) {
            return SXW_ERROR_INVALID_HANDLE;
        }
        if (!sxw_guest_range(s->process, a[1], 4u)) {
            return SXW_ERROR_INVALID_PARAMETER;
        }
        const uint32_t handle = sxw_new_handle(s, o);
        if (handle == 0u) {
            return SXW_ERROR_TOO_MANY_OPEN_FILES;
        }
        sxw_write_word(s->process, a[1], handle);
        return SXW_ERROR_SUCCESS;
    }
    case SXW_SVC_FILE_OPEN:
        return file_open(s, a);
    case SXW_SVC_FILE_READ:
        return file_transfer(s, a, 0);
    case SXW_SVC_FILE_WRITE:
        return file_transfer(s, a, 1);
    case SXW_SVC_FILE_SEEK:
        return file_seek(s, a);
    case SXW_SVC_FILE_SET_END: {
        sxw_object *o = sxw_object_of(s, a[0], SXW_OBJECT_FILE);
        if (o == 0) {
            return SXW_ERROR_INVALID_HANDLE;
        }
        if (!o->u.file.write) {
            return SXW_ERROR_ACCESS_DENIED;
        }
        return win32_error(s->files->set_size(s->context, o->u.file.id, o->u.file.position));
    }
    case SXW_SVC_FILE_INFO: {
        sxw_object *o = sxw_object_of(s, a[0], SXW_OBJECT_FILE);
        if (o == 0) {
            return SXW_ERROR_INVALID_HANDLE;
        }
        sxw_host_info info;
        const int status = s->files->query(s->context, o->u.file.id, &info);
        return status != SXW_FS_OK ? win32_error(status) : write_header(s, a[1], &info);
    }
    case SXW_SVC_FILE_FLUSH: {
        sxw_object *o = sxw_object_of(s, a[0], SXW_OBJECT_FILE);
        if (o == 0) {
            return SXW_ERROR_INVALID_HANDLE;
        }
        if (!o->u.file.write) {
            return SXW_ERROR_ACCESS_DENIED;
        }
        return win32_error(s->files->flush(s->context, o->u.file.id));
    }
    case SXW_SVC_PATH_INFO: {
        char path[SXW_HOST_PATH];
        int missing;
        int root;
        const uint32_t error = guest_path(s, a[0], path, &missing, &root);
        if (error != SXW_ERROR_SUCCESS) {
            return error;
        }
        sxw_host_info info;
        if (missing || s->files->stat(s->context, path, &info) != SXW_FS_OK) {
            return SXW_ERROR_FILE_NOT_FOUND;
        }
        return write_header(s, a[1], &info);
    }
    case SXW_SVC_PATH_ACTION:
        return path_action(s, a);
    case SXW_SVC_FIND_FIRST:
        return find_first(s, a);
    case SXW_SVC_FIND_NEXT:
        return find_next(s, a);
    default:
        return SXW_ERROR_INVALID_PARAMETER;
    }
}
