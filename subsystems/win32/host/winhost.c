/*
 * winhost.c -- winhost.sxe: runs a PE32 executable on SavanXP.
 *
 *     winhost [--dlls DIR] [--registry FILE] [--time SECONDS] /path/to/program.exe [arguments...]
 *
 * A SUN program (docs/userland/WIN32.md, "winhost"). It loads the image and
 * its DLLs below 4 GiB with the portable loader, each protection run in its
 * own section placed with SXN_SYS_MAP_VIEW_AT, and runs the 32-bit guest in
 * the user 32-bit code segment (0x2b). DLLs are looked up next to the
 * executable, then in DIR (default /system/win/drive_c/windows/system32).
 * The arguments follow the program in its command line. HKEY_CURRENT_USER
 * lives in FILE (default /users/user/Settings/win32.reg); --time fixes the
 * guest's clock at that second, so a run that seeds from the time repeats.
 *
 * The guest's standard output and error are winhost's. The exit status is
 * the guest's 32-bit status, or the NT status Windows XP gives a failed load;
 * `win32: exit status 0x...` and any load diagnostic go to standard error. A
 * failed load is checked to have released every section and file it took;
 * if not, winhost says so and exits with 124, as the host harness does.
 *
 * FS holds the user data selector with the TEB as its base for the whole
 * run: the native runtime does not use FS, so unlike the host harness the
 * gate never switches it. DS and ES hold the user data selector too, which
 * 32-bit code needs and 64-bit code ignores; the kernel keeps all three per
 * thread (docs/kernel/ARCH.md#segments). Every guest thread is a SUN thread
 * with its own selectors and FS base (docs/userland/WIN32.md, "Threads").
 */
#include <stdint.h>
#include <stdlib.h>

#include "abi/savanxp_native_abi.h"
#include "sxn/sxn.h"
#include "sxn/sxnclip.h"
#include "sxn/sxnwm.h"

#include "loader.h"
#include "services.h"
#include "sxw_abi.h"

#define USER_DATA_SELECTOR 0x1b
#define USER_CODE64_SELECTOR 0x23
#define USER_CODE32_SELECTOR 0x2b

#define DRIVE_C "/system/win/drive_c"
#define DEFAULT_SYSTEM_DIR DRIVE_C "/windows/system32"
#define DEFAULT_REGISTRY "/users/user/Settings/win32.reg"

/* The 32-bit halves of the gate, filled at start-up (as in the harness):
 * back32 and service32 far-jump to 64-bit landings that jump into this
 * program, and svc_ret32 is the guest's `ret 8`. */
#define TRAMPOLINE 0x7ffe0000u
#define TRAMPOLINE_BACK32 0x00u
#define TRAMPOLINE_LANDING_RETURN 0x10u
#define TRAMPOLINE_SERVICE32 0x20u
#define TRAMPOLINE_LANDING_SERVICE 0x30u
#define TRAMPOLINE_SVC_RET32 0x40u

/* ---- The 64-bit side of the gate ---------------------------------------- */

/* A guest thread's gate state: 64 bytes each, found on the way back from
 * the guest by the index in its TEB (SXW_TEB_HOST_THREAD), read through FS,
 * whose base is the TEB for the whole life of the thread. %r12 holds the
 * state's address on the host side; guest code cannot see it. */
typedef struct gate_state {
    uint64_t host_rsp;
    uint64_t reserved0;
    uint64_t guest_esp;
    uint32_t guest_regs[4];
    uint32_t result;
    uint32_t leave;
    uint32_t index;
    uint32_t reserved1[3];
} gate_state;
_Static_assert(sizeof(gate_state) == 64, "the gate assumes 64-byte states");

gate_state sxw_gates[SXW_MAX_THREADS];
uint64_t sxw_svc_ret32;

uint32_t sxw_gate_enter(uint32_t entry, uint32_t guest_esp, gate_state *state);
void sxw_gate_return(void);
void sxw_gate_service(void);
uint32_t sxw_host_service(gate_state *state);

/* sxw_gate_enter(entry, guest_esp, state): guest_esp points at
 * [return address = back32][arguments...]; far-return into 32-bit code.
 * sxw_gate_return: back32 lands here with the guest function's eax.
 * sxw_gate_service: service32 lands here; the guest stack holds
 * [return address][service][arguments]. */
__asm__(
    "    .text\n"
    "    .globl sxw_gate_enter\n"
    "sxw_gate_enter:\n"
    "    push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
    "    sub $8, %rsp\n"
    "    mov %rsp, 0(%rdx)\n"
    "    mov %edi, %edi\n"
    "    mov %esi, %esp\n"
    "    pushq $0x2b\n"
    "    push %rdi\n"
    "    lretq\n"
    "    .globl sxw_gate_return\n"
    "sxw_gate_return:\n"
    "    mov %esp, %esp\n"
    "    movl %fs:0xff0, %r12d\n"
    "    shl $6, %r12\n"
    "    lea sxw_gates(%rip), %r11\n"
    "    add %r11, %r12\n"
    "    mov %eax, 40(%r12)\n"
    "sxw_gate_leave:\n"
    "    movl 40(%r12), %eax\n"
    "    mov 0(%r12), %rsp\n"
    "    add $8, %rsp\n"
    "    pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n"
    "    ret\n"
    "    .globl sxw_gate_service\n"
    "sxw_gate_service:\n"
    "    mov %esp, %eax\n"
    "    movl %fs:0xff0, %r12d\n"
    "    shl $6, %r12\n"
    "    lea sxw_gates(%rip), %r11\n"
    "    add %r11, %r12\n"
    "    mov %rax, 16(%r12)\n"
    "    mov %ebx, 24(%r12)\n"
    "    mov %ebp, 28(%r12)\n"
    "    mov %esi, 32(%r12)\n"
    "    mov %edi, 36(%r12)\n"
    "    mov 0(%r12), %rsp\n"
    "    mov %r12, %rdi\n"
    "    call sxw_host_service\n"
    "    mov %eax, 40(%r12)\n"
    "    cmpl $0, 44(%r12)\n"
    "    jne sxw_gate_leave\n"
    "    mov 24(%r12), %ebx\n"
    "    mov 28(%r12), %ebp\n"
    "    mov 32(%r12), %esi\n"
    "    mov 36(%r12), %edi\n"
    "    mov sxw_svc_ret32(%rip), %r11\n"
    "    mov 16(%r12), %rsp\n"
    "    movl 40(%r12), %eax\n"
    "    pushq $0x2b\n"
    "    push %r11\n"
    "    lretq\n");

/* ---- Output ---------------------------------------------------------------- */

static uint32_t text_length(const char *text) {
    uint32_t n = 0;
    while (text[n] != 0) {
        ++n;
    }
    return n;
}

static void say(const char *text) {
    (void)sxn_write(2, text, text_length(text));
}

