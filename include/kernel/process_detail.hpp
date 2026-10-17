#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/process.hpp"

// Kernel-internal sharing for the POSIX translation that lives in
// subsystems/posix/kernel/. This is NOT subsystem API: nothing outside the
// kernel image includes it, and the names here carry no stability contract.
// It exists so the translation can keep calling the lifecycle, wait and
// handle machinery that must stay next to the scheduler without dragging
// POSIX bodies back into kernel/process.cpp.

namespace process {
namespace detail {

// Pool geometry owned by kernel/process.cpp (pipe storage, kernel stacks).
constexpr size_t kMaxPipeCount = 64;
constexpr size_t kPipeCapacity = 8192;
constexpr size_t kPipeChunkSize = 256;
constexpr uint64_t kKernelStackPages = 8;

uint64_t now_ms();
void proc_log(const char* format, ...);
void reset_time_slice(Process& proc);
// init's pid, for the kill guard. Published as an accessor so the mutable
// slot counter state never leaves kernel/process.cpp.
uint32_t init_pid();

Process* allocate_process_slot(bool idle);
void reset_process_slot(Process& proc);
void release_address_space(Process& proc);
void release_process_resources(Process& proc);
bool inherit_all_handles(Process& child, const Process& parent);

object::IoObject* allocate_io_object();
void discard_io_object(object::IoObject*& file_ptr);
void destroy_io_object(object::Header* header);
uint32_t object_access_for_open_flags(uint32_t open_flags);
int allocate_fd(Process& proc, object::Header* handle_object, uint32_t access, uint32_t flags);
int install_handle(Process& proc, int fd, object::Header* handle_object, uint32_t access, uint32_t flags);
object::IoObject* fd_to_io_object(Process& proc, uint64_t fd, uint32_t desired_access);
object::Header* lookup_handle(Process& proc, uint64_t fd, uint32_t desired_access);

Pipe* allocate_pipe();
int try_pipe_read(Process& proc, object::IoObject& file, uint64_t user_address, size_t count);
int pipe_write_chunk(Process& proc, Pipe& pipe, uint64_t user_address, size_t count, size_t progress);

int evaluate_poll_entries(Process& proc);
bool commit_poll_entries(Process& proc);

int begin_object_wait(Process& proc, object::Header* const* handles, size_t count, bool wait_all, int64_t timeout_ms);
// Los usa tambien la traduccion POSIX (rebanada C): completar un poll y
// avaliar un descriptor. El kernel no los llama para si.
int complete_blocked_wait(Process& proc, int result);
short poll_open_file(const object::IoObject* file, short events);
void wake_waiters_for_object(object::Header* handle_object);
void terminate_process(Process& proc, int exit_code);
// True when some open file still references the vnode (unlink/rmdir/rename
// guard). The IoObject pool lives in kernel/process.cpp, so the check does.
bool vnode_has_open_references(const vfs::Vnode& node);

} // namespace detail
} // namespace process
