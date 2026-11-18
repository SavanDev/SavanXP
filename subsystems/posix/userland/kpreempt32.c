/* Real blocking CONNECT waits, a non-cooperating sibling and deferred kills.
 * No test syscall: reuse v0.4.0's TCP fault injector to retain SYNs locally. */
#include "libc.h"

_Static_assert(SAVANXP_SYS_CONNECT == 29 && SAVANXP_ETIMEDOUT == 110, "assembly probe ABI");
extern int context_probe32(unsigned int marker, unsigned int deadline);
extern int context_connect_probe32(unsigned int marker, unsigned int fd, const void* address, unsigned int timeout);

struct progress { uint32_t started, done; };
static volatile struct progress* g_shared;
static volatile uint32_t g_private;
static const unsigned int kTimeout = 1200;

static int fail(const char* reason) { printf("KPREEMPT FAIL %s\n", reason); return 1; }
static unsigned int socket_capacity(void) {
    long sockets[64];
    unsigned int count = 0;
    while (count < 64) {
        long fd = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, SAVANXP_IPPROTO_TCP);
        if (fd < 0) break;
        sockets[count++] = fd;
    }
    for (unsigned int n = 0; n < count; ++n)
        if (savanxp_close((int)sockets[n]) < 0) return 0;
    return count;
}
static uint64_t self_ticks(void) {
    struct savanxp_process_info info;
    const uint32_t self = (uint32_t)savanxp_getpid();
    for (unsigned int n = 0; proc_info(n, &info) > 0; ++n)
        if (info.pid == self) return info.cpu_ticks;
    return 0;
}
static int pending(unsigned int count) {
    for (unsigned int n = 0; n < count; ++n)
        if (__atomic_load_n(&g_shared[n].started, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&g_shared[n].done, __ATOMIC_ACQUIRE)) return 1;
    return 0;
}
static long launch(unsigned int index) {
    g_shared[index].started = g_shared[index].done = 0;
    long pid = savanxp_fork();
    if (pid) return pid;
    volatile uint32_t local = 0xcafe0000u + index;
    g_private = 0xface0000u + index;
    long fd = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, SAVANXP_IPPROTO_TCP);
    if (fd < 0) exit(90);
    struct savanxp_sockaddr_in address = {0};
    address.ipv4 = (10u << 24) | (2u << 8) | 2u;
    address.port = 12345;
    __atomic_store_n(&g_shared[index].started, 1, __ATOMIC_RELEASE);
    const int result = context_connect_probe32(1 + (index & 1), (unsigned int)fd, &address, kTimeout);
    if (result || local != 0xcafe0000u + index || g_private != 0xface0000u + index) exit(80 + result);
    __atomic_store_n(&g_shared[index].done, 1, __ATOMIC_RELEASE);
    exit(0);
}

int main(void) {
    struct savanxp_system_info before, after;
    if (system_info(&before) < 0 || before.cpu_online > 8) return fail("use 1..8 CPUs");
    g_shared = (volatile struct progress*)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (result_is_error((long)g_shared)) return fail("shared progress map");
    long net = savanxp_open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    struct savanxp_net_tcp_fault fault = {1, 0, 0, 0};
    if (net < 0 || savanxp_ioctl((int)net, NET_IOC_UP, 0) < 0 ||
        savanxp_ioctl((int)net, NET_IOC_SET_TCP_FAULT, (unsigned long)&fault) < 0) return fail("TCP fault injector");
    const unsigned int capacity = socket_capacity();
    if (capacity < before.cpu_online + 1 || system_info(&before) < 0) return fail("socket baseline");
    g_private = 0xabcdef01;

    // More retained kernel continuations than CPUs force actual timesharing.
    const unsigned int workers = before.cpu_online + 1;
    long children[9];
    for (unsigned int n = 0; n < workers; ++n) {
        children[n] = launch(n);
        if (children[n] < 0) return fail("fork");
        printf("KPREEMPT worker pid=%ld\n", children[n]);
    }
    unsigned long began = uptime_ms();
    uint64_t overlap = 0;
    while (uptime_ms() - began < 6000) {
        unsigned int started = 0;
        for (unsigned int n = 0; n < workers; ++n) started += !!g_shared[n].started;
        if (started == workers && !pending(workers)) break;
        const int was_pending = pending(workers);
        const uint64_t ticks = self_ticks();
        const unsigned int step = (unsigned int)uptime_ms();
        if (context_probe32(2, step + 60)) return fail("sibling CPU/FPU context");
        // A probe that was frozen for the entire CONNECT timeout is not
        // sibling progress. Count actual charged CPU ticks in short probes.
        if (started == workers && was_pending && pending(workers) && uptime_ms() - step <= 200)
            overlap += self_ticks() - ticks;
    }
    for (unsigned int n = 0; n < workers; ++n) {
        int status = -1;
        if (savanxp_waitpid((int)children[n], &status) != children[n] || status) return fail("waiter context or timeout");
    }
    if (overlap < 10 || g_private != 0xabcdef01) return fail("sibling did not progress during a kernel wait");
    printf("KPREEMPT overlap PASS ticks=%llu workers=%u cpus=%u\n", (unsigned long long)overlap, workers, before.cpu_online);
    if (socket_capacity() != capacity || system_info(&after) < 0 ||
        after.memory_free_bytes != before.memory_free_bytes || after.process_count != before.process_count)
        return fail("waiter reclamation");

    for (unsigned int round = 0; round < 3; ++round) {
        long victim = launch(0);
        if (victim < 0) return fail("victim fork");
        printf("KPREEMPT victim pid=%ld\n", victim);
        const unsigned long limit = uptime_ms() + 4000;
        while (!g_shared[0].started && uptime_ms() < limit) yield();
        if (!g_shared[0].started || context_probe32(2, (unsigned int)uptime_ms() + 100) || g_shared[0].done)
            return fail("victim did not retain its syscall");
        if (savanxp_kill((int)victim, SAVANXP_SIGTERM) < 0) return fail("deferred kill");
        int status = -1;
        if (savanxp_waitpid((int)victim, &status) != victim || status != 128 + SAVANXP_SIGTERM || g_shared[0].done)
            return fail("kill did not stop at the syscall boundary");
        if (socket_capacity() != capacity || system_info(&after) < 0 ||
            after.memory_free_bytes != before.memory_free_bytes || after.process_count != before.process_count)
            return fail("kill reclamation");
    }
    printf("KPREEMPT sockets PASS capacity=%u\n", capacity);
    memset(&fault, 0, sizeof(fault));
    if (savanxp_ioctl((int)net, NET_IOC_SET_TCP_FAULT, (unsigned long)&fault) < 0 || savanxp_close((int)net) < 0 ||
        munmap((void*)g_shared, 4096) < 0) return fail("cleanup");
    puts_out("KPREEMPT deferred kills PASS\nKPREEMPT PASS\n");
    return 0;
}