static void say_hex(const char *before, uint32_t value) {
    char line[64];
    uint32_t n = 0;
    for (const char *p = before; *p != 0 && n < 40u; ++p) {
        line[n++] = *p;
    }
    line[n++] = '0';
    line[n++] = 'x';
    for (int i = 0; i < 8; ++i) {
        uint32_t nibble = (value >> (28 - 4 * i)) & 0xfu;
        line[n++] = (char)(nibble < 10u ? '0' + nibble : 'a' + nibble - 10u);
    }
    line[n++] = '\n';
    (void)sxn_write(2, line, n);
}

/* ---- Platform ------------------------------------------------------------ */

typedef struct mapping {
    uint32_t base;
    uint32_t size;
    long section;
} mapping;

/* One per loader region, plus the trampoline. */
static mapping g_mappings[SXW_MAX_REGIONS + 1u];
static uint32_t g_open_files;
static const char *g_system_dir = DEFAULT_SYSTEM_DIR;
static char g_exe_dir[256];

static mapping *find_mapping(uint32_t base, uint32_t size) {
    for (uint32_t i = 0; i < SXW_MAX_REGIONS + 1u; ++i) {
        if (g_mappings[i].section > 0 && g_mappings[i].base == base &&
            (size == 0u || g_mappings[i].size == size)) {
            return &g_mappings[i];
        }
    }
    return 0;
}

/* A section of its own, granted read, write and execute so that the same
 * pages can later be viewed read-execute; the view itself is never both
 * writable and executable. */
static int map_section_at(uint32_t base, uint32_t size) {
    mapping *slot = 0;
    for (uint32_t i = 0; i < SXW_MAX_REGIONS + 1u && slot == 0; ++i) {
        if (g_mappings[i].section <= 0) {
            slot = &g_mappings[i];
        }
    }
    if (slot == 0) {
        return 1;
    }
    long section = sxn_section_create(size, SXN_SECTION_READ | SXN_SECTION_WRITE | SXN_SECTION_EXEC);
    if (section < 0) {
        return 1;
    }
    if (sxn_map_view_at(section, base, SXN_SECTION_READ | SXN_SECTION_WRITE) != (long)base) {
        (void)sxn_close(section);
        return 1;
    }
    slot->base = base;
    slot->size = size;
    slot->section = section;
    return 0;
}

static int remap(mapping *m, unsigned flags) {
    if (sxn_unmap_view(m->base) != 0) {
        return 1;
    }
    return sxn_map_view_at(m->section, m->base, flags) == (long)m->base ? 0 : 1;
}

static int platform_map(void *context, uint32_t base, uint32_t size) {
    (void)context;
    return map_section_at(base, size);
}

static int platform_protect(void *context, uint32_t base, uint32_t size, pe_protection protection) {
    (void)context;
    mapping *m = find_mapping(base, size);
    if (m == 0) {
        return 1;
    }
    unsigned flags = protection == PE_PROT_RX   ? SXN_SECTION_READ | SXN_SECTION_EXEC
                     : protection == PE_PROT_RW ? SXN_SECTION_READ | SXN_SECTION_WRITE
                                                : SXN_SECTION_READ;
    return protection == PE_PROT_RW ? 0 : remap(m, flags);
}

static void platform_unmap(void *context, uint32_t base, uint32_t size) {
    (void)context;
    mapping *m = find_mapping(base, size);
    if (m != 0) {
        (void)sxn_unmap_view(m->base);
        (void)sxn_close(m->section);
        m->section = 0;
    }
}

typedef struct file_buffer {
    uint8_t *data;
    uint64_t size;
} file_buffer;

static int read_whole(const char *path, file_buffer *file) {
    struct sxn_file_info info;
    if (sxn_query_path(path, &info) != 0 || info.type != SXN_FILE_TYPE_FILE || info.size == 0u ||
        info.size > (64u << 20)) {
        return 1;
    }
    long handle = sxn_open_file(path, SXN_FILE_READ);
    if (handle < 0) {
        return 1;
    }
    file->size = info.size;
    file->data = malloc((size_t)info.size);
    uint64_t done = 0;
    while (file->data != 0 && done < file->size) {
        long n = sxn_read(handle, file->data + done, (unsigned long)(file->size - done));
        if (n <= 0) {
            break;
        }
        done += (uint64_t)n;
    }
    (void)sxn_close(handle);
    if (file->data == 0 || done != file->size) {
        free(file->data);
        return 1;
    }
    return 0;
}

/* dir + "/" + name, lower-cased (names are installed lower-case). */
static int join(char *out, uint32_t capacity, const char *dir, const char *name) {
    uint32_t n = 0;
    for (const char *p = dir; *p != 0; ++p) {
        if (n + 1u >= capacity) {
            return 0;
        }
        out[n++] = *p;
    }
    if (n + 1u >= capacity) {
        return 0;
    }
    out[n++] = '/';
    for (const char *p = name; *p != 0; ++p) {
        if (n + 1u >= capacity) {
            return 0;
        }
        out[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
    }
    out[n] = 0;
    return 1;
}

/* Copies a path, truncated to fit. */
static void copy_path(char *out, size_t capacity, const char *path) {
    size_t n = 0;
    while (path[n] != 0 && n + 1u < capacity) {
        out[n] = path[n];
        ++n;
    }
    if (capacity != 0u) {
        out[n] = 0;
    }
}

static int platform_open(void *context, const char *name, const uint8_t **data, size_t *size,
                         void **handle, char *found_path, size_t capacity) {
    (void)context;
    file_buffer *file = malloc(sizeof(*file));
    if (file == 0) {
        return 1;
    }
    int found = 0;
    if (name[0] == '/') {
        found = read_whole(name, file) == 0;
        copy_path(found_path, capacity, name);
    } else {
        char path[512];
        found = join(path, sizeof(path), g_exe_dir, name) && read_whole(path, file) == 0;
        if (!found) {
            found = join(path, sizeof(path), g_system_dir, name) && read_whole(path, file) == 0;
        }
        copy_path(found_path, capacity, path);
    }
    if (!found) {
        free(file);
        return 1;
    }
    *data = file->data;
    *size = (size_t)file->size;
    *handle = file;
    ++g_open_files;
    return 0;
}

static void platform_close(void *context, void *handle) {
    (void)context;
    file_buffer *file = handle;
    free(file->data);
    free(file);
    --g_open_files;
}

/* Sections still held besides the trampoline's. */
static uint32_t live_sections(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < SXW_MAX_REGIONS + 1u; ++i) {
        if (g_mappings[i].section > 0 && g_mappings[i].base != TRAMPOLINE) {
            ++count;
        }
    }
    return count;
}

/* ---- Services ------------------------------------------------------------ */

static sxw_process g_process;

static long write_stream(void *context, int stream, const void *buffer, uint32_t length) {
    (void)context;
    return sxn_write(stream, buffer, length);
}

