#pragma once

// SavanXP native kernel API (Sx): neutral user-memory, current-process and
// error primitives shared by every subsystem dispatcher and driver.
//
// Naming: Sx, not Nt. The shape is NT-inspired (subsystems talk to one native
// surface instead of reaching into each other's internals) but the names are
// this system's own: sx::current, sx::copy_from_user, sx::negative_error.
//
// Errors are the kernel's own space now (slice E): `sx_status`, returned
// negative. The numbers still equal errno, because changing them would change
// every userland-visible error for nothing; what changed is ownership -- POSIX
// derives its error names from this space instead of the kernel borrowing
// errno's. A userland with a different error vocabulary maps it in its own
// translation.
//
// The current-process flavors cover syscalls and ioctls (the caller is
// always current). The explicit-process flavors cover wake paths, fork and
// spawn, which touch processes that are not running here and need the CR3
// switch. There is exactly one implementation (kernel/sx.cpp); the process::
// wrappers in kernel/process.cpp exist so drivers keep compiling untouched
// while the translation layer converges onto sx::.

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_abi.h"

namespace process {
struct Process;
struct Thread;
}

namespace object {
struct EventObject;
struct Header;
struct SectionObject;
struct SemaphoreObject;
struct TimerObject;
}

namespace object {
struct IoObject;
}

namespace cred {
struct Credentials;
}

namespace vfs {
struct Vnode;
}

namespace sx {

process::Process* current();
process::Thread* current_thread();
uint32_t current_pid();

bool copy_from_user(void* destination, uint64_t user_address, size_t count);
bool copy_to_user(uint64_t user_address, const void* source, size_t count);
bool validate_user_range(uint64_t user_address, size_t count, bool require_write);
bool copy_user_string(char* buffer, size_t capacity, uint64_t user_address);

bool read_user_memory(const process::Process& proc, void* destination,
                      uint64_t user_address, size_t count, bool require_write);
bool write_user_memory(process::Process& proc, uint64_t user_address,
                       const void* source, size_t count);
bool copy_user_string_from(const process::Process& proc, uint64_t user_address,
                           char* buffer, size_t capacity);

int negative_error(sx_status code);

// Minimal standalone lifecycle: terminate the calling process. Backed by the
// same mechanism as the POSIX EXIT path; it does not return.
void exit_current(int exit_code);

// Neutral object operations (events, timers, semaphores): thin constructors
// and signals over object::, with no fd-table or flag parsing. The POSIX
// translation keeps flag translation, access masks and handle allocation;
// future native paths call these directly.
object::EventObject* create_event(bool manual_reset, bool initial_state);
void set_event(object::EventObject* event_object);
void reset_event(object::EventObject* event_object);
object::TimerObject* create_timer(bool manual_reset);
void set_timer(object::TimerObject* timer_object, uint64_t due_tick, uint64_t period_ticks);
void cancel_timer(object::TimerObject* timer_object);
void poll_timers(uint64_t now_ms, void (*on_signal)(object::Header* object));
object::SemaphoreObject* create_semaphore(int32_t initial_count, int32_t max_count);
bool release_semaphore(object::SemaphoreObject* semaphore_object, int32_t release_count, int32_t* previous_count);

// Wait policy (satisfaction + consumption): the neutral core behind every
// wait, polled or blocking. try_consume_objects fills the caller's wait
// state and, if something is already signaled, consumes it: >=0 consumed,
// -errno on misuse, -EAGAIN when nothing is signaled yet and the caller
// should park (or time out immediately).
bool is_waitable(const object::Header* handle_object);
// What a wait may name: a waitable object (event, timer, semaphore, port), or
// a stream opened for reading -- a pipe, a device, a socket -- which counts as
// signaled while a read would not block (data, or end of stream). Streams are
// level-triggered: waiting consumes nothing.
bool is_wait_target(const object::Header* handle_object);
bool can_complete_object_wait(const process::Thread& thread);
int consume_object_wait(process::Thread& thread);
int try_consume_objects(process::Thread& thread, object::Header* const* handles, size_t count, bool wait_all, const uint32_t* interests = nullptr);

// Sections and views: neutral constructors plus the W^X and mask policy.
// Flag translation (SAVANXP_SECTION_*/VIEW_*), file checks and handle
// allocation stay on the translation side; granted_access travels as an
// explicit mask, like any handle table would carry it.
object::SectionObject* create_section(uint64_t size_bytes, uint32_t access_mask);
object::SectionObject* open_file_section(vfs::Vnode& node, uint64_t offset, uint64_t length, uint32_t access_mask);
int64_t map_view(process::Process& proc, object::SectionObject& section, uint32_t requested_access, uint32_t granted_access, uint64_t base, bool share_on_fork);
int unmap_view(process::Process& proc, uint64_t base_address);

// Files: the one open/read/write/seek/close path every subsystem goes through
// (kernel/sx_file.cpp). Results carry a status and a length instead of
// leaving the error in vfs::last_error(). Checking the user buffer, handle
// allocation, and turning a result into a syscall's return value stay with
// each subsystem, as for sections.

// sx_status has no name for success; this is it.
constexpr sx_status kStatusOk = static_cast<sx_status>(0);

// What a transfer did: `length` bytes moved, then it stopped for `status`
// (kStatusOk: done, or end of file). A late failure keeps the bytes already
// moved.
struct IoResult {
    sx_status status;
    size_t length;
};

// The bytes moved when there were any, otherwise -status (0 at end of file).
int io_result_value(const IoResult& result);

// The largest count a call that reports its length as an int can ask for.
constexpr size_t kMaxIoCount = 0x7fffffffu;

// What open_file accepts besides a regular file. A directory opens for
// reading only (writing one is EISDIR); a device node becomes a device
// handle. Without the bit, a directory is EISDIR and a device EINVAL.
enum OpenAccept : uint32_t {
    open_accept_file = 0,
    open_accept_directory = 1u << 0,
    open_accept_device = 1u << 1,
};

// Resolves `path` through the VFS with SAVANXP_OPEN_* flags and builds the
// I/O object over the node, positioned at the end with SAVANXP_OPEN_APPEND.
// The object is in no handle table yet: on success the caller installs it or
// discards it.
sx_status open_file(const cred::Credentials& who, const char* path, uint32_t flags,
                    uint16_t create_permissions, uint32_t accept, object::IoObject*& out);

// Reads from / writes to an open file at its offset, and advances it by what
// moved. `user_buffer` is in `proc` and has already been checked.
IoResult read_file(process::Process& proc, object::IoObject& file, uint64_t user_buffer, size_t count);
IoResult write_file(process::Process& proc, object::IoObject& file, uint64_t user_buffer, size_t count);

enum class SeekOrigin : uint8_t {
    start,
    current,
    end,
};

// Moves an open file's offset; past the end is allowed, before 0 is EINVAL.
sx_status seek_file(object::IoObject& file, int64_t offset, SeekOrigin origin, uint64_t& position);

// Closes a handle of `proc`: the object goes when its last handle does.
sx_status close_handle(process::Process& proc, uint64_t handle);

} // namespace sx
