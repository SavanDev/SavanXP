/* L2 fixture "regions": the edges of anonymous memory, libc-free.
 *
 * Each line names a case and prints a value: a result that is an error
 * number, or 1 and 0 for a property ("reads zero", "same address"). It
 * predicts nothing: the lines Linux prints for this file are the reference,
 * and SSL must print the same ones (subsystems/linux/profile/expected).
 *
 *   - a hole punched in a mapping whose pages were touched, and the kernel
 *     refusing to read from it;
 *   - MAP_FIXED over touched pages, which come back zeroed;
 *   - one munmap across several mappings and a hole, and over nothing;
 *   - the kernel reading a page nobody touched yet, and refusing PROT_NONE;
 *   - mremap growing in place, refused without MREMAP_MAYMOVE when the next
 *     page is taken, moving with it, shrinking, and on an unmapped address;
 *   - brk growing, shrinking and growing again onto fresh pages;
 *   - the argument errors of mmap, munmap and mremap.
 *
 * One line holds a NUL byte: a write from an untouched page sends its zero.
 * It ends with exit_group(0).
 *
 *   clang -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector \
 *         -O1 -fuse-ld=lld -o regions regions.c
 */

#define PAGE 4096L
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MREMAP_MAYMOVE 1
#define RW (PROT_READ | PROT_WRITE)
#define ANON (MAP_PRIVATE | MAP_ANONYMOUS)

static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

static long map(long address, long length, long prot, long flags)
{
    return sys6(9, address, length, prot, flags, -1, 0);
}

static long unmap(long address, long length)
{
    return sys3(11, address, length, 0);
}

static long remap(long address, long old_length, long new_length, long flags)
{
    return sys6(25, address, old_length, new_length, flags, 0, 0);
}

static long brk(long address)
{
    return sys3(12, address, 0, 0);
}

static long write_out(long address, long length)
{
    return sys3(1, 1, address, length);
}

static int failed(long result)
{
    return result < 0 && result > -4096;
}

/* 0 for a success, the negative error otherwise. */
static long error_of(long result)
{
    return failed(result) ? result : 0;
}

static void fill(long address, long length, unsigned char value)
{
    volatile unsigned char *p = (volatile unsigned char *)address;
    for (long i = 0; i < length; i++) {
        p[i] = value;
    }
}

static int all(long address, long length, unsigned char value)
{
    volatile unsigned char *p = (volatile unsigned char *)address;
    for (long i = 0; i < length; i++) {
        if (p[i] != value) {
            return 0;
        }
    }
    return 1;
}

static long len(const char *s)
{
    long n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void out(const char *s)
{
    write_out((long)s, len(s));
}

static void line(const char *what, long value)
{
    char digits[24];
    int at = 23;
    unsigned long magnitude = value < 0 ? -(unsigned long)value : (unsigned long)value;
    digits[at] = 0;
    do {
        digits[--at] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (value < 0) {
        digits[--at] = '-';
    }
    out(what);
    out(" ");
    out(digits + at);
    out("\n");
}

__attribute__((force_align_arg_pointer, noreturn)) void _start(void)
{
    /* Four pages, touched; then a hole in the middle. */
    long a = map(0, 4 * PAGE, RW, ANON);
    line("mmap four pages", error_of(a));
    line("they read zero", all(a, 4 * PAGE, 0));
    fill(a, 4 * PAGE, 0x5a);
    line("munmap the second page", unmap(a + PAGE, PAGE));
    line("the first page kept its bytes", all(a, PAGE, 0x5a));
    line("the last two kept theirs", all(a + 2 * PAGE, 2 * PAGE, 0x5a));
    line("write from the hole", write_out(a + PAGE, 1));

    /* MAP_FIXED over a touched page. */
    line("MAP_FIXED over the third page", map(a + 2 * PAGE, PAGE, RW, ANON | MAP_FIXED) == a + 2 * PAGE);
    line("it reads zero", all(a + 2 * PAGE, PAGE, 0));
    line("the fourth kept its bytes", all(a + 3 * PAGE, PAGE, 0x5a));

    /* One munmap over the pieces and the hole, then over nothing. */
    line("munmap all four", unmap(a, 4 * PAGE));
    line("write from the first page after it", write_out(a, 1));
    line("munmap of nothing", unmap(a, 4 * PAGE));

    /* The kernel reads a page nobody touched: its zero. */
    long fresh = map(0, PAGE, RW, ANON);
    out("an untouched page reads as ");
    long sent = write_out(fresh, 1);
    out(" through write\n");
    line("write from an untouched page", sent);
    line("write from a PROT_NONE page", write_out(map(0, PAGE, PROT_NONE, ANON), 1));

    /* mremap: two pages with six free after them. */
    long m = map(0, 8 * PAGE, RW, ANON);
    unmap(m + 2 * PAGE, 6 * PAGE);
    fill(m, 2 * PAGE, 0x33);
    line("mremap grows in place", remap(m, 2 * PAGE, 4 * PAGE, 0) == m);
    line("the old bytes stay", all(m, 2 * PAGE, 0x33));
    line("the new pages read zero", all(m + 2 * PAGE, 2 * PAGE, 0));
    fill(m + 2 * PAGE, 2 * PAGE, 0x44);
    line("a read-only page right after it", map(m + 4 * PAGE, PAGE, PROT_READ, ANON | MAP_FIXED) == m + 4 * PAGE);
    line("mremap without MREMAP_MAYMOVE when blocked", remap(m, 4 * PAGE, 6 * PAGE, 0));
    long moved = remap(m, 4 * PAGE, 6 * PAGE, MREMAP_MAYMOVE);
    line("mremap with MREMAP_MAYMOVE moves", !failed(moved) && moved != m);
    line("the moved bytes stay", all(moved, 2 * PAGE, 0x33) && all(moved + 2 * PAGE, 2 * PAGE, 0x44));
    line("the grown pages read zero", all(moved + 4 * PAGE, 2 * PAGE, 0));
    line("write from the old address", write_out(m, 1));
    line("mremap shrinks in place", remap(moved, 6 * PAGE, PAGE, 0) == moved);
    line("write from the cut pages", write_out(moved + PAGE, 1));
    line("mremap of an unmapped address", remap(m, PAGE, 2 * PAGE, MREMAP_MAYMOVE));

    /* The break. */
    long start = brk(0);
    line("brk grows three pages", brk(start + 3 * PAGE) == start + 3 * PAGE);
    fill(start, 3 * PAGE, 0x77);
    line("brk shrinks to one", brk(start + PAGE) == start + PAGE);
    line("brk grows again", brk(start + 3 * PAGE) == start + 3 * PAGE);
    line("the regrown pages read zero", all(start + PAGE, 2 * PAGE, 0));
    line("the kept page kept its bytes", all(start, PAGE, 0x77));
    line("brk below its start answers where it is", brk(start - PAGE) == start + 3 * PAGE);

    /* Argument errors. */
    line("mmap of zero bytes", map(0, 0, RW, ANON));
    line("MAP_FIXED at an unaligned address", map(start + 3 * PAGE + 1, PAGE, RW, ANON | MAP_FIXED));
    line("mmap neither private nor shared", map(0, PAGE, RW, MAP_ANONYMOUS));
    line("munmap at an unaligned address", unmap(moved + 1, PAGE));
    line("mremap to zero bytes", remap(moved, PAGE, 0, MREMAP_MAYMOVE));

    sys3(231, 0, 0, 0);
    for (;;) {
    }
}
