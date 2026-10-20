/* Standalone native ABI validation (no POSIX).
 *
 * Built as an external app and stamped EI_OSABI=0x53:
 *
 *     ./tools/build-user.sh --source sdk/nativetest --name nativetest --native
 *
 * Runs headless from init:
 *
 *     ./build.sh smoke native-test
 *
 * Speaks through include/sxn/sxn.h, the header-only native userland
 * surface, with one raw trap kept as a wire-format cross-check. Linking
 * the POSIX runtime objects is fine (dead code never runs), but calling
 * into them would prove nothing about the native dispatcher. Reporting
 * goes through sxn_log (kernel log) and the verdict through sxn_exit
 * (waitpid status). Neither path ever returns: on return crt0 would
 * invoke the POSIX EXIT, which is ENOSYS for a native process.
 *
 * Covers: INFO handshake, LOG, ENOSYS for POSIX numbers and for unknown
 * native numbers, self-spawn plus WAITPID, cross-process event plus
 * shared section (parent writes a marker, child answers, parent waits on
 * the event, then reset plus a zero-timeout poll), and EXIT with the
 * failure count as status. Then the phase 3 calls: CLOCK, DUPLICATE and
 * CLOSE on an event, READ refusing a non-stream handle, OPEN_DEVICE refusing
 * paths and unknown names, DEVICE_CONTROL on gpu0 and WRITE to serial; and a
 * port: a second child ("portchild") connects by name, exchanges a message
 * each way and signals an event handed over its reply pipe; and TERMINATE:
 * a third child ("sleeper") waits forever until the parent ends it, and
 * waitpid reports the status the parent chose. The first
 * child runs this same binary with
 * argv = ["nativetest", "child", ev, sec] and exits 42.
 */

#include <stdint.h>

#include "abi/savanxp_native_abi.h"
#include "savanxp/syscall.h"
#include "sxn/sxn.h"

#define SXN_CHILD_STATUS 42
#define SXN_PARENT_MARKER 0x5EED1u
#define SXN_CHILD_MARKER 0x5EED2u
#define SXN_RING_ROUNDS 50
#define SXN_RING_WAIT_MS 5000

static long raw_info(struct sxn_native_info* out) {
    long result;

    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"((unsigned long)SXN_SYS_INFO), "D"((unsigned long)out), "S"(0ul), "d"(0ul)
                     : "rcx", "r11", "cc", "memory");
    return result;
}

static int g_failures = 0;

static void check(int ok) {
    if (!ok) {
        g_failures += 1;
    }
}

static int streq(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        a += 1;
        b += 1;
    }
    return *a == *b;
}

static void itoau(long value, char* buffer) {
    char reversed[24];
    int length = 0;

    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    while (value > 0 && length < 23) {
        reversed[length] = (char)('0' + (value % 10));
        value /= 10;
        length += 1;
    }
    for (int index = 0; index < length; index += 1) {
        buffer[index] = reversed[length - 1 - index];
    }
    buffer[length] = '\0';
}

static long atou(const char* text) {
    long value = 0;

    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (*text - '0');
        text += 1;
    }
    return value;
}

