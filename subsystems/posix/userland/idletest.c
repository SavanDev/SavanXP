/* Validate the v0.4.0 idle-process ABI and CPU accounting through guest APIs. */
#include "libc.h"

struct sample {
    struct savanxp_system_info before, after;
    struct savanxp_process_info tasks[64];
    unsigned int count;
};
static struct sample first, second;

static int fail(const char* reason) {
    printf("IDLE FAIL %s\n", reason);
    return 1;
}
static int snapshot(struct sample* out) {
    if (system_info(&out->before) < 0) return 0;
    for (out->count = 0; out->count < 64; ++out->count)
        if (proc_info(out->count, &out->tasks[out->count]) <= 0) break;
    return system_info(&out->after) == 0 && out->before.process_count == out->count &&
        out->after.process_count == out->count;
}
static uint64_t sum_ticks(const struct sample* s, int idle, uint32_t self) {
    uint64_t total = 0;
    for (unsigned int n = 0; n < s->count; ++n) {
        const struct savanxp_process_info* p = &s->tasks[n];
        if ((idle && (p->flags & SAVANXP_PROC_FLAG_IDLE)) || (!idle && (!self || p->pid == self)))
            total += p->cpu_ticks;
    }
    return total;
}
static int stable_ticks(const struct sample* a, const struct sample* b) {
    if (a->count != b->count) return 0;
    for (unsigned int n = 0; n < a->count; ++n)
        if (a->tasks[n].pid != b->tasks[n].pid || b->tasks[n].cpu_ticks < a->tasks[n].cpu_ticks) return 0;
    const uint64_t delta = sum_ticks(b, 0, 0) - sum_ticks(a, 0, 0);
    // Individual proc_info calls are not an atomic snapshot. Sandwich their
    // sum between system totals instead of requiring equal sample instants.
    return delta >= b->before.cpu_ticks_total - a->after.cpu_ticks_total &&
        delta <= b->after.cpu_ticks_total - a->before.cpu_ticks_total;
}

int main(void) {
    unsigned int idles = 0;
    uint32_t init = 0;
    const uint32_t self = (uint32_t)savanxp_getpid();
    if (!snapshot(&first)) return fail("initial enumeration");
    for (unsigned int n = 0; n < first.count; ++n) {
        const struct savanxp_process_info* p = &first.tasks[n];
        if (!(p->flags & SAVANXP_PROC_FLAG_IDLE)) {
            if (!p->parent_pid) init = p->pid;
            continue;
        }
        ++idles;
        if (strcmp(p->name, "idle") || p->parent_pid || p->memory_bytes != 8192 || p->handle_count ||
            (p->state != SAVANXP_PROC_READY && p->state != SAVANXP_PROC_RUNNING))
            return fail("idle identity, memory or state");
        if (savanxp_kill((int)p->pid, SAVANXP_SIGTERM) != -SAVANXP_EACCES ||
            savanxp_waitpid((int)p->pid, 0) != -SAVANXP_ECHILD)
            return fail("idle protection");
    }
    if (idles != first.before.cpu_online || !idles || init <= idles ||
        savanxp_kill((int)init, SAVANXP_SIGKILL) != -SAVANXP_EACCES)
        return fail("idle count or init protection");
    printf("IDLE identity PASS cpus=%u init=%u slots=%u\n", idles, init, first.count);

    if (!snapshot(&first)) return fail("sleep baseline");
    sleep_ms(600);
    if (!snapshot(&second) || !stable_ticks(&first, &second)) return fail("sleep tick conservation");
    uint64_t total = second.before.cpu_ticks_total - first.before.cpu_ticks_total;
    uint64_t idle = sum_ticks(&second, 1, 0) - sum_ticks(&first, 1, 0);
    const uint64_t low = second.before.cpu_ticks_total - first.after.cpu_ticks_total;
    const uint64_t high = second.after.cpu_ticks_total - first.before.cpu_ticks_total;
    printf("IDLE sleep sample idle=%llu total=%llu bounds=%llu..%llu\n", (unsigned long long)idle,
           (unsigned long long)total, (unsigned long long)low, (unsigned long long)high);
    if (!low || idle * 100 < low * 75 || idle > high) return fail("sleep attribution");
    puts_out("IDLE sleep PASS\n");

    first = second;
    const unsigned long until = uptime_ms() + 600;
    while (uptime_ms() < until) { /* Consume one CPU without blocking. */ }
    if (!snapshot(&second) || !stable_ticks(&first, &second)) return fail("busy tick conservation");
    total = second.before.cpu_ticks_total - first.before.cpu_ticks_total;
    const uint64_t mine = sum_ticks(&second, 0, self) - sum_ticks(&first, 0, self);
    if (!total || mine * idles * 100 < total * 50 || mine * idles > total * 2 + idles * 2)
        return fail("busy attribution");
    printf("IDLE busy PASS mine=%llu total=%llu cpus=%u\n", (unsigned long long)mine,
           (unsigned long long)total, idles);

    if (!snapshot(&first)) return fail("zombie baseline");
    long child = savanxp_fork();
    if (child < 0) return fail("fork");
    if (!child) exit(37);
    sleep_ms(100);
    if (!snapshot(&second) || second.count != first.count + 1) return fail("zombie process count");
    int seen = 0;
    for (unsigned int n = 0; n < second.count; ++n)
        if (second.tasks[n].pid == (uint32_t)child)
            seen = second.tasks[n].state == SAVANXP_PROC_ZOMBIE && !second.tasks[n].memory_bytes;
    int status = -1;
    if (!seen || savanxp_waitpid((int)child, &status) != child || status != 37 ||
        !snapshot(&second) || second.count != first.count ||
        second.after.memory_free_bytes != first.after.memory_free_bytes)
        return fail("zombie reap or page reclamation");
    puts_out("IDLE zombie/reap PASS\nIDLE PASS\n");
    return 0;
}
