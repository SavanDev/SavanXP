/*
 * threads.c -- Guest threads and the objects they wait on (sxw_abi.h,
 * SXW_SVC_THREAD_CREATE to SXW_SVC_END_OTHER_THREADS).
 *
 * Portable C, no library dependency. Every guest thread is a host thread
 * with its own TEB, guest stack and host stack, but they take turns: one
 * holds the turn and runs guest code, the others wait for it. A thread gives
 * the turn up while it waits on an object, sleeps or waits for input, and at
 * each call to the host while another thread is ready. The DLLs were written
 * for one thread at a time and stay correct without a lock of their own;
 * the order threads run in follows their waits, as the fixtures need to
 * match Windows XP's output at any number of processors. What it costs: no
 * two guest threads run at once, and a thread that spins in guest code
 * without calling the host keeps the others waiting. See
 * docs/userland/WIN32.md, "Threads".
 *
 * The turn is a ticket lock over the platform's word waits. A thread parks
 * on its own word, which whoever wakes it changes while holding the turn, so
 * no wake-up is lost between giving the turn up and parking.
 */
#include "services.h"

#include "guest.h"

#define THREAD_ID_FIRST 0x100u
#define PROCESS_ID 0x10u
#define MAXIMUM_WAIT_OBJECTS 64u
#define SLICE_MS 20u

/* ---- The turn ------------------------------------------------------------------ */

void sxw_turn_take(sxw_services *s) {
    const uint32_t ticket = __atomic_fetch_add(&s->turn_next, 1u, __ATOMIC_ACQ_REL);
    for (;;) {
        const uint32_t serving = __atomic_load_n(&s->turn_serving, __ATOMIC_ACQUIRE);
        if (serving == ticket) {
            return;
        }
        s->thread_platform->wait_word(s->context, &s->turn_serving, serving, SXW_INFINITE);
    }
}

void sxw_turn_give(sxw_services *s) {
    __atomic_fetch_add(&s->turn_serving, 1u, __ATOMIC_ACQ_REL);
    s->thread_platform->wake_word(s->context, &s->turn_serving);
}

/* Another thread waits for the turn. */
static int turn_wanted(const sxw_services *s) {
    return __atomic_load_n(&s->turn_next, __ATOMIC_ACQUIRE) != __atomic_load_n(&s->turn_serving, __ATOMIC_ACQUIRE) + 1u;
}

static void unpark(sxw_services *s, sxw_thread *t) {
    __atomic_fetch_add(&t->park, 1u, __ATOMIC_ACQ_REL);
    s->thread_platform->wake_word(s->context, &t->park);
}

static void stop_for_good(sxw_services *s, sxw_thread *t);

/* Gives the turn up and parks until unparked or timeout, then takes the
 * turn again; a thread that ExitProcess ended, or one suspended meanwhile,
 * does not come back from here until it may run. */
static void park(sxw_services *s, sxw_thread *t, uint32_t timeout) {
    const uint32_t word = __atomic_load_n(&t->park, __ATOMIC_ACQUIRE);
    sxw_turn_give(s);
    s->thread_platform->wait_word(s->context, &t->park, word, timeout);
    sxw_turn_take(s);
    if (s->ending != 0u && s->ending != (uint32_t)(t - s->threads) + 1u) {
        stop_for_good(s, t);
    }
}

/* What a thread ExitProcess ended does: it never runs guest code again. */
static void stop_for_good(sxw_services *s, sxw_thread *t) {
    for (;;) {
        const uint32_t word = __atomic_load_n(&t->park, __ATOMIC_ACQUIRE);
        sxw_turn_give(s);
        s->thread_platform->wait_word(s->context, &t->park, word, SXW_INFINITE);
        sxw_turn_take(s);
    }
}

/* Stops while suspended. */
static void honour_suspend(sxw_services *s, sxw_thread *t) {
    while (t->suspend != 0u) {
        park(s, t, SXW_INFINITE);
    }
}

static uint32_t now(const sxw_services *s) {
    return s->thread_platform->milliseconds(s->context);
}

/* ---- Threads ---------------------------------------------------------------------- */

