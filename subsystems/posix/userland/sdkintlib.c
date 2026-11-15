#include "sdkint_cases.h"
int sdk_integer_library_test(void) { return sdk_integer_cases(); }
imaxdiv_t sdk_integer_library_div(intmax_t numerator, intmax_t denominator) {
    return imaxdiv(numerator,denominator);
}
uint64_t sdk_integer_library_mix(uint64_t left, uint64_t right) { return (left << 32) | right; }
