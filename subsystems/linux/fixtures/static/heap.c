/* L2 fixture "heap": malloc from small blocks to several megabytes, with
 * the large ones returned to the system when freed (musl unmaps them). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t sum(const unsigned char *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) {
        s = s * 31 + p[i];
    }
    return s;
}

int main(void)
{
    static const size_t sizes[] = {1, 16, 100, 4000, 65536, 300000, 1 << 20, 6 << 20};
    void *small[1000];

    for (int i = 0; i < 1000; i++) {
        small[i] = malloc((size_t)(i % 97) + 1);
        if (!small[i]) {
            printf("small malloc %d failed\n", i);
            return 1;
        }
        memset(small[i], i & 0xff, (size_t)(i % 97) + 1);
    }
    for (int i = 0; i < 1000; i += 2) {
        free(small[i]);
    }

    for (int round = 0; round < 3; round++) {
        for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
            unsigned char *p = calloc(sizes[k], 1);
            if (!p) {
                printf("calloc %zu failed\n", sizes[k]);
                return 1;
            }
            for (size_t i = 0; i < sizes[k]; i++) {
                if (p[i] != 0) {
                    printf("calloc %zu not zero at %zu\n", sizes[k], i);
                    return 1;
                }
                p[i] = (unsigned char)(i * 7 + k);
            }
            if (round == 0) {
                printf("size %zu sum %08x\n", sizes[k], sum(p, sizes[k]));
            }
            free(p);
        }
    }

    unsigned char *g = NULL;
    size_t n = 0;
    for (size_t want = 64; want <= (8u << 20); want *= 4) {
        g = realloc(g, want);
        if (!g) {
            printf("realloc %zu failed\n", want);
            return 1;
        }
        for (size_t i = n; i < want; i++) {
            g[i] = (unsigned char)i;
        }
        n = want;
    }
    printf("realloc to %zu sum %08x\n", n, sum(g, n));
    free(g);

    for (int i = 1; i < 1000; i += 2) {
        free(small[i]);
    }
    printf("heap ok\n");
    return 0;
}
