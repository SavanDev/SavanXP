#include <stdint.h>

/* Compiler runtime for 64-bit integer division on i386. Build the quotient
 * one bit at a time without using division instructions or hosted libgcc. */
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
int64_t __divdi3(int64_t dividend, int64_t divisor) {
    uint64_t left = (uint64_t)dividend;
    uint64_t right = (uint64_t)divisor;
    if (dividend < 0) left = 0u - left;
    if (divisor < 0) right = 0u - right;
    uint64_t remainder = 0;
    uint64_t quotient = divide(left, right, &remainder);
    if ((dividend < 0) != (divisor < 0)) quotient = 0u - quotient;
    return (int64_t)quotient;
}

int64_t __moddi3(int64_t dividend, int64_t divisor) {
    uint64_t left = (uint64_t)dividend;
    uint64_t right = (uint64_t)divisor;
    if (dividend < 0) left = 0u - left;
    if (divisor < 0) right = 0u - right;
    uint64_t remainder = 0;
    divide(left, right, &remainder);
    if (dividend < 0) remainder = 0u - remainder;
    return (int64_t)remainder;
}
