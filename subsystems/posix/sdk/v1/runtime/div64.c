#include <stdint.h>

/* clang no tiene instruccion de division de 64 bits en modo 32 bits, asi que
 * el runtime trae las dos rutinas que el compilador pide por nombre (mismo
 * contrato que libgcc). Solo hacen falta porque algun programa del userland
 * divide un uint64_t entre un valor que no conoce en tiempo de compilacion,
 * por ejemplo al imprimir un numero decimal. */

/* Division bit a bit: el cociente se construye de arriba abajo, que es lento
 * pero son 64 iteraciones y solo aparece en rutas de formato. Asume divisor
 * <= 2^63, porque con un divisor mayor el resto desbordaria al desplazar. */
static uint64_t divide(uint64_t dividend, uint64_t divisor, uint64_t* remainder_out) {
    if (divisor == 0) {
        *remainder_out = dividend;
        return 0;
    }

    uint64_t quotient = 0;
    uint64_t remainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        remainder = (remainder << 1) | ((dividend >> bit) & 1u);
        if (remainder >= divisor) {
            remainder -= divisor;
            quotient |= (uint64_t)1 << bit;
        }
    }

    *remainder_out = remainder;
    return quotient;
}

uint64_t __udivdi3(uint64_t dividend, uint64_t divisor) {
    uint64_t remainder = 0;
    return divide(dividend, divisor, &remainder);
}

uint64_t __umoddi3(uint64_t dividend, uint64_t divisor) {
    uint64_t remainder = 0;
    divide(dividend, divisor, &remainder);
    return remainder;
}