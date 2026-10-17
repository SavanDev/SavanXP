#pragma once

#include <stddef.h>
#include <stdint.h>

namespace process {
struct SavedContext;
struct Process;
}

namespace subsystem {

enum class Id : uint8_t {
    posix = 0,   // default; coincide con el zero-init de allocate_process_slot
    native = 1,  // stub de validacion
    count
};

using Dispatcher = process::SavedContext* (*)(process::SavedContext*);

void reset();
bool register_dispatcher(Id id, Dispatcher fn);
Dispatcher dispatcher_for(Id id);
process::SavedContext* dispatch(Id id, process::SavedContext* context);

// Per-process private state, declared by the subsystem that owns it.
//
// The kernel allocates and frees the bytes and never looks inside: a
// subsystem registers how much its per-process state needs, and that is all
// the kernel knows about it. POSIX keeps its current directory there, which
// is why the kernel no longer has a cwd field (docs/KERNEL_NATIVE_API.md,
// isolation slice B). A subsystem with no state registers 0 and pays
// nothing.
void register_process_state_size(Id id, size_t bytes);
size_t process_state_size(Id id);

// Re-evaluation hook for processes parked with a deadline.
//
// The kernel's neutral concept is "this process is parked and something may
// have changed"; what it is waiting for belongs to the layer that parked it.
// poll() is the reason this exists (isolation slice C): the kernel used to
// walk the process table looking for POSIX poll waiters and re-evaluate their
// descriptors on every tick and every wakeup. Now the owning subsystem gets
// the parked process and decides. A subsystem with nothing parked registers
// nothing and the hook is a single null check.
using WaitRecheck = void (*)(process::Process& proc, uint64_t now_ms, bool honour_deadline);

void register_wait_recheck(Id id, WaitRecheck fn);
void recheck_parked_process(process::Process& proc, uint64_t now_ms, bool honour_deadline);

} // namespace subsystem