static void run_child(int argc, char** argv) {
    struct sxn_native_info info;
    long event_handle;
    long section_handle;
    long ev_req;
    long base;

    check(argc == 5);
    check(sxn_info(&info) == 0);
    check(info.abi_version == SXN_ABI_VERSION);
    check(info.subsystem_id == 1u);
    check(info.pid != 0u);
    check(sxn_log("nativetest: child alive") == 0);

    event_handle = atou(argv[2]);
    section_handle = atou(argv[3]);
    ev_req = atou(argv[4]);
    base = sxn_map_view(section_handle, SXN_SECTION_READ | SXN_SECTION_WRITE);
    check(base > 0);
    if (base > 0) {
        volatile uint32_t* cell = (volatile uint32_t*)base;
        check(*cell == SXN_PARENT_MARKER);
        *cell = SXN_CHILD_MARKER;
    }
    check(sxn_log("nativetest: child mapped") == 0);
    check(sxn_event_set(event_handle) == 0);

    /* Ring ping-pong: strict alternation over the shared section, the same
     * shape as a WM submit/retire channel. Each side resets what it
     * consumed; any failure aborts the loop so a broken channel fails in
     * seconds, not at the smoke timeout. */
    if (base > 0) {
        volatile uint32_t* ring = (volatile uint32_t*)base + 16;
        int round;

        for (round = 1; round <= SXN_RING_ROUNDS; round += 1) {
            int round_failures = g_failures;

            check(sxn_wait_one(ev_req, SXN_RING_WAIT_MS) == 0);
            check(ring[0] == (uint32_t)round);
            check(ring[1] == (uint32_t)(round * 3 + 1));
            ring[2] = (uint32_t)(round * 5 + 2);
            ring[3] = (uint32_t)round;
            check(sxn_event_reset(ev_req) == 0);
            check(sxn_event_set(event_handle) == 0);
            if (g_failures != round_failures) {
                break;
            }
        }
        check(sxn_log("nativetest: child ring done") == 0);
    }

    sxn_exit(g_failures == 0 ? SXN_CHILD_STATUS : 1);
    __builtin_unreachable();
}

#define SXN_PORT_CHILD_STATUS 7
#define SXN_TERMINATE_STATUS 99

/* Sleeper child: blocks on an event nobody sets, so only TERMINATE ends it. */
static void run_sleeper(void) {
    long event = sxn_event_create(SXN_EVENT_MANUAL_RESET);

    for (;;) {
        (void)sxn_wait_one(event, 60000);
    }
}

/* Port child: connects to the parent's port by name, says ping, reads pong,
 * takes the event the parent handed over and signals it. Native calls only. */
static void run_port_child(void) {
    int32_t handles[2] = {-1, -1};
    char reply[4] = {0};
    long event = -1;

    if (sxn_port_connect("nativetest", handles) != 0 ||
        sxn_write(handles[0], "ping", 4) != 4 ||
        sxn_read(handles[1], reply, 4) != 4 || reply[0] != 'p' || reply[1] != 'o' ||
        (event = sxn_pipe_receive_handle(handles[1])) < 0 ||
        sxn_event_set(event) != 0) {
        sxn_exit(1);
    }
    sxn_exit(SXN_PORT_CHILD_STATUS);
}