static sxw_thread *thread_by_handle(sxw_services *s, uint32_t self, uint32_t handle) {
    if (handle == SXW_CURRENT_THREAD) {
        return &s->threads[self];
    }
    sxw_object *o = sxw_object_of(s, handle, SXW_OBJECT_THREAD);
    return o != 0 ? &s->threads[o->u.thread.index] : 0;
}

static uint32_t live_threads(const sxw_services *s) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < SXW_MAX_THREADS; ++i) {
        n += s->threads[i].state == SXW_THREAD_CREATED || s->threads[i].state == SXW_THREAD_RUNNING;
    }
    return n;
}

static void wake_waiters(sxw_services *s) {
    for (uint32_t i = 0; i < SXW_MAX_THREADS; ++i) {
        if (s->threads[i].waiting) {
            unpark(s, &s->threads[i]);
        }
    }
}

void sxw_threads_start(sxw_services *s) {
    sxw_thread *first = &s->threads[0];
    first->state = SXW_THREAD_RUNNING;
    first->id = THREAD_ID_FIRST;
    first->teb = s->process->teb;
    first->stack_top = s->process->stack_top;
    uint8_t *teb = (uint8_t *)(uintptr_t)first->teb;
    sxw_set32(teb + SXW_TEB_PROCESS_ID, PROCESS_ID);
    sxw_set32(teb + SXW_TEB_THREAD_ID, first->id);
    sxw_set32(teb + SXW_TEB_HOST_THREAD, 0);
    /* Its object, which a duplicated pseudo-handle names. */
    sxw_object *o = sxw_new_object(s, SXW_OBJECT_THREAD);
    if (o != 0) {
        o->u.thread.index = 0;
        o->references = 1;
        first->object = (uint32_t)(o - s->objects) + 1u;
    }
    if (s->thread_platform != 0) {
        sxw_turn_take(s);
    }
}

static uint32_t thread_create(sxw_services *s, uint32_t self, const uint32_t *a) {
    if (s->thread_platform == 0 || s->thread_entry == 0u) {
        return SXW_ERROR_NOT_ENOUGH_MEMORY;
    }
    if (!sxw_guest_range(s->process, a[4], 8u)) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    uint32_t index = SXW_MAX_THREADS;
    for (uint32_t i = 1; i < SXW_MAX_THREADS; ++i) {
        if (s->threads[i].state == SXW_THREAD_FREE) {
            index = i;
            break;
        }
    }
    sxw_object *o = index < SXW_MAX_THREADS ? sxw_new_object(s, SXW_OBJECT_THREAD) : 0;
    if (o == 0) {
        return SXW_ERROR_NOT_ENOUGH_MEMORY;
    }
    sxw_thread_block block;
    if (sxw_new_thread(s->process, a[2], &block) != 0) {
        o->kind = SXW_OBJECT_NONE;
        return SXW_ERROR_NOT_ENOUGH_MEMORY;
    }
    sxw_thread *t = &s->threads[index];
    const uint32_t park_word = t->park;
    for (uint32_t i = 0; i < sizeof(*t); ++i) {
        ((uint8_t *)t)[i] = 0;
    }
    t->park = park_word;
    t->state = SXW_THREAD_CREATED;
    t->id = THREAD_ID_FIRST + 4u * index;
    t->teb = block.teb;
    t->region = block.region;
    t->stack_top = block.stack_top;
    t->start = a[0];
    t->parameter = a[1];
    t->suspend = (a[3] & 0x4u) != 0u ? 1u : 0u; /* CREATE_SUSPENDED */
    uint8_t *teb = (uint8_t *)(uintptr_t)t->teb;
    sxw_set32(teb + SXW_TEB_PROCESS_ID, PROCESS_ID);
    sxw_set32(teb + SXW_TEB_THREAD_ID, t->id);
    sxw_set32(teb + SXW_TEB_HOST_THREAD, index);
    o->u.thread.index = index;
    /* The thread holds its object while it runs, as a handle would. */
    o->references = 1;
    t->object = (uint32_t)(o - s->objects) + 1u;
    const uint32_t handle = sxw_new_handle(s, o);
    if (handle == 0u || s->thread_platform->start(s->context, index) != 0) {
        if (handle != 0u) {
            (void)sxw_close_handle(s, handle);
        }
        o->kind = SXW_OBJECT_NONE;
        sxw_free_region(s->process, t->region);
        t->state = SXW_THREAD_FREE;
        return SXW_ERROR_NOT_ENOUGH_MEMORY;
    }
    (void)self;
    sxw_write_word(s->process, a[4], handle);
    sxw_write_word(s->process, a[4] + 4u, t->id);
    return SXW_ERROR_SUCCESS;
}

