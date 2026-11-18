/* v0.4.0 wakeup handoff: observe user execution order through a shared section.
 * Only one CPU can prove local handoff this way; SMP checks delivery/results. */
#include "libc.h"

enum test_case {
    AUTO_EVENT, MANUAL_EVENT, WAIT_ALL, SEMAPHORE, EVENT_POLL,
    PIPE_READ, PIPE_POLLIN, PIPE_EOF, PIPE_HUP, PIPE_WRITE,
    PIPE_POLLOUT, PIPE_EPIPE, PIPE_DUP_EOF, PIPE_PARTIAL, CASE_COUNT
};
static const char* names[] = {
    "auto-event", "manual-event", "wait-all", "semaphore", "event-poll",
    "pipe-read", "pipe-pollin", "pipe-eof", "pipe-hup", "pipe-write",
    "pipe-pollout", "pipe-epipe", "dup2-eof", "partial-write"
};
struct shared_state { volatile uint32_t done, probe, seen; volatile long result; volatile int revents; };
static struct shared_state* shared;
static char buffer[16384];

static int blocked(long pid, uint32_t state) {
    const unsigned long deadline = uptime_ms() + 2000;
    do {
        struct savanxp_process_info p;
        for (unsigned int n = 0; proc_info(n, &p) > 0; ++n)
            if (p.pid == (uint32_t)pid && p.state == state) return 1;
        yield();
    } while (uptime_ms() < deadline);
    return 0;
}

static int check(enum test_case test, int strict) {
    int fds[2] = {-1, -1}, events[2] = {-1, -1};
    const int pipe_case = test >= PIPE_READ;
    const int writer = test == PIPE_WRITE || test == PIPE_POLLOUT ||
                       test == PIPE_EPIPE || test == PIPE_PARTIAL;
    shared->done = 0;
    if (pipe_case) {
        if (savanxp_pipe(fds) < 0) return 0;
        if (writer && savanxp_write(fds[1], buffer, 8192) != 8192) return 0;
    } else {
        events[0] = test == SEMAPHORE ? semaphore_create(0, 1) :
            event_create(test == MANUAL_EVENT ? SAVANXP_EVENT_MANUAL_RESET : SAVANXP_EVENT_AUTO_RESET);
        if (events[0] < 0) return 0;
        if (test == WAIT_ALL) {
            events[1] = event_create(SAVANXP_EVENT_AUTO_RESET);
            if (events[1] < 0 || event_set(events[0]) < 0) return 0;
        }
    }
    const long child = savanxp_fork();
    if (child < 0) return 0;
    if (!child) {
        long result;
        int revents = 0;
        if (pipe_case) savanxp_close(fds[writer ? 0 : 1]);
        if (test == EVENT_POLL || test == PIPE_POLLIN || test == PIPE_HUP || test == PIPE_POLLOUT) {
            struct savanxp_pollfd p = {pipe_case ? fds[writer ? 1 : 0] : events[0],
                writer ? SAVANXP_POLLOUT : SAVANXP_POLLIN, 0};
            result = savanxp_poll(&p, 1, 2000);
            revents = p.revents;
        } else if (test == WAIT_ALL) {
            result = wait_many(events, 2, SAVANXP_WAIT_FLAG_ALL, 2000);
        } else if (!pipe_case) {
            result = wait_one(events[0], 2000);
        } else if (writer) {
            result = savanxp_write(fds[1], buffer, test == PIPE_PARTIAL ? sizeof(buffer) : 1);
        } else {
            result = savanxp_read(fds[0], buffer, 1);
        }
        shared->result = result;
        shared->revents = revents;
        shared->done = 1;
        exit(0);
    }
    if (pipe_case) savanxp_close(fds[writer ? 1 : 0]);
    const uint32_t state = test == PIPE_READ || test == PIPE_EOF || test == PIPE_DUP_EOF ?
        SAVANXP_PROC_BLOCKED_READ : test == PIPE_WRITE || test == PIPE_EPIPE || test == PIPE_PARTIAL ?
        SAVANXP_PROC_BLOCKED_WRITE : SAVANXP_PROC_BLOCKED_WAIT;
    if (!blocked(child, state)) {
        savanxp_kill((int)child, SAVANXP_SIGKILL);
        savanxp_waitpid((int)child, 0);
        printf("WAKEUP FAIL %s did not block\n", names[test]);
        return 0;
    }
    long trigger;
    if (test == SEMAPHORE) trigger = semaphore_release(events[0], 1);
    else if (!pipe_case) trigger = event_set(events[test == WAIT_ALL ? 1 : 0]);
    else if (test == PIPE_EOF || test == PIPE_HUP) trigger = savanxp_close(fds[1]);
    else if (test == PIPE_DUP_EOF) trigger = savanxp_dup2(1, fds[1]);
    else if (test == PIPE_EPIPE) trigger = savanxp_close(fds[0]);
    else if (writer) {
        trigger = savanxp_read(fds[0], buffer, 8192);
        if (test == PIPE_PARTIAL) {
            // The first drain advances half the write without completing it.
            if (trigger != 8192 || shared->done) return 0;
            trigger = savanxp_read(fds[0], buffer, 8192);
        }
    } else trigger = savanxp_write(fds[1], "x", 1);
    // No syscall, yield or wait between the trigger and this observation.
    const uint32_t handed_off = shared->done;
    int status = -1;
    if (savanxp_waitpid((int)child, &status) != child || status || trigger < 0) return 0;
    long expected = !pipe_case ? 0 : 1;
    if (test == EVENT_POLL) expected = 1;
    if (test == PIPE_EOF || test == PIPE_DUP_EOF) expected = 0;
    if (test == PIPE_EPIPE) expected = -SAVANXP_EPIPE;
    if (test == PIPE_PARTIAL) expected = sizeof(buffer);
    int ok = shared->result == expected && (!strict || handed_off);
    if (test == PIPE_HUP) ok = ok && (shared->revents & SAVANXP_POLLHUP);
    if (test == PIPE_POLLOUT) ok = ok && (shared->revents & SAVANXP_POLLOUT);
    if (test == PIPE_POLLIN || test == EVENT_POLL) ok = ok && (shared->revents & SAVANXP_POLLIN);
    if (pipe_case) {
        if (test != PIPE_EOF && test != PIPE_HUP && test != PIPE_EPIPE)
            savanxp_close(fds[writer ? 0 : 1]);
    } else {
        if (test == AUTO_EVENT || test == WAIT_ALL || test == SEMAPHORE)
            ok = ok && wait_one(events[0], 0) == -SAVANXP_ETIMEDOUT;
        if (test == WAIT_ALL) ok = ok && wait_one(events[1], 0) == -SAVANXP_ETIMEDOUT;
        if (test == MANUAL_EVENT || test == EVENT_POLL)
            ok = ok && wait_one(events[0], 0) == 0;
        savanxp_close(events[0]);
        if (events[1] >= 0) savanxp_close(events[1]);
    }
    if (!ok) printf("WAKEUP FAIL %s result=%ld expected=%ld handoff=%u\n",
                    names[test], shared->result, expected, handed_off);
    return ok;
}