int main(int argc, char** argv) {
    struct sxn_native_info info;
    struct sxn_native_info raw;
    long event_handle;
    long section_handle;
    long base;

    if (argc > 1 && streq(argv[1], "child")) {
        run_child(argc, argv);
    }
    if (argc > 1 && streq(argv[1], "portchild")) {
        run_port_child();
    }
    if (argc > 1 && streq(argv[1], "sleeper")) {
        run_sleeper();
    }

    check(sxn_info(&info) == 0);
    check(info.abi_version == SXN_ABI_VERSION);
    check(info.subsystem_id == 1u);
    check(info.pid != 0u);

    /* Wire-format cross-check: the header must match the trap. */
    check(raw_info(&raw) == 0);
    check(raw.pid == info.pid);

    check(sxn_log("nativetest: standalone SXN alive") == 0);

    /* POSIX numbers from a native process: ENOSYS, no delegation. */
    check(sxn_trap(SAVANXP_SYS_WRITE, 1, (unsigned long)"x", 1) == -SAVANXP_ENOSYS);
    check(sxn_trap(SAVANXP_SYS_GETPID, 0, 0, 0) == -SAVANXP_ENOSYS);

    /* Unknown native number: ENOSYS as well. */
    check(sxn_trap(0x1FFF, 0, 0, 0) == -SAVANXP_ENOSYS);

    /* Waiting on an unrelated pid is ECHILD, like the POSIX path. */
    {
        int status = 0;
        check(sxn_waitpid(0x7fffffffL, &status) == -SAVANXP_ECHILD);
    }

    /* Shared event plus section with the child (handles are inherited). */
    event_handle = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    check(event_handle >= 0);
    section_handle = sxn_section_create(4096, SXN_SECTION_READ | SXN_SECTION_WRITE);
    check(section_handle >= 0);
    base = sxn_map_view(section_handle, SXN_SECTION_READ | SXN_SECTION_WRITE);
    check(base > 0);
    check(sxn_log("nativetest: parent mapped") == 0);
    if (base > 0) {
        *(volatile uint32_t*)base = SXN_PARENT_MARKER;
    }

    /* Self-spawn: the child maps the same section, answers, signals. */
    if (event_handle >= 0 && section_handle >= 0 && base > 0) {
        char event_text[24];
        char section_text[24];
        char request_text[24];
        char* child_argv[5];
        int status = -1;
        long ev_req = -1;
        long child;
        long reaped;
        int round;

        itoau(event_handle, event_text);
        itoau(section_handle, section_text);
        ev_req = sxn_event_create(SXN_EVENT_MANUAL_RESET);
        check(ev_req >= 0);
        itoau(ev_req, request_text);
        child_argv[0] = "/disk/bin/nativetest";
        child_argv[1] = "child";
        child_argv[2] = event_text;
        child_argv[3] = section_text;
        child_argv[4] = request_text;
        child = sxn_spawn("/disk/bin/nativetest", child_argv, 5);
        check(child > 0);
        if (child > 0) {
            volatile uint32_t* ring = (volatile uint32_t*)base + 16;

            check(sxn_wait_one(event_handle, 5000) == 0);
            check(sxn_log("nativetest: parent woke") == 0);
            check(*(volatile uint32_t*)base == SXN_CHILD_MARKER);
            /* The marker handshake leaves ev signaled; clear it so round 1
             * really waits for the child's first reply. */
            check(sxn_event_reset(event_handle) == 0);
            for (round = 1; round <= SXN_RING_ROUNDS; round += 1) {
                int round_failures = g_failures;

                ring[0] = (uint32_t)round;
                ring[1] = (uint32_t)(round * 3 + 1);
                check(sxn_event_set(ev_req) == 0);
                check(sxn_wait_one(event_handle, SXN_RING_WAIT_MS) == 0);
                check(ring[2] == (uint32_t)(round * 5 + 2));
                check(ring[3] == (uint32_t)round);
                check(sxn_event_reset(event_handle) == 0);
                if (g_failures != round_failures) {
                    break;
                }
            }
            check(sxn_log("nativetest: parent ring done") == 0);
            check(sxn_wait_one(event_handle, 0) == -SAVANXP_ETIMEDOUT);
            reaped = sxn_waitpid(child, &status);
            check(reaped == child);
            check(status == SXN_CHILD_STATUS);
        }
    } else {
        check(0);
    }

    if (base > 0) {
        check(sxn_unmap_view((uint64_t)base) == 0);
    }

    /* Handles, devices and the clock (phase 3 of
     * docs/desktop/DESKTOP_ARCHITECTURE.md). */
    {
        uint64_t first = sxn_clock_ns();
        uint64_t second = sxn_clock_ns();
        long duplicate = -1;
        long gpu = -1;
        long serial = -1;
        struct savanxp_gpu_info gpu_info;
        static const char k_serial_line[] = "nativetest: serial device\n";

        check(first != 0u && second >= first);

        duplicate = sxn_duplicate(event_handle);
        check(duplicate >= 0 && duplicate != event_handle);
        check(sxn_event_set(duplicate) == 0);
        check(sxn_wait_one(event_handle, 0) == 0);
        check(sxn_close(duplicate) == 0);
        check(sxn_close(duplicate) == -SAVANXP_EBADF);
        check(sxn_event_set(duplicate) == -SAVANXP_EBADF);
        /* An event is not a stream. */
        check(sxn_read(event_handle, &gpu_info, sizeof(gpu_info)) == -SAVANXP_EBADF);

        check(sxn_open_device("../disk", SXN_DEVICE_READ) == -SAVANXP_EINVAL);
        check(sxn_open_device("gpu0", 0) == -SAVANXP_EINVAL);
        check(sxn_open_device("nosuchdevice", SXN_DEVICE_READ) == -SAVANXP_ENOENT);
        gpu = sxn_open_device("gpu0", SXN_DEVICE_READ | SXN_DEVICE_WRITE);
        check(gpu >= 0);
        if (gpu >= 0) {
            gpu_info.width = 0;
            check(sxn_device_control(gpu, GPU_IOC_GET_INFO, (unsigned long)&gpu_info) == 0);
            check(gpu_info.width != 0u && gpu_info.height != 0u);
            check(sxn_close(gpu) == 0);
        }
        serial = sxn_open_device("serial", SXN_DEVICE_WRITE);
        check(serial >= 0);
        if (serial >= 0) {
            check(sxn_write(serial, k_serial_line, sizeof(k_serial_line) - 1u) ==
                (long)(sizeof(k_serial_line) - 1u));
            check(sxn_close(serial) == 0);
        }
        check(sxn_log("nativetest: handles, devices and clock done") == 0);
    }

    /* Ports: a service found by name instead of an inherited channel. */
    {
        struct sxn_port_connection connection;
        char* child_argv[2];
        char request[4] = {0};
        long port = sxn_port_create("nativetest");
        long event = sxn_event_create(SXN_EVENT_MANUAL_RESET);
        long child = -1;
        int status = -1;

        check(port >= 0 && event >= 0);
        check(sxn_port_create("nativetest") == -SAVANXP_EEXIST);
        child_argv[0] = "/disk/bin/nativetest";
        child_argv[1] = "portchild";
        child = sxn_spawn("/disk/bin/nativetest", child_argv, 2);
        check(child > 0);
        if (port >= 0 && event >= 0 && child > 0) {
            check(sxn_wait_one(port, 5000) == 0);
            check(sxn_port_accept(port, &connection) == 0);
            check(connection.client_pid == (uint32_t)child);
            check(sxn_read(connection.read_handle, request, 4) == 4 && request[0] == 'p' && request[1] == 'i');
            check(sxn_pipe_send_handle(connection.write_handle, event) == 0);
            check(sxn_write(connection.write_handle, "pong", 4) == 4);
            check(sxn_wait_one(event, 5000) == 0);
            check(sxn_waitpid(child, &status) == child && status == SXN_PORT_CHILD_STATUS);
            check(sxn_close(connection.read_handle) == 0 && sxn_close(connection.write_handle) == 0);
            check(sxn_close(port) == 0);
            {
                int32_t none[2] = {-1, -1};
                check(sxn_port_connect("nativetest", none) == -SAVANXP_ENOENT);
            }
        }
        check(sxn_log("nativetest: ports done") == 0);
    }

    /* TERMINATE: a parent ends its child; anybody else's process is refused. */
    {
        char* child_argv[2];
        long child = -1;
        int status = -1;

        child_argv[0] = "/disk/bin/nativetest";
        child_argv[1] = "sleeper";
        child = sxn_spawn("/disk/bin/nativetest", child_argv, 2);
        check(child > 0);
        if (child > 0) {
            check(sxn_terminate(child, SXN_TERMINATE_STATUS) == 0);
            check(sxn_waitpid(child, &status) == child && status == SXN_TERMINATE_STATUS);
            /* Reaped: the pid names nothing any more. */
            check(sxn_terminate(child, 1) == -SAVANXP_ENOENT);
        }
        /* pid 1 is the idle task: not this process's child. */
        check(sxn_terminate(1, 1) == -SAVANXP_ECHILD);
        check(sxn_terminate(0, 1) == -SAVANXP_ENOENT);
        check(sxn_log("nativetest: terminate done") == 0);
    }

    sxn_exit(g_failures == 0 ? 0 : 1);
    __builtin_unreachable();
}
