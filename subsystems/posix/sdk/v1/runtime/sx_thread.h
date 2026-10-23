#pragma once

/*
 * What the rest of the POSIX runtime needs from pthread.c: the thread control
 * block %fs points at, and the lock the heap and stdio take once a second
 * thread exists. Internal to the runtime; programs include <pthread.h>.
 */

#include <stddef.h>
#include <stdint.h>

struct sx_pthread;

/* The x86-64 TLS ABI, variant II: %fs points at this block, and the
 * executable's static TLS block (PT_TLS) sits right below it. The first
 * words follow the layout compilers and ports expect: %fs:0 is the block's
 * own address, %fs:0x28 a copy of the stack guard (where a compiler targeting
 * Linux reads it; ours uses the global __stack_chk_guard). */
struct sx_tcb {
    struct sx_tcb* self;
    void* dtv;
    struct sx_tcb* self_again;
    uint64_t reserved[2];
    uint64_t stack_guard;
    uint64_t pointer_guard;
    int errno_value;
    int reserved_int;
    struct sx_pthread* thread;
};

/* A recursive lock for the runtime's own state. It costs nothing until the
 * first pthread_create: before that there is nobody to exclude. */
struct sx_lock {
    volatile uint32_t word;
    volatile uintptr_t owner;
    uint32_t depth;
};

#define SX_LOCK_INIT {0, 0, 0}

/* Set once the main thread's TCB is in %fs. Until then errno is a global. */
extern int sx_tls_ready;
/* Set by the first pthread_create and never cleared. */
extern volatile int sx_threads_started;

static inline struct sx_tcb* sx_tcb_self(void) {
    struct sx_tcb* tcb;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(tcb));
    return tcb;
}

void sx_lock_acquire(struct sx_lock* lock);
void sx_lock_release(struct sx_lock* lock);

/* Called by sx_start_dynamic once the loader has run: finds PT_TLS, builds
 * the main thread's block and points %fs at it. */
void sx_tls_init(void);