/* The thread's object stops naming a live thread when it ends; the thread
 * keeps it until then. */
static void release_thread_object(sxw_services *s, sxw_thread *t) {
    if (t->object == 0u) {
        return;
    }
    sxw_object *o = &s->objects[t->object - 1u];
    t->object = 0;
    if (--o->references == 0u) {
        o->kind = SXW_OBJECT_NONE;
    }
}

/* What ends with a thread: its mutexes are abandoned, its waiters woken. */
static void thread_ended(sxw_services *s, uint32_t index, uint32_t code) {
    sxw_thread *t = &s->threads[index];
    t->exit_code = code;
    t->state = SXW_THREAD_ENDED;
    for (uint32_t i = 0; i < SXW_MAX_OBJECTS; ++i) {
        sxw_object *o = &s->objects[i];
        if (o->kind == SXW_OBJECT_MUTEX && o->u.mutex.owner == index + 1u) {
            o->u.mutex.owner = 0;
            o->u.mutex.recursion = 0;
            o->u.mutex.abandoned = 1;
        }
    }
    for (uint32_t i = 0; i < sizeof s->window_owner; ++i) {
        if (s->window_owner[i] == index + 1u) {
            s->window_owner[i] = 0;
        }
    }
    wake_waiters(s);
}

int sxw_thread_main(sxw_services *s, uint32_t index,
                    void (*enter)(void *context, uint32_t index, uint32_t entry, uint32_t esp), void *context) {
    sxw_thread *t = &s->threads[index];
    sxw_turn_take(s);
    if (s->ending != 0u) {
        stop_for_good(s, t);
    }
    honour_suspend(s, t);
    t->state = SXW_THREAD_RUNNING;
    /* ntdll!SxwThreadStart(start, parameter), returning to the host. */
    const uint32_t esp = t->stack_top - 16u;
    uint8_t *stack = (uint8_t *)(uintptr_t)esp;
    sxw_set32(stack + 4u, t->start);
    sxw_set32(stack + 8u, t->parameter);
    enter(context, index, s->thread_entry, esp);
    if (s->exit_requested) {
        return 1; /* the host ends the process, holding the turn */
    }
    sxw_free_region(s->process, t->region);
    t->region = 0;
    release_thread_object(s, t);
    sxw_turn_give(s);
    return 0;
}

int sxw_should_leave(const sxw_services *s, uint32_t thread) {
    return s->exit_requested || (thread < SXW_MAX_THREADS && s->threads[thread].leaving);
}

/* ---- Objects ------------------------------------------------------------------------- */

static int signaled(const sxw_services *s, const sxw_object *o, uint32_t self) {
    switch (o->kind) {
    case SXW_OBJECT_EVENT: return o->u.event.signaled != 0u;
    case SXW_OBJECT_MUTEX: return o->u.mutex.owner == 0u || o->u.mutex.owner == self + 1u;
    case SXW_OBJECT_SEMAPHORE: return o->u.semaphore.count > 0;
    case SXW_OBJECT_THREAD: return s->threads[o->u.thread.index].state == SXW_THREAD_ENDED;
    default: return 0;
    }
}

/* Takes what a satisfied wait takes; 1 when it was an abandoned mutex. */
static int acquire(sxw_object *o, uint32_t self) {
    switch (o->kind) {
    case SXW_OBJECT_EVENT:
        if (!o->u.event.manual) {
            o->u.event.signaled = 0;
        }
        return 0;
    case SXW_OBJECT_MUTEX: {
        const int abandoned = o->u.mutex.abandoned != 0u;
        o->u.mutex.abandoned = 0;
        o->u.mutex.owner = self + 1u;
        ++o->u.mutex.recursion;
        return abandoned;
    }
    case SXW_OBJECT_SEMAPHORE:
        --o->u.semaphore.count;
        return 0;
    default:
        return 0;
    }
}

