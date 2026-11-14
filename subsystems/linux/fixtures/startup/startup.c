/* L1 fixture "startup": the Linux process start-up state, libc-free.
 *
 * Prints, in a form that is identical on Linux and on SavanXP for the same
 * arguments and environment:
 *   - argc, argv and envp as found on the initial stack;
 *   - which general registers were not zero at the entry point;
 *   - the stack alignment at the entry point;
 *   - the auxiliary vector entries the subsystem promises, checked against
 *     the executable's own headers where a value is predictable;
 *   - the result of regcheck(): write calls with sentinels in every register
 *     the ABI preserves, many times, so preemption lands between them.
 * It ends with exit_group(3); the hello fixture covers exit.
 *
 *   clang -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector \
 *         -O1 -fuse-ld=lld -o startup entry.S startup.c
 */

typedef unsigned long u64;

extern u64 regcheck(u64 iterations, void *buffer);
extern char _start[];
extern char __ehdr_start[];

enum {
    SYS_write = 1,
    SYS_exit_group = 231,
};

enum {
    AT_NULL = 0,
    AT_PHDR = 3,
    AT_PHENT = 4,
    AT_PHNUM = 5,
    AT_PAGESZ = 6,
    AT_BASE = 7,
    AT_ENTRY = 9,
    AT_UID = 11,
    AT_EUID = 12,
    AT_GID = 13,
    AT_EGID = 14,
    AT_SECURE = 23,
    AT_RANDOM = 25,
    AT_EXECFN = 31,
};

static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static u64 len(const char *s)
{
    u64 n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void puts_raw(const char *s)
{
    sys3(SYS_write, 1, (long)s, (long)len(s));
}

static void put_dec(u64 v)
{
    char buf[24];
    int i = sizeof buf;
    buf[--i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    puts_raw(buf + i);
}

static void line(const char *a, const char *b)
{
    puts_raw(a);
    puts_raw(b);
    puts_raw("\n");
}

static void line_dec(const char *a, u64 v)
{
    puts_raw(a);
    put_dec(v);
    puts_raw("\n");
}

struct elf_header {
    unsigned char ident[16];
    unsigned short type, machine;
    unsigned int version;
    u64 entry, phoff, shoff;
    unsigned int flags;
    unsigned short ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

static const char *const reg_names[16] = {
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8",
    "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rsp",
};

__attribute__((noreturn)) void startup_main(u64 *sp, const u64 *regs)
{
    u64 argc = sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    u64 envc = 0;
    u64 *auxv;
    u64 seen = 0;
    u64 val[32];
    const struct elf_header *eh = (const struct elf_header *)__ehdr_start;
    int nonzero = 0;
    static char empty[1];

    line_dec("argc=", argc);
    for (u64 i = 0; i < argc; i++) {
        puts_raw("argv[");
        put_dec(i);
        line("]=", argv[i]);
    }
    if (argv[argc] != 0) {
        puts_raw("argv not terminated\n");
    }
    while (envp[envc]) {
        line("env=", envp[envc]);
        envc++;
    }
    line_dec("envc=", envc);

    puts_raw("nonzero registers at entry:");
    for (int i = 0; i < 15; i++) {
        if (regs[i] != 0) {
            puts_raw(" ");
            puts_raw(reg_names[i]);
            nonzero = 1;
        }
    }
    puts_raw(nonzero ? "\n" : " none\n");
    line_dec("rsp mod 16 at entry=", regs[15] % 16);

    auxv = (u64 *)(envp + envc + 1);
    for (u64 *a = auxv; a[0] != AT_NULL; a += 2) {
        if (a[0] < 32) {
            seen |= 1ul << a[0];
            val[a[0]] = a[1];
        }
    }

    static const struct {
        int type;
        const char *name;
    } required[] = {
        {AT_PHDR, "AT_PHDR"},     {AT_PHENT, "AT_PHENT"},   {AT_PHNUM, "AT_PHNUM"},
        {AT_PAGESZ, "AT_PAGESZ"}, {AT_BASE, "AT_BASE"},     {AT_ENTRY, "AT_ENTRY"},
        {AT_UID, "AT_UID"},       {AT_EUID, "AT_EUID"},     {AT_GID, "AT_GID"},
        {AT_EGID, "AT_EGID"},     {AT_SECURE, "AT_SECURE"}, {AT_RANDOM, "AT_RANDOM"},
        {AT_EXECFN, "AT_EXECFN"},
    };
    for (u64 i = 0; i < sizeof required / sizeof required[0]; i++) {
        int t = required[i].type;
        puts_raw(required[i].name);
        if (!(seen & (1ul << t))) {
            puts_raw(" missing\n");
            continue;
        }
        switch (t) {
        case AT_PHDR:
            puts_raw(val[t] == (u64)__ehdr_start + eh->phoff ? " ok\n" : " wrong\n");
            break;
        case AT_PHENT:
            puts_raw(val[t] == eh->phentsize ? " ok\n" : " wrong\n");
            break;
        case AT_PHNUM:
            puts_raw(val[t] == eh->phnum ? " ok\n" : " wrong\n");
            break;
        case AT_PAGESZ:
            line_dec("=", val[t]);
            break;
        case AT_BASE:
            line_dec("=", val[t]);
            break;
        case AT_ENTRY:
            puts_raw(val[t] == (u64)_start ? " ok\n" : " wrong\n");
            break;
        case AT_SECURE:
            line_dec("=", val[t]);
            break;
        case AT_RANDOM: {
            /* Sixteen readable bytes; reading them is the check. */
            const unsigned char *r = (const unsigned char *)val[t];
            unsigned sum = 0;
            for (int k = 0; k < 16; k++) {
                sum += r[k];
            }
            (void)sum;
            puts_raw(" ok\n");
            break;
        }
        case AT_EXECFN:
            puts_raw(len((const char *)val[t]) != 0 ? " ok\n" : " empty\n");
            break;
        default:
            puts_raw(" present\n");
            break;
        }
    }

    line_dec("regcheck failures=", regcheck(2000000, empty));
    puts_raw("exit_group 3\n");
    sys3(SYS_exit_group, 3, 0, 0);
    for (;;) {
    }
}
