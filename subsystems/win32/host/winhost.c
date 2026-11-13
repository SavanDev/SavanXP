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
 * guest clock's start, so a run that seeds from the time repeats.
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
 * thread (docs/kernel/ARCH.md#segments). Single thread only.
 */
#include <stdint.h>
#include <stdlib.h>

#include "abi/savanxp_native_abi.h"
#include "sxn/sxn.h"

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

uint64_t sxw_host_rsp;
uint64_t sxw_guest_esp;
uint64_t sxw_svc_ret32;
uint32_t sxw_guest_regs[4];
uint32_t sxw_result;
uint32_t sxw_exit_requested;

uint32_t sxw_gate_enter(uint32_t entry, uint32_t guest_esp);
void sxw_gate_return(void);
void sxw_gate_service(void);
uint32_t sxw_host_service(uint64_t guest_esp);

/* sxw_gate_enter(entry, guest_esp): guest_esp points at
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
    "    mov %rsp, sxw_host_rsp(%rip)\n"
    "    mov %edi, %edi\n"
    "    mov %esi, %esp\n"
    "    pushq $0x2b\n"
    "    push %rdi\n"
    "    lretq\n"
    "    .globl sxw_gate_return\n"
    "sxw_gate_return:\n"
    "    mov %esp, %esp\n"
    "    mov %eax, sxw_result(%rip)\n"
    "sxw_gate_leave:\n"
    "    mov sxw_host_rsp(%rip), %rsp\n"
    "    add $8, %rsp\n"
    "    pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n"
    "    movl sxw_result(%rip), %eax\n"
    "    ret\n"
    "    .globl sxw_gate_service\n"
    "sxw_gate_service:\n"
    "    mov %esp, %eax\n"
    "    mov %rax, sxw_guest_esp(%rip)\n"
    "    mov %ebx, sxw_guest_regs + 0(%rip)\n"
    "    mov %ebp, sxw_guest_regs + 4(%rip)\n"
    "    mov %esi, sxw_guest_regs + 8(%rip)\n"
    "    mov %edi, sxw_guest_regs + 12(%rip)\n"
    "    mov sxw_host_rsp(%rip), %rsp\n"
    "    mov sxw_guest_esp(%rip), %rdi\n"
    "    call sxw_host_service\n"
    "    mov %eax, sxw_result(%rip)\n"
    "    cmpl $0, sxw_exit_requested(%rip)\n"
    "    jne sxw_gate_leave\n"
    "    mov sxw_guest_regs + 0(%rip), %ebx\n"
    "    mov sxw_guest_regs + 4(%rip), %ebp\n"
    "    mov sxw_guest_regs + 8(%rip), %esi\n"
    "    mov sxw_guest_regs + 12(%rip), %edi\n"
    "    mov sxw_svc_ret32(%rip), %r11\n"
    "    mov sxw_guest_esp(%rip), %rsp\n"
    "    movl sxw_result(%rip), %eax\n"
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

static int platform_open(void *context, const char *name, const uint8_t **data, size_t *size,
                         void **handle) {
    (void)context;
    file_buffer *file = malloc(sizeof(*file));
    if (file == 0) {
        return 1;
    }
    int found = 0;
    if (name[0] == '/') {
        found = read_whole(name, file) == 0;
    } else {
        char path[512];
        found = join(path, sizeof(path), g_exe_dir, name) && read_whole(path, file) == 0;
        if (!found) {
            found = join(path, sizeof(path), g_system_dir, name) && read_whole(path, file) == 0;
        }
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

static sxw_services g_services = {
    .process = &g_process,
    .write = write_stream,
    .now = now_seconds,
    .save_registry = save_registry_file,
    .registry = &g_registry,
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

uint32_t sxw_host_service(uint64_t guest_esp) {
    uint32_t result = sxw_service(&g_services, (uint32_t)guest_esp);
    if (g_services.exit_requested) {
        sxw_exit_requested = 1;
    }
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
        .guest_low = 0x400000u, /* kUserBase: the user half starts there */
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
    sxw_services_start(&g_services);
    uint32_t returned = sxw_gate_enter(g_process.start_entry, esp);
    if (!sxw_exit_requested) {
        say_hex("win32: SxwProcessStart returned without exiting: ", returned);
        sxn_exit(2);
    }
    finish(g_services.exit_status);
}