static const char *g_registry_path = DEFAULT_REGISTRY;
static sxw_registry g_registry;

static uint64_t now_seconds(void *context) {
    (void)context;
    return sxn_system_time();
}

/* The whole file, replaced and made durable. */
static int save_registry_file(void *context, const char *text, size_t length) {
    (void)context;
    long handle = sxn_create_file(g_registry_path, SXN_CREATE_ALWAYS);
    if (handle < 0) {
        return 1;
    }
    size_t done = 0;
    while (done < length) {
        long n = sxn_write(handle, text + done, (unsigned long)(length - done));
        if (n <= 0) {
            break;
        }
        done += (size_t)n;
    }
    int ok = done == length && sxn_flush_file(handle) == 0;
    (void)sxn_close(handle);
    return ok ? 0 : 1;
}

/* ---- Files ------------------------------------------------------------------ */
/*
 * The guest's files over SUN's file calls: a file is a SUN handle, and a
 * read or write at a position seeks it first. A file is writable when any of
 * its write bits is set (the volume clears them all when it is read-only).
 */

static int fs_status(long error) {
    switch (-error) {
    case SAVANXP_ENOENT: return SXW_FS_NOT_FOUND;
    case SAVANXP_EEXIST: return SXW_FS_EXISTS;
    case SAVANXP_EACCES:
    case SAVANXP_EPERM:
    case SAVANXP_EROFS: return SXW_FS_DENIED;
    case SAVANXP_ENOTDIR: return SXW_FS_NOT_DIRECTORY;
    case SAVANXP_EISDIR: return SXW_FS_IS_DIRECTORY;
    case SAVANXP_ENOTEMPTY: return SXW_FS_NOT_EMPTY;
    case SAVANXP_EBUSY: return SXW_FS_BUSY;
    case SAVANXP_ENOSPC: return SXW_FS_FULL;
    case SAVANXP_EINVAL:
    case SAVANXP_ENAMETOOLONG: return SXW_FS_INVALID;
    default: return SXW_FS_IO;
    }
}

static void host_info(const struct sxn_file_info *in, sxw_host_info *out) {
    out->type = in->type == SXN_FILE_TYPE_FILE        ? SXW_HOST_FILE
                : in->type == SXN_FILE_TYPE_DIRECTORY ? SXW_HOST_DIRECTORY
                                                      : SXW_HOST_OTHER;
    out->writable = (in->mode & 0222u) != 0u;
    out->size = in->size;
    out->mtime = in->mtime;
}

static int fs_stat(void *context, const char *path, sxw_host_info *info) {
    (void)context;
    struct sxn_file_info raw;
    const long result = sxn_query_path(path, &raw);
    if (result != 0) {
        return fs_status(result);
    }
    host_info(&raw, info);
    return SXW_FS_OK;
}

static int fs_open(void *context, const char *path, uint32_t mode, long *file) {
    (void)context;
    long handle;
    if ((mode & SXW_OPEN_CREATE) != 0u) {
        handle = sxn_create_file(path, ((mode & SXW_OPEN_NEW) != 0u ? SXN_CREATE_NEW : SXN_CREATE_ALWAYS) |
                                           SXN_CREATE_READ);
    } else {
        handle = sxn_open_file(path, ((mode & SXW_OPEN_READ) != 0u ? SXN_FILE_READ : 0u) |
                                         ((mode & SXW_OPEN_WRITE) != 0u ? SXN_FILE_WRITE : 0u));
    }
    if (handle < 0) {
        return fs_status(handle);
    }
    *file = handle;
    return SXW_FS_OK;
}

static int fs_read(void *context, long file, uint64_t position, void *buffer, uint32_t count, uint32_t *done) {
    (void)context;
    *done = 0;
    const long at = sxn_seek_file(file, (int64_t)position, SXN_SEEK_START);
    if (at < 0) {
        return fs_status(at);
    }
    while (*done < count) {
        const long n = sxn_read(file, (uint8_t *)buffer + *done, count - *done);
        if (n < 0) {
            return fs_status(n);
        }
        if (n == 0) {
            break;
        }
        *done += (uint32_t)n;
    }
    return SXW_FS_OK;
}

static int fs_write(void *context, long file, uint64_t position, const void *buffer, uint32_t count,
                    uint32_t *done) {
    (void)context;
    *done = 0;
    const long at = sxn_seek_file(file, (int64_t)position, SXN_SEEK_START);
    if (at < 0) {
        return fs_status(at);
    }
    while (*done < count) {
        const long n = sxn_write(file, (const uint8_t *)buffer + *done, count - *done);
        if (n <= 0) {
            return n < 0 ? fs_status(n) : SXW_FS_IO;
        }
        *done += (uint32_t)n;
    }
    return SXW_FS_OK;
}

static int fs_set_size(void *context, long file, uint64_t size) {
    (void)context;
    const long result = sxn_set_file_size(file, size);
    return result == 0 ? SXW_FS_OK : fs_status(result);
}

static int fs_query(void *context, long file, sxw_host_info *info) {
    (void)context;
    struct sxn_file_info raw;
    const long result = sxn_query_file(file, &raw);
    if (result != 0) {
        return fs_status(result);
    }
    host_info(&raw, info);
    return SXW_FS_OK;
}

static int fs_flush(void *context, long file) {
    (void)context;
    const long result = sxn_flush_file(file);
    return result == 0 ? SXW_FS_OK : fs_status(result);
}

static void fs_close(void *context, long file) {
    (void)context;
    (void)sxn_close(file);
}

static int fs_result(long result) {
    return result == 0 ? SXW_FS_OK : fs_status(result);
}

static int fs_make_directory(void *context, const char *path) {
    (void)context;
    return fs_result(sxn_make_directory(path));
}

static int fs_remove_directory(void *context, const char *path) {
    (void)context;
    return fs_result(sxn_delete_directory(path));
}

static int fs_remove_file(void *context, const char *path) {
    (void)context;
    return fs_result(sxn_delete_path(path));
}

static int fs_rename(void *context, const char *old_path, const char *new_path) {
    (void)context;
    return fs_result(sxn_rename_path(old_path, new_path));
}

static int fs_list_open(void *context, const char *path, long *list) {
    (void)context;
    const long handle = sxn_open_directory(path);
    if (handle < 0) {
        return fs_status(handle);
    }
    *list = handle;
    return SXW_FS_OK;
}

static struct sxn_directory_entry g_entry;

static int fs_list_next(void *context, long list, char *name, sxw_host_info *info) {
    (void)context;
    for (;;) {
        const long result = sxn_read_directory(list, &g_entry);
        if (result != 1) {
            return 0;
        }
        uint32_t n = 0;
        while (g_entry.name[n] != 0 && n < 255u) {
            name[n] = g_entry.name[n];
            ++n;
        }
        if (g_entry.name[n] != 0) {
            continue; /* longer than a Windows name */
        }
        name[n] = 0;
        host_info(&g_entry.info, info);
        return 1;
    }
}