static int waitable(uint32_t kind) {
    return kind == SXW_OBJECT_EVENT || kind == SXW_OBJECT_MUTEX || kind == SXW_OBJECT_SEMAPHORE ||
           kind == SXW_OBJECT_THREAD;
}

/* An object by name, of any kind; 0 for none. */
static sxw_object *named(sxw_services *s, const char *name) {
    for (uint32_t i = 0; i < SXW_MAX_OBJECTS; ++i) {
        sxw_object *o = &s->objects[i];
        if (o->kind != SXW_OBJECT_NONE && o->name[0] != 0) {
            const char *a = o->name;
            const char *b = name;
            while (*a != 0 && *a == *b) {
                ++a;
                ++b;
            }
            if (*a == 0 && *b == 0) {
                return o;
            }
        }
    }
    return 0;
}

static uint32_t object_create(sxw_services *s, uint32_t self, const uint32_t *a) {
    char name[SXW_OBJECT_NAME];
    if (!sxw_guest_range(s->process, a[4], 8u) || !sxw_read_utf16(s->process, a[3], name, sizeof(name))) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    const uint32_t kind = a[0];
    if (kind == SXW_OBJECT_SEMAPHORE && ((int32_t)a[2] <= 0 || (int32_t)a[1] < 0 || (int32_t)a[1] > (int32_t)a[2])) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    if (kind != SXW_OBJECT_EVENT && kind != SXW_OBJECT_MUTEX && kind != SXW_OBJECT_SEMAPHORE) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    sxw_object *o = name[0] != 0 ? named(s, name) : 0;
    uint32_t existed = 0;
    if (o != 0) {
        if (o->kind != kind) {
            return 6u; /* ERROR_INVALID_HANDLE: the name is another kind's */
        }
        existed = 1;
    } else {
        o = sxw_new_object(s, kind);
        if (o == 0) {
            return SXW_ERROR_NOT_ENOUGH_MEMORY;
        }
        for (uint32_t i = 0; i < SXW_OBJECT_NAME; ++i) {
            o->name[i] = name[i];
        }
        switch (kind) {
        case SXW_OBJECT_EVENT:
            o->u.event.manual = a[1] != 0u;
            o->u.event.signaled = a[2] != 0u;
            break;
        case SXW_OBJECT_MUTEX:
            o->u.mutex.owner = a[1] != 0u ? self + 1u : 0u;
            o->u.mutex.recursion = a[1] != 0u ? 1u : 0u;
            o->u.mutex.abandoned = 0;
            break;
        default:
            o->u.semaphore.count = (int32_t)a[1];
            o->u.semaphore.maximum = (int32_t)a[2];
            break;
        }
    }
    const uint32_t handle = sxw_new_handle(s, o);
    if (handle == 0u) {
        if (o->references == 0u) {
            o->kind = SXW_OBJECT_NONE;
        }
        return SXW_ERROR_NOT_ENOUGH_MEMORY;
    }
    sxw_write_word(s->process, a[4], handle);
    sxw_write_word(s->process, a[4] + 4u, existed);
    return SXW_ERROR_SUCCESS;
}

static uint32_t object_signal(sxw_services *s, uint32_t self, const uint32_t *a) {
    sxw_object *o = sxw_object_of(s, a[0], SXW_OBJECT_NONE);
    if (o == 0) {
        return SXW_ERROR_INVALID_HANDLE;
    }
    if (a[3] != 0u && !sxw_guest_range(s->process, a[3], 4u)) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    switch (a[1]) {
    case SXW_SIGNAL_SET:
    case SXW_SIGNAL_RESET:
        if (o->kind != SXW_OBJECT_EVENT) {
            return SXW_ERROR_INVALID_HANDLE;
        }
        o->u.event.signaled = a[1] == SXW_SIGNAL_SET;
        break;
    case SXW_SIGNAL_RELEASE:
        if (o->kind == SXW_OBJECT_MUTEX) {
            if (o->u.mutex.owner != self + 1u) {
                return SXW_ERROR_NOT_OWNER;
            }
            if (--o->u.mutex.recursion == 0u) {
                o->u.mutex.owner = 0;
            }
        } else if (o->kind == SXW_OBJECT_SEMAPHORE) {
            const int32_t count = (int32_t)a[2];
            if (count <= 0) {
                return SXW_ERROR_INVALID_PARAMETER;
            }
            if (count > o->u.semaphore.maximum - o->u.semaphore.count) {
                return SXW_ERROR_TOO_MANY_POSTS;
            }
            if (a[3] != 0u) {
                sxw_write_word(s->process, a[3], (uint32_t)o->u.semaphore.count);
            }
            o->u.semaphore.count += count;
        } else {
            return SXW_ERROR_INVALID_HANDLE;
        }
        break;
    default:
        return SXW_ERROR_INVALID_PARAMETER;
    }
    wake_waiters(s);
    return SXW_ERROR_SUCCESS;
}

