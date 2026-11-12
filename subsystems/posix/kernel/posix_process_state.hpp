#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/process.hpp"
#include "kernel/vfs.hpp"

/*
 * POSIX's private per-process state (isolation slice B).
 *
 * The kernel reserves these bytes for what POSIX declares in
 * subsystem::register_process_state_size and never looks inside. The current
 * directory lives here: it is path-resolution state, and belongs to the layer
 * that translates, not to the kernel. That is why Process has no cwd field.
 *
 * The current directory is held: a retained vnode, not a path string. The
 * kernel tells POSIX when it copies these bytes into a child or is about to
 * free them (subsystem::register_process_state_hooks), and POSIX takes or
 * drops the reference then.
 */

namespace posix {

// Descriptores que admite un poll. Es el tamano de la tabla de handles del
// proceso porque no puede haber mas: pedir mas solo se logra repitiendo un fd,
// que nadie hace. El tope importa porque la copia en kernel del pedido vive
// en el estado privado y es lo que deja reevaluar a un proceso bloqueado sin
// tocar su memoria de usuario.
constexpr size_t POSIX_MAX_POLL_ENTRIES = process::kMaxFileHandles;

struct ProcessState {
    // The current directory, retained (vfs::retain) so it cannot be removed
    // or unmounted under the process; its path is rebuilt from the tree when
    // needed, so it follows renames. nullptr is "/".
    vfs::Vnode* cwd;
};

// POSIX per-thread blocking requests. The common kernel knows only that the
// thread is parked; POSIX rechecks poll descriptors or a sync durability ticket.
// Poll keeps its copied descriptor array in kernel memory, never a user pointer
// that would require switching address spaces on each timer tick. Sync waits
// until every write/flush before its ticket has completed, without holding the
// global kernel lock while the whole storage queue drains.
struct ThreadState {
    uint64_t sync_ticket;
    uint32_t sync_active;
    uint64_t poll_user_fds;
    uint32_t poll_count;
    uint32_t poll_active;
    savanxp_pollfd poll_entries[POSIX_MAX_POLL_ENTRIES];
};

// The thread's state, allocated on first use; nullptr without memory.
inline ThreadState* thread_state(process::Thread& thread) {
    return reinterpret_cast<ThreadState*>(process::thread_subsystem_state(thread));
}

// The state a parked thread already has, without allocating: the wait hook
// runs for every parked thread, and one that never polled has nothing to see.
inline ThreadState* existing_thread_state(const process::Thread& thread) {
    return thread.subsystem_state_bytes == sizeof(ThreadState)
        ? reinterpret_cast<ThreadState*>(thread.subsystem_state)
        : nullptr;
}

// The process's state, or nullptr when it has none (native, or no memory).
// The heap returns 16-byte aligned memory (heap::allocate), but Process
// stores a uint8_t*: hence reinterpret_cast rather than static_cast.
inline ProcessState* state(const process::Process& proc) {
    return reinterpret_cast<ProcessState*>(proc.subsystem_state);
}

// The current directory's path now into `out`, with POSIX's rule that no
// directory or no state is "/". False only when the path does not fit.
bool current_directory(const process::Process& proc, char* out, size_t capacity);

// The state hooks (posix_translation.cpp): a forked or spawned child takes its
// own reference on the inherited directory; freed state drops it.
void state_copied(process::Process& proc);
void state_released(process::Process& proc);

// The poll path (slice C), implemented in posix_translation.cpp.
bool commit_poll_entries(ThreadState& state, process::Process& proc);
int evaluate_poll_entries(ThreadState& state, process::Process& proc);
void recheck_parked_poll(process::Thread& thread, uint64_t now_ms, bool honour_deadline);

} // namespace posix
