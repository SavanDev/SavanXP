/*
 * pthreadtest: POSIX threads over the native thread calls
 * (docs/kernel/THREADS_ROADMAP.md, phase 5).
 *
 * Several threads on several cores hammer what a threaded port leans on: a
 * mutex-protected counter, malloc and free, errno, __thread variables,
 * condition variables, once, keys, and two threads parked in poll() at the
 * same time. Then the process-level rules, each in a child: exec from a
 * process with threads running, blocked and asleep ends them all; exit() from
 * a second thread ends the process; a forked child of a threaded process can
 * still allocate and print.
 */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 4
#define COUNTER_ROUNDS 20000
#define HEAP_ROUNDS 3000
#define ERRNO_ROUNDS 2000
#define QUEUE_ITEMS 2000
#define DETACHED_ROUNDS 80
#define EXEC_STATUS 42
#define EXIT_STATUS 33

static int g_failures = 0;

static void check_at(int ok, const char* what, int line) {
    if (!ok) {
        printf("pthreadtest: FAIL line %d: %s\n", line, what);
        __atomic_fetch_add(&g_failures, 1, __ATOMIC_SEQ_CST);
    }
}

#define check(ok) check_at((ok), #ok, __LINE__)

/* --- TLS ---------------------------------------------------------------------- */

static __thread int t_counter = 7;
static __thread char t_name[16] = "initial";
static __thread unsigned long t_scratch[32];

static int tls_round(long index) {
    int round;

    /* Every thread starts from the initial image, whatever the others did. */
    if (t_counter != 7 || strcmp(t_name, "initial") != 0 || t_scratch[31] != 0u) {
        return 0;
    }
    for (round = 0; round < 1000; ++round) {
        t_counter = (int)index * 100000 + round;
        t_scratch[round % 32] = (unsigned long)index;
        if ((round % 100) == 0) {
            sched_yield();
        }
        if (t_counter != (int)index * 100000 + round || t_scratch[round % 32] != (unsigned long)index) {
            return 0;
        }
    }
    snprintf(t_name, sizeof(t_name), "thread-%ld", index);
    return 1;
}

/* --- the worker --------------------------------------------------------------- */

static pthread_mutex_t g_counter_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned long g_counter = 0;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static volatile int g_once_runs = 0;

static pthread_key_t g_key;
static volatile int g_key_destructions = 0;

static void once_routine(void) {
    __atomic_fetch_add(&g_once_runs, 1, __ATOMIC_SEQ_CST);
    /* Long enough that the others arrive while it runs. */
    usleep(20000);
}

static void key_destructor(void* value) {
    free(value);
    __atomic_fetch_add(&g_key_destructions, 1, __ATOMIC_SEQ_CST);
}

static int heap_rounds(long index) {
    unsigned char* blocks[8] = {0};
    unsigned long seed = 0x9e3779b9ul * (unsigned long)(index + 1);
    int round;

    for (round = 0; round < HEAP_ROUNDS; ++round) {
        const int slot = round % 8;
        size_t size;
        size_t at;

        if (blocks[slot] != 0) {
            const size_t old_size = ((size_t*)blocks[slot])[0];
            for (at = sizeof(size_t); at < old_size; ++at) {
                if (blocks[slot][at] != (unsigned char)(index + at)) {
                    return 0;
                }
            }
            free(blocks[slot]);
        }
        seed = seed * 6364136223846793005ul + 1442695040888963407ul;
        size = sizeof(size_t) + (size_t)((seed >> 33) % 3000u);
        blocks[slot] = (unsigned char*)malloc(size);
        if (blocks[slot] == 0) {
            return 0;
        }
        ((size_t*)blocks[slot])[0] = size;
        for (at = sizeof(size_t); at < size; ++at) {
            blocks[slot][at] = (unsigned char)(index + at);
        }
    }
    for (round = 0; round < 8; ++round) {
        free(blocks[round]);
    }
    return 1;
}

static int errno_rounds(long index) {
    int round;

    for (round = 0; round < ERRNO_ROUNDS; ++round) {
        errno = 1000 + (int)index;
        if ((round % 50) == 0) {
            sched_yield();
        }
        if (errno != 1000 + (int)index) {
            return 0;
        }
        /* A failing call sets this thread's errno, not another's. */
        if (close(-1) != -1 || errno != EBADF) {
            return 0;
        }
    }
    return 1;
}

