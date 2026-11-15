/* Dynamic TLS, resolved by the distribution's linker rather than Sx's. */
#include <stdint.h>
static __thread int initialized = 17;
static __thread int zeroed;
static __thread unsigned char aligned[64] __attribute__((aligned(64)));
int l4_tls(void) {
    int correct = initialized == 17 && zeroed == 0 && ((uintptr_t)aligned & 63) == 0;
    initialized += 5;
    zeroed = 9;
    aligned[63] = 7;
    return correct && initialized == 22 && zeroed == 9 && aligned[63] == 7;
}
