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
 * each way and signals an event handed over its reply pipe; WAIT_MANY on
 * events and streams; and TERMINATE:
 * a third child ("sleeper") waits forever until the parent ends it, and
 * waitpid reports the status the parent chose; PROCESS_QUERY sees it running,
 * then ended with that status, then gone. And the heap
 * (docs/userland/NATIVE_HEAP.md): several MiB across arenas from the native
 * page source. And threads (docs/kernel/THREADS_ROADMAP.md): workers that
 * share memory and end with their own codes, joined through their handles; a
 * process killed with threads blocked and running; the last thread ending the
 * process; SXN_SYS_EXIT from a second thread ending them all. The first
 * child runs this same binary with
 * argv = ["nativetest", "child", ev, sec] and exits 42.
 */

#include <stdint.h>
#include <stdlib.h>

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

/* A failed check logs its line, so a FAIL says where. */
static void check_at(int ok, int line) {
    char text[40] = "nativetest: check failed at line ";
    char digits[12];
    int count = 0;
    int length = 33;

    if (ok) {
        return;
    }
    g_failures += 1;
    do {
        digits[count++] = (char)('0' + line % 10);
        line /= 10;
    } while (line > 0 && count < 11);
    while (count > 0 && length < (int)sizeof(text) - 1) {
        text[length++] = digits[--count];
    }
    text[length] = '\0';
    (void)sxn_log(text);
}

#define check(ok) check_at((ok), __LINE__)

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

/* SPAWN_EX child: owns exactly 0, 1, 2 and 3. Echoes the line read on 0 to
 * 1, says "err" on 2 and "x3" on 3, so the parent can tell every handle
 * landed where the request put it. */
static void run_stdio_child(void) {
    struct sxn_native_info self_info;
    struct sxn_process_query self;
    char line[16] = {0};
    long count = 0;

    if (sxn_info(&self_info) != 0 || sxn_process_query(self_info.pid, &self) != 0 ||
        self.handle_count != 4u) {
        sxn_exit(2);
    }
    count = sxn_read(0, line, sizeof(line));
    if (count <= 0 || sxn_write(1, line, (unsigned long)count) != count ||
        sxn_write(2, "err", 3) != 3 || sxn_write(3, "x3", 2) != 2) {
        sxn_exit(3);
    }
    sxn_exit(0);
}

/* TERMINATE across a generation: starts a sleeper, tells the parent its pid
 * on 1, and exits with whatever status the sleeper ends with. */
static void run_middle_child(void) {
    char* sleeper_argv[2];
    long sleeper = -1;
    int32_t pid_value = 0;
    int status = -1;

    sleeper_argv[0] = "/bin/nativetest.sxe";
    sleeper_argv[1] = "sleeper";
    sleeper = sxn_spawn_stdio("/bin/nativetest.sxe", sleeper_argv, 2);
    if (sleeper <= 0) {
        sxn_exit(2);
    }
    pid_value = (int32_t)sleeper;
    if (sxn_write(1, &pid_value, sizeof(pid_value)) != (long)sizeof(pid_value) ||
        sxn_waitpid(sleeper, &status) != sleeper) {
        sxn_exit(3);
    }
    sxn_exit(status);
}

/* --- threads (docs/kernel/THREADS_ROADMAP.md, phase 1) ------------------ */

#define SXN_THREAD_STACK_BYTES (64ul * 1024ul)
#define SXN_THREAD_WORKERS 3
#define SXN_THREAD_KILL_STATUS 77
#define SXN_THREAD_LAST_STATUS 55
#define SXN_THREAD_EXITALL_STATUS 66
#define SXN_SHOOTDOWN_STALE_STATUS 44
/* What a #PF ends a process with: 128 + the vector. */
#define SXN_PAGE_FAULT_STATUS (128 + 14)
#define SXN_SHOOTDOWN_WORD 0x5eedu

/* A thread's stack: a private section, mapped, its handle closed (the view
 * keeps it). Returns the top, or 0. */
static void* thread_stack(void) {
    long section = sxn_section_create(SXN_THREAD_STACK_BYTES, SXN_SECTION_READ | SXN_SECTION_WRITE);
    long base;

    if (section < 0) {
        return 0;
    }
    base = sxn_map_view(section, SXN_SECTION_READ | SXN_SECTION_WRITE);
    (void)sxn_close(section);
    return base > 0 ? (void*)(base + (long)SXN_THREAD_STACK_BYTES) : 0;
}

static volatile uint32_t g_thread_marks[SXN_THREAD_WORKERS];
static long g_thread_go = -1;

/* Waits for the go event, leaves its mark in memory the whole process sees,
 * and ends with a code of its own. */
static void thread_worker(void* argument) {
    long index = (long)argument;

    if (sxn_wait_one(g_thread_go, 5000) != 0) {
        sxn_thread_exit(1);
    }
    g_thread_marks[index] = 0x7700u + (uint32_t)index;
    sxn_thread_exit(100 + (int)index);
}

/* Blocked forever: only the end of its process ends it. */
static void thread_blocked(void* argument) {
    long event = (long)argument;

    for (;;) {
        (void)sxn_wait_one(event, 60000);
    }
}

/* Never enters the kernel: ready or running whenever it is looked at. */
static void thread_spinning(void* argument) {
    volatile unsigned long* counter = (volatile unsigned long*)argument;

    for (;;) {
        *counter += 1;
    }
}