static void fs_list_close(void *context, long list) {
    (void)context;
    (void)sxn_close(list);
}

static const sxw_file_platform g_file_platform = {
    .stat = fs_stat,
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .set_size = fs_set_size,
    .query = fs_query,
    .flush = fs_flush,
    .close = fs_close,
    .make_directory = fs_make_directory,
    .remove_directory = fs_remove_directory,
    .remove_file = fs_remove_file,
    .rename = fs_rename,
    .list_open = fs_list_open,
    .list_next = fs_list_next,
    .list_close = fs_list_close,
};

/* ---- Windows and input ---------------------------------------------------- */
/*
 * The guest's first top-level window is a windowd window (sxnwm): the frame,
 * the caption and the window's place are the window manager's, the client
 * area is the surface, and input comes from the native ring. Its dialogs and
 * menus are windows it owns (docs/desktop/OWNED_WINDOWS.md): a dialog is an
 * owned window the window manager places, a menu a popup at a point of the
 * main window's client area; their input comes from the event pipe.
 */

#define SIZE_WAIT_MS 1000u

typedef struct host_window {
    struct sxnwm_client client;
    int open;
    int close_sent;
    /* The drag the window was last told about, which its answer is for
     * (docs/desktop/DRAG_AND_DROP.md); 0 for none. */
    uint32_t drag;
} host_window;

/* The main window is index 0 (guest id 1); owned window i, 1 to
 * SAVANXP_WM_MAX_OWNED_WINDOWS, is the window manager's owned window i and
 * the guest's id i + 1. */
#define HOST_WINDOWS (1u + SAVANXP_WM_MAX_OWNED_WINDOWS)
static host_window g_hosts[HOST_WINDOWS];
static uint64_t g_started_ns;

/* What the main window says about itself (sxnwm_set_presentation): its
 * title and, once the guest gives one, its icon. Sent whole each time. */
static char g_main_title[128];
static uint32_t g_main_icon[SXW_ICON_EXTENT * SXW_ICON_EXTENT];
static int g_main_has_icon;

static int present_main(void) {
    struct sxnwm_presentation_source presentation = {0};
    presentation.title = g_main_title;
    if (g_main_has_icon) {
        presentation.icon_extent = SXW_ICON_EXTENT;
        presentation.icon_pixels = g_main_icon;
    }
    return sxnwm_set_presentation(&g_hosts[0].client, &presentation) == 0 ? 0 : 1;
}

static void keep_title(const char *title) {
    uint32_t n = 0;
    while (title[n] != 0 && n + 1u < sizeof(g_main_title)) {
        g_main_title[n] = title[n];
        ++n;
    }
    g_main_title[n] = 0;
}

static uint32_t ticks(void *context) {
    (void)context;
    return (uint32_t)((sxn_clock_ns() - g_started_ns) / 1000000ull);
}

static void display(void *context, uint32_t *width, uint32_t *height) {
    (void)context;
    struct sxn_gfx_info info;
    if (sxn_gfx_info(&info) == 0 && info.width != 0u && info.height != 0u) {
        *width = info.width;
        *height = info.height;
    } else {
        *width = 1024u;
        *height = 768u;
    }
}

static host_window *host_of(uint32_t id) {
    return id >= 1u && id <= HOST_WINDOWS && g_hosts[id - 1u].open ? &g_hosts[id - 1u] : NULL;
}

static uint32_t create_main(const char *title, uint32_t *width, uint32_t *height) {
    struct sxnwm_client *wm = &g_hosts[0].client;
    if (sxnwm_connect(wm) != 0) {
        return 0;
    }
    g_hosts[0].open = 1;
    g_hosts[0].close_sent = 0;
    keep_title(title);
    (void)present_main();
    /* Ask for the client size and wait a little for the window manager's
     * answer; a later change arrives as a resize event. */
    if (wm->width != *width || wm->height != *height) {
        (void)sxnwm_request_size(wm, *width, *height);
        const uint64_t deadline = sxn_clock_ns() + (uint64_t)SIZE_WAIT_MS * 1000000ull;
        while (sxn_clock_ns() < deadline) {
            (void)sxnwm_check_resize(wm);
            if (wm->width == *width && wm->height == *height) {
                break;
            }
            (void)sxn_event_reset(wm->wake_fd);
            (void)sxn_wait_one(wm->wake_fd, 20);
        }
    }
    *width = wm->width;
    *height = wm->height;
    return 1u;
}

/* Every other window is owned by the main window, which is the only owner
 * the window manager knows; a second unowned window becomes owned too. */
static uint32_t window_create(void *context, const char *title, uint32_t *width, uint32_t *height, uint32_t owner,
                              uint32_t kind, int32_t x, int32_t y) {
    (void)context;
    (void)owner;
    if (!g_hosts[0].open) {
        return kind == SXW_WINDOW_MAIN ? create_main(title, width, height) : 0u;
    }
    for (uint32_t i = 1; i < HOST_WINDOWS; ++i) {
        if (g_hosts[i].open) {
            continue;
        }
        const int popup = kind == SXW_WINDOW_POPUP;
        const uint32_t flags = popup                         ? SAVANXP_WM_POPUP_DISMISS_ON_OUTSIDE
                               : kind == SXW_WINDOW_MODELESS ? SAVANXP_WM_WINDOW_MODELESS
                                                             : 0u;
        if (sxnwm_window_open_ex(&g_hosts[0].client, &g_hosts[i].client, i, *width, *height, popup ? 0 : title,
                                 popup ? SAVANXP_WM_WINDOW_ROLE_POPUP : SAVANXP_WM_WINDOW_ROLE_NORMAL, flags, x,
                                 y) != 0) {
            return 0;
        }
        g_hosts[i].open = 1;
        g_hosts[i].close_sent = 0;
        *width = g_hosts[i].client.width;
        *height = g_hosts[i].client.height;
        return i + 1u;
    }
    return 0;
}

static void window_destroy(void *context, uint32_t window) {
    (void)context;
    host_window *h = host_of(window);
    if (h == NULL) {
        return;
    }
    if (window != 1u) {
        (void)sxnwm_window_close(&g_hosts[0].client, &h->client, window - 1u);
        h->open = 0;
        return;
    }
    for (uint32_t i = 2; i <= HOST_WINDOWS; ++i) {
        window_destroy(context, i);
    }
    struct sxnwm_client *wm = &h->client;
    (void)sxnwm_wait_idle(wm, 200);
    (void)sxn_unmap_view((uint64_t)(uintptr_t)wm->ring);
    (void)sxn_unmap_view((uint64_t)(uintptr_t)wm->header);
    const long handles[4] = {wm->wake_fd, wm->submit_fd, wm->events_fd, wm->request_fd};
    for (int i = 0; i < 4; ++i) {
        if (handles[i] >= 0) {
            (void)sxn_close(handles[i]);
        }
    }
    h->open = 0;
}

