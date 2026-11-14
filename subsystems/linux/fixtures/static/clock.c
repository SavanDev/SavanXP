/* L2 fixture "clock": the clocks musl reads with real system calls, since
 * the subsystem offers no vDSO. */
#include <stdio.h>
#include <time.h>

static long long ns(struct timespec t)
{
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

int main(void)
{
    struct timespec a, b, r;
    int fail = 0;

    if (clock_gettime(CLOCK_REALTIME, &r) != 0 || r.tv_sec < 1700000000) {
        printf("realtime wrong\n");
        fail = 1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) {
        printf("monotonic failed\n");
        fail = 1;
    }
    struct timespec d = {0, 20 * 1000 * 1000};
    if (nanosleep(&d, NULL) != 0) {
        printf("nanosleep failed\n");
        fail = 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    if (ns(b) - ns(a) < 20 * 1000 * 1000) {
        printf("slept %lld ns, less than asked\n", ns(b) - ns(a));
        fail = 1;
    }
    if (time(NULL) < 1700000000) {
        printf("time() wrong\n");
        fail = 1;
    }
    printf("clock %s\n", fail ? "FAILED" : "ok");
    return fail;
}