/* Outlives the main thread a little, then ends the process as its last
 * thread. */
static void thread_last(void* argument) {
    long event = (long)argument;

    (void)sxn_wait_one(event, 50);
    sxn_thread_exit(SXN_THREAD_LAST_STATUS);
}

/* SXN_SYS_EXIT from a thread that is not the first ends every thread. */
static void thread_exit_all(void* argument) {
    (void)argument;
    sxn_exit(SXN_THREAD_EXITALL_STATUS);
}

/* Reads a view in a loop that never enters the kernel. Unmapping the view
 * under it has to fault it, even when it runs on another core, whose TLB
 * still holds the translation until the kernel makes it flush. */
static volatile unsigned long g_shootdown_reads = 0;

static void thread_reader(void* argument) {
    volatile uint32_t* word = (volatile uint32_t*)argument;

    for (;;) {
        if (*word != SXN_SHOOTDOWN_WORD) {
            sxn_exit(4);
        }
        g_shootdown_reads += 1;
    }
}

/* "shootdown" child: the main thread unmaps the view a reader is reading,
 * while the reader runs (on another core, when there is one). The view holds
 * the section's last reference, so its pages are freed too -- which must not
 * happen while that core can still reach them. The reader faulting ends the
 * process with SXN_PAGE_FAULT_STATUS; a reader that keeps going on a stale
 * translation leaves the main thread to end it with
 * SXN_SHOOTDOWN_STALE_STATUS. */
static void run_shootdown_child(void) {
    long section = sxn_section_create(4096, SXN_SECTION_READ | SXN_SECTION_WRITE);
    long event = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    void* stack = thread_stack();
    long base;
    unsigned long seen;

    if (section < 0 || event < 0 || stack == 0) {
        sxn_exit(2);
    }
    base = sxn_map_view(section, SXN_SECTION_READ | SXN_SECTION_WRITE);
    (void)sxn_close(section);
    if (base <= 0) {
        sxn_exit(2);
    }
    *(volatile uint32_t*)base = SXN_SHOOTDOWN_WORD;
    if (sxn_thread_create(thread_reader, (void*)base, stack, 0) < 0) {
        sxn_exit(2);
    }
    while (g_shootdown_reads < 1000u) {
        (void)sxn_wait_one(event, 1);
    }
    /* Running right now: the count moves while this thread stays in user
     * mode. */
    seen = g_shootdown_reads;
    while (g_shootdown_reads == seen) {
    }
    if (sxn_unmap_view((uint64_t)base) != 0) {
        sxn_exit(3);
    }
    (void)sxn_wait_one(event, 2000);
    sxn_exit(SXN_SHOOTDOWN_STALE_STATUS);
}

/* "threads" child: one thread blocked, two spinning (on two cores, when
 * there are), and the main thread blocked too, once it told the parent (on 1)
 * they all exist. */
static void run_threads_child(void) {
    static volatile unsigned long spins[2] = {0, 0};
    long event = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    void* blocked_stack = thread_stack();
    void* spinning_stack = thread_stack();
    void* second_spinning_stack = thread_stack();
    char ready = 'T';

    if (event < 0 || blocked_stack == 0 || spinning_stack == 0 || second_spinning_stack == 0 ||
        sxn_thread_create(thread_blocked, (void*)event, blocked_stack, 0) < 0 ||
        sxn_thread_create(thread_spinning, (void*)&spins[0], spinning_stack, 0) < 0 ||
        sxn_thread_create(thread_spinning, (void*)&spins[1], second_spinning_stack, 0) < 0) {
        sxn_exit(2);
    }
    while (spins[0] == 0 || spins[1] == 0) {
        (void)sxn_wait_one(event, 1);
    }
    if (sxn_write(1, &ready, 1) != 1) {
        sxn_exit(3);
    }
    for (;;) {
        (void)sxn_wait_one(event, 60000);
    }
}

/* "lastexit" child: the main thread ends first; the other one ends the
 * process. */
static void run_last_exit_child(void) {
    long event = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    void* stack = thread_stack();

    if (event < 0 || stack == 0 || sxn_thread_create(thread_last, (void*)event, stack, 0) < 0) {
        sxn_exit(2);
    }
    sxn_thread_exit(1);
}

/* "exitall" child: a second thread calls SXN_SYS_EXIT while the main thread
 * spins, on another core when there is one: the exit has to wait for that
 * core to stop it. */
static void run_exit_all_child(void) {
    static volatile unsigned long spins = 0;
    void* stack = thread_stack();

    if (stack == 0 || sxn_thread_create(thread_exit_all, 0, stack, 0) < 0) {
        sxn_exit(2);
    }
    for (;;) {
        spins += 1;
    }
}

/* Thread-local storage through the segment bases. Each thread points %fs and
 * %gs at blocks of its own and reads them back many times, giving its core
 * away now and then: whichever core it resumes on, and whoever ran there in
 * between, it must see its own values. */
#define SXN_TLS_THREADS 3
#define SXN_TLS_ROUNDS 2000

struct tls_block {
    struct tls_block* self;
    uint64_t value;
};

static struct tls_block g_tls_blocks[SXN_TLS_THREADS + 1][2];
static long g_tls_pause = -1;

static uint64_t fs_value(void) {
    uint64_t value;
    __asm__ volatile("movq %%fs:8, %0" : "=r"(value));
    return value;
}