/* The objects of a wait, or an error. */
static uint32_t wait_objects(sxw_services *s, uint32_t self, uint32_t address, uint32_t count, sxw_object **out) {
    uint32_t handles[MAXIMUM_WAIT_OBJECTS];
    if (count == 0u || count > MAXIMUM_WAIT_OBJECTS) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    if (!sxw_read_words(s->process, address, count, handles)) {
        return SXW_ERROR_INVALID_PARAMETER; /* WaitForMultipleObjects answers ERROR_NOACCESS */
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (handles[i] == SXW_CURRENT_THREAD) {
            out[i] = s->threads[self].object != 0u ? &s->objects[s->threads[self].object - 1u] : 0;
        } else {
            out[i] = sxw_object_of(s, handles[i], SXW_OBJECT_NONE);
        }
        if (out[i] == 0 || !waitable(out[i]->kind)) {
            return SXW_ERROR_INVALID_HANDLE;
        }
    }
    return SXW_ERROR_SUCCESS;
}

/* A wait that is satisfied now takes what it waits for: its result, or
 * SXW_FAILED when it must go on waiting. */
static uint32_t try_wait(sxw_services *s, uint32_t self, sxw_object **objects, uint32_t count, int all) {
    if (all) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!signaled(s, objects[i], self)) {
                return SXW_FAILED;
            }
        }
        int abandoned = 0;
        for (uint32_t i = 0; i < count; ++i) {
            abandoned |= acquire(objects[i], self);
        }
        return abandoned ? SXW_WAIT_ABANDONED_0 : SXW_WAIT_OBJECT_0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (signaled(s, objects[i], self)) {
            return (acquire(objects[i], self) ? SXW_WAIT_ABANDONED_0 : SXW_WAIT_OBJECT_0) + i;
        }
    }
    return SXW_FAILED;
}

static uint32_t wait_for(sxw_services *s, uint32_t self, const uint32_t *a) {
    sxw_object *objects[MAXIMUM_WAIT_OBJECTS];
    if (!sxw_guest_range(s->process, a[4], 4u)) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    const uint32_t error = wait_objects(s, self, a[0], a[1], objects);
    if (error != SXW_ERROR_SUCCESS) {
        return error;
    }
    sxw_thread *t = &s->threads[self];
    const uint32_t timeout = a[3];
    const uint32_t started = s->thread_platform != 0 ? now(s) : 0u;
    for (;;) {
        /* The handles may have closed while this thread waited. */
        if (wait_objects(s, self, a[0], a[1], objects) != SXW_ERROR_SUCCESS) {
            return SXW_ERROR_INVALID_HANDLE;
        }
        const uint32_t result = try_wait(s, self, objects, a[1], a[2] != 0u);
        if (result != SXW_FAILED) {
            sxw_write_word(s->process, a[4], result);
            return SXW_ERROR_SUCCESS;
        }
        uint32_t left = SXW_INFINITE;
        if (timeout != SXW_INFINITE) {
            const uint32_t spent = s->thread_platform != 0 ? now(s) - started : timeout;
            if (spent >= timeout) {
                sxw_write_word(s->process, a[4], SXW_WAIT_TIMEOUT);
                return SXW_ERROR_SUCCESS;
            }
            left = timeout - spent;
        }
        if (s->thread_platform == 0) {
            /* One thread: nothing else can signal it. */
            sxw_write_word(s->process, a[4], SXW_WAIT_TIMEOUT);
            return SXW_ERROR_SUCCESS;
        }
        t->waiting = 1;
        park(s, t, left);
        t->waiting = 0;
        honour_suspend(s, t);
    }
}

