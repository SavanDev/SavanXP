/*
 * math.c -- The arithmetic msvcrt.dll exports for the profile: abs, labs,
 * fabs, floor, ceil, modf and _ftol.
 *
 * floor, ceil and modf round with the x87 unit (frndint under the rounding
 * mode each needs), which is exact for every double. _ftol is the compiler's
 * helper for a cast from floating point to an integer: its argument is in
 * st(0), not on the stack, and it returns 64 bits in edx:eax, truncated.
 */
#include <windows.h>

/* The marker a Visual C++ object that uses floating point refers to; the
 * value is the one Microsoft's runtime gives it. */
int _fltused = 0x9875;

int __cdecl abs(int value) {
    return value < 0 ? -value : value;
}

long __cdecl labs(long value) {
    return value < 0 ? -value : value;
}

double __cdecl fabs(double value) {
    double result;
    __asm__("fabs" : "=t"(result) : "0"(value));
    return result;
}

/* Rounds value with the x87 rounding control set to mode (bits 10-11 of the
 * control word: 1 down, 2 up, 3 toward zero), restoring it after. */
static double round_with(double value, unsigned short mode) {
    unsigned short saved;
    unsigned short changed;
    double result;
    __asm__ volatile("fnstcw %0" : "=m"(saved));
    changed = (unsigned short)((saved & ~0x0c00u) | (mode << 10));
    __asm__ volatile("fldcw %0" : : "m"(changed));
    __asm__ volatile("frndint" : "=t"(result) : "0"(value));
    __asm__ volatile("fldcw %0" : : "m"(saved));
    return result;
}

double __cdecl floor(double value) {
    return round_with(value, 1);
}

double __cdecl ceil(double value) {
    return round_with(value, 2);
}

double __cdecl modf(double value, double *integer) {
    double whole = round_with(value, 3);
    *integer = whole;
    return value - whole;
}

/* long long _ftol(st(0)): truncates, whatever the current rounding mode. */
__asm__(".text\n"
        ".globl __ftol\n"
        "__ftol:\n"
        "\tpushl %ebp\n"
        "\tmovl %esp, %ebp\n"
        "\tsubl $12, %esp\n"
        "\tfnstcw 8(%esp)\n"
        "\tmovw 8(%esp), %ax\n"
        "\torw $0x0c00, %ax\n"
        "\tmovw %ax, 10(%esp)\n"
        "\tfldcw 10(%esp)\n"
        "\tfistpll (%esp)\n"
        "\tfldcw 8(%esp)\n"
        "\tmovl (%esp), %eax\n"
        "\tmovl 4(%esp), %edx\n"
        "\tleave\n"
        "\tret\n");