static uint64_t gs_value(void) {
    uint64_t value;
    __asm__ volatile("movq %%gs:8, %0" : "=r"(value));
    return value;
}

static int tls_round_trips(long index) {
    struct tls_block* fs = &g_tls_blocks[index][0];
    struct tls_block* gs = &g_tls_blocks[index][1];
    long round;

    if (sxn_set_thread_base(SXN_THREAD_BASE_FS, fs) != 0 ||
        sxn_set_thread_base(SXN_THREAD_BASE_GS, gs) != 0) {
        return 0;
    }
    for (round = 0; round < SXN_TLS_ROUNDS; ++round) {
        if (fs_value() != fs->value || gs_value() != gs->value) {
            return 0;
        }
        if ((round % 64) == 0) {
            (void)sxn_wait_one(g_tls_pause, 1);
        }
    }
    return 1;
}

static void thread_tls(void* argument) {
    sxn_thread_exit(tls_round_trips((long)argument) ? 0 : 1);
}

static void test_thread_bases(void) {
    int32_t handles[SXN_TLS_THREADS];
    int32_t code = -1;
    long index;

    for (index = 0; index <= SXN_TLS_THREADS; ++index) {
        g_tls_blocks[index][0].self = &g_tls_blocks[index][0];
        g_tls_blocks[index][0].value = 0xf5000000u + (uint64_t)index;
        g_tls_blocks[index][1].self = &g_tls_blocks[index][1];
        g_tls_blocks[index][1].value = 0x65000000u + (uint64_t)index;
    }
    g_tls_pause = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    check(g_tls_pause >= 0);

    for (index = 0; index < SXN_TLS_THREADS; ++index) {
        void* stack = thread_stack();

        check(stack != 0);
        handles[index] = (int32_t)sxn_thread_create(thread_tls, (void*)index, stack, 0);
        check(handles[index] >= 0);
    }
    /* The main thread takes part too, with the last pair of blocks. */
    check(tls_round_trips(SXN_TLS_THREADS));
    for (index = 0; index < SXN_TLS_THREADS; ++index) {
        check(sxn_wait_one(handles[index], 10000) == 0);
        check(sxn_thread_exit_code(handles[index], &code) == 0 && code == 0);
        (void)sxn_close(handles[index]);
    }

    /* Refusals: an unknown base, a non-canonical address, a kernel one. */
    check(sxn_set_thread_base(0, &g_tls_blocks[0][0]) == -SAVANXP_EINVAL);
    check(sxn_set_thread_base(SXN_THREAD_BASE_FS, (void*)0x0000800000000000ul) == -SAVANXP_EINVAL);
    check(sxn_set_thread_base(SXN_THREAD_BASE_GS, (void*)0xffff800000000000ul) == -SAVANXP_EINVAL);
    /* Still its own after the refusals. */
    check(fs_value() == g_tls_blocks[SXN_TLS_THREADS][0].value);
    check(sxn_set_thread_base(SXN_THREAD_BASE_FS, 0) == 0 && sxn_set_thread_base(SXN_THREAD_BASE_GS, 0) == 0);
    (void)sxn_close(g_tls_pause);
    check(sxn_log("nativetest: thread bases done") == 0);
}

/* Wait on an address (threads phase 4). Waiters parked on a word are
 * released by count, an address wait that times out or finds the word changed
 * comes straight back, and a three-state mutex built on the pair (the classic
 * futex mutex) keeps a counter exact while four threads hammer it. */
#define SXN_GATE_THREADS 3
#define SXN_LOCK_THREADS 4
#define SXN_LOCK_ROUNDS 5000

static volatile uint32_t g_gate;
static volatile uint32_t g_gate_ready;
static volatile long g_gate_results[SXN_GATE_THREADS];
static volatile uint32_t g_lock;
static volatile uint64_t g_locked_counter;
static volatile uint32_t g_lock_parks;
static volatile uint64_t g_wide = 0x1122334455667788ull;
static long g_address_pause = -1;

static void address_pause(long ms) {
    (void)sxn_wait_one(g_address_pause, ms);
}

static void gate_waiter(void* argument) {
    long index = (long)argument;

    __atomic_fetch_add(&g_gate_ready, 1u, __ATOMIC_SEQ_CST);
    g_gate_results[index] = sxn_wait_address(&g_gate, 0, sizeof(g_gate), 10000);
    sxn_thread_exit(0);
}