static int window_title(void *context, uint32_t window, const char *title) {
    (void)context;
    host_window *h = host_of(window);
    if (h == NULL) {
        return 1;
    }
    if (window != 1u) {
        return sxnwm_set_title(&h->client, title) == 0 ? 0 : 1;
    }
    keep_title(title);
    return present_main();
}

/* The main window's icon; an owned window shows its owner's. */
static int window_icon(void *context, uint32_t window, const uint32_t *pixels) {
    (void)context;
    if (host_of(window) == NULL) {
        return 1;
    }
    if (window != 1u) {
        return 0;
    }
    for (uint32_t i = 0; i < SXW_ICON_EXTENT * SXW_ICON_EXTENT; ++i) {
        g_main_icon[i] = pixels[i];
    }
    g_main_has_icon = 1;
    return present_main();
}

static int window_present(void *context, uint32_t window, const uint32_t *pixels, uint32_t stride, uint32_t width,
                          uint32_t height, const int32_t *rects, uint32_t count) {
    (void)context;
    (void)width;
    (void)height;
    host_window *h = host_of(window);
    if (h == NULL) {
        return 1;
    }
    struct sxnwm_client *wm = &h->client;
    struct savanxp_gpu_dirty_rect dirty[64];
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count && kept < 64u; ++i) {
        const int32_t *r = &rects[4u * i];
        const uint32_t right = (uint32_t)r[2] < wm->width ? (uint32_t)r[2] : wm->width;
        const uint32_t bottom = (uint32_t)r[3] < wm->height ? (uint32_t)r[3] : wm->height;
        if ((uint32_t)r[0] >= right || (uint32_t)r[1] >= bottom) {
            continue;
        }
        for (uint32_t y = (uint32_t)r[1]; y < bottom; ++y) {
            uint32_t *out = wm->pixels + y * wm->stride;
            const uint32_t *in = pixels + y * stride;
            for (uint32_t x = (uint32_t)r[0]; x < right; ++x) {
                out[x] = in[x];
            }
        }
        dirty[kept].x = (uint32_t)r[0];
        dirty[kept].y = (uint32_t)r[1];
        dirty[kept].width = right - (uint32_t)r[0];
        dirty[kept].height = bottom - (uint32_t)r[1];
        ++kept;
    }
    if (kept == 0u) {
        return 0;
    }
    long sequence = sxnwm_submit_rects(wm, dirty, kept);
    if (sequence == -SAVANXP_EAGAIN) {
        (void)sxnwm_wait_idle(wm, 500);
        sequence = sxnwm_submit_rects(wm, dirty, kept);
    }
    return sequence >= 0 ? 0 : 1;
}

/* The keyboard layout the kernel applies (SAVANXP_KEYBOARD_LAYOUT_*): a
 * key arrives as the character it typed, and Shift with a digit types a
 * different one in each. */
static long g_layout = -1;

static long keyboard_layout(void) {
    if (g_layout < 0) {
        g_layout = SAVANXP_KEYBOARD_LAYOUT_ES;
        long device = sxn_open_device("input0", SXN_DEVICE_READ);
        if (device >= 0) {
            long layout = sxn_device_control(device, INPUT_IOC_GET_LAYOUT, 0);
            if (layout >= 0) {
                g_layout = layout;
            }
            (void)sxn_close(device);
        }
    }
    return g_layout;
}

/* A native key as a Windows virtual key, or 0. */
static uint32_t virtual_key(uint32_t key, int shift) {
    if (key >= 'a' && key <= 'z') {
        return key - 'a' + 'A';
    }
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        return key;
    }
    /* Shift and 0 to 9, by layout. */
    static const char k_shifted_es[] = "=!\"#$%&/()";
    static const char k_shifted_en[] = ")!@#$%^&*(";
    const char *shifted = keyboard_layout() == SAVANXP_KEYBOARD_LAYOUT_EN ? k_shifted_en : k_shifted_es;
    for (uint32_t i = 0; shift && i < 10u; ++i) {
        if (key == (uint32_t)(unsigned char)shifted[i]) {
            return '0' + i;
        }
    }
    if (key >= SAVANXP_KEY_F1 && key <= SAVANXP_KEY_F12) {
        return 0x70u + (key - SAVANXP_KEY_F1);
    }
    switch (key) {
    case ' ': return 0x20u;
    case SAVANXP_KEY_BACKSPACE: return 0x08u;
    case SAVANXP_KEY_TAB: return 0x09u;
    case SAVANXP_KEY_ENTER: return 0x0du;
    case SAVANXP_KEY_ESC: return 0x1bu;
    case SAVANXP_KEY_SHIFT: return 0x10u;
    case SAVANXP_KEY_CTRL: return 0x11u;
    case SAVANXP_KEY_ALT: return 0x12u;
    case SAVANXP_KEY_CAPSLOCK: return 0x14u;
    case SAVANXP_KEY_PAGE_UP: return 0x21u;
    case SAVANXP_KEY_PAGE_DOWN: return 0x22u;
    case SAVANXP_KEY_END: return 0x23u;
    case SAVANXP_KEY_HOME: return 0x24u;
    case SAVANXP_KEY_LEFT: return 0x25u;
    case SAVANXP_KEY_UP: return 0x26u;
    case SAVANXP_KEY_RIGHT: return 0x27u;
    case SAVANXP_KEY_DOWN: return 0x28u;
    case SAVANXP_KEY_INSERT: return 0x2du;
    case SAVANXP_KEY_DELETE: return 0x2eu;
    case ';': case ':': return 0xbau;
    case '=': case '+': return 0xbbu;
    case ',': case '<': return 0xbcu;
    case '-': case '_': return 0xbdu;
    case '.': case '>': return 0xbeu;
    case '/': case '?': return 0xbfu;
    case '`': case '~': return 0xc0u;
    case '[': case '{': return 0xdbu;
    case '\\': case '|': return 0xdcu;
    case ']': case '}': return 0xddu;
    case '\'': case '"': return 0xdeu;
    default: return 0;
    }
}

/* One input record of window id as an event; 0 when it is not one the guest
 * sees. A drag's end is the source's, and a Win32 program is never one. */
