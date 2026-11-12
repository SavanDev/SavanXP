/*
 * harness.c -- win32-harness: run a PE32 executable on the Linux x86-64
 * build host through the same loader and DLLs as winhost.
 *
 *     win32-harness [--dlls DIR] program.exe
 *
 * DLLs are looked up next to the executable, then in DIR. The guest's
 * standard output and error are this process's. The guest's 32-bit exit
 * status goes to standard error as `win32: exit status 0x...`, and its low
 * byte is this process's exit status. A load that fails prints
 * `win32: <cause>` and ends the same way with the NT status Windows XP gives
 * that failure (0xc0000135 for a missing DLL, 0xc0000139 for a missing
 * function). It is also checked to have released every mapping and file it
 * took; if not, the harness exits with 124.
 *
 * See docs/userland/WIN32.md, "The host harness".
 */
#define _GNU_SOURCE
#include <asm/ldt.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "loader.h"
#include "sxw_abi.h"

#define TRAMPOLINE 0x7ffe0000u
#define TRAMPOLINE_BACK32 0x00u
#define TRAMPOLINE_LANDING_RETURN 0x10u
#define TRAMPOLINE_SERVICE32 0x20u
#define TRAMPOLINE_LANDING_SERVICE 0x30u
#define TRAMPOLINE_SVC_RET32 0x40u
#define LINUX_CODE64 0x33u
#define TEB_LDT_ENTRY 0
#define TEB_SELECTOR ((TEB_LDT_ENTRY << 3) | 4 | 3) /* LDT, ring 3 */

/* State shared with gate_x86_64.S. */
uint64_t sxw_host_rsp;
uint64_t sxw_host_fsbase;
uint64_t sxw_guest_esp;
uint64_t sxw_svc_ret32;
uint32_t sxw_guest_regs[4];
uint32_t sxw_result;
uint32_t sxw_exit_requested;
uint16_t sxw_teb_selector = TEB_SELECTOR;

uint32_t sxw_gate_enter(uint32_t entry, uint32_t guest_esp);
void sxw_gate_return(void);
void sxw_gate_service(void);

static sxw_process g_process;
static const char *g_dll_dir;
static char g_exe_dir[4096];
static uint64_t g_live_bytes;
static int g_open_files;
static uint32_t g_exit_status;

/* ---- Platform ----------------------------------------------------------- */

static int platform_map(void *context, uint32_t base, uint32_t size) {
    (void)context;
    void *want = (void *)(uintptr_t)base;
    void *got = mmap(want, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (got == MAP_FAILED) {
        return 1;
    }
    if (got != want) {
        munmap(got, size);
        return 1;
    }
    g_live_bytes += size;
    return 0;
}

static int platform_protect(void *context, uint32_t base, uint32_t size, pe_protection protection) {
    (void)context;
    int flags = protection == PE_PROT_RX ? PROT_READ | PROT_EXEC
                : protection == PE_PROT_RW ? PROT_READ | PROT_WRITE
                                           : PROT_READ;
    return mprotect((void *)(uintptr_t)base, size, flags) == 0 ? 0 : 1;
}

static void platform_unmap(void *context, uint32_t base, uint32_t size) {
    (void)context;
    munmap((void *)(uintptr_t)base, size);
    g_live_bytes -= size;
}

typedef struct file_buffer {
    uint8_t *data;
    size_t size;
} file_buffer;

static int read_whole(const char *path, file_buffer *out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (1 << 28)) {
        close(fd);
        return 1;
    }
    out->size = (size_t)st.st_size;
    out->data = malloc(out->size);
    size_t done = 0;
    while (out->data != NULL && done < out->size) {
        ssize_t n = read(fd, out->data + done, out->size - done);
        if (n <= 0) {
            break;
        }
        done += (size_t)n;
    }
    close(fd);
    if (out->data == NULL || done != out->size) {
        free(out->data);
        return 1;
    }
    return 0;
}

static int platform_open(void *context, const char *name, const uint8_t **data, size_t *size,
                         void **handle) {
    (void)context;
    file_buffer *file = calloc(1, sizeof(*file));
    if (file == NULL) {
        return 1;
    }
    int found;
    if (strchr(name, '/') != NULL) {
        found = read_whole(name, file) == 0;
    } else {
        /* Next to the executable, then the DLL folder; names are installed
         * lower-case. */
        char lower[256];
        size_t n = strlen(name);
        if (n >= sizeof(lower)) {
            free(file);
            return 1;
        }
        for (size_t i = 0; i <= n; ++i) {
            lower[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] - 'A' + 'a' : name[i]);
        }
        char path[4096 + 256];
        snprintf(path, sizeof(path), "%s/%s", g_exe_dir, lower);
        found = read_whole(path, file) == 0;
        if (!found && g_dll_dir != NULL) {
            snprintf(path, sizeof(path), "%s/%s", g_dll_dir, lower);
            found = read_whole(path, file) == 0;
        }
    }
    if (!found) {
        free(file);
        return 1;
    }
    *data = file->data;
    *size = file->size;
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

