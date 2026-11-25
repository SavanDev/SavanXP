/* v0.4.0 SMP regression: observe another running child while the parent is
 * inside a syscall, terminate a remote runner, then exchange pipe records.
 * This checks scheduler ownership rather than guessing from elapsed time.
 * Native i386 also samples CPUID in both the parent and spinning children. */
#include "libc.h"

#define SPIN_MS 3000ul
#define PING_ROUNDS 2000

static void record_cpu(const char* role) {
    unsigned int a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    printf("smptest: %s lapic=%u\n", role, b >> 24);
}

/* A child owned by another scheduling CPU. */
static int child_seen_running(const long* children, unsigned int count) {
    struct savanxp_process_info info;
    unsigned long index;
    unsigned int child;

    for (index = 0; proc_info(index, &info) > 0; ++index) {
        if (info.state != SAVANXP_PROC_RUNNING) {
            continue;
        }
        for (child = 0; child < count; ++child) {
            if (info.pid == (uint32_t)children[child]) {
                return 1;
            }
        }
    }
    return 0;
}

static int check_parallel_spin(unsigned int cores, unsigned int processes) {
    long children[32];
    unsigned int spinners = cores - 1u;
    // Idle processes consume the same reference 64-slot table. At 32 CPUs
    // reserve the existing init/shell/test slots instead of asking fork to
    // exceed that capacity. At least one child must still run concurrently.
    if (spinners > 64u - processes) spinners = 64u - processes;
    if (!spinners) return 0;
    unsigned int index;
    int seen = 0;
    const unsigned long deadline = uptime_ms() + SPIN_MS;

    for (index = 0; index < spinners; ++index) {
        children[index] = savanxp_fork();
        if (children[index] < 0) {
            printf("smptest: fork failed (%s)\n", result_error_string(children[index]));
            return 0;
        }
        if (children[index] == 0) {
            record_cpu("worker");
            while (uptime_ms() < deadline) { /* Remain runnable. */ }
            exit(0);
        }
    }

    /* The parent owns one CPU while enumerating the running children. */
    while (uptime_ms() < deadline) {
        if (child_seen_running(children, spinners)) {
            record_cpu("parent");
            seen = 1;
            break;
        }
    }

    for (index = 0; index < spinners; ++index) {
        int status = -1;
        if (savanxp_waitpid((int)children[index], &status) < 0 || status != 0) {
            printf("smptest: FAIL spinner waitpid %ld (%d)\n", children[index], status);
            return 0;
        }
    }

    if (!seen) {
        printf("smptest: FAIL no child ran concurrently with the parent\n");
        return 0;
    }
    printf("smptest: %u spinners ran in parallel\n", spinners);
    return 1;
}

static int check_cross_core_kill(void) {
    long victim = savanxp_fork();
    int status = -1;

    if (victim < 0) {
        puts_out("smptest: fork failed\n");
        return 0;
    }
    if (victim == 0) {
        for (;;) {
            /* Stay in ring 3 until another process terminates this runner. */
        }
    }

    sleep_ms(50);
    if (savanxp_kill((int)victim, SAVANXP_SIGTERM) < 0) {
        puts_out("smptest: kill failed\n");
        return 0;
    }
    if (savanxp_waitpid((int)victim, &status) < 0) {
        puts_out("smptest: waitpid after kill failed\n");
        return 0;
    }
    if (status != 128 + SAVANXP_SIGTERM) {
        printf("smptest: FAIL kill expected %d, received %d\n", 128 + SAVANXP_SIGTERM, status);
        return 0;
    }
    return 1;
}

static int check_pipe_ping_pong(void) {
    int to_child[2];
    int to_parent[2];
    long child;
    int round;

    if (savanxp_pipe(to_child) < 0 || savanxp_pipe(to_parent) < 0) {
        puts_out("smptest: pipe failed\n");
        return 0;
    }

    child = savanxp_fork();
    if (child < 0) {
        puts_out("smptest: fork failed\n");
        return 0;
    }
    if (child == 0) {
        uint32_t value = 0;
        savanxp_close(to_child[1]);
        savanxp_close(to_parent[0]);
        while (savanxp_read(to_child[0], &value, sizeof(value)) == (long)sizeof(value)) {
            value += 1;
            if (savanxp_write(to_parent[1], &value, sizeof(value)) != (long)sizeof(value)) {
                exit(2);
            }
        }
        exit(0);
    }

    savanxp_close(to_child[0]);
    savanxp_close(to_parent[1]);
    {
        const unsigned long start = uptime_ms();
        for (round = 0; round < PING_ROUNDS; ++round) {
            uint32_t value = (uint32_t)round * 2u;
            if (savanxp_write(to_child[1], &value, sizeof(value)) != (long)sizeof(value) ||
                savanxp_read(to_parent[0], &value, sizeof(value)) != (long)sizeof(value)) {
                printf("smptest: FAIL pipe interrupted at round %d\n", round);
                return 0;
            }
            if (value != (uint32_t)round * 2u + 1u) {
                printf("smptest: FAIL round %d returned %u\n", round, (unsigned)value);
                return 0;
            }
        }
        printf("smptest: %d pipe round trips in %lu ms\n", PING_ROUNDS, uptime_ms() - start);
    }

    savanxp_close(to_child[1]);
    {
        int status = -1;
        if (savanxp_waitpid((int)child, &status) < 0 || status != 0) {
            printf("smptest: FAIL echo exited with %d\n", status);
            return 0;
        }
    }
    savanxp_close(to_parent[0]);
    return 1;
}

int main(void) {
    struct savanxp_system_info info;
    unsigned int cores;

    if (system_info(&info) < 0) {
        puts_out("smptest: system_info failed\n");
        return 1;
    }
    cores = info.cpu_online != 0u ? info.cpu_online : 1u;
    if (cores > 32u) {
        cores = 32u;
    }
    printf("smptest: %u scheduling CPU(s)\n", cores);

    if (cores > 1u && !check_parallel_spin(cores, info.process_count)) {
        return 1;
    }
    for (unsigned int round = 0; round < 16; ++round) {
        if (!check_cross_core_kill()) return 1;
    }
    puts_out("smptest: 16 termination/reap rounds passed\n");
    if (!check_pipe_ping_pong()) return 1;

    puts_out("smptest: ok\n");
    return 0;
}