static int translate(const struct savanxp_wm_event *record, uint32_t id, sxw_event *event) {
    event->window = id;
    event->time = ticks(0);
    if (record->kind == SAVANXP_WM_EVENT_DRAG) {
        const struct savanxp_wm_drag_event *d = &record->payload.drag;
        host_window *host = id >= 1u && id <= HOST_WINDOWS ? &g_hosts[id - 1u] : NULL;
        if (d->phase < SAVANXP_WM_DRAG_ENTER || d->phase > SAVANXP_WM_DRAG_DROP || host == NULL) {
            return 0;
        }
        host->drag = d->phase == SAVANXP_WM_DRAG_LEAVE || d->phase == SAVANXP_WM_DRAG_DROP ? 0u : d->drag;
        event->kind = SXW_EVENT_DRAG;
        event->a = (uint32_t)d->x;
        event->b = (uint32_t)d->y;
        event->c = d->phase | ((d->effects & 0xffu) << 8) | ((d->effect & 0xffu) << 16);
        return 1;
    }
    if (record->kind == SAVANXP_WM_EVENT_POINTER) {
        event->kind = SXW_EVENT_POINTER;
        event->a = (uint32_t)record->payload.pointer.x;
        event->b = (uint32_t)record->payload.pointer.y;
        event->c = record->payload.pointer.buttons &
                   (SAVANXP_MOUSE_BUTTON_LEFT | SAVANXP_MOUSE_BUTTON_RIGHT | SAVANXP_MOUSE_BUTTON_MIDDLE);
        return 1;
    }
    if (record->kind == SAVANXP_WM_EVENT_KEY) {
        const struct savanxp_input_event *k = &record->payload.key;
        const uint32_t vk = virtual_key(k->key, (k->modifiers & SAVANXP_KEY_MOD_SHIFT) != 0u);
        if (vk == 0u || (k->type != SAVANXP_INPUT_EVENT_KEY_DOWN && k->type != SAVANXP_INPUT_EVENT_KEY_UP)) {
            return 0;
        }
        event->kind = SXW_EVENT_KEY;
        event->a = vk;
        event->b = k->type == SAVANXP_INPUT_EVENT_KEY_DOWN ? 1u : 0u;
        /* With Alt the character still goes along: it is the WM_SYSCHAR a
         * menu or a dialog looks its mnemonics up by. */
        const int control = (k->modifiers & SAVANXP_KEY_MOD_CTRL) != 0u;
        event->c = event->b && !control && k->ascii >= 0x20 && k->ascii < 0x100 && k->ascii != 0x7f
                       ? (uint32_t)k->ascii
                       : event->b && (vk == 0x08u || vk == 0x09u || vk == 0x0du || vk == 0x1bu) ? vk : 0u;
        return 1;
    }
    return 0;
}

static int simple_event(sxw_event *event, uint32_t kind, uint32_t id, uint32_t a, uint32_t b) {
    event->kind = kind;
    event->window = id;
    event->a = a;
    event->b = b;
    event->c = 0;
    event->time = ticks(0);
    return 1;
}

static int any_owned(void) {
    for (uint32_t i = 1; i < HOST_WINDOWS; ++i) {
        if (g_hosts[i].open) {
            return 1;
        }
    }
    return 0;
}

/* What is waiting, without waiting: 1 with an event. */
static int poll_window_events(sxw_event *event) {
    struct sxnwm_client *wm = &g_hosts[0].client;
    if (sxnwm_check_resize(wm)) {
        return simple_event(event, SXW_EVENT_RESIZE, 1u, wm->width, wm->height);
    }
    for (uint32_t i = 0; i < HOST_WINDOWS; ++i) {
        if (g_hosts[i].open && !g_hosts[i].close_sent && sxnwm_close_requested(&g_hosts[i].client)) {
            g_hosts[i].close_sent = 1;
            return simple_event(event, SXW_EVENT_CLOSE, i + 1u, 0, 0);
        }
    }
    struct savanxp_wm_event record;
    while (sxnwm_poll_input(wm, &record) == 0) {
        if (translate(&record, 1u, event)) {
            return 1;
        }
    }
    while (any_owned() && sxnwm_poll_owned_input(wm, &record) == 0) {
        const uint32_t id = record.window_id + 1u;
        if (record.window_id >= 1u && host_of(id) != NULL && translate(&record, id, event)) {
            return 1;
        }
    }
    return 0;
}

static int wait_event(void *context, uint32_t timeout, sxw_event *event) {
    (void)context;
    if (!g_hosts[0].open) {
        /* No window, so no input: only the time passes. */
        static long idle = -1;
        if (idle < 0) {
            idle = sxn_event_create(0);
        }
        if (timeout != 0u && idle >= 0) {
            (void)sxn_wait_one(idle, timeout == SXW_INFINITE ? 1000l : (long)timeout);
        }
        return 0;
    }
    struct sxnwm_client *wm = &g_hosts[0].client;
    const uint64_t start = sxn_clock_ns();
    for (;;) {
        if (poll_window_events(event)) {
            return 1;
        }
        const uint64_t waited_ms = (sxn_clock_ns() - start) / 1000000ull;
        if (timeout != SXW_INFINITE && waited_ms >= timeout) {
            return 0;
        }
        long slice = 100;
        if (timeout != SXW_INFINITE && (uint64_t)slice > timeout - waited_ms) {
            slice = (long)(timeout - waited_ms);
        }
        /* Reset, then look again: what arrives after the reset sets the
         * event, and the wait returns at once. */
        (void)sxn_event_reset(wm->wake_fd);
        if (wm->ring->tail < wm->ring->head) {
            continue;
        }
        int32_t handles[2] = {(int32_t)wm->wake_fd, (int32_t)wm->events_fd};
        (void)sxn_wait_many(handles, any_owned() ? 2u : 1u, slice);
    }
}

/* The guest's answer to the drag over one of its windows. */
static int drag_reply(void *context, uint32_t window, uint32_t effect) {
    (void)context;
    host_window *host = host_of(window);
    if (host == NULL || host->drag == 0u) {
        return -1;
    }
    return sxnwm_drag_reply(&host->client, host->drag, effect) == 0 ? 0 : -1;
}

static const sxw_window_platform g_window_platform = {
    .ticks = ticks,
    .display = display,
    .create = window_create,
    .destroy = window_destroy,
    .set_title = window_title,
    .set_icon = window_icon,
    .present = window_present,
    .wait_event = wait_event,
    .drag_reply = drag_reply,
};

/* ---- The clipboard ------------------------------------------------------------ */
/*
 * The system's clipboard through SUN's typed calls (sxn/sxnclip.h): each
 * call opens /dev/clipboard, does its one thing and closes it, as a copy
 * or a paste does, so winhost keeps no descriptor for it.
 */

static int clip_set(void *context, const sxw_clip_format *formats, uint32_t count) {
    (void)context;
    struct savanxp_clipboard_entry entries[SXW_CLIPBOARD_FORMATS];
    for (uint32_t i = 0; i < count; ++i) {
        unsigned k = 0;
        for (; formats[i].name[k] != 0 && k + 1 < SAVANXP_CLIPBOARD_NAME_CAPACITY; ++k) {
            entries[i].name[k] = formats[i].name[k];
        }
        for (; k < SAVANXP_CLIPBOARD_NAME_CAPACITY; ++k) {
            entries[i].name[k] = 0;
        }
        entries[i].data = (uint64_t)(uintptr_t)formats[i].data;
        entries[i].length = formats[i].length;
        entries[i].reserved0 = 0;
    }
    const long handle = sxn_clipboard_open(1);
    if (handle < 0) {
        return -1;
    }
    const long status = sxn_clipboard_set(handle, entries, count);
    (void)sxn_close(handle);
    return status == 0 ? 0 : -1;
}

