/* L1 fixture "errors": the failures of the first calls, libc-free.
 *
 * Each line names a case and says whether the result was the one Linux
 * gives; the same file prints the same lines on Linux and on SSL.
 *   - write from an address that is not mapped: -EFAULT;
 *   - write of zero bytes from such an address: 0, nothing is read;
 *   - write to a descriptor that is not open: -EBADF;
 *   - a system call number Linux does not have: -ENOSYS;
 *   - int 0x80, the 32-bit entry, with a number it does not have: -ENOSYS.
 * It ends with exit_group(0).
 *
 *   clang -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector \
 *         -O1 -fuse-ld=lld -o errors errors.c
 */

typedef unsigned long u64;

static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static long int80(long n)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n) : "memory");
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

static void say(const char *what, int ok)
{
    sys3(1, 1, (long)what, (long)len(what));
    const char *verdict = ok ? " ok\n" : " WRONG\n";
    sys3(1, 1, (long)verdict, (long)len(verdict));
}

__attribute__((force_align_arg_pointer, noreturn)) void _start(void)
{
    /* The first page is never mapped, on Linux or on SSL. */
    const long unmapped = 0x1000;

    say("write from an unmapped address is -EFAULT", sys3(1, 1, unmapped, 8) == -14);
    say("write of zero bytes from it is 0", sys3(1, 1, unmapped, 0) == 0);
    say("write to a closed descriptor is -EBADF", sys3(1, 99, (long)"x", 1) == -9);
    say("system call 1000 is -ENOSYS", sys3(1000, 0, 0, 0) == -38);
    say("int 0x80 with number 0xffff is -ENOSYS", int80(0xffff) == -38);
    sys3(231, 0, 0, 0);
    for (;;) {
    }
}
