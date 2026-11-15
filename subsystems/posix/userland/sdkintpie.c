#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#define SDK_CHECK(x) do { if (!(x)) { puts("SDK32 shared ABI failure"); return 1; } } while (0)
#include <savanxp/ldso.h>
extern int sdk_integer_library_test(void);
extern imaxdiv_t sdk_integer_library_div(intmax_t numerator, intmax_t denominator);
extern uint64_t sdk_integer_library_mix(uint64_t left, uint64_t right);
int main(void) {
    SDK_CHECK(!ldso_missing() && ldso_symbol_is_shared("sdk_integer_library_test"));
    SDK_CHECK(sdk_integer_library_test() == 0);
    imaxdiv_t result = sdk_integer_library_div(-INT64_C(4294967303),3);
    SDK_CHECK(result.quot == -INT64_C(1431655767) && result.rem == -2);
    SDK_CHECK(sdk_integer_library_mix(UINT64_C(0x12345678),UINT64_C(0x9abcdef0)) == UINT64_C(0x123456789abcdef0));
    puts("SDK32 shared integer ABI PASS");
    return 0;
}