static int clip_info(void *context, uint64_t *sequence, uint32_t *count) {
    (void)context;
    struct savanxp_clipboard_info info;
    const long handle = sxn_clipboard_open(0);
    if (handle < 0) {
        return -1;
    }
    const long status = sxn_clipboard_info(handle, &info);
    (void)sxn_close(handle);
    if (status != 0) {
        return -1;
    }
    *sequence = info.sequence;
    *count = info.format_count;
    return 0;
}

static int clip_format(void *context, uint32_t index, char *name, uint32_t *length) {
    (void)context;
    const long handle = sxn_clipboard_open(0);
    if (handle < 0) {
        return -1;
    }
    const long status = sxn_clipboard_format(handle, index, name, length);
    (void)sxn_close(handle);
    return status == 0 ? 0 : -1;
}

static long clip_get(void *context, const char *name, void *buffer, uint32_t capacity, uint32_t *length) {
    (void)context;
    const long handle = sxn_clipboard_open(0);
    if (handle < 0) {
        return -1;
    }
    const long copied = sxn_clipboard_get(handle, name, buffer, capacity, length);
    (void)sxn_close(handle);
    return copied >= 0 ? copied : -1;
}

static const sxw_clipboard_platform g_clipboard_platform = {
    .set = clip_set,
    .info = clip_info,
    .format = clip_format,
    .get = clip_get,
};

/* The drag's formats, /dev/dragdata (docs/desktop/DRAG_AND_DROP.md), read
 * the same way: a target reads, it never sets. */
static int drag_format(void *context, uint32_t index, char *name, uint32_t *length) {
    (void)context;
    const long handle = sxn_dragdata_open(0);
    if (handle < 0) {
        return -1;
    }
    const long status = sxn_clipboard_format(handle, index, name, length);
    (void)sxn_close(handle);
    return status == 0 ? 0 : -1;
}

static long drag_get(void *context, const char *name, void *buffer, uint32_t capacity, uint32_t *length) {
    (void)context;
    const long handle = sxn_dragdata_open(0);
    if (handle < 0) {
        return -1;
    }
    const long copied = sxn_clipboard_get(handle, name, buffer, capacity, length);
    (void)sxn_close(handle);
    return copied >= 0 ? copied : -1;
}

static const sxw_clipboard_platform g_dragdata_platform = {
    .format = drag_format,
    .get = drag_get,
};

static sxw_services g_services = {
    .process = &g_process,
    .write = write_stream,
    .now = now_seconds,
    .save_registry = save_registry_file,
    .registry = &g_registry,
    .windows = &g_window_platform,
    .files = &g_file_platform,
    .clipboard = &g_clipboard_platform,
    .dragdata = &g_dragdata_platform,
    .drive_c = DRIVE_C,
};

static void load_registry(void) {
    sxw_reg_init(&g_registry);
    file_buffer file;
    if (read_whole(g_registry_path, &file) == 0) {
        if (sxw_reg_parse(&g_registry, (const char *)file.data, (size_t)file.size) != 0) {
            say("win32: the registry file is malformed; starting empty\n");
        }
        free(file.data);
    }
}

/* The arguments as a command-line tail: each quoted when it holds a blank
 * or is empty, which is what a program splitting it gets back. */
static void join_arguments(char **arguments, int count, char *out, uint32_t capacity) {
    uint32_t n = 0;
    for (int i = 0; i < count; ++i) {
        const char *a = arguments[i];
        int quote = a[0] == 0;
        for (const char *c = a; *c != 0; ++c) {
            quote |= *c == ' ' || *c == '\t';
        }
        if (n + text_length(a) + 4u >= capacity) {
            break;
        }
        if (i != 0) {
            out[n++] = ' ';
        }
        if (quote) {
            out[n++] = '"';
        }
        for (const char *c = a; *c != 0; ++c) {
            out[n++] = *c;
        }
        if (quote) {
            out[n++] = '"';
        }
    }
    out[n] = 0;
}

static uint32_t parse_decimal(const char *text) {
    uint32_t value = 0;
    while (*text >= '0' && *text <= '9') {
        value = value * 10u + (uint32_t)(*text++ - '0');
    }
    return value;
}

static int same_text(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

uint32_t sxw_host_service(gate_state *gate) {
    const uint32_t result = sxw_service(&g_services, gate->index, (uint32_t)gate->guest_esp);
    gate->leave = (uint32_t)sxw_should_leave(&g_services, gate->index);
    return result;
}

/* ---- Set-up --------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void put64(uint8_t *p, uint64_t value) {
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static int build_trampoline(void) {
    if (map_section_at(TRAMPOLINE, PE_PAGE_SIZE) != 0) {
        return 1;
    }
    uint8_t *t = (uint8_t *)(uintptr_t)TRAMPOLINE;
    const uint32_t far_jumps[2][2] = {{TRAMPOLINE_BACK32, TRAMPOLINE_LANDING_RETURN},
                                      {TRAMPOLINE_SERVICE32, TRAMPOLINE_LANDING_SERVICE}};
    for (int i = 0; i < 2; ++i) {
        uint8_t *p = t + far_jumps[i][0];
        p[0] = 0xea; /* jmp far ptr16:32 */
        put32(p + 1, TRAMPOLINE + far_jumps[i][1]);
        p[5] = USER_CODE64_SELECTOR;
        p[6] = 0;
    }
    void (*const targets[2])(void) = {sxw_gate_return, sxw_gate_service};
    const uint32_t landings[2] = {TRAMPOLINE_LANDING_RETURN, TRAMPOLINE_LANDING_SERVICE};
    for (int i = 0; i < 2; ++i) {
        uint8_t *p = t + landings[i];
        p[0] = 0x49; /* movabs r11, target */
        p[1] = 0xbb;
        put64(p + 2, (uint64_t)(uintptr_t)targets[i]);
        p[10] = 0x41; /* jmp r11 */
        p[11] = 0xff;
        p[12] = 0xe3;
    }
    t[TRAMPOLINE_SVC_RET32 + 0] = 0xc2; /* ret 8: the gate's two arguments */
    t[TRAMPOLINE_SVC_RET32 + 1] = 0x08;
    t[TRAMPOLINE_SVC_RET32 + 2] = 0x00;
    sxw_svc_ret32 = TRAMPOLINE + TRAMPOLINE_SVC_RET32;
    return remap(find_mapping(TRAMPOLINE, PE_PAGE_SIZE), SXN_SECTION_READ | SXN_SECTION_EXEC);
}