/* ---- Services ------------------------------------------------------------ */

/* Whether [address, address + length) lies inside one region of the guest. */
static int guest_range(uint32_t address, uint32_t length) {
    for (uint32_t i = 0; i < g_process.region_count; ++i) {
        const sxw_region *r = &g_process.regions[i];
        if (address >= r->base && address - r->base <= r->size && length <= r->size - (address - r->base)) {
            return 1;
        }
    }
    return 0;
}

static int guest_arguments(uint32_t address, uint32_t count, uint32_t *out) {
    if (count == 0) {
        return 1;
    }
    if (!guest_range(address, 4u * count)) {
        return 0;
    }
    memcpy(out, (const void *)(uintptr_t)address, 4u * count);
    return 1;
}

static int handle_fd(uint32_t handle) {
    switch (handle) {
    case 3: return 0;
    case 7: return 1;
    case 11: return 2;
    default: return -1;
    }
}

uint32_t sxw_host_service(uint64_t guest_esp) {
    uint32_t frame[3];
    if (!guest_arguments((uint32_t)guest_esp, 3, frame)) {
        sxw_exit_requested = 1;
        g_exit_status = 0xc0000005u;
        return 0;
    }
    uint32_t service = frame[1];
    uint32_t a[3];
    switch (service) {
    case SXW_SVC_VERSION:
        return SXW_ABI_VERSION;
    case SXW_SVC_EXIT_PROCESS:
        if (!guest_arguments(frame[2], 1, a)) {
            a[0] = 0xc0000005u;
        }
        g_exit_status = a[0];
        sxw_exit_requested = 1;
        return 0;
    case SXW_SVC_STD_HANDLE:
        /* The console handles Windows XP gives a console program. */
        if (!guest_arguments(frame[2], 1, a) || a[0] > 2) {
            return 0;
        }
        return 3u + 4u * a[0];
    case SXW_SVC_WRITE: {
        if (!guest_arguments(frame[2], 3, a) || !guest_range(a[1], a[2])) {
            return SXW_FAILED;
        }
        int fd = handle_fd(a[0]);
        if (fd < 0) {
            return SXW_FAILED;
        }
        uint32_t done = 0;
        while (done < a[2]) {
            ssize_t n = write(fd, (const char *)(uintptr_t)a[1] + done, a[2] - done);
            if (n <= 0) {
                return done != 0 ? done : SXW_FAILED;
            }
            done += (uint32_t)n;
        }
        return done;
    }
    case SXW_SVC_DEBUG_OUTPUT:
        if (guest_arguments(frame[2], 2, a) && guest_range(a[0], a[1])) {
            dprintf(2, "win32: debug: %.*s\n", (int)a[1], (const char *)(uintptr_t)a[0]);
        }
        return 0;
    default:
        dprintf(2, "win32: unknown service %u\n", service);
        sxw_exit_requested = 1;
        g_exit_status = 0xc000001cu; /* STATUS_INVALID_SYSTEM_SERVICE */
        return 0;
    }
}

/* ---- Faults ------------------------------------------------------------- */

/* FS may still hold the TEB selector here, so no libc: raw system calls. */
static long raw_syscall3(long number, long a, long b, long c) {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return result;
}

static void raw_hex(char *out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        unsigned nibble = (unsigned)(value >> (28 - 4 * i)) & 0xfu;
        out[i] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
}

static void on_fault(int signal, siginfo_t *info, void *context) {
    (void)signal;
    ucontext_t *uc = context;
    char line[] = "win32: guest fault at eip 0x________, address 0x________\n";
    raw_hex(line + 28, (uint64_t)uc->uc_mcontext.gregs[REG_RIP]);
    raw_hex(line + 49, (uint64_t)(uintptr_t)info->si_addr);
    raw_syscall3(SYS_write, 2, (long)line, (long)sizeof(line) - 1);
    raw_syscall3(SYS_exit_group, 125, 0, 0);
}