static uint32_t sleep_for(sxw_services *s, uint32_t self, uint32_t milliseconds) {
    if (s->thread_platform == 0) {
        return SXW_ERROR_SUCCESS;
    }
    sxw_thread *t = &s->threads[self];
    if (milliseconds == 0u) {
        if (turn_wanted(s)) {
            sxw_turn_give(s);
            sxw_turn_take(s);
        }
        return SXW_ERROR_SUCCESS;
    }
    const uint32_t started = now(s);
    for (;;) {
        const uint32_t spent = now(s) - started;
        if (milliseconds != SXW_INFINITE && spent >= milliseconds) {
            return SXW_ERROR_SUCCESS;
        }
        park(s, t, milliseconds == SXW_INFINITE ? SXW_INFINITE : milliseconds - spent);
        honour_suspend(s, t);
    }
}

/* ---- Input for the threads' windows ---------------------------------------------------- */

void sxw_window_created(sxw_services *s, uint32_t thread, uint32_t window) {
    if (window < sizeof s->window_owner) {
        s->window_owner[window] = (uint8_t)(thread + 1u);
    }
    ++s->threads[thread].host_windows;
}

/* Puts a host event where it belongs: 1 when it is the caller's. */
static int route(sxw_services *s, uint32_t self, const sxw_event *event) {
    uint32_t owner = event->window < sizeof s->window_owner ? s->window_owner[event->window] : 0u;
    owner = owner != 0u ? owner - 1u : 0u;
    if (owner == self) {
        return 1;
    }
    sxw_thread *t = &s->threads[owner];
    if (t->pending_count < SXW_MAX_PENDING) {
        t->pending[(t->pending_head + t->pending_count) % SXW_MAX_PENDING] = *event;
        ++t->pending_count;
    }
    unpark(s, t);
    return 0;
}

uint32_t sxw_wait_event(sxw_services *s, uint32_t self, uint32_t timeout, sxw_event *event) {
    const sxw_window_platform *w = s->windows;
    sxw_thread *t = &s->threads[self];
    if (s->thread_platform == 0 || live_threads(s) <= 1u) {
        /* One thread: the platform's own wait, as long as it is asked for. */
        for (;;) {
            if (t->pending_count != 0u) {
                *event = t->pending[t->pending_head];
                t->pending_head = (t->pending_head + 1u) % SXW_MAX_PENDING;
                --t->pending_count;
                return 1;
            }
            if (t->woken) {
                t->woken = 0;
                return 2;
            }
            if (w == 0 || !w->wait_event(s->context, timeout, event)) {
                return 0;
            }
            if (route(s, self, event)) {
                return 1;
            }
        }
    }
    const uint32_t started = now(s);
    for (;;) {
        if (t->pending_count != 0u) {
            *event = t->pending[t->pending_head];
            t->pending_head = (t->pending_head + 1u) % SXW_MAX_PENDING;
            --t->pending_count;
            return 1;
        }
        if (t->woken) {
            t->woken = 0;
            return 2;
        }
        const uint32_t spent = now(s) - started;
        if (timeout != SXW_INFINITE && spent >= timeout) {
            return 0;
        }
        const uint32_t left = timeout == SXW_INFINITE ? SXW_INFINITE : timeout - spent;
        if (w != 0 && t->host_windows != 0u) {
            /* Input comes through the platform, which this thread polls a
             * slice at a time with the turn held, giving it up between
             * slices so that the others run. */
            if (w->wait_event(s->context, left < SLICE_MS ? left : SLICE_MS, event) && route(s, self, event)) {
                return 1;
            }
            if (timeout == 0u) {
                return 0;
            }
            if (turn_wanted(s)) {
                sxw_turn_give(s);
                sxw_turn_take(s);
            }
            honour_suspend(s, t);
            continue;
        }
        if (timeout == 0u) {
            return 0;
        }
        park(s, t, left);
        honour_suspend(s, t);
    }
}

/* ---- The services ---------------------------------------------------------------------------- */

