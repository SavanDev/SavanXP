#include "libc.h"
#include "abi/savanxp_native_abi.h"

static int16_t samples[2048];
static int fail(const char* reason) { eprintf("DEV32 FAIL %s\n", reason); return 1; }

int main(int argc, char** argv) {
    long fd;
    int status = 0;
    if (argc > 1 && strcmp(argv[1], "shutdown") == 0) {
        puts_out("DEV32 SHUTDOWN REQUEST\n");
        return power_shutdown() < 0 ? fail("shutdown") : 1;
    }
    if (argc > 1 && strcmp(argv[1], "reboot") == 0) {
        puts_out("DEV32 REBOOT REQUEST\n");
        return power_reboot() < 0 ? fail("reboot") : 1;
    }
    {
        long denied;
        struct sxn_native_info native_info;
        __asm__ volatile("int $0x80" : "=a"(denied) : "a"(SXN_SYS_INFO), "D"(&native_info) : "memory");
        if (denied != -SAVANXP_ENOSYS) return fail("POSIX/native syscall isolation");
        struct savanxp_system_info system = {};
        struct savanxp_realtime clock = {};
        if (system_info(&system) < 0 || !system.cpu_vendor[0] || !system.cpu_khz || !system.pci_device_count)
            return fail("CPU/PCI discovery");
        if (realtime(&clock) < 0 || !clock.valid || clock.month < 1 || clock.month > 12 || clock.second > 59)
            return fail("CMOS realtime clock");
    }
    fd = savanxp_open_mode("/dev/audio0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (fd < 0) return fail("audio open");
    if (argc > 1 && strcmp(argv[1], "owner") == 0) {
        if (savanxp_write((int)fd, samples, sizeof(samples)) != sizeof(samples)) return fail("child audio write");
        puts_out("DEV32 CHILD OWNS AUDIO\n");
        for (;;) sleep_ms(100);
    }
    {
        long readonly = savanxp_open("/dev/audio0");
        if (readonly < 0 || savanxp_ioctl((int)readonly, AUDIO_IOC_SET_VOLUME, 25) != -SAVANXP_EACCES)
            return fail("read-only volume permissions");
        savanxp_close((int)readonly);
    }
    if (savanxp_ioctl((int)fd, AUDIO_IOC_SET_VOLUME, 101) != -SAVANXP_EINVAL)
        return fail("volume range");
    if (savanxp_write((int)fd, samples, 3) != -SAVANXP_EINVAL) return fail("frame alignment");
    if (savanxp_write((int)fd, (void*)1, 4) != -SAVANXP_EFAULT) return fail("invalid user buffer");
    {
        const char* args[] = {"/bin/devtest32", "owner", 0};
        long child = spawn(args[0], args, 2);
        unsigned long deadline = uptime_ms() + 3000;
        int busy = 0;
        if (child < 0) return fail("owner spawn");
        while (uptime_ms() < deadline) {
            sleep_ms(20);
            long result = savanxp_write((int)fd, samples, sizeof(samples));
            if (result == -SAVANXP_EBUSY) { busy = 1; break; }
            // If the child has not acquired the device yet, closing releases
            // this trial write before letting it run again.
            savanxp_close((int)fd);
            fd = savanxp_open_mode("/dev/audio0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
        }
        if (savanxp_kill((int)child, 9) < 0 || savanxp_waitpid((int)child, &status) != child)
            return fail("owner kill/reap");
        if (!busy || status != 137) return fail("audio ownership");
        if (savanxp_write((int)fd, samples, sizeof(samples)) != sizeof(samples)) return fail("killed owner release");
    }
    savanxp_close((int)fd);
    fd = savanxp_open("/dev/power");
    if (fd < 0 || savanxp_ioctl((int)fd, POWER_IOC_SHUTDOWN, 0) != -SAVANXP_EACCES)
        return fail("read-only power permissions");
    savanxp_close((int)fd);
    fd = savanxp_open_mode("/dev/serial", SAVANXP_OPEN_WRITE);
    if (fd < 0 || savanxp_write((int)fd, "DEV32 SERIAL PASS\n", 18) != 18) return fail("serial output");
    savanxp_close((int)fd);
    puts_out("DEV32 PASS\n");
    return 0;
}