static void* worker(void* argument) {
    const long index = (long)argument;
    int round;
    int spin;

    if (!tls_round(index)) {
        return (void*)1;
    }
    if (pthread_once(&g_once, once_routine) != 0) {
        return (void*)2;
    }
    if (g_once_runs != 1) {
        return (void*)3;
    }
    if (pthread_setspecific(g_key, malloc(32)) != 0 || pthread_getspecific(g_key) == 0) {
        return (void*)4;
    }
    for (round = 0; round < COUNTER_ROUNDS; ++round) {
        unsigned long value;

        pthread_mutex_lock(&g_counter_lock);
        value = g_counter;
        for (spin = 0; spin < 8; ++spin) {
            __asm__ volatile("pause");
        }
        g_counter = value + 1u;
        pthread_mutex_unlock(&g_counter_lock);
    }
    if (!heap_rounds(index)) {
        return (void*)5;
    }
    if (!errno_rounds(index)) {
        return (void*)6;
    }
    printf("pthreadtest: worker %ld (%s) done\n", index, t_name);
    return (void*)(100 + index);
}

static void test_workers(void) {
    pthread_t threads[WORKERS];
    long index;

    check(pthread_key_create(&g_key, key_destructor) == 0);
    for (index = 0; index < WORKERS; ++index) {
        check(pthread_create(&threads[index], 0, worker, (void*)index) == 0);
    }
    /* The main thread keeps its own TLS while they run. */
    t_counter = 99;
    for (index = 0; index < WORKERS; ++index) {
        void* result = 0;

        check(pthread_join(threads[index], &result) == 0);
        check(result == (void*)(100 + index));
    }
    check(t_counter == 99 && strcmp(t_name, "initial") == 0);
    check(g_counter == (unsigned long)WORKERS * COUNTER_ROUNDS);
    check(g_once_runs == 1);
    check(g_key_destructions == WORKERS);
    check(pthread_key_delete(g_key) == 0);
    printf("pthreadtest: workers done, counter=%lu\n", g_counter);
}

/* --- mutex kinds ---------------------------------------------------------------- */

static pthread_mutex_t g_held;

static void* try_from_other(void* argument) {
    (void)argument;
    if (pthread_mutex_trylock(&g_held) != EBUSY) {
        return (void*)1;
    }
    return pthread_mutex_unlock(&g_held) == EPERM ? 0 : (void*)2;
}

static void test_mutex_kinds(void) {
    pthread_mutexattr_t attr;
    pthread_t other;
    void* result = (void*)1;

    check(pthread_mutexattr_init(&attr) == 0);
    check(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);
    check(pthread_mutex_init(&g_held, &attr) == 0);
    check(pthread_mutex_lock(&g_held) == 0 && pthread_mutex_lock(&g_held) == 0);
    check(pthread_create(&other, 0, try_from_other, 0) == 0);
    check(pthread_join(other, &result) == 0 && result == 0);
    check(pthread_mutex_unlock(&g_held) == 0 && pthread_mutex_unlock(&g_held) == 0);
    check(pthread_mutex_unlock(&g_held) == EPERM);

    check(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK) == 0);
    check(pthread_mutex_init(&g_held, &attr) == 0);
    check(pthread_mutex_lock(&g_held) == 0);
    check(pthread_mutex_lock(&g_held) == EDEADLK);
    check(pthread_mutex_unlock(&g_held) == 0);
    check(pthread_mutex_destroy(&g_held) == 0);
}

/* --- condition variables -------------------------------------------------------- */

static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_ready = PTHREAD_COND_INITIALIZER;
static int g_queue[16];
static int g_queue_count = 0;
static int g_queue_closed = 0;
static long g_consumed_sum = 0;
static int g_consumed_items = 0;

static void* consumer(void* argument) {
    (void)argument;
    pthread_mutex_lock(&g_queue_lock);
    for (;;) {
        while (g_queue_count == 0 && !g_queue_closed) {
            pthread_cond_wait(&g_queue_ready, &g_queue_lock);
        }
        if (g_queue_count == 0) {
            break;
        }
        g_queue_count -= 1;
        g_consumed_sum += g_queue[g_queue_count];
        g_consumed_items += 1;
        pthread_cond_broadcast(&g_queue_ready);
    }
    pthread_mutex_unlock(&g_queue_lock);
    return 0;
}

