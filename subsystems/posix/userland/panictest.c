#include "libc.h"

/*
 * panictest: asks the kernel to panic (POWER_IOC_PANIC on /dev/power).
 *
 * panic-smoke runs it and waits for the line the panic prints after its
 * message, "smp: other cores stopped (n/n)": every other core took the NMI
 * and halted, instead of running on or spinning for the kernel lock the
 * panicking core still holds (docs/roadmaps/open/SMP_ROADMAP.md, phase 4).
 *
 * It needs the system identity, so the smoke runs it straight from init.
 * Returning at all is the failure.
 */
int main(void) {
    long fd = savanxp_open_mode("/dev/power", SAVANXP_OPEN_WRITE);
    long result;

    if (fd < 0) {
        printf("PANICTEST FAIL open /dev/power (%ld)\n", fd);
        return 1;
    }
    printf("PANICTEST requesting a kernel panic\n");
    result = savanxp_ioctl((int)fd, POWER_IOC_PANIC, 0);
    printf("PANICTEST FAIL the kernel did not panic (%ld)\n", result);
    savanxp_close((int)fd);
    return 1;
}