static int no_spurious_handoff(void) {
    shared->probe = shared->seen = 0;
    const long child = savanxp_fork();
    if (child < 0) return 0;
    if (!child) for (;;) { if (shared->probe) shared->seen = 1; }
    int samples = 0, ok = 1;
    for (unsigned int n = 0; n < 64; ++n) {
        const unsigned long before = uptime_ms();
        shared->seen = 0;
        shared->probe = 1;
        savanxp_getpid();
        const uint32_t seen = shared->seen;
        shared->probe = 0;
        const unsigned long after = uptime_ms();
        // Discard actual timer preemption; it is allowed to run the child.
        if (before == after) { ++samples; if (seen) ok = 0; }
    }
    savanxp_kill((int)child, SAVANXP_SIGKILL);
    int status = -1;
    printf("WAKEUP unrelated-syscall samples=%d ok=%d\n", samples, ok);
    return savanxp_waitpid((int)child, &status) == child && status == 128 + SAVANXP_SIGKILL && samples && ok;
}

int main(void) {
    struct savanxp_system_info before, after;
    if (system_info(&before) < 0) return 1;
    long section = section_create(4096, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (section < 0) return 1;
    shared = map_view((int)section, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)shared)) return 1;
    for (int test = 0; test < CASE_COUNT; ++test) {
        for (int round = 0; round < 16; ++round)
            if (!check((enum test_case)test, before.cpu_online == 1)) {
                printf("WAKEUP FAIL case=%s round=%d\n", names[test], round); return 1;
            }
        printf("WAKEUP %s PASS rounds=16\n", names[test]);
    }
    if (before.cpu_online == 1 && !no_spurious_handoff()) {
        puts_out("WAKEUP FAIL unrelated syscall yielded\n"); return 1;
    }
    unmap_view(shared);
    savanxp_close((int)section);
    if (system_info(&after) < 0 || after.process_count != before.process_count ||
        after.memory_free_bytes != before.memory_free_bytes) {
        puts_out("WAKEUP FAIL resource recovery\n"); return 1;
    }
    printf("WAKEUP PASS cpus=%u order=%s\n", before.cpu_online,
           before.cpu_online == 1 ? "checked" : "single-CPU-only");
    return 0;
}