static void test_condition_variables(void) {
    pthread_t consumers[3];
    pthread_condattr_t attr;
    pthread_cond_t timed;
    struct timespec before;
    struct timespec deadline;
    struct timespec after;
    long expected = 0;
    long elapsed_ms;
    int index;

    for (index = 0; index < 3; ++index) {
        check(pthread_create(&consumers[index], 0, consumer, 0) == 0);
    }
    for (index = 1; index <= QUEUE_ITEMS; ++index) {
        pthread_mutex_lock(&g_queue_lock);
        while (g_queue_count == 16) {
            pthread_cond_wait(&g_queue_ready, &g_queue_lock);
        }
        g_queue[g_queue_count++] = index;
        expected += index;
        pthread_cond_signal(&g_queue_ready);
        pthread_mutex_unlock(&g_queue_lock);
    }
    pthread_mutex_lock(&g_queue_lock);
    g_queue_closed = 1;
    pthread_cond_broadcast(&g_queue_ready);
    pthread_mutex_unlock(&g_queue_lock);
    for (index = 0; index < 3; ++index) {
        check(pthread_join(consumers[index], 0) == 0);
    }
    check(g_consumed_items == QUEUE_ITEMS && g_consumed_sum == expected);

    /* A timed wait nobody signals, on the monotonic clock. */
    check(pthread_condattr_init(&attr) == 0);
    check(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0);
    check(pthread_cond_init(&timed, &attr) == 0);
    check(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    deadline = before;
    deadline.tv_nsec += 60000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_queue_lock);
    check(pthread_cond_timedwait(&timed, &g_queue_lock, &deadline) == ETIMEDOUT);
    pthread_mutex_unlock(&g_queue_lock);
    check(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    elapsed_ms = (long)(after.tv_sec - before.tv_sec) * 1000L + (after.tv_nsec - before.tv_nsec) / 1000000L;
    check(elapsed_ms >= 50);
    printf("pthreadtest: condition variables done\n");
}

/* --- poll from two threads at once --------------------------------------------- */

struct poll_job {
    int fd;
    int result;
    short revents;
};

static void* poller(void* argument) {
    struct poll_job* job = (struct poll_job*)argument;
    struct pollfd entry;

    entry.fd = job->fd;
    entry.events = POLLIN;
    entry.revents = 0;
    job->result = poll(&entry, 1, 5000);
    job->revents = entry.revents;
    return 0;
}

static void test_parallel_poll(void) {
    int first[2];
    int second[2];
    struct poll_job jobs[2];
    pthread_t threads[2];
    char byte = 'x';

    check(pipe(first) == 0 && pipe(second) == 0);
    jobs[0].fd = first[0];
    jobs[1].fd = second[0];
    jobs[0].result = jobs[1].result = -2;
    check(pthread_create(&threads[0], 0, poller, &jobs[0]) == 0);
    check(pthread_create(&threads[1], 0, poller, &jobs[1]) == 0);
    /* Both parked, each with its own request; then each one's pipe. */
    usleep(100000);
    check(write(second[1], &byte, 1) == 1);
    usleep(50000);
    check(write(first[1], &byte, 1) == 1);
    check(pthread_join(threads[0], 0) == 0 && pthread_join(threads[1], 0) == 0);
    check(jobs[0].result == 1 && (jobs[0].revents & POLLIN) != 0);
    check(jobs[1].result == 1 && (jobs[1].revents & POLLIN) != 0);
    close(first[0]);
    close(first[1]);
    close(second[0]);
    close(second[1]);
}

/* --- detached threads ------------------------------------------------------------- */

static volatile int g_detached_done = 0;

static void* detached_body(void* argument) {
    (void)argument;
    __atomic_fetch_add(&g_detached_done, 1, __ATOMIC_SEQ_CST);
    return 0;
}

/* More detached threads than a process has handles: each one's handle and
 * mapping must be released after it ends. */
static void test_detached(void) {
    pthread_attr_t attr;
    int round;

    check(pthread_attr_init(&attr) == 0);
    check(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
    check(pthread_attr_setstacksize(&attr, 64u * 1024u) == 0);
    for (round = 0; round < DETACHED_ROUNDS; ++round) {
        pthread_t thread;
        int tries;

        if (round % 2 == 0) {
            check(pthread_create(&thread, &attr, detached_body, 0) == 0);
        } else {
            check(pthread_create(&thread, 0, detached_body, 0) == 0);
            check(pthread_detach(thread) == 0);
        }
        for (tries = 0; tries < 2000 && g_detached_done != round + 1; ++tries) {
            usleep(1000);
        }
        check(g_detached_done == round + 1);
    }
    printf("pthreadtest: %d detached threads done\n", DETACHED_ROUNDS);
}

/* --- children ------------------------------------------------------------------------ */

static pthread_mutex_t g_never = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_never_cond = PTHREAD_COND_INITIALIZER;

static void* spin_forever(void* argument) {
    volatile unsigned long count = 0;
    (void)argument;
    for (;;) {
        count += 1u;
    }
    return 0;
}

static void* block_forever(void* argument) {
    (void)argument;
    pthread_mutex_lock(&g_never);
    for (;;) {
        pthread_cond_wait(&g_never_cond, &g_never);
    }
    return 0;
}

static void* sleep_forever(void* argument) {
    (void)argument;
    for (;;) {
        usleep(1000000);
    }
    return 0;
}

static void* exit_from_thread(void* argument) {
    (void)argument;
    usleep(20000);
    exit(EXIT_STATUS);
    return 0;
}

static int run_child(const char* self, const char* mode) {
    if (strcmp(mode, "exec") == 0) {
        pthread_t thread;
        char* argv[3];

        pthread_create(&thread, 0, spin_forever, 0);
        pthread_create(&thread, 0, spin_forever, 0);
        pthread_create(&thread, 0, block_forever, 0);
        pthread_create(&thread, 0, sleep_forever, 0);
        usleep(50000);
        argv[0] = (char*)self;
        argv[1] = (char*)"execed";
        argv[2] = 0;
        execvp(self, argv);
        return 1;
    }
    if (strcmp(mode, "execed") == 0) {
        /* A fresh image: one thread, TLS from the initial image. */
        return t_counter == 7 ? EXEC_STATUS : 2;
    }
    if (strcmp(mode, "exit") == 0) {
        pthread_t thread;

        pthread_create(&thread, 0, exit_from_thread, 0);
        pthread_join(thread, 0);
        return 1;
    }
    return 3;
}

static int child_status(const char* self, const char* mode) {
    int status = -1;
    pid_t child = fork();

    if (child == 0) {
        char* argv[3];
        argv[0] = (char*)self;
        argv[1] = (char*)mode;
        argv[2] = 0;
        execvp(self, argv);
        exit(120);
    }
    if (child < 0 || waitpid(child, &status, 0) != child) {
        return -1;
    }
    return WEXITSTATUS(status);
}

static void* hold_heap(void* argument) {
    volatile int* stop = (volatile int*)argument;
    while (!*stop) {
        free(malloc(256));
        printf("%s", "");
    }
    return 0;
}

/* A fork while other threads allocate and print: the child gets the heap and
 * stdio free, and can use both. */
static void test_fork_with_threads(void) {
    volatile int stop = 0;
    pthread_t threads[2];
    int round;

    check(pthread_create(&threads[0], 0, hold_heap, (void*)&stop) == 0);
    check(pthread_create(&threads[1], 0, hold_heap, (void*)&stop) == 0);
    for (round = 0; round < 10; ++round) {
        int status = -1;
        pid_t child = fork();

        if (child == 0) {
            void* block = malloc(4096);
            printf("pthreadtest: forked child %d allocates\n", round);
            free(block);
            exit(block != 0 ? 0 : 1);
        }
        check(child > 0 && waitpid(child, &status, 0) == child && status == 0);
    }
    stop = 1;
    check(pthread_join(threads[0], 0) == 0 && pthread_join(threads[1], 0) == 0);
}

int main(int argc, char** argv) {
    const char* self = argc > 0 ? argv[0] : "pthreadtest";

    if (argc > 1) {
        return run_child(self, argv[1]);
    }

    check(pthread_self() != 0 && pthread_equal(pthread_self(), pthread_self()));
    check(t_counter == 7 && strcmp(t_name, "initial") == 0);
    test_workers();
    test_mutex_kinds();
    test_condition_variables();
    test_parallel_poll();
    test_detached();
    test_fork_with_threads();
    check(child_status(self, "exec") == EXEC_STATUS);
    check(child_status(self, "exit") == EXIT_STATUS);

    if (g_failures != 0) {
        printf("PTHREAD TEST FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("PTHREAD TEST PASS\n");
    return 0;
}