/* ---- Set-up ------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static void put64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
}

static int build_trampoline(void) {
    if (platform_map(NULL, TRAMPOLINE, 0x1000) != 0) {
        return 1;
    }
    g_live_bytes -= 0x1000; /* not the loader's */
    uint8_t *t = (uint8_t *)(uintptr_t)TRAMPOLINE;
    /* back32 and service32: jmp far 0x33:landing */
    const uint32_t far_jumps[2][2] = {{TRAMPOLINE_BACK32, TRAMPOLINE_LANDING_RETURN},
                                      {TRAMPOLINE_SERVICE32, TRAMPOLINE_LANDING_SERVICE}};
    for (int i = 0; i < 2; ++i) {
        uint8_t *p = t + far_jumps[i][0];
        p[0] = 0xea;
        put32(p + 1, TRAMPOLINE + far_jumps[i][1]);
        p[5] = LINUX_CODE64;
        p[6] = 0;
    }
    /* The landings: movabs r11, target; jmp r11 */
    const struct {
        uint32_t at;
        void (*target)(void);
    } landings[2] = {{TRAMPOLINE_LANDING_RETURN, sxw_gate_return},
                     {TRAMPOLINE_LANDING_SERVICE, sxw_gate_service}};
    for (int i = 0; i < 2; ++i) {
        uint8_t *p = t + landings[i].at;
        p[0] = 0x49;
        p[1] = 0xbb;
        put64(p + 2, (uint64_t)(uintptr_t)landings[i].target);
        p[10] = 0x41;
        p[11] = 0xff;
        p[12] = 0xe3;
    }
    /* svc_ret32: ret 8, the gate's two stdcall arguments */
    t[TRAMPOLINE_SVC_RET32 + 0] = 0xc2;
    t[TRAMPOLINE_SVC_RET32 + 1] = 0x08;
    t[TRAMPOLINE_SVC_RET32 + 2] = 0x00;
    sxw_svc_ret32 = TRAMPOLINE + TRAMPOLINE_SVC_RET32;
    return mprotect(t, 0x1000, PROT_READ | PROT_EXEC) == 0 ? 0 : 1;
}

static int set_teb_segment(uint32_t teb) {
    struct user_desc d;
    memset(&d, 0, sizeof(d));
    d.entry_number = TEB_LDT_ENTRY;
    d.base_addr = teb;
    d.limit = SXW_TEB_SIZE - 1;
    d.seg_32bit = 1;
    d.contents = 0; /* data, expand-up */
    d.read_exec_only = 0;
    d.limit_in_pages = 0;
    d.seg_not_present = 0;
    d.useable = 1;
    return syscall(SYS_modify_ldt, 1, &d, sizeof(d)) == 0 ? 0 : 1;
}

static void usage(void) {
    fprintf(stderr, "usage: win32-harness [--dlls DIR] program.exe\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *exe = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dlls") == 0 && i + 1 < argc) {
            g_dll_dir = argv[++i];
        } else if (exe == NULL && argv[i][0] != '-') {
            exe = argv[i];
        } else {
            usage();
        }
    }
    if (exe == NULL) {
        usage();
    }
    const char *slash = strrchr(exe, '/');
    if (slash == NULL) {
        snprintf(g_exe_dir, sizeof(g_exe_dir), ".");
    } else {
        snprintf(g_exe_dir, sizeof(g_exe_dir), "%.*s", (int)(slash - exe), exe);
    }

    if (syscall(SYS_arch_prctl, 0x1003 /* ARCH_GET_FS */, &sxw_host_fsbase) != 0 ||
        build_trampoline() != 0) {
        fprintf(stderr, "win32: cannot set up the gate\n");
        return 126;
    }

    sxw_platform platform = {
        .context = NULL,
        .map = platform_map,
        .protect = platform_protect,
        .unmap = platform_unmap,
        .open_image = platform_open,
        .close_image = platform_close,
        .gate = TRAMPOLINE + TRAMPOLINE_SERVICE32,
    };
    if (sxw_load(&g_process, &platform, exe) != 0) {
        fprintf(stderr, "win32: %s\n", g_process.diagnostic);
        if (g_live_bytes != 0 || g_open_files != 0) {
            fprintf(stderr, "win32: failed load left %llu bytes mapped and %d files open\n",
                    (unsigned long long)g_live_bytes, g_open_files);
            return 124;
        }
        fprintf(stderr, "win32: exit status 0x%08x\n", g_process.status);
        return (int)(g_process.status & 0xffu);
    }
    if (g_open_files != 0) {
        fprintf(stderr, "win32: %d files left open after the load\n", g_open_files);
        return 124;
    }
    if (set_teb_segment(g_process.teb) != 0) {
        fprintf(stderr, "win32: modify_ldt failed\n");
        return 126;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGFPE, &action, NULL);

    /* ntdll!SxwProcessStart(start block), returning to back32. */
    uint32_t esp = g_process.stack_top - 16u;
    put32((uint8_t *)(uintptr_t)esp, TRAMPOLINE + TRAMPOLINE_BACK32);
    put32((uint8_t *)(uintptr_t)(esp + 4u), g_process.start_block);
    uint32_t returned = sxw_gate_enter(g_process.start_entry, esp);
    if (!sxw_exit_requested) {
        fprintf(stderr, "win32: SxwProcessStart returned 0x%08x without exiting\n", returned);
        return 125;
    }
    fprintf(stderr, "win32: exit status 0x%08x\n", g_exit_status);
    sxw_unload(&g_process);
    return (int)(g_exit_status & 0xffu);
}