uint32_t sxw_thread_service(sxw_services *s, uint32_t self, uint32_t service, const uint32_t *a) {
    switch (service) {
    case SXW_SVC_THREAD_CREATE:
        return thread_create(s, self, a);
    case SXW_SVC_THREAD_EXIT: {
        if (live_threads(s) <= 1u) {
            return 1; /* the last thread: ntdll ends the process */
        }
        thread_ended(s, self, a[0]);
        s->threads[self].leaving = 1;
        return 0;
    }
    case SXW_SVC_THREAD_SUSPEND: {
        sxw_thread *t = thread_by_handle(s, self, a[0]);
        if (t == 0) {
            return SXW_ERROR_INVALID_HANDLE;
        }
        if (!sxw_guest_range(s->process, a[2], 4u)) {
            return SXW_ERROR_INVALID_PARAMETER;
        }
        const uint32_t previous = t->suspend;
        if (t->state != SXW_THREAD_ENDED) {
            if (a[1] == 0u) {
                if (t->suspend == 127u) {
                    return 156u; /* ERROR_SIGNAL_REFUSED: MAXIMUM_SUSPEND_COUNT */
                }
                ++t->suspend;
            } else if (t->suspend != 0u && --t->suspend == 0u) {
                unpark(s, t);
            }
        }
        sxw_write_word(s->process, a[2], previous);
        if (t == &s->threads[self]) {
            honour_suspend(s, t);
        }
        return SXW_ERROR_SUCCESS;
    }
    case SXW_SVC_THREAD_QUERY: {
        sxw_thread *t = thread_by_handle(s, self, a[0]);
        if (t == 0) {
            return SXW_ERROR_INVALID_HANDLE;
        }
        if (!sxw_guest_range(s->process, a[1], 12u)) {
            return SXW_ERROR_INVALID_PARAMETER;
        }
        sxw_write_word(s->process, a[1], t->state == SXW_THREAD_ENDED ? t->exit_code : SXW_STILL_ACTIVE);
        sxw_write_word(s->process, a[1] + 4u, (uint32_t)t->priority);
        sxw_write_word(s->process, a[1] + 8u, t->id);
        return SXW_ERROR_SUCCESS;
    }
    case SXW_SVC_THREAD_PRIORITY: {
        sxw_thread *t = thread_by_handle(s, self, a[0]);
        if (t == 0) {
            return SXW_ERROR_INVALID_HANDLE;
        }
        const int32_t priority = (int32_t)a[1];
        if (priority != -15 && priority != 15 && (priority < -2 || priority > 2)) {
            return SXW_ERROR_INVALID_PARAMETER;
        }
        t->priority = priority;
        return SXW_ERROR_SUCCESS;
    }
    case SXW_SVC_OBJECT_CREATE:
        return object_create(s, self, a);
    case SXW_SVC_OBJECT_SIGNAL:
        return object_signal(s, self, a);
    case SXW_SVC_WAIT:
        return wait_for(s, self, a);
    case SXW_SVC_SLEEP:
        return sleep_for(s, self, a[0]);
    case SXW_SVC_WAKE_THREAD:
        for (uint32_t i = 0; i < SXW_MAX_THREADS; ++i) {
            sxw_thread *t = &s->threads[i];
            if ((t->state == SXW_THREAD_CREATED || t->state == SXW_THREAD_RUNNING) && t->id == a[0]) {
                t->woken = 1;
                unpark(s, t);
                return SXW_ERROR_SUCCESS;
            }
        }
        return SXW_ERROR_INVALID_THREAD_ID;
    case SXW_SVC_END_OTHER_THREADS:
        s->ending = self + 1u;
        return SXW_ERROR_SUCCESS;
    default:
        return SXW_ERROR_INVALID_PARAMETER;
    }
}

/* At each call to the host: let a waiting thread have the turn, and stop
 * here while suspended or ended by ExitProcess. */
void sxw_thread_checkpoint(sxw_services *s, uint32_t self) {
    if (s->thread_platform == 0 || self >= SXW_MAX_THREADS) {
        return;
    }
    sxw_thread *t = &s->threads[self];
    if (turn_wanted(s)) {
        sxw_turn_give(s);
        sxw_turn_take(s);
    }
    if (s->ending != 0u && s->ending != self + 1u) {
        stop_for_good(s, t);
    }
    honour_suspend(s, t);
}
