/*
 * POSIX threads and thread-local storage (docs/kernel/THREADS_ROADMAP.md,
 * phase 5).
 *
 * A pthread is a native thread of the process (SXN_SYS_THREAD_CREATE): the
 * kernel gives it a handle that is signaled when it ends, which is what join
 * waits on. Its stack, its TLS block, its TCB and this file's descriptor live
 * in one anonymous mapping, unmapped by whoever joins it -- or, for a
 * detached thread, by the next pthread_create, join or detach after the
 * kernel says it ended.
 *
 * Mutexes, condition variables and once are words in user memory: the
 * uncontended path is one atomic instruction, and a thread enters the kernel
 * only to sleep on the word (SXN_SYS_WAIT_ADDRESS) or wake its sleepers.
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "savanxp/libc.h"
#include "sxn/sxn.h"
#include "sx_thread.h"

extern uintptr_t __stack_chk_guard;

int sx_tls_ready = 0;
volatile int sx_threads_started = 0;

/* --- the word locks -------------------------------------------------------- */

/* 0 free, 1 taken, 2 taken and somebody may sleep on it. */
static void sx_word_lock(volatile uint32_t* word) {
    uint32_t seen = 0;

    if (__atomic_compare_exchange_n(word, &seen, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return;
    }
    if (seen != 2u) {
        seen = __atomic_exchange_n(word, 2u, __ATOMIC_ACQUIRE);
    }
    while (seen != 0u) {
        (void)sxn_wait_address(word, 2u, sizeof(*word), -1);
        seen = __atomic_exchange_n(word, 2u, __ATOMIC_ACQUIRE);
    }
}

static int sx_word_trylock(volatile uint32_t* word) {
    uint32_t seen = 0;
    return __atomic_compare_exchange_n(word, &seen, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void sx_word_unlock(volatile uint32_t* word) {
    if (__atomic_exchange_n(word, 0u, __ATOMIC_RELEASE) == 2u) {
        (void)sxn_wake_address(word, 1);
    }
}

void sx_lock_acquire(struct sx_lock* lock) {
    uintptr_t self;

    if (!__atomic_load_n(&sx_threads_started, __ATOMIC_ACQUIRE)) {
        return;
    }
    self = (uintptr_t)sx_tcb_self();
    if (__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) == self) {
        lock->depth += 1u;
        return;
    }
    sx_word_lock(&lock->word);
    __atomic_store_n(&lock->owner, self, __ATOMIC_RELAXED);
    lock->depth = 1u;
}

void sx_lock_release(struct sx_lock* lock) {
    /* Taken before the first thread existed: there was nothing to release.
     * pthread_create sets the flag before it takes any lock itself, and
     * nothing creates a thread from inside the heap or stdio, so a lock is
     * never released by a thread that did not take it. */
    if (!__atomic_load_n(&sx_threads_started, __ATOMIC_ACQUIRE) || lock->depth == 0u) {
        return;
    }
    lock->depth -= 1u;
    if (lock->depth != 0u) {
        return;
    }
    __atomic_store_n(&lock->owner, 0, __ATOMIC_RELAXED);
    sx_word_unlock(&lock->word);
}

/* --- thread-local storage -------------------------------------------------- */

#define SX_PT_LOAD 1u
#define SX_PT_TLS 7u
/* EI_OSABI of a native program (docs/userland/NATIVE_APPS.md). */
#define SX_OSABI_NATIVE 0x53u

struct sx_elf_header {
    unsigned char ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct sx_program_header {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

/* The executable's PT_TLS: the initial image every thread's block starts as.
 * Libraries cannot have one -- the loader refuses their TLS relocations --
 * so this is all the static TLS there is. */
static const unsigned char* g_tls_image = 0;
static size_t g_tls_image_bytes = 0;
static size_t g_tls_bytes = 0;
static size_t g_tls_align = 16;

static size_t sx_align_up(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static void sx_find_tls_image(void) {
    const struct sx_elf_header* header = (const struct sx_elf_header*)sx_image_base;
    const struct sx_program_header* headers;
    uint64_t first_vaddr = 0;
    int found_first = 0;
    unsigned long bias;
    uint16_t index;

    if (header == 0 || header->ident[0] != 0x7f || header->phentsize != sizeof(struct sx_program_header)) {
        return;
    }
    headers = (const struct sx_program_header*)(sx_image_base + header->phoff);
    /* The load bias the way the loader computes it (ldso.c): where the
     * header landed minus where the first PT_LOAD says the image starts. */
    for (index = 0; index < header->phnum; ++index) {
        if (headers[index].type == SX_PT_LOAD && headers[index].memsz != 0) {
            first_vaddr = headers[index].vaddr;
            found_first = 1;
            break;
        }
    }
    if (!found_first) {
        return;
    }
    bias = sx_image_base - (unsigned long)(first_vaddr & ~(uint64_t)4095u);
    for (index = 0; index < header->phnum; ++index) {
        const struct sx_program_header* tls = &headers[index];

        if (tls->type != SX_PT_TLS) {
            continue;
        }
        g_tls_image = (const unsigned char*)(bias + tls->vaddr);
        g_tls_image_bytes = (size_t)tls->filesz;
        g_tls_align = tls->align > 16u ? (size_t)tls->align : 16u;
        g_tls_bytes = sx_align_up((size_t)tls->memsz, g_tls_align);
        return;
    }
}

/* Bytes a thread needs for its TLS block and TCB, wherever they land. */
static size_t sx_tls_area_bytes(void) {
    return g_tls_bytes + g_tls_align + sizeof(struct sx_tcb);
}

/* Lays out the static TLS block and the TCB inside `area` and returns the
 * TCB: the block ends exactly where the TCB starts, which is what the
 * offsets the linker computed (variant II) assume. */
static struct sx_tcb* sx_tls_setup(unsigned char* area) {
    const uintptr_t tcb_address = sx_align_up((uintptr_t)area + g_tls_bytes, g_tls_align);
    struct sx_tcb* tcb = (struct sx_tcb*)tcb_address;
    unsigned char* block = (unsigned char*)(tcb_address - g_tls_bytes);

    if (g_tls_bytes != 0u) {
        if (g_tls_image_bytes != 0u) {
            memcpy(block, g_tls_image, g_tls_image_bytes);
        }
        memset(block + g_tls_image_bytes, 0, g_tls_bytes - g_tls_image_bytes);
    }
    memset(tcb, 0, sizeof(*tcb));
    tcb->self = tcb;
    tcb->self_again = tcb;
    tcb->stack_guard = (uint64_t)__stack_chk_guard;
    return tcb;
}

/* --- threads ---------------------------------------------------------------- */

enum {
    SX_THREAD_JOINABLE = 0,
    SX_THREAD_DETACHED = 1,
    SX_THREAD_EXITED = 2,
};

struct sx_specific {
    unsigned int generation;
    void* value;
};

struct sx_pthread {
    struct sx_tcb* tcb;
    void* (*start)(void*);
    void* argument;
    void* result;
    /* The native handle; -1 until pthread_create stores it. */
    volatile long handle;
    unsigned char* block;
    size_t block_bytes;
    volatile int state;
    struct sx_pthread* next_ended;
    struct sx_specific specific[PTHREAD_KEYS_MAX];
};

static struct sx_pthread g_main_thread;
/* The main thread's TLS block and TCB, when they fit; a bigger PT_TLS is
 * mapped instead. */
static unsigned char g_main_tls_area[1024] __attribute__((aligned(64)));

/* Detached threads that ended, waiting for their mapping to be released. */
static struct sx_lock g_ended_lock = SX_LOCK_INIT;
static struct sx_pthread* g_ended = 0;

void sx_tls_init(void) {
    const struct sx_elf_header* header = (const struct sx_elf_header*)sx_image_base;
    unsigned char* area = g_main_tls_area;
    struct sx_tcb* tcb;
    const int early_errno = errno;

    /* A native program links this runtime too, but its segment bases are its
     * own (SXN_SYS_SET_THREAD_BASE): it keeps a global errno and has no
     * pthreads. */
    if (header == 0 || header->ident[7] == SX_OSABI_NATIVE) {
        return;
    }
    sx_find_tls_image();
    if (sx_tls_area_bytes() > sizeof(g_main_tls_area)) {
        area = (unsigned char*)mmap(0, sx_tls_area_bytes(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (area == (unsigned char*)MAP_FAILED) {
            /* No TLS and no threads: errno stays the global it was. */
            return;
        }
    }
    tcb = sx_tls_setup(area);
    g_main_thread.tcb = tcb;
    g_main_thread.handle = -1;
    g_main_thread.state = SX_THREAD_DETACHED;
    tcb->thread = &g_main_thread;
    if (sxn_set_thread_base(SXN_THREAD_BASE_FS, tcb) != 0) {
        return;
    }
    tcb->errno_value = early_errno;
    sx_tls_ready = 1;
}

static struct sx_pthread* sx_current(void) {
    return sx_tls_ready ? sx_tcb_self()->thread : &g_main_thread;
}

static void sx_release_thread_block(struct sx_pthread* thread) {
    (void)sxn_close(thread->handle);
    (void)munmap(thread->block, thread->block_bytes);
}

/* Frees the mappings of detached threads the kernel says have ended. */
static void sx_reap_ended_threads(void) {
    struct sx_pthread** link;

    sx_lock_acquire(&g_ended_lock);
    link = &g_ended;
    while (*link != 0) {
        struct sx_pthread* thread = *link;
        const long handle = __atomic_load_n(&thread->handle, __ATOMIC_ACQUIRE);

        if (handle >= 0 && sxn_wait_one(handle, 0) == 0) {
            *link = thread->next_ended;
            sx_release_thread_block(thread);
            continue;
        }
        link = &thread->next_ended;
    }
    sx_lock_release(&g_ended_lock);
}

static void sx_run_key_destructors(struct sx_pthread* self);

static void sx_thread_entry(void* argument) {
    struct sx_pthread* self = (struct sx_pthread*)argument;

    /* A new thread starts with %fs at 0: nothing may touch errno or TLS
     * before this. */
    if (sxn_set_thread_base(SXN_THREAD_BASE_FS, self->tcb) != 0) {
        sxn_thread_exit(127);
    }
    pthread_exit(self->start(self->argument));
}

int pthread_create(pthread_t* out, const pthread_attr_t* attr, void* (*start)(void*), void* argument) {
    size_t stack_bytes = attr != 0 ? attr->__stacksize : PTHREAD_STACK_DEFAULT;
    const int detached = attr != 0 && attr->__detachstate == PTHREAD_CREATE_DETACHED;
    size_t total;
    unsigned char* block;
    struct sx_pthread* thread;
    long handle;

    if (out == 0 || start == 0) {
        return EINVAL;
    }
    if (!sx_tls_ready) {
        return EAGAIN;
    }
    /* Before any runtime lock is taken: from here on they are real. */
    __atomic_store_n(&sx_threads_started, 1, __ATOMIC_SEQ_CST);
    sx_reap_ended_threads();

    stack_bytes = sx_align_up(stack_bytes, 16u);
    total = sx_align_up(stack_bytes + sx_tls_area_bytes() + sx_align_up(sizeof(struct sx_pthread), 16u) + 16u, 4096u);
    block = (unsigned char*)mmap(0, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == (unsigned char*)MAP_FAILED) {
        return EAGAIN;
    }
    thread = (struct sx_pthread*)sx_align_up((uintptr_t)block + stack_bytes + sx_tls_area_bytes(), 16u);
    memset(thread, 0, sizeof(*thread));
    thread->tcb = sx_tls_setup(block + stack_bytes);
    thread->tcb->thread = thread;
    thread->start = start;
    thread->argument = argument;
    thread->block = block;
    thread->block_bytes = total;
    thread->handle = -1;
    thread->state = detached ? SX_THREAD_DETACHED : SX_THREAD_JOINABLE;

    handle = sxn_thread_create(sx_thread_entry, thread, block + stack_bytes, 0);
    if (handle < 0) {
        (void)munmap(block, total);
        return handle == -SAVANXP_ENOMEM ? EAGAIN : (int)-handle;
    }
    /* The thread may already be running, and even have ended: a detached one
     * waits on the ended list until this is stored. */
    __atomic_store_n(&thread->handle, handle, __ATOMIC_RELEASE);
    *out = (pthread_t)thread;
    return 0;
}

void pthread_exit(void* result) {
    struct sx_pthread* self = sx_current();
    int expected = SX_THREAD_JOINABLE;

    sx_run_key_destructors(self);
    self->result = result;
    /* The main thread has no mapping and nobody joins it; when it ends while
     * others run, the process goes on until the last one ends. */
    if (self == &g_main_thread) {
        sxn_thread_exit(0);
    }
    if (__atomic_compare_exchange_n(&self->state, &expected, SX_THREAD_EXITED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        sxn_thread_exit(0);
    }
    /* Detached: its stack is the one in use, so another thread unmaps it
     * once the kernel has seen this thread end. */
    sx_lock_acquire(&g_ended_lock);
    self->next_ended = g_ended;
    g_ended = self;
    sx_lock_release(&g_ended_lock);
    sxn_thread_exit(0);
}

int pthread_join(pthread_t handle, void** result) {
    struct sx_pthread* thread = (struct sx_pthread*)handle;

    if (thread == 0) {
        return ESRCH;
    }
    if (thread == sx_current()) {
        return EDEADLK;
    }
    if (thread == &g_main_thread || __atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) == SX_THREAD_DETACHED) {
        return EINVAL;
    }
    if (sxn_wait_one(thread->handle, -1) != 0) {
        return ESRCH;
    }
    if (result != 0) {
        *result = thread->result;
    }
    sx_release_thread_block(thread);
    sx_reap_ended_threads();
    return 0;
}

int pthread_detach(pthread_t handle) {
    struct sx_pthread* thread = (struct sx_pthread*)handle;
    int expected = SX_THREAD_JOINABLE;

    if (thread == 0) {
        return ESRCH;
    }
    if (thread == &g_main_thread) {
        return EINVAL;
    }
    if (__atomic_compare_exchange_n(&thread->state, &expected, SX_THREAD_DETACHED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        sx_reap_ended_threads();
        return 0;
    }
    if (expected == SX_THREAD_DETACHED) {
        return EINVAL;
    }
    /* It ended before anybody joined it: nobody will, so release it now. */
    (void)sxn_wait_one(thread->handle, -1);
    sx_release_thread_block(thread);
    return 0;
}

pthread_t pthread_self(void) {
    return (pthread_t)sx_current();
}

int pthread_equal(pthread_t left, pthread_t right) {
    return left == right;
}

int sched_yield(void) {
    (void)yield();
    return 0;
}

/* --- attributes ------------------------------------------------------------- */

int pthread_attr_init(pthread_attr_t* attr) {
    if (attr == 0) {
        return EINVAL;
    }
    attr->__stacksize = PTHREAD_STACK_DEFAULT;
    attr->__detachstate = PTHREAD_CREATE_JOINABLE;
    attr->__reserved = 0;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t* attr) {
    return attr != 0 ? 0 : EINVAL;
}

int pthread_attr_setstacksize(pthread_attr_t* attr, size_t size) {
    if (attr == 0 || size < PTHREAD_STACK_MIN) {
        return EINVAL;
    }
    attr->__stacksize = size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t* attr, size_t* size) {
    if (attr == 0 || size == 0) {
        return EINVAL;
    }
    *size = attr->__stacksize;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t* attr, int state) {
    if (attr == 0 || (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)) {
        return EINVAL;
    }
    attr->__detachstate = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t* attr, int* state) {
    if (attr == 0 || state == 0) {
        return EINVAL;
    }
    *state = attr->__detachstate;
    return 0;
}

/* --- mutexes ---------------------------------------------------------------- */

int pthread_mutexattr_init(pthread_mutexattr_t* attr) {
    if (attr == 0) {
        return EINVAL;
    }
    attr->__kind = PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t* attr) {
    return attr != 0 ? 0 : EINVAL;
}

int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int kind) {
    if (attr == 0 || kind < PTHREAD_MUTEX_NORMAL || kind > PTHREAD_MUTEX_ERRORCHECK) {
        return EINVAL;
    }
    attr->__kind = kind;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t* attr, int* kind) {
    if (attr == 0 || kind == 0) {
        return EINVAL;
    }
    *kind = attr->__kind;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    if (mutex == 0) {
        return EINVAL;
    }
    memset((void*)mutex, 0, sizeof(*mutex));
    mutex->__kind = attr != 0 ? attr->__kind : PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex) {
    if (mutex == 0) {
        return EINVAL;
    }
    return mutex->__lock != 0u ? EBUSY : 0;
}

int pthread_mutex_lock(pthread_mutex_t* mutex) {
    unsigned long self;

    if (mutex == 0) {
        return EINVAL;
    }
    if (mutex->__kind == PTHREAD_MUTEX_NORMAL) {
        sx_word_lock((volatile uint32_t*)&mutex->__lock);
        return 0;
    }
    self = (unsigned long)sx_current();
    if (mutex->__owner == self) {
        if (mutex->__kind == PTHREAD_MUTEX_ERRORCHECK) {
            return EDEADLK;
        }
        if (mutex->__count == 0xffffffffu) {
            return EAGAIN;
        }
        mutex->__count += 1u;
        return 0;
    }
    sx_word_lock((volatile uint32_t*)&mutex->__lock);
    mutex->__owner = self;
    mutex->__count = 1u;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    unsigned long self;

    if (mutex == 0) {
        return EINVAL;
    }
    if (mutex->__kind == PTHREAD_MUTEX_NORMAL) {
        return sx_word_trylock((volatile uint32_t*)&mutex->__lock) ? 0 : EBUSY;
    }
    self = (unsigned long)sx_current();
    if (mutex->__owner == self) {
        if (mutex->__kind == PTHREAD_MUTEX_ERRORCHECK) {
            return EBUSY;
        }
        mutex->__count += 1u;
        return 0;
    }
    if (!sx_word_trylock((volatile uint32_t*)&mutex->__lock)) {
        return EBUSY;
    }
    mutex->__owner = self;
    mutex->__count = 1u;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    if (mutex == 0) {
        return EINVAL;
    }
    if (mutex->__kind != PTHREAD_MUTEX_NORMAL) {
        if (mutex->__owner != (unsigned long)sx_current()) {
            return EPERM;
        }
        mutex->__count -= 1u;
        if (mutex->__count != 0u) {
            return 0;
        }
        mutex->__owner = 0;
    }
    sx_word_unlock((volatile uint32_t*)&mutex->__lock);
    return 0;
}

/* --- condition variables --------------------------------------------------- */

int pthread_condattr_init(pthread_condattr_t* attr) {
    if (attr == 0) {
        return EINVAL;
    }
    attr->__clock = CLOCK_REALTIME;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t* attr) {
    return attr != 0 ? 0 : EINVAL;
}

int pthread_condattr_setclock(pthread_condattr_t* attr, int clock) {
    if (attr == 0 || (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)) {
        return EINVAL;
    }
    attr->__clock = clock;
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t* attr, int* clock) {
    if (attr == 0 || clock == 0) {
        return EINVAL;
    }
    *clock = attr->__clock;
    return 0;
}

int pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    if (cond == 0) {
        return EINVAL;
    }
    cond->__sequence = 0u;
    cond->__clock = attr != 0 ? attr->__clock : CLOCK_REALTIME;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* cond) {
    return cond != 0 ? 0 : EINVAL;
}

/* Waits for the sequence to move past the value read while the mutex was
 * still held: a signal sent after the unlock changes it, so the wait comes
 * straight back instead of missing it. Waking says "look again", which is
 * all a condition variable promises. */
static int sx_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex, long timeout_ms) {
    const unsigned int sequence = __atomic_load_n(&cond->__sequence, __ATOMIC_ACQUIRE);
    long waited;
    int status;

    status = pthread_mutex_unlock(mutex);
    if (status != 0) {
        return status;
    }
    waited = sxn_wait_address(&cond->__sequence, sequence, sizeof(cond->__sequence), timeout_ms);
    (void)pthread_mutex_lock(mutex);
    return waited == -SAVANXP_ETIMEDOUT ? ETIMEDOUT : 0;
}

int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if (cond == 0 || mutex == 0) {
        return EINVAL;
    }
    return sx_cond_wait(cond, mutex, -1);
}

int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* deadline) {
    struct timespec now;
    long long remaining_ns;
    long timeout_ms;

    if (cond == 0 || mutex == 0 || deadline == 0 || deadline->tv_nsec < 0 || deadline->tv_nsec >= 1000000000L) {
        return EINVAL;
    }
    if (clock_gettime(cond->__clock, &now) != 0) {
        return EINVAL;
    }
    remaining_ns = ((long long)deadline->tv_sec - (long long)now.tv_sec) * 1000000000LL +
        ((long long)deadline->tv_nsec - (long long)now.tv_nsec);
    if (remaining_ns <= 0) {
        return ETIMEDOUT;
    }
    timeout_ms = (long)((remaining_ns + 999999LL) / 1000000LL);
    return sx_cond_wait(cond, mutex, timeout_ms);
}

int pthread_cond_signal(pthread_cond_t* cond) {
    if (cond == 0) {
        return EINVAL;
    }
    __atomic_fetch_add(&cond->__sequence, 1u, __ATOMIC_RELEASE);
    (void)sxn_wake_address(&cond->__sequence, 1);
    return 0;
}

/* Wakes them all, and they all go for the mutex: there is no requeue onto
 * the mutex, which only costs with many waiters. */
int pthread_cond_broadcast(pthread_cond_t* cond) {
    if (cond == 0) {
        return EINVAL;
    }
    __atomic_fetch_add(&cond->__sequence, 1u, __ATOMIC_RELEASE);
    (void)sxn_wake_address(&cond->__sequence, SXN_WAKE_ALL);
    return 0;
}

/* --- once -------------------------------------------------------------------- */

enum {
    SX_ONCE_NEW = 0,
    SX_ONCE_RUNNING = 1,
    SX_ONCE_DONE = 2,
};

int pthread_once(pthread_once_t* once, void (*routine)(void)) {
    int expected = SX_ONCE_NEW;

    if (once == 0 || routine == 0) {
        return EINVAL;
    }
    if (__atomic_load_n(once, __ATOMIC_ACQUIRE) == SX_ONCE_DONE) {
        return 0;
    }
    if (__atomic_compare_exchange_n(once, &expected, SX_ONCE_RUNNING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        routine();
        __atomic_store_n(once, SX_ONCE_DONE, __ATOMIC_RELEASE);
        (void)sxn_wake_address(once, SXN_WAKE_ALL);
        return 0;
    }
    while (__atomic_load_n(once, __ATOMIC_ACQUIRE) == SX_ONCE_RUNNING) {
        (void)sxn_wait_address(once, SX_ONCE_RUNNING, sizeof(*once), -1);
    }
    return 0;
}

/* --- keys ---------------------------------------------------------------------- */

/* A key's generation changes every time it is created, so a value a thread
 * stored under a deleted key never shows through the next key that reuses
 * the slot. */
struct sx_key {
    int used;
    unsigned int generation;
    void (*destructor)(void*);
};

static struct sx_lock g_key_lock = SX_LOCK_INIT;
static struct sx_key g_keys[PTHREAD_KEYS_MAX];

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
    unsigned int index;

    if (key == 0) {
        return EINVAL;
    }
    sx_lock_acquire(&g_key_lock);
    for (index = 0; index < PTHREAD_KEYS_MAX; ++index) {
        if (!g_keys[index].used) {
            g_keys[index].used = 1;
            g_keys[index].generation += 1u;
            g_keys[index].destructor = destructor;
            *key = index;
            sx_lock_release(&g_key_lock);
            return 0;
        }
    }
    sx_lock_release(&g_key_lock);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= PTHREAD_KEYS_MAX) {
        return EINVAL;
    }
    sx_lock_acquire(&g_key_lock);
    if (!g_keys[key].used) {
        sx_lock_release(&g_key_lock);
        return EINVAL;
    }
    g_keys[key].used = 0;
    g_keys[key].destructor = 0;
    sx_lock_release(&g_key_lock);
    return 0;
}

void* pthread_getspecific(pthread_key_t key) {
    const struct sx_pthread* self = sx_current();

    if (key >= PTHREAD_KEYS_MAX || !g_keys[key].used ||
        self->specific[key].generation != g_keys[key].generation) {
        return 0;
    }
    return self->specific[key].value;
}

int pthread_setspecific(pthread_key_t key, const void* value) {
    struct sx_pthread* self = sx_current();

    if (key >= PTHREAD_KEYS_MAX || !g_keys[key].used) {
        return EINVAL;
    }
    self->specific[key].generation = g_keys[key].generation;
    self->specific[key].value = (void*)value;
    return 0;
}

static void sx_run_key_destructors(struct sx_pthread* self) {
    int round;

    for (round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; ++round) {
        int ran = 0;
        unsigned int index;

        for (index = 0; index < PTHREAD_KEYS_MAX; ++index) {
            void (*destructor)(void*) = g_keys[index].destructor;
            void* value = self->specific[index].value;

            if (!g_keys[index].used || destructor == 0 || value == 0 ||
                self->specific[index].generation != g_keys[index].generation) {
                continue;
            }
            self->specific[index].value = 0;
            destructor(value);
            ran = 1;
        }
        if (!ran) {
            return;
        }
    }
}