/* 0 free, 1 taken, 2 taken with waiters. */
static void futex_lock(volatile uint32_t* word) {
    uint32_t seen = 0;

    if (__atomic_compare_exchange_n(word, &seen, 1u, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        return;
    }
    if (seen != 2u) {
        seen = __atomic_exchange_n(word, 2u, __ATOMIC_SEQ_CST);
    }
    while (seen != 0u) {
        __atomic_fetch_add(&g_lock_parks, 1u, __ATOMIC_RELAXED);
        (void)sxn_wait_address(word, 2u, sizeof(*word), -1);
        seen = __atomic_exchange_n(word, 2u, __ATOMIC_SEQ_CST);
    }
}

static void futex_unlock(volatile uint32_t* word) {
    if (__atomic_fetch_sub(word, 1u, __ATOMIC_SEQ_CST) != 1u) {
        __atomic_store_n(word, 0u, __ATOMIC_SEQ_CST);
        (void)sxn_wake_address(word, 1);
    }
}

static void lock_worker(void* argument) {
    long round;
    int spin;

    (void)argument;
    for (round = 0; round < SXN_LOCK_ROUNDS; ++round) {
        uint64_t value;

        futex_lock(&g_lock);
        /* A read-modify-write wide enough to lose updates without the lock. */
        value = g_locked_counter;
        for (spin = 0; spin < 16; ++spin) {
            __asm__ volatile("pause");
        }
        g_locked_counter = value + 1u;
        /* Now and then sleep holding it, so the others park even on one core. */
        if ((round % 500) == 250) {
            address_pause(1);
        }
        futex_unlock(&g_lock);
    }
    sxn_thread_exit(0);
}

static void test_address_waits(void) {
    int32_t handles[SXN_LOCK_THREADS];
    volatile uint8_t* wide_bytes = (volatile uint8_t*)&g_wide;
    int32_t code = -1;
    uint64_t started;
    long woken = 0;
    long index;
    int tries;

    g_address_pause = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    check(g_address_pause >= 0);

    /* Straight back: the word differs, a zero timeout, a timeout that runs
     * out, the low `size` bytes compared and nothing else, bad requests. */
    g_gate = 5u;
    check(sxn_wait_address(&g_gate, 0, 4, -1) == -SAVANXP_EAGAIN);
    check(sxn_wait_address(&g_gate, 5, 4, 0) == -SAVANXP_ETIMEDOUT);
    started = sxn_clock_ns();
    check(sxn_wait_address(&g_gate, 5, 4, 30) == -SAVANXP_ETIMEDOUT);
    check(sxn_clock_ns() - started >= 25000000ull);
    check(sxn_wait_address(&g_wide, 0x1122334455667788ull, 8, 0) == -SAVANXP_ETIMEDOUT);
    check(sxn_wait_address(&g_wide, 0x1122334400000000ull, 8, 0) == -SAVANXP_EAGAIN);
    check(sxn_wait_address(wide_bytes, 0xff88u, 1, 0) == -SAVANXP_ETIMEDOUT);
    check(sxn_wait_address(wide_bytes, 0x0011u, 2, 0) == -SAVANXP_EAGAIN);
    check(sxn_wait_address(&g_gate, 5, 3, 0) == -SAVANXP_EINVAL);
    check(sxn_wait_address(wide_bytes + 1, 0, 4, 0) == -SAVANXP_EINVAL);
    check(sxn_wait_address((void*)0, 0, 4, 0) == -SAVANXP_EINVAL);
    check(sxn_wait_address((void*)0xffff800000000000ul, 0, 4, 0) == -SAVANXP_EINVAL);
    check(sxn_wake_address(&g_gate, SXN_WAKE_ALL) == 0);

    /* Three waiters on one word: one released alone, then the other two. The
     * word never changes, so only a wake lets a waiter go; a wake that finds
     * fewer parked than expected is retried after a pause. */
    g_gate = 0u;
    g_gate_ready = 0u;
    for (index = 0; index < SXN_GATE_THREADS; ++index) {
        void* stack = thread_stack();

        g_gate_results[index] = 1;
        check(stack != 0);
        handles[index] = (int32_t)sxn_thread_create(gate_waiter, (void*)index, stack, 0);
        check(handles[index] >= 0);
    }
    for (tries = 0; tries < 2000 && g_gate_ready != SXN_GATE_THREADS; ++tries) {
        address_pause(1);
    }
    check(g_gate_ready == SXN_GATE_THREADS);
    address_pause(20);
    for (tries = 0; tries < 500 && woken < 1; ++tries) {
        long step = sxn_wake_address(&g_gate, 1);

        check(step == 0 || step == 1);
        woken += step;
        if (step == 0) {
            address_pause(2);
        }
    }
    check(woken == 1);
    for (tries = 0; tries < 500 && woken < SXN_GATE_THREADS; ++tries) {
        long step = sxn_wake_address(&g_gate, SXN_WAKE_ALL);

        check(step >= 0 && step <= SXN_GATE_THREADS - woken);
        woken += step;
        if (woken < SXN_GATE_THREADS) {
            address_pause(2);
        }
    }
    check(woken == SXN_GATE_THREADS);
    for (index = 0; index < SXN_GATE_THREADS; ++index) {
        check(sxn_wait_one(handles[index], 10000) == 0);
        check(g_gate_results[index] == 0);
        (void)sxn_close(handles[index]);
    }

    /* The futex mutex. */
    g_lock = 0u;
    g_locked_counter = 0u;
    g_lock_parks = 0u;
    for (index = 0; index < SXN_LOCK_THREADS; ++index) {
        void* stack = thread_stack();

        check(stack != 0);
        handles[index] = (int32_t)sxn_thread_create(lock_worker, 0, stack, 0);
        check(handles[index] >= 0);
    }
    for (index = 0; index < SXN_LOCK_THREADS; ++index) {
        check(sxn_wait_one(handles[index], 60000) == 0);
        check(sxn_thread_exit_code(handles[index], &code) == 0 && code == 0);
        (void)sxn_close(handles[index]);
    }
    check(g_locked_counter == (uint64_t)SXN_LOCK_THREADS * SXN_LOCK_ROUNDS);
    check(g_lock == 0u);
    check(g_lock_parks != 0u);
    (void)sxn_close(g_address_pause);
    {
        /* How many times a locker parked: contention reached the kernel. */
        static const char prefix[] = "nativetest: address waits done, parks=";
        char line[sizeof(prefix) + 12];
        char digits[12];
        unsigned parks = g_lock_parks;
        unsigned count = 0;
        unsigned at;

        for (at = 0; at < sizeof(prefix) - 1u; ++at) {
            line[at] = prefix[at];
        }
        do {
            digits[count++] = (char)('0' + parks % 10u);
            parks /= 10u;
        } while (parks != 0u);
        while (count != 0u) {
            line[at++] = digits[--count];
        }
        line[at] = '\0';
        check(sxn_log(line) == 0);
    }
}

static void test_threads(const struct sxn_native_info* info) {
    int32_t handles[SXN_THREAD_WORKERS] = {-1, -1, -1};
    uint32_t tids[SXN_THREAD_WORKERS] = {0, 0, 0};
    struct sxn_process_query before;
    struct sxn_process_query after;
    int32_t code = 0;
    long index;

    check(sxn_process_query(info->pid, &before) == 0);
    g_thread_go = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    check(g_thread_go >= 0);

    for (index = 0; index < SXN_THREAD_WORKERS; ++index) {
        void* stack = thread_stack();

        check(stack != 0);
        handles[index] = (int32_t)sxn_thread_create(thread_worker, (void*)index, stack, &tids[index]);
        check(handles[index] >= 0);
        check(tids[index] != 0u && tids[index] != info->pid);
    }
    check(tids[0] != tids[1] && tids[1] != tids[2] && tids[0] != tids[2]);

    /* Nothing ended yet: no exit code, and a zero-timeout wait times out. */
    check(sxn_thread_exit_code(handles[0], &code) == -SAVANXP_EAGAIN);
    check(sxn_wait_many(handles, SXN_THREAD_WORKERS, 0) == -SAVANXP_ETIMEDOUT);

    check(sxn_event_set(g_thread_go) == 0);
    for (index = 0; index < SXN_THREAD_WORKERS; ++index) {
        check(sxn_wait_one(handles[index], 5000) == 0);
        check(sxn_thread_exit_code(handles[index], &code) == 0 && code == 100 + (int32_t)index);
        check(g_thread_marks[index] == 0x7700u + (uint32_t)index);
        /* Manual reset: an ended thread stays signaled. */
        check(sxn_wait_one(handles[index], 0) == 0);
    }

    /* Refusals: no entry, no stack, a handle that is not a thread. */
    check(sxn_thread_create(0, 0, thread_stack(), 0) == -SAVANXP_EINVAL);
    check(sxn_thread_create(thread_worker, 0, 0, 0) == -SAVANXP_EINVAL);
    check(sxn_thread_exit_code(g_thread_go, &code) == -SAVANXP_EBADF);

    for (index = 0; index < SXN_THREAD_WORKERS; ++index) {
        (void)sxn_close(handles[index]);
    }
    (void)sxn_close(g_thread_go);
    check(sxn_process_query(info->pid, &after) == 0 && after.handle_count == before.handle_count);

    /* A process killed with a thread blocked, one spinning and the main one
     * blocked: TERMINATE ends them all and waitpid sees the status. */
    {
        char* child_argv[2];
        struct sxn_spawn_request request;
        int32_t report[2] = {-1, -1};
        char ready = 0;
        long child = -1;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "threads";
        check(sxn_pipe_create(report) == 0);
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 2);
        request.std_handles[1] = report[1];
        child = sxn_spawn_ex(&request);
        check(child > 0);
        (void)sxn_close(report[1]);
        if (child > 0) {
            check(sxn_read(report[0], &ready, 1) == 1 && ready == 'T');
            check(sxn_terminate(child, SXN_THREAD_KILL_STATUS) == 0);
            check(sxn_waitpid(child, &status) == child && status == SXN_THREAD_KILL_STATUS);
        }
        (void)sxn_close(report[0]);
    }

    /* The last live thread ends the process, with its own code. */
    {
        char* child_argv[2];
        long child;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "lastexit";
        child = sxn_spawn_stdio("/bin/nativetest.sxe", child_argv, 2);
        check(child > 0);
        check(child <= 0 || (sxn_waitpid(child, &status) == child && status == SXN_THREAD_LAST_STATUS));
    }

    /* SXN_SYS_EXIT from any thread ends them all. */
    {
        char* child_argv[2];
        long child;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "exitall";
        child = sxn_spawn_stdio("/bin/nativetest.sxe", child_argv, 2);
        check(child > 0);
        check(child <= 0 || (sxn_waitpid(child, &status) == child && status == SXN_THREAD_EXITALL_STATUS));
    }

    /* A view unmapped while a sibling reads it on another core: the reader
     * faults instead of reading on through a stale translation. */
    {
        char* child_argv[2];
        long child;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "shootdown";
        child = sxn_spawn_stdio("/bin/nativetest.sxe", child_argv, 2);
        check(child > 0);
        check(child <= 0 || (sxn_waitpid(child, &status) == child && status == SXN_PAGE_FAULT_STATUS));
    }

    check(sxn_log("nativetest: threads done") == 0);
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
    if (argc > 1 && streq(argv[1], "stdiochild")) {
        run_stdio_child();
    }
    if (argc > 1 && streq(argv[1], "middle")) {
        run_middle_child();
    }
    if (argc > 1 && streq(argv[1], "threads")) {
        run_threads_child();
    }
    if (argc > 1 && streq(argv[1], "lastexit")) {
        run_last_exit_child();
    }
    if (argc > 1 && streq(argv[1], "exitall")) {
        run_exit_all_child();
    }
    if (argc > 1 && streq(argv[1], "shootdown")) {
        run_shootdown_child();
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
        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "child";
        child_argv[2] = event_text;
        child_argv[3] = section_text;
        child_argv[4] = request_text;
        child = sxn_spawn("/bin/nativetest.sxe", child_argv, 5);
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
        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "portchild";
        child = sxn_spawn("/bin/nativetest.sxe", child_argv, 2);
        check(child > 0);
        if (port >= 0 && event >= 0 && child > 0) {
            check(sxn_wait_one(port, 5000) == 0);
            check(sxn_port_accept(port, &connection) == 0);
            check(connection.client_pid == (uint32_t)child);
            /* WAIT_MANY on an event and a stream: the child's "ping" makes the
             * pipe readable, which is index 1 (and may still be on its way:
             * this also exercises waking a parked wait from a pipe write). */
            {
                int32_t targets[2];

                targets[0] = (int32_t)event;
                targets[1] = connection.read_handle;
                check(sxn_wait_many(targets, 2, 5000) == 1);
            }
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

    /* WAIT_MANY without a second process: the index of what is signaled, a
     * timeout when nothing is, and a stream that is readable or at its end.
     * The port connects to itself to get a pipe. */
    {
        struct sxn_port_connection self;
        int32_t pipes[2] = {-1, -1};
        int32_t targets[3];
        long port = sxn_port_create("nativetest-wait");
        long quiet = sxn_event_create(SXN_EVENT_MANUAL_RESET);
        long loud = sxn_event_create(SXN_EVENT_MANUAL_RESET);

        check(port >= 0 && quiet >= 0 && loud >= 0);
        check(sxn_port_connect("nativetest-wait", pipes) == 0);
        check(sxn_port_accept(port, &self) == 0);
        targets[0] = (int32_t)quiet;
        targets[1] = (int32_t)loud;
        targets[2] = self.read_handle;
        check(sxn_wait_many(targets, 3, 0) == -SAVANXP_ETIMEDOUT);
        check(sxn_wait_many(targets, 3, 20) == -SAVANXP_ETIMEDOUT);
        check(sxn_event_set(loud) == 0);
        check(sxn_wait_many(targets, 3, 0) == 1);
        check(sxn_event_reset(loud) == 0);
        check(sxn_write(pipes[0], "x", 1) == 1);
        check(sxn_wait_many(targets, 3, 0) == 2);
        /* Level-triggered: still readable until read. */
        check(sxn_wait_many(targets, 3, 0) == 2);
        {
            char byte = 0;
            check(sxn_read(self.read_handle, &byte, 1) == 1 && byte == 'x');
        }
        check(sxn_wait_many(targets, 3, 0) == -SAVANXP_ETIMEDOUT);
        /* The writer gone: end of stream is readable too. */
        check(sxn_close(pipes[0]) == 0);
        check(sxn_wait_many(targets, 3, 0) == 2);
        /* A write end cannot be waited on; neither can nothing. */
        targets[0] = self.write_handle;
        check(sxn_wait_many(targets, 1, 0) == -SAVANXP_EINVAL);
        check(sxn_wait_many(targets, 0, 0) == -SAVANXP_EINVAL);
        /* SET_NONBLOCKING: an empty read and a write with no room say EAGAIN
         * instead of parking the only thread of this process. */
        {
            static char fill[8192];
            char byte = 0;

            /* The reply direction: its writer is alive, so empty is EAGAIN
             * (the request direction already reads as ended). */
            check(sxn_set_nonblocking(pipes[1], 1) == 0);
            check(sxn_read(pipes[1], &byte, 1) == -SAVANXP_EAGAIN);
            check(sxn_set_nonblocking(self.write_handle, 1) == 0);
            check(sxn_write(self.write_handle, fill, sizeof(fill)) == (long)sizeof(fill));
            check(sxn_write(self.write_handle, fill, 1) == -SAVANXP_EAGAIN);
            check(sxn_set_nonblocking(quiet, 1) == -SAVANXP_EBADF);
        }
        check(sxn_close(pipes[1]) == 0 && sxn_close(self.read_handle) == 0 && sxn_close(self.write_handle) == 0);
        check(sxn_close(quiet) == 0 && sxn_close(loud) == 0 && sxn_close(port) == 0);
        check(sxn_log("nativetest: wait many done") == 0);
    }

    /* SPAWN_STDIO: the child gets stdio and nothing else this process holds;
     * SPAWN clones the whole table. */
    {
        char* child_argv[2];
        struct sxn_process_query whole;
        struct sxn_process_query bare;
        long cloned = -1;
        long isolated = -1;
        int status = 0;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "sleeper";
        cloned = sxn_spawn("/bin/nativetest.sxe", child_argv, 2);
        isolated = sxn_spawn_stdio("/bin/nativetest.sxe", child_argv, 2);
        check(cloned > 0 && isolated > 0);
        if (cloned > 0 && isolated > 0) {
            check(sxn_process_query(cloned, &whole) == 0 && sxn_process_query(isolated, &bare) == 0);
            /* stdio, plus the event a sleeper creates as soon as it runs: on
             * several cores it may already be running here. */
            check(bare.handle_count <= 4u && whole.handle_count > bare.handle_count);
            check(sxn_terminate(cloned, 0) == 0 && sxn_waitpid(cloned, &status) == cloned);
            check(sxn_terminate(isolated, 0) == 0 && sxn_waitpid(isolated, &status) == isolated);
        }
        check(sxn_log("nativetest: spawn stdio done") == 0);
    }

    /* TERMINATE: a parent ends its child; anybody else's process is refused. */
    {
        char* child_argv[2];
        long child = -1;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "sleeper";
        child = sxn_spawn("/bin/nativetest.sxe", child_argv, 2);
        check(child > 0);
        if (child > 0) {
            struct sxn_process_query query;

            check(sxn_process_query(child, &query) == 0);
            check(query.pid == (uint32_t)child && query.parent_pid == info.pid &&
                query.state == SXN_PROCESS_RUNNING && streq(query.name, "nativetest.sxe"));
            check(sxn_terminate(child, SXN_TERMINATE_STATUS) == 0);
            /* Ended but not reaped: still there, with its status. */
            check(sxn_process_query(child, &query) == 0);
            check(query.state == SXN_PROCESS_ENDED && query.exit_code == SXN_TERMINATE_STATUS);
            check(sxn_waitpid(child, &status) == child && status == SXN_TERMINATE_STATUS);
            /* Reaped: the pid names nothing any more. */
            check(sxn_terminate(child, 1) == -SAVANXP_ENOENT);
            check(sxn_process_query(child, &query) == -SAVANXP_ENOENT);
        }
        {
            struct sxn_process_query self;

            check(sxn_process_query(info.pid, &self) == 0);
            check(self.pid == info.pid && self.state == SXN_PROCESS_RUNNING && self.handle_count >= 3u);
            check(sxn_process_query(0, &self) == -SAVANXP_ENOENT);
        }
        /* pid 1 is the idle task: not this process's child. */
        check(sxn_terminate(1, 1) == -SAVANXP_ECHILD);
        check(sxn_terminate(0, 1) == -SAVANXP_ENOENT);
        check(sxn_log("nativetest: terminate and query done") == 0);
    }

    /* PIPE_CREATE: a pipe made by a native process, both ends in its table.
     * Data crosses, the read end is a wait target, and closing the write end
     * is end of stream. */
    {
        int32_t pipe_handles[2] = {-1, -1};
        char buffer[8] = {0};

        check(sxn_pipe_create(pipe_handles) == 0);
        check(pipe_handles[0] >= 0 && pipe_handles[1] >= 0 && pipe_handles[0] != pipe_handles[1]);
        check(sxn_wait_one(pipe_handles[0], 0) == -SAVANXP_ETIMEDOUT);
        check(sxn_write(pipe_handles[1], "abc", 3) == 3);
        check(sxn_wait_many(&pipe_handles[0], 1, 100) == 0);
        check(sxn_read(pipe_handles[0], buffer, sizeof(buffer)) == 3 && buffer[0] == 'a' && buffer[2] == 'c');
        check(sxn_write(pipe_handles[0], "z", 1) < 0);
        check(sxn_close(pipe_handles[1]) == 0);
        check(sxn_read(pipe_handles[0], buffer, sizeof(buffer)) == 0);
        check(sxn_close(pipe_handles[0]) == 0);
        check(sxn_pipe_create(0) == -SAVANXP_EINVAL);
        check(sxn_log("nativetest: pipe create done") == 0);
    }

    /* SPAWN_EX: the child's 0, 1, 2 and 3 are exactly the handles named, and
     * it holds nothing else. Two pipes for stdio (stderr shares stdout, as a
     * console host does) and a third as handle 3. */
    {
        char* child_argv[2];
        struct sxn_spawn_request request;
        int32_t input[2] = {-1, -1};
        int32_t output[2] = {-1, -1};
        int32_t extra[2] = {-1, -1};
        char buffer[16] = {0};
        long child = -1;
        long total = 0;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "stdiochild";
        check(sxn_pipe_create(input) == 0 && sxn_pipe_create(output) == 0 && sxn_pipe_create(extra) == 0);
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 2);
        request.std_handles[0] = input[0];
        request.std_handles[1] = output[1];
        request.std_handles[2] = output[1];
        request.handles[0] = extra[1];
        request.handle_count = 1;
        child = sxn_spawn_ex(&request);
        check(child > 0);
        /* The child holds its own references: the parent's copies of the ends
         * it gave away go, so end of stream reaches the parent. */
        (void)sxn_close(input[0]);
        (void)sxn_close(output[1]);
        (void)sxn_close(extra[1]);
        if (child > 0) {
            check(sxn_write(input[1], "hi\n", 3) == 3);
            for (;;) {
                long count = sxn_read(output[0], buffer + total, sizeof(buffer) - 1u - (unsigned long)total);
                if (count <= 0) {
                    break;
                }
                total += count;
            }
            buffer[total] = '\0';
            check(streq(buffer, "hi\nerr"));
            check(sxn_read(extra[0], buffer, sizeof(buffer)) == 2 && buffer[0] == 'x' && buffer[1] == '3');
            check(sxn_waitpid(child, &status) == child && status == 0);
        }
        (void)sxn_close(input[1]);
        (void)sxn_close(output[0]);
        (void)sxn_close(extra[0]);

        /* Refusals: a handle that is not open, a flag that does not exist,
         * more extra handles than fit. */
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 2);
        request.std_handles[1] = 60;
        check(sxn_spawn_ex(&request) == -SAVANXP_EBADF);
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 2);
        request.flags = 1;
        check(sxn_spawn_ex(&request) == -SAVANXP_EINVAL);
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 2);
        request.handle_count = SXN_SPAWN_MAX_HANDLES + 1u;
        check(sxn_spawn_ex(&request) == -SAVANXP_EINVAL);
        check(sxn_log("nativetest: spawn ex done") == 0);
    }

    /* TERMINATE reaches a grandchild: the middle child reports its sleeper's
     * pid, this process ends the sleeper, and the middle child exits with the
     * status its waitpid saw. */
    {
        char* child_argv[2];
        struct sxn_spawn_request request;
        int32_t report[2] = {-1, -1};
        int32_t grandchild = 0;
        long middle = -1;
        int status = -1;

        child_argv[0] = "/bin/nativetest.sxe";
        child_argv[1] = "middle";
        check(sxn_pipe_create(report) == 0);
        sxn_spawn_request_init(&request, "/bin/nativetest.sxe", child_argv, 2);
        request.std_handles[1] = report[1];
        middle = sxn_spawn_ex(&request);
        check(middle > 0);
        (void)sxn_close(report[1]);
        if (middle > 0) {
            check(sxn_read(report[0], &grandchild, sizeof(grandchild)) == (long)sizeof(grandchild));
            check(grandchild > 0 && grandchild != (int32_t)middle);
            if (grandchild > 0) {
                struct sxn_process_query query;

                check(sxn_process_query(grandchild, &query) == 0 && query.parent_pid == (uint32_t)middle);
                check(sxn_terminate(grandchild, SXN_TERMINATE_STATUS) == 0);
            }
            check(sxn_waitpid(middle, &status) == middle && status == SXN_TERMINATE_STATUS);
        }
        (void)sxn_close(report[0]);
        check(sxn_log("nativetest: terminate descendant done") == 0);
    }

    test_threads(&info);
    test_thread_bases();
    test_address_waits();

    /* The heap (docs/userland/NATIVE_HEAP.md). The bootstrap arena is BSS and
     * needs no system call; the arenas past it come from the native page
     * source (system/runtime/sxn_runtime.c): sections, mapped and their
     * handles closed. Several MiB held at once, each block its own pattern so
     * an overlap shows; no descriptor left behind; then allocate-and-free
     * round after round, where every free empties an arena and gives it back
     * (sx_arena_release) and the next allocation maps one again. A refused
     * 1 MiB here would mean the runtime's weak POSIX page source is the one
     * linked: ENOSYS. */
    {
        enum { HEAP_BLOCKS = 4 };
        static const unsigned long sizes[HEAP_BLOCKS] = {
            64ul * 1024ul, 1024ul * 1024ul, 3ul * 1024ul * 1024ul, 6ul * 1024ul * 1024ul};
        unsigned char* blocks[HEAP_BLOCKS] = {0, 0, 0, 0};
        struct sxn_process_query before;
        struct sxn_process_query after;
        unsigned long index;
        unsigned long offset;
        int round;
        int intact = 1;

        check(sxn_process_query(info.pid, &before) == 0);
        for (index = 0; index < HEAP_BLOCKS; ++index) {
            blocks[index] = (unsigned char*)malloc(sizes[index]);
            check(blocks[index] != 0);
        }
        for (index = 0; index < HEAP_BLOCKS; ++index) {
            if (blocks[index] == 0) {
                continue;
            }
            for (offset = 0; offset < sizes[index]; offset += 4096ul) {
                blocks[index][offset] = (unsigned char)(index * 37u + (offset >> 12));
            }
            blocks[index][sizes[index] - 1u] = (unsigned char)(0xA0u + index);
        }
        for (index = 0; index < HEAP_BLOCKS; ++index) {
            if (blocks[index] == 0) {
                continue;
            }
            for (offset = 0; offset < sizes[index]; offset += 4096ul) {
                if (blocks[index][offset] != (unsigned char)(index * 37u + (offset >> 12))) {
                    intact = 0;
                }
            }
            if (blocks[index][sizes[index] - 1u] != (unsigned char)(0xA0u + index)) {
                intact = 0;
            }
        }
        check(intact);
        for (index = 0; index < HEAP_BLOCKS; ++index) {
            free(blocks[index]);
        }
        check(sxn_process_query(info.pid, &after) == 0 && after.handle_count == before.handle_count);

        for (round = 0; round < 24; ++round) {
            unsigned long size = 2ul * 1024ul * 1024ul + (unsigned long)round * 4096ul;
            unsigned char* block = (unsigned char*)malloc(size);

            check(block != 0);
            if (block == 0) {
                break;
            }
            block[0] = (unsigned char)round;
            block[size - 1u] = (unsigned char)~round;
            check(block[0] == (unsigned char)round && block[size - 1u] == (unsigned char)~round);
            free(block);
        }
        check(sxn_log("nativetest: heap: arenas from the native page source") == 0);
    }

    sxn_exit(g_failures == 0 ? 0 : 1);
    __builtin_unreachable();
}
