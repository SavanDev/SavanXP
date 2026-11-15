#include "sdkint_cases.h"
int main(void) {
    if (sdk_integer_cases()) return 1;
    puts("SDK32 static integer ABI PASS");
    return 0;
}