/* Selectors before the FS base: loading FS resets it. */
static int use_teb(uint32_t teb) {
    const uint16_t data = USER_DATA_SELECTOR;
    __asm__ volatile("movw %0, %%ds\n\tmovw %0, %%es\n\tmovw %0, %%fs" : : "r"(data) : "memory");
    return sxn_set_thread_base(SXN_THREAD_BASE_FS, (void *)(uintptr_t)teb) == 0;
}

static __attribute__((noreturn)) void finish(uint32_t status) {
    say_hex("win32: exit status ", status);
    sxn_exit((int)status);
}

/* ---- Threads ------------------------------------------------------------------- */
/*
 * A guest thread is a SUN thread: its own selectors and FS base (the TEB),
 * and a native stack per thread slot, reused once the slot's previous
 * thread has ended for good.
 */

#define HOST_STACK (256u * 1024u)

static void *g_host_stacks[SXW_MAX_THREADS];
static long g_host_threads[SXW_MAX_THREADS];

static void wait_word(void *context, volatile uint32_t *word, uint32_t expected, uint32_t timeout) {
    (void)context;
    (void)sxn_wait_address(word, expected, 4, timeout == SXW_INFINITE ? -1 : (long)timeout);
}

static void wake_word(void *context, volatile uint32_t *word) {
    (void)context;
    (void)sxn_wake_address(word, SXN_WAKE_ALL);
}

static uint32_t milliseconds(void *context) {
    (void)context;
    return (uint32_t)(sxn_clock_ns() / 1000000u);
}

static void enter_guest(void *context, uint32_t index, uint32_t entry, uint32_t esp) {
    (void)context;
    gate_state *g = &sxw_gates[index];
    g->index = index;
    g->leave = 0;
    if (!use_teb(g_services.threads[index].teb)) {
        say("win32: cannot set a thread's TEB\n");
        sxn_exit(2);
    }
    put32((uint8_t *)(uintptr_t)esp, TRAMPOLINE + TRAMPOLINE_BACK32);
    sxw_gate_enter(entry, esp, g);
}

static void host_thread(void *argument) {
    const uint32_t index = (uint32_t)(uintptr_t)argument;
    if (sxw_thread_main(&g_services, index, enter_guest, 0)) {
        finish(g_services.exit_status);
    }
    sxn_thread_exit(0);
}

static int start_thread(void *context, uint32_t index) {
    (void)context;
    if (g_host_threads[index] > 0) {
        /* The slot's last thread gave up its turn; its stack is free once
         * it has ended. */
        (void)sxn_wait_one(g_host_threads[index], -1);
        (void)sxn_close(g_host_threads[index]);
        g_host_threads[index] = 0;
    }
    if (g_host_stacks[index] == 0) {
        g_host_stacks[index] = malloc(HOST_STACK);
        if (g_host_stacks[index] == 0) {
            return 1;
        }
    }
    void *top = (uint8_t *)g_host_stacks[index] + HOST_STACK;
    const long handle = sxn_thread_create(host_thread, (void *)(uintptr_t)index, top, 0);
    if (handle < 0) {
        return 1;
    }
    g_host_threads[index] = handle;
    return 0;
}

static const sxw_thread_platform g_thread_platform = {
    .start = start_thread,
    .wait_word = wait_word,
    .wake_word = wake_word,
    .milliseconds = milliseconds,
};

int main(int argc, char **argv) {
    const char *exe = 0;
    static char tail[2048];
    for (int i = 1; i < argc; ++i) {
        if (same_text(argv[i], "--dlls") && i + 1 < argc) {
            g_system_dir = argv[++i];
        } else if (same_text(argv[i], "--registry") && i + 1 < argc) {
            g_registry_path = argv[++i];
        } else if (same_text(argv[i], "--time") && i + 1 < argc) {
            g_services.fixed_time = parse_decimal(argv[++i]);
        } else if (argv[i][0] == '/') {
            exe = argv[i];
            join_arguments(argv + i + 1, argc - i - 1, tail, sizeof(tail));
            break;
        } else {
            break;
        }
    }
    if (exe == 0) {
        say("usage: winhost [--dlls DIR] [--registry FILE] [--time SECONDS] /path/to/program.exe [arguments...]\n");
        sxn_exit(2);
    }
    load_registry();
    uint32_t slash = 0;
    for (uint32_t i = 0; exe[i] != 0; ++i) {
        if (exe[i] == '/') {
            slash = i;
        }
    }
    if (slash >= sizeof(g_exe_dir)) {
        say("win32: path too long\n");
        sxn_exit(2);
    }
    for (uint32_t i = 0; i < slash; ++i) {
        g_exe_dir[i] = exe[i];
    }
    g_exe_dir[slash] = 0;

    if (build_trampoline() != 0) {
        say("win32: cannot set up the gate\n");
        sxn_exit(2);
    }
    sxw_platform platform = {
        .context = 0,
        .guest_low = 0x400000u, /* kUserBase: where the SDK puts an image */
        .map = platform_map,
        .protect = platform_protect,
        .unmap = platform_unmap,
        .open_image = platform_open,
        .close_image = platform_close,
        .gate = TRAMPOLINE + TRAMPOLINE_SERVICE32,
        .drive_c = DRIVE_C,
    };
    if (sxw_load(&g_process, &platform, exe, tail) != 0) {
        say("win32: ");
        say(g_process.diagnostic);
        say("\n");
        if (live_sections() != 0u || g_open_files != 0u) {
            say("win32: the failed load left sections or files behind\n");
            sxn_exit(124);
        }
        finish(g_process.status);
    }
    if (!use_teb(g_process.teb)) {
        say("win32: cannot set the TEB\n");
        sxn_exit(2);
    }

    /* ntdll!SxwProcessStart(start block), returning to back32. */
    uint32_t esp = g_process.stack_top - 16u;
    put32((uint8_t *)(uintptr_t)esp, TRAMPOLINE + TRAMPOLINE_BACK32);
    put32((uint8_t *)(uintptr_t)(esp + 4u), g_process.start_block);
    g_started_ns = sxn_clock_ns();
    g_services.thread_platform = &g_thread_platform;
    sxw_services_start(&g_services);
    sxw_gates[0].index = 0;
    uint32_t returned = sxw_gate_enter(g_process.start_entry, esp, &sxw_gates[0]);
    if (!g_services.exit_requested) {
        if (g_services.threads[0].leaving) {
            /* The first thread ended and others run on. */
            sxw_turn_give(&g_services);
            sxn_thread_exit(0);
        }
        say_hex("win32: SxwProcessStart returned without exiting: ", returned);
        sxn_exit(2);
    }
    finish(g_services.exit_status);
}
