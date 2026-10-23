#pragma once

/*
 * POSIX threads over the native thread calls (docs/kernel/THREADS_ROADMAP.md,
 * phase 5): a thread is a native thread of the process, a mutex or condition
 * variable is a word user space waits on with SXN_SYS_WAIT_ADDRESS.
 *
 * What is here: create, join, detach, self, equal, exit; attributes for the
 * stack size and the detach state; normal, recursive and error-checking
 * mutexes; condition variables with an optional CLOCK_MONOTONIC clock; once;
 * keys. What is not: cancellation, signals per thread, scheduling policy,
 * read-write locks, barriers, spin locks, process-shared objects.
 */

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The handle is the address of the thread's descriptor, as an integer. */
typedef unsigned long pthread_t;

typedef struct {
    size_t __stacksize;
    int __detachstate;
    int __reserved;
} pthread_attr_t;

/* 0 free, 1 locked, 2 locked with waiters; the owner and the count only for
 * the recursive and error-checking kinds. */
typedef struct {
    volatile unsigned int __lock;
    int __kind;
    volatile unsigned long __owner;
    unsigned int __count;
    int __reserved;
} pthread_mutex_t;

typedef struct {
    int __kind;
} pthread_mutexattr_t;

/* A sequence number waiters sleep on; signal and broadcast advance it. */
typedef struct {
    volatile unsigned int __sequence;
    int __clock;
} pthread_cond_t;

typedef struct {
    int __clock;
} pthread_condattr_t;

typedef volatile int pthread_once_t;
typedef unsigned int pthread_key_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL

#define PTHREAD_MUTEX_INITIALIZER {0, PTHREAD_MUTEX_NORMAL, 0, 0, 0}
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP {0, PTHREAD_MUTEX_RECURSIVE, 0, 0, 0}
#define PTHREAD_COND_INITIALIZER {0, CLOCK_REALTIME}
#define PTHREAD_ONCE_INIT 0

#define PTHREAD_KEYS_MAX 64
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_STACK_MIN 16384u
/* Stacks are committed up front (there is no reserve/commit memory yet), so
 * the default is small; pthread_attr_setstacksize asks for more. */
#define PTHREAD_STACK_DEFAULT (256u * 1024u)

int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* argument);
int pthread_join(pthread_t thread, void** result);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t left, pthread_t right);
void pthread_exit(void* result) __attribute__((noreturn));

int pthread_attr_init(pthread_attr_t* attr);
int pthread_attr_destroy(pthread_attr_t* attr);
int pthread_attr_setstacksize(pthread_attr_t* attr, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t* attr, size_t* size);
int pthread_attr_setdetachstate(pthread_attr_t* attr, int state);
int pthread_attr_getdetachstate(const pthread_attr_t* attr, int* state);

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int pthread_mutex_destroy(pthread_mutex_t* mutex);
int pthread_mutex_lock(pthread_mutex_t* mutex);
int pthread_mutex_trylock(pthread_mutex_t* mutex);
int pthread_mutex_unlock(pthread_mutex_t* mutex);
int pthread_mutexattr_init(pthread_mutexattr_t* attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t* attr);
int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int kind);
int pthread_mutexattr_gettype(const pthread_mutexattr_t* attr, int* kind);

int pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
int pthread_cond_destroy(pthread_cond_t* cond);
int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* deadline);
int pthread_cond_signal(pthread_cond_t* cond);
int pthread_cond_broadcast(pthread_cond_t* cond);
int pthread_condattr_init(pthread_condattr_t* attr);
int pthread_condattr_destroy(pthread_condattr_t* attr);
int pthread_condattr_setclock(pthread_condattr_t* attr, int clock);
int pthread_condattr_getclock(const pthread_condattr_t* attr, int* clock);

int pthread_once(pthread_once_t* once, void (*routine)(void));

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*));
int pthread_key_delete(pthread_key_t key);
void* pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void* value);

#ifdef __cplusplus
}
#endif
