/*
 * SavanXP - runtime del subsistema nativo, Fase 0.
 *
 * Envoltura de syscalls sobre `int $0x80` con la convencion del kernel:
 *   numero -> rax, arg0 -> rdi, arg1 -> rsi, arg2 -> rdx, resultado -> rax.
 * (Identica a subsystems/posix/sdk/v1/runtime/libc.c.)
 */
#include "savanxp_native.h"

long sxn_syscall3(long number, long a, long b, long c) {
    long result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c)
                     : "memory");
    return result;
}

long sxn_write(int fd, const char *buf, int len) {
    return sxn_syscall3(SXN_SYS_WRITE, fd, (long)buf, len);
}

void sxn_exit(int code) {
    sxn_syscall3(SXN_SYS_EXIT, code, 0, 0);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void sxn_hello(void) {
    static const char msg[] = "hola desde Haxe\n";
    sxn_write(1, msg, (int)sizeof(msg) - 1);
}
