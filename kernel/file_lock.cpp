#include "kernel/file_lock.hpp"

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "kernel/object.hpp"
#include "kernel/process.hpp"
#include "kernel/process_detail.hpp"

// The lock table (docs/kernel/FILE_LOCKS.md). One array for the system, walked
// whole: a lock is found by its file, and a system with more than a few dozen
// held at once is not one this table is sized for.

namespace file_lock {
namespace {

struct Lock {
    bool used;
    Policy policy;
    Kind kind;
    vfs::Vnode* node;
    const void* owner;
    uint32_t pid;
    uint64_t first;
    uint64_t last;
};

Lock g_locks[kMaxLocks] = {};
uint64_t g_tickets = 0;
// How far a deadlock search follows owners waiting on owners. A cycle longer
// than this is not found and simply waits, as on systems that do not look at
// all.
constexpr int kDeadlockDepth = 16;

int negative(sx_status status) {
    return -static_cast<int>(status);
}

bool overlaps(uint64_t first, uint64_t last, const Lock& lock) {
    return lock.first <= last && first <= lock.last;
}

bool owned_by(const Lock& lock, Policy policy, const void* owner) {
    return lock.policy == policy && lock.owner == owner;
}

// Whether `lock` stands in the way of `request`. Two shared locks never do.
// A POSIX owner's own locks never do: its new lock replaces them. Everyone
// else's do, the same open file's SUN locks included.
bool blocks(const Lock& lock, const Request& request) {
    if (!lock.used || lock.node != request.node || !overlaps(request.first, request.last, lock)) {
        return false;
    }
    if (lock.kind != Kind::exclusive && request.kind != Kind::exclusive) {
        return false;
    }
    return !(request.policy == Policy::process && owned_by(lock, Policy::process, request.owner));
}

const Lock* first_blocker(const Request& request, size_t from = 0) {
    for (size_t index = from; index < kMaxLocks; ++index) {
        if (blocks(g_locks[index], request)) {
            return &g_locks[index];
        }
    }
    return nullptr;
}

size_t free_slots() {
    size_t count = 0;
    for (const Lock& lock : g_locks) {
        count += lock.used ? 0u : 1u;
    }
    return count;
}

Lock* take_slot() {
    for (Lock& lock : g_locks) {
        if (!lock.used) {
            return &lock;
        }
    }
    return nullptr;
}

// POSIX: lays `request` over its owner's locks on the file. Its own locks
// inside the range are dropped, the ones crossing an edge trimmed, one that
// covers the range on both sides split in two; then the new lock goes in
// (unless it is an unlock) and absorbs its owner's adjacent locks of the same
// kind. Returns false, with nothing changed, when the table has no room.
// `released` is set when any byte stopped being held.
bool apply_posix(const Request& request, bool& released) {
    const size_t needed = request.kind == Kind::unlock ? 0u : 1u;
    size_t splits = 0;
    for (const Lock& lock : g_locks) {
        if (lock.used && lock.node == request.node && owned_by(lock, Policy::process, request.owner) &&
            lock.first < request.first && lock.last > request.last) {
            ++splits;
        }
    }
    if (free_slots() < needed + splits) {
        return false;
    }

    released = false;
    for (Lock& lock : g_locks) {
        if (!lock.used || lock.node != request.node || !owned_by(lock, Policy::process, request.owner) ||
            !overlaps(request.first, request.last, lock)) {
            continue;
        }
        // Replacing a range with the same kind frees nothing; anything else
        // (an unlock, exclusive turned shared or the other way) may let a
        // waiter in.
        if (lock.kind != request.kind) {
            released = true;
        }
        if (lock.first < request.first && lock.last > request.last) {
            Lock* right = take_slot();
            *right = lock;
            right->first = request.last + 1;
            lock.last = request.first - 1;
        } else if (lock.first < request.first) {
            lock.last = request.first - 1;
        } else if (lock.last > request.last) {
            lock.first = request.last + 1;
        } else {
            lock.used = false;
        }
    }
    if (request.kind == Kind::unlock) {
        return true;
    }

    uint64_t first = request.first;
    uint64_t last = request.last;
    for (bool merged = true; merged;) {
        merged = false;
        for (Lock& lock : g_locks) {
            if (!lock.used || lock.node != request.node || !owned_by(lock, Policy::process, request.owner) ||
                lock.kind != request.kind) {
                continue;
            }
            if ((first != 0 && lock.last == first - 1) || (last != kToEnd && lock.first == last + 1)) {
                first = lock.first < first ? lock.first : first;
                last = lock.last > last ? lock.last : last;
                lock.used = false;
                merged = true;
            }
        }
    }
    Lock* lock = take_slot();
    *lock = {true, Policy::process, request.kind, request.node, request.owner, request.pid, first, last};
    return true;
}

bool apply_open_file(const Request& request) {
    Lock* lock = take_slot();
    if (lock == nullptr) {
        return false;
    }
    *lock = {true, Policy::open_file, request.kind, request.node, request.owner, request.pid, request.first,
             request.last};
    return true;
}

// Whether `owner` (a POSIX process) waiting on `request` would wait on
// itself: some lock in its way belongs to a process that is itself parked on
// a lock `owner` holds, directly or further down the chain.
bool would_deadlock(const void* owner, const Request& request, int depth) {
    if (depth > kDeadlockDepth) {
        return false;
    }
    for (size_t index = 0; index < kMaxLocks; ++index) {
        const Lock& lock = g_locks[index];
        if (!blocks(lock, request) || lock.policy != Policy::process) {
            continue;
        }
        if (lock.owner == owner) {
            return true;
        }
        for (process::Thread* waiter = process::detail::file_lock_waiter_after(0); waiter != nullptr;
             waiter = process::detail::file_lock_waiter_after(waiter->lock_request.ticket)) {
            const Request& next = waiter->lock_request;
            if (next.policy == Policy::process && next.owner == lock.owner &&
                would_deadlock(owner, next, depth + 1)) {
                return true;
            }
        }
    }
    return false;
}

// Grants what the threads parked on `node` asked for, oldest first, while
// their ranges are free. A grant can free bytes too (a POSIX lock turned from
// exclusive to shared), so the walk starts over until a pass grants nothing.
void grant_waiters(vfs::Vnode* node) {
    for (bool granted = true; granted;) {
        granted = false;
        for (process::Thread* waiter = process::detail::file_lock_waiter_after(0); waiter != nullptr;) {
            process::Thread* const next = process::detail::file_lock_waiter_after(waiter->lock_request.ticket);
            const Request request = waiter->lock_request;
            if (request.node == node && first_blocker(request) == nullptr) {
                bool released = false;
                const bool applied =
                    request.policy == Policy::process ? apply_posix(request, released) : apply_open_file(request);
                process::detail::complete_blocked_wait(*waiter, applied ? 0 : negative(SX_ENOLCK));
                granted = true;
            }
            waiter = next;
        }
    }
}

int set_or_park(process::Thread& self, Request& request, bool wait) {
    if (first_blocker(request) == nullptr) {
        bool released = false;
        const bool applied = request.policy == Policy::process ? apply_posix(request, released)
                                                               : apply_open_file(request);
        if (!applied) {
            return negative(SX_ENOLCK);
        }
        if (released) {
            grant_waiters(request.node);
        }
        return 0;
    }
    if (!wait) {
        return negative(SX_EAGAIN);
    }
    if (request.policy == Policy::process && would_deadlock(request.owner, request, 0)) {
        return negative(SX_EDEADLK);
    }
    request.ticket = ++g_tickets;
    return process::detail::park_on_file_lock(self, request);
}

void drop_matching(Policy policy, const void* owner, vfs::Vnode* node) {
    bool released = false;
    vfs::Vnode* touched[kMaxLocks] = {};
    size_t touched_count = 0;
    for (Lock& lock : g_locks) {
        if (!lock.used || !owned_by(lock, policy, owner) || (node != nullptr && lock.node != node)) {
            continue;
        }
        bool seen = false;
        for (size_t index = 0; index < touched_count; ++index) {
            seen = seen || touched[index] == lock.node;
        }
        if (!seen) {
            touched[touched_count++] = lock.node;
        }
        lock.used = false;
        released = true;
    }
    if (!released) {
        return;
    }
    for (size_t index = 0; index < touched_count; ++index) {
        grant_waiters(touched[index]);
    }
}

} // namespace

int posix_set(process::Thread& self, vfs::Vnode& node, Kind kind, uint64_t first, uint64_t last, bool wait) {
    if (first > last) {
        return negative(SX_EINVAL);
    }
    Request request = {&node, self.process, self.process->pid, Policy::process, kind, first, last, 0};
    if (kind == Kind::unlock) {
        bool released = false;
        if (!apply_posix(request, released)) {
            return negative(SX_ENOLCK);
        }
        if (released) {
            grant_waiters(&node);
        }
        return 0;
    }
    return set_or_park(self, request, wait);
}

bool posix_query(const process::Process& proc, vfs::Vnode& node, Kind kind, uint64_t first, uint64_t last,
                 Holder& holder) {
    const Request request = {&node, &proc, proc.pid, Policy::process, kind, first, last, 0};
    const Lock* lock = first_blocker(request);
    if (lock == nullptr) {
        return false;
    }
    holder = {lock->kind, lock->first, lock->last, lock->pid};
    return true;
}

int open_file_lock(process::Thread& self, object::IoObject& file, Kind kind, uint64_t first, uint64_t last,
                   bool wait) {
    if (file.node == nullptr || kind == Kind::unlock || first > last) {
        return negative(SX_EINVAL);
    }
    Request request = {file.node, &file, self.process->pid, Policy::open_file, kind, first, last, 0};
    return set_or_park(self, request, wait);
}

int open_file_unlock(object::IoObject& file, uint64_t first, uint64_t last) {
    for (Lock& lock : g_locks) {
        if (lock.used && owned_by(lock, Policy::open_file, &file) && lock.node == file.node &&
            lock.first == first && lock.last == last) {
            lock.used = false;
            grant_waiters(file.node);
            return 0;
        }
    }
    return negative(SX_ENOENT);
}

void handle_dropped(process::Process& proc, object::Header* handle_object) {
    object::IoObject* file = object::as_io(handle_object);
    if (file == nullptr || file->kind != process::HandleKind::vnode || file->node == nullptr) {
        return;
    }
    drop_matching(Policy::process, &proc, file->node);
}

void open_file_destroyed(object::IoObject& file) {
    drop_matching(Policy::open_file, &file, nullptr);
}

void process_ended(process::Process& proc) {
    drop_matching(Policy::process, &proc, nullptr);
}

size_t held_count() {
    return kMaxLocks - free_slots();
}

} // namespace file_lock
