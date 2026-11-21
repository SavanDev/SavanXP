#pragma once

#include <stddef.h>
#include <stdint.h>

// Byte-range locks on files (docs/kernel/FILE_LOCKS.md): one table for the
// system, two owner policies over it. POSIX record locks belong to the process
// and go when it closes any descriptor of the file; SUN's belong to the open
// file and go with it, as LockFileEx's do. Advisory: reads and writes never
// look at them. Everything here runs under the big kernel lock.

namespace vfs {
struct Vnode;
}
namespace object {
struct Header;
struct IoObject;
}
namespace process {
struct Process;
struct Thread;
}

namespace file_lock {

// Room for the whole system: a handful of programs that lock a few ranges
// each. A full table is a clean ENOLCK.
constexpr size_t kMaxLocks = 128;
// The last byte of a range that runs to the end of the file and past it.
constexpr uint64_t kToEnd = ~0ULL;

enum class Policy : uint8_t {
    none = 0,
    // POSIX fcntl: the owner is the Process. Its own locks never conflict
    // with each other; a new one replaces, splits or merges them.
    process = 1,
    // SUN: the owner is the IoObject. Locks are not reentrant: a range the
    // same open file holds exclusively blocks it like anyone else.
    open_file = 2,
};

enum class Kind : uint8_t {
    shared = 0,
    exclusive = 1,
    // POSIX F_UNLCK: release the range.
    unlock = 2,
};

// What a thread parked on a lock asks for. It lives in the Thread
// (process::Thread::lock_request) while wait_reason is file_lock.
struct Request {
    vfs::Vnode* node;
    const void* owner;
    uint32_t pid;
    Policy policy;
    Kind kind;
    uint64_t first;
    uint64_t last;
    uint64_t ticket;
};

// The lock F_GETLK reports.
struct Holder {
    Kind kind;
    uint64_t first;
    uint64_t last;
    uint32_t pid;
};

// POSIX F_SETLK / F_SETLKW for `self`'s process over [first, last]. 0 when
// set or released; -EAGAIN when another owner holds a conflicting range and
// `wait` is false; -EDEADLK when waiting would close a cycle of processes
// waiting on each other; -ENOLCK with the table full; and
// process::kBlockedResult when the thread parked until the range frees.
int posix_set(process::Thread& self, vfs::Vnode& node, Kind kind, uint64_t first, uint64_t last, bool wait);
// POSIX F_GETLK: true, with the first lock of another owner that would block
// a `kind` lock of `proc` over [first, last].
bool posix_query(const process::Process& proc, vfs::Vnode& node, Kind kind, uint64_t first, uint64_t last,
                 Holder& holder);

// SUN: lock [first, last] for the open file `file` (a vnode handle). 0, or
// -EAGAIN with `wait` false, -ENOLCK, or process::kBlockedResult.
int open_file_lock(process::Thread& self, object::IoObject& file, Kind kind, uint64_t first, uint64_t last,
                   bool wait);
// SUN: release the lock `file` holds over exactly [first, last]; -ENOENT when
// it holds none there, as UnlockFileEx needs the range it locked.
int open_file_unlock(object::IoObject& file, uint64_t first, uint64_t last);

// A process stops holding `handle_object` in one of its slots (a close, a
// dup2 over it, the table released at exit). When it is a vnode handle, the
// process's POSIX locks on that file go, as POSIX asks.
void handle_dropped(process::Process& proc, object::Header* handle_object);
// The open file is being destroyed: its SUN locks go.
void open_file_destroyed(object::IoObject& file);
// The process's slot is being reset: whatever POSIX locks it still holds go.
// Its handles released them already on every path that ends a process; this
// keeps a lock from outliving its owner on one that does not.
void process_ended(process::Process& proc);

// Locks held now, for tests and diagnostics.
size_t held_count();

} // namespace file_lock
