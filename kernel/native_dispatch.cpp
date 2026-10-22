#include "kernel/process.hpp"

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_native_abi.h"
#include "kernel/console.hpp"
#include "kernel/device.hpp"
#include "kernel/display.hpp"
#include "kernel/heap.hpp"
#include "kernel/object.hpp"
#include "kernel/process_detail.hpp"
#include "kernel/string.hpp"
#include "kernel/sx.hpp"
#include "kernel/timer.hpp"
#include "kernel/vfs.hpp"

namespace process {

namespace {

/* --- Handles and I/O for native processes ---------------------------------
 *
 * Built on the kernel's own cores (handle table, pipe chunking, device
 * dispatch), not on the POSIX translation: a native process reads a pipe the
 * way the kernel defines a pipe, and the POSIX layer's tty, file and socket
 * semantics stay where they belong. Blocking parks the process exactly as the
 * POSIX side does (blocked_read/blocked_write), so the existing wakeups --
 * a write making room, a reader appearing -- serve both. */

int native_close(Process& proc, uint64_t handle) {
    if (handle >= kMaxFileHandles || proc.handles[handle].object == nullptr) {
        return sx::negative_error(SX_EBADF);
    }
    object::release(proc.handles[handle].object);
    proc.handles[handle].granted_access = object::access_none;
    proc.handles[handle].flags = object::handle_none;
    return 0;
}

int native_duplicate(Process& proc, uint64_t handle) {
    object::Header* target = detail::lookup_handle(proc, handle, object::access_query);
    if (target == nullptr) {
        return sx::negative_error(SX_EBADF);
    }
    return detail::allocate_fd(proc, target, proc.handles[handle].granted_access, proc.handles[handle].flags);
}

int native_read(Thread& self, uint64_t handle, uint64_t user_buffer, size_t count) {
    Process& proc = *self.process;
    object::IoObject* file = detail::fd_to_io_object(proc, handle, object::access_read);
    if (file == nullptr || (file->open_flags & open_read) == 0) {
        return sx::negative_error(SX_EBADF);
    }
    if (count == 0) {
        return 0;
    }
    if (!sx::validate_user_range(user_buffer, count, true)) {
        return sx::negative_error(SX_EINVAL);
    }
    if (file->kind == HandleKind::device) {
        return device::read(file->device, user_buffer, count);
    }
    if (file->kind != HandleKind::pipe) {
        return sx::negative_error(SX_EBADF);
    }
    const int result = detail::try_pipe_read(proc, *file, user_buffer, count);
    if (result != kBlockedResult) {
        return result;
    }
    if ((file->open_flags & open_nonblock) != 0) {
        return sx::negative_error(SX_EAGAIN);
    }
    self.blocked_io_fd = handle;
    self.blocked_read_buffer = user_buffer;
    self.blocked_read_capacity = count;
    self.state = State::blocked_read;
    return kBlockedResult;
}

int native_write(Thread& self, uint64_t handle, uint64_t user_buffer, size_t count) {
    Process& proc = *self.process;
    object::IoObject* file = detail::fd_to_io_object(proc, handle, object::access_write);
    if (file == nullptr || (file->open_flags & open_write) == 0) {
        return sx::negative_error(SX_EBADF);
    }
    if (count == 0) {
        return 0;
    }
    if (!sx::validate_user_range(user_buffer, count, false)) {
        return sx::negative_error(SX_EINVAL);
    }
    if (file->kind == HandleKind::device) {
        return device::write(file->device, user_buffer, count);
    }
    if (file->kind != HandleKind::pipe || file->pipe == nullptr) {
        return sx::negative_error(SX_EBADF);
    }

    const bool nonblocking = (file->open_flags & open_nonblock) != 0;
    // Records up to the pipe's capacity are written whole or not at all, so a
    // reader of fixed-size records never sees half of one.
    if (nonblocking && count <= detail::kPipeCapacity && file->pipe->reader_refs != 0 &&
        (detail::kPipeCapacity - file->pipe->size) < count) {
        return sx::negative_error(SX_EAGAIN);
    }
    const int step = detail::pipe_write_chunk(proc, *file->pipe, user_buffer, count, 0);
    if (step == kBlockedResult && nonblocking) {
        return sx::negative_error(SX_EAGAIN);
    }
    if (step < 0 && step != kBlockedResult) {
        return step;
    }
    if (step != kBlockedResult && (static_cast<size_t>(step) == count || nonblocking)) {
        return step;
    }
    self.blocked_io_fd = handle;
    self.blocked_write_buffer = user_buffer;
    self.blocked_write_length = count;
    self.blocked_write_progress = step == kBlockedResult ? 0 : static_cast<uint64_t>(step);
    self.state = State::blocked_write;
    return kBlockedResult;
}

/* Devices live under /dev; a native process names them without the
 * directory, so it cannot reach anything else through this call. */
int native_open_device(Process& proc, uint64_t user_name, uint32_t access) {
    char name[32] = {};
    char path[40] = "/dev/";
    size_t length = 0;

    if (!sx::copy_user_string(name, sizeof(name), user_name) || name[0] == '\0' ||
        (access & ~(SXN_DEVICE_READ | SXN_DEVICE_WRITE)) != 0 || access == 0) {
        return sx::negative_error(SX_EINVAL);
    }
    for (length = 0; name[length] != '\0'; ++length) {
        const char c = name[length];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            return sx::negative_error(SX_EINVAL);
        }
        path[5 + length] = c;
    }

    const uint32_t open_flags =
        ((access & SXN_DEVICE_READ) != 0 ? SAVANXP_OPEN_READ : 0u) |
        ((access & SXN_DEVICE_WRITE) != 0 ? SAVANXP_OPEN_WRITE : 0u);
    vfs::Vnode* node = vfs::open(path, open_flags);
    if (node == nullptr || node->backend != vfs::Backend::device) {
        return sx::negative_error(SX_ENOENT);
    }

    object::IoObject* file = detail::allocate_io_object();
    if (file == nullptr) {
        return sx::negative_error(SX_ENOMEM);
    }
    file->kind = HandleKind::device;
    file->open_flags =
        ((access & SXN_DEVICE_READ) != 0 ? open_read : 0u) |
        ((access & SXN_DEVICE_WRITE) != 0 ? open_write : 0u);
    file->node = node;
    file->device = static_cast<device::Device*>(node->data);
    file->header.destroy = detail::destroy_io_object;
    const int handle = detail::allocate_fd(
        proc, &file->header, detail::object_access_for_open_flags(file->open_flags), object::handle_none);
    if (handle < 0) {
        detail::discard_io_object(file);
    }
    return handle;
}

int native_device_control(Process& proc, uint64_t handle, uint64_t code, uint64_t argument) {
    object::IoObject* file = detail::fd_to_io_object(proc, handle, object::access_query);
    if (file == nullptr || file->kind != HandleKind::device || file->device == nullptr) {
        return sx::negative_error(SX_EBADF);
    }
    return device::ioctl(file->device, code, argument, proc.handles[handle].granted_access);
}

/* --- Ports -------------------------------------------------------------- */

int native_port_create(Process& proc, uint64_t user_name) {
    char name[object::kPortNameCapacity] = {};

    if (!sx::copy_user_string(name, sizeof(name), user_name) || name[0] == '\0') {
        return sx::negative_error(SX_EINVAL);
    }
    if (object::find_port(name) != nullptr) {
        return sx::negative_error(SX_EEXIST);
    }
    object::PortObject* port = object::create_port(name);
    if (port == nullptr) {
        return sx::negative_error(SX_ENOMEM);
    }
    const int handle = detail::allocate_fd(
        proc, &port->header, object::access_query | object::access_synchronize, object::handle_none);
    if (handle < 0) {
        // Never referenced: free the slot and the name directly.
        object::Header* header = &port->header;
        object::retain(header);
        object::release(header);
    }
    return handle;
}

int native_port_connect(Process& proc, uint64_t user_name, uint64_t user_handles) {
    char name[object::kPortNameCapacity] = {};
    object::IoObject* request_reader = nullptr;
    object::IoObject* request_writer = nullptr;
    object::IoObject* reply_reader = nullptr;
    object::IoObject* reply_writer = nullptr;

    if (!sx::copy_user_string(name, sizeof(name), user_name) || name[0] == '\0' ||
        !sx::validate_user_range(user_handles, sizeof(int32_t) * 2, true)) {
        return sx::negative_error(SX_EINVAL);
    }
    object::PortObject* port = object::find_port(name);
    if (port == nullptr) {
        return sx::negative_error(SX_ENOENT);
    }
    if (port->pending_count >= object::kPortPendingCapacity) {
        return sx::negative_error(SX_EAGAIN);
    }
    if (!detail::create_pipe_pair(request_reader, request_writer)) {
        return sx::negative_error(SX_ENOMEM);
    }
    if (!detail::create_pipe_pair(reply_reader, reply_writer)) {
        object::Header* a = &request_reader->header;
        object::Header* b = &request_writer->header;
        object::release(a);
        object::release(b);
        return sx::negative_error(SX_ENOMEM);
    }

    const int write_handle = detail::allocate_fd(
        proc, &request_writer->header, detail::object_access_for_open_flags(open_write), object::handle_none);
    const int read_handle = write_handle < 0 ? write_handle : detail::allocate_fd(
        proc, &reply_reader->header, detail::object_access_for_open_flags(open_read), object::handle_none);
    // The handle table took its own references; the creation ones go.
    object::Header* owned_request_writer = &request_writer->header;
    object::Header* owned_reply_reader = &reply_reader->header;
    object::release(owned_request_writer);
    object::release(owned_reply_reader);

    object::PortConnection connection = {};
    connection.client_pid = proc.pid;
    connection.server_read = &request_reader->header;
    connection.server_write = &reply_writer->header;
    const int32_t values[2] = {write_handle, read_handle};

    if (write_handle < 0 || read_handle < 0 ||
        !sx::copy_to_user(user_handles, values, sizeof(values)) ||
        !object::port_enqueue(port, connection)) {
        if (write_handle >= 0) {
            (void)native_close(proc, static_cast<uint64_t>(write_handle));
        }
        if (read_handle >= 0) {
            (void)native_close(proc, static_cast<uint64_t>(read_handle));
        }
        object::release(connection.server_read);
        object::release(connection.server_write);
        return write_handle < 0 ? write_handle : (read_handle < 0 ? read_handle : sx::negative_error(SX_EAGAIN));
    }
    detail::wake_waiters_for_object(&port->header);
    return 0;
}

// The handles of a WAIT_MANY: objects as WAIT_ONE takes them, or streams opened
// for reading (sx::is_wait_target); the wait is the same core as POSIX
// wait_many. Fills `handles` and returns the count, or -errno.
int native_wait_targets(Process& proc, uint64_t user_handles, uint64_t count, object::Header** handles) {
    if (count == 0 || count > kMaxWaitHandles) {
        return sx::negative_error(SX_EINVAL);
    }
    int32_t raw[kMaxWaitHandles] = {};
    if (!sx::copy_from_user(raw, user_handles, static_cast<size_t>(count) * sizeof(int32_t))) {
        return sx::negative_error(SX_EINVAL);
    }
    for (size_t index = 0; index < count; ++index) {
        if (raw[index] < 0) {
            return sx::negative_error(SX_EBADF);
        }
        handles[index] = detail::lookup_handle(proc, static_cast<uint64_t>(raw[index]), object::access_synchronize);
        if (handles[index] == nullptr) {
            return sx::negative_error(SX_EBADF);
        }
        if (!sx::is_wait_target(handles[index])) {
            return sx::negative_error(SX_EINVAL);
        }
    }
    return static_cast<int>(count);
}

// The non-blocking mode of a stream: shared by every handle to it, as POSIX
// O_NONBLOCK is.
int native_set_nonblocking(Process& proc, uint64_t handle, uint64_t on) {
    object::IoObject* file = detail::fd_to_io_object(proc, handle, object::access_query);
    if (file == nullptr || (file->kind != HandleKind::pipe && file->kind != HandleKind::device)) {
        return sx::negative_error(SX_EBADF);
    }
    if (on != 0) {
        file->open_flags |= open_nonblock;
    } else {
        file->open_flags &= ~static_cast<uint32_t>(open_nonblock);
    }
    return 0;
}

// Public facts about a process, as a task list shows them.
int native_process_query(uint64_t pid_value, uint64_t user_out) {
    sxn_process_query out = {};

    if (pid_value == 0 || pid_value > 0xffffffffull) {
        return sx::negative_error(SX_ENOENT);
    }
    if (!sx::validate_user_range(user_out, sizeof(out), true)) {
        return sx::negative_error(SX_EINVAL);
    }
    Process* target = process::find(static_cast<uint32_t>(pid_value));
    if (target == nullptr) {
        return sx::negative_error(SX_ENOENT);
    }
    out.pid = target->pid;
    out.parent_pid = target->parent_pid;
    out.state = SXN_PROCESS_RUNNING;
    out.exit_code = 0;
    if (target->life == Lifecycle::zombie) {
        out.state = SXN_PROCESS_ENDED;
        out.exit_code = target->exit_code;
    } else {
        // Killed while it ran on another core: TERMINATE returned before that
        // core could tear it down, but it will never run user code again. A
        // caller that just ended it must not see it running.
        int pending_code = 0;
        if (exit_pending(*target, pending_code)) {
            out.state = SXN_PROCESS_ENDED;
            out.exit_code = pending_code;
        }
    }
    for (size_t index = 0; index < kMaxFileHandles; ++index) {
        if (target->handles[index].object != nullptr) {
            out.handle_count += 1u;
        }
    }
    static_assert(sizeof(out.name) <= sizeof(target->name), "process name capacity");
    memcpy(out.name, target->name, sizeof(out.name));
    out.name[sizeof(out.name) - 1u] = '\0';
    return sx::copy_to_user(user_out, &out, sizeof(out)) ? 0 : sx::negative_error(SX_EINVAL);
}

// Ends a child of the caller. Children only: having created the process is
// the right to end it (NT asks for a handle with terminate access; there are no
// process handles yet). terminate_process already ignores a zombie, so a child
// that ended on its own is not an error.
int native_terminate(Process& proc, uint64_t pid_value, uint64_t status) {
    if (pid_value == 0 || pid_value > 0xffffffffull) {
        return sx::negative_error(SX_ENOENT);
    }
    Process* target = process::find(static_cast<uint32_t>(pid_value));
    if (target == nullptr) {
        return sx::negative_error(SX_ENOENT);
    }
    // Any descendant, not only a child: walk up from the target. The chain
    // ends at init (an orphan is re-parented there) and is never longer than
    // the process table, which bounds the walk if a parent link ever loops.
    uint32_t ancestor = target->parent_pid;
    bool descendant = false;
    for (size_t depth = 0; ancestor != 0 && depth < process::kMaxProcesses; ++depth) {
        if (ancestor == proc.pid) {
            descendant = true;
            break;
        }
        const Process* step = process::find(ancestor);
        if (step == nullptr) {
            break;
        }
        ancestor = step->parent_pid;
    }
    if (!descendant) {
        return sx::negative_error(SX_ECHILD);
    }
    detail::terminate_process(*target, static_cast<int>(status));
    return 0;
}

// Whether `handle` names an open entry of the caller's table.
bool handle_is_open(const Process& proc, int32_t handle) {
    return handle >= 0 && handle < static_cast<int32_t>(process::kMaxFileHandles) &&
        proc.handles[handle].object != nullptr;
}

// CreatePipe: both ends of a fresh pipe into the caller's table, the same
// pair a port connection makes.
int native_pipe_create(Process& proc, uint64_t user_handles) {
    object::IoObject* reader = nullptr;
    object::IoObject* writer = nullptr;

    if (!sx::validate_user_range(user_handles, sizeof(int32_t) * 2, true)) {
        return sx::negative_error(SX_EINVAL);
    }
    if (!detail::create_pipe_pair(reader, writer)) {
        return sx::negative_error(SX_ENOMEM);
    }
    const int read_handle = detail::allocate_fd(
        proc, &reader->header, detail::object_access_for_open_flags(open_read), object::handle_none);
    const int write_handle = read_handle < 0 ? read_handle : detail::allocate_fd(
        proc, &writer->header, detail::object_access_for_open_flags(open_write), object::handle_none);
    // The handle table took its own references; the creation ones go.
    object::Header* owned_reader = &reader->header;
    object::Header* owned_writer = &writer->header;
    object::release(owned_reader);
    object::release(owned_writer);

    const int32_t values[2] = {read_handle, write_handle};
    if (read_handle < 0 || write_handle < 0 || !sx::copy_to_user(user_handles, values, sizeof(values))) {
        if (read_handle >= 0) {
            (void)native_close(proc, static_cast<uint64_t>(read_handle));
        }
        if (write_handle >= 0) {
            (void)native_close(proc, static_cast<uint64_t>(write_handle));
        }
        return read_handle < 0 ? read_handle : (write_handle < 0 ? write_handle : sx::negative_error(SX_EINVAL));
    }
    return 0;
}

int native_port_accept(Process& proc, uint64_t port_handle, uint64_t user_connection) {
    object::PortObject* port = object::as_port(detail::lookup_handle(proc, port_handle, object::access_query));
    object::PortConnection connection = {};
    sxn_port_connection out = {};

    if (port == nullptr) {
        return sx::negative_error(SX_EBADF);
    }
    if (!sx::validate_user_range(user_connection, sizeof(out), true)) {
        return sx::negative_error(SX_EINVAL);
    }
    if (!object::port_dequeue(port, connection)) {
        return sx::negative_error(SX_EAGAIN);
    }
    const int read_handle = detail::allocate_fd(
        proc, connection.server_read, detail::object_access_for_open_flags(open_read), object::handle_none);
    const int write_handle = read_handle < 0 ? read_handle : detail::allocate_fd(
        proc, connection.server_write, detail::object_access_for_open_flags(open_write), object::handle_none);
    object::release(connection.server_read);
    object::release(connection.server_write);
    if (read_handle < 0 || write_handle < 0) {
        if (read_handle >= 0) {
            (void)native_close(proc, static_cast<uint64_t>(read_handle));
        }
        return read_handle < 0 ? read_handle : write_handle;
    }
    out.client_pid = connection.client_pid;
    out.read_handle = read_handle;
    out.write_handle = write_handle;
    if (!sx::copy_to_user(user_connection, &out, sizeof(out))) {
        (void)native_close(proc, static_cast<uint64_t>(read_handle));
        (void)native_close(proc, static_cast<uint64_t>(write_handle));
        return sx::negative_error(SX_EINVAL);
    }
    return 0;
}

/* Handle passing over a pipe: holding the write end is the permission. Only
 * objects that are not streams travel -- a shared stream would drag its offset
 * and its reader/writer counts along. Same queue as the POSIX side's. */
int native_pipe_send_handle(Process& proc, uint64_t pipe_handle, uint64_t handle) {
    object::IoObject* file = detail::fd_to_io_object(proc, pipe_handle, object::access_write);
    if (file == nullptr || file->kind != HandleKind::pipe || file->pipe == nullptr ||
        (file->open_flags & open_write) == 0) {
        return sx::negative_error(SX_EBADF);
    }
    object::Header* target = detail::lookup_handle(proc, handle, object::access_none);
    if (target == nullptr) {
        return sx::negative_error(SX_EBADF);
    }
    if (target->type == object::Type::io) {
        return sx::negative_error(SX_EINVAL);
    }
    Pipe& pipe = *file->pipe;
    if (pipe.reader_refs == 0) {
        return sx::negative_error(SX_EPIPE);
    }
    if (pipe.handle_count >= kPipeHandleQueueCapacity) {
        return sx::negative_error(SX_EAGAIN);
    }
    const uint32_t tail = (pipe.handle_head + pipe.handle_count) % kPipeHandleQueueCapacity;
    object::retain(target);
    pipe.handles[tail] = target;
    pipe.handle_access[tail] = proc.handles[handle].granted_access;
    pipe.handle_count += 1u;
    return 0;
}

int native_pipe_receive_handle(Process& proc, uint64_t pipe_handle) {
    object::IoObject* file = detail::fd_to_io_object(proc, pipe_handle, object::access_read);
    if (file == nullptr || file->kind != HandleKind::pipe || file->pipe == nullptr ||
        (file->open_flags & open_read) == 0) {
        return sx::negative_error(SX_EBADF);
    }
    Pipe& pipe = *file->pipe;
    if (pipe.handle_count == 0) {
        return sx::negative_error(SX_EAGAIN);
    }
    const int handle = detail::allocate_fd(
        proc, pipe.handles[pipe.handle_head], pipe.handle_access[pipe.handle_head], object::handle_none);
    if (handle < 0) {
        return handle; // stays queued: the receiver can close something and retry
    }
    object::release(pipe.handles[pipe.handle_head]);
    pipe.handle_access[pipe.handle_head] = object::access_none;
    pipe.handle_head = (pipe.handle_head + 1u) % kPipeHandleQueueCapacity;
    pipe.handle_count -= 1u;
    return handle;
}

} // namespace

// El subsistema nativo declara su estado privado por proceso como cualquier
// otro (rebanada B). Hoy declara cero: un proceso nativo no tiene estado propio,
// y decirlo es lo que hace medible la afirmacion de que al kernel no le queda
// nada de POSIX.
void initialize_native_subsystem() {
    subsystem::register_process_state_size(subsystem::Id::native, 0);
}

/* The native calls any subsystem's processes may make: the object floor
 * (handles, events, sections, waits, pipes and devices, ports, the clock), as
 * a Win32 process calls ntdll. Not the lifecycle ones -- EXIT, SPAWN, WAITPID
 * -- which a subsystem defines in its own terms, nor INFO/LOG/GFX, which are a
 * standalone native program's. */
bool native_call_open_to_all_subsystems(uint64_t number) {
    switch (number) {
        case SXN_SYS_EVENT_CREATE:
        case SXN_SYS_EVENT_SET:
        case SXN_SYS_EVENT_RESET:
        case SXN_SYS_WAIT_ONE:
        case SXN_SYS_WAIT_MANY:
        case SXN_SYS_SET_NONBLOCKING:
        case SXN_SYS_SECTION_CREATE:
        case SXN_SYS_MAP_VIEW:
        case SXN_SYS_UNMAP_VIEW:
        case SXN_SYS_CLOSE:
        case SXN_SYS_DUPLICATE:
        case SXN_SYS_READ:
        case SXN_SYS_WRITE:
        case SXN_SYS_OPEN_DEVICE:
        case SXN_SYS_DEVICE_CONTROL:
        case SXN_SYS_CLOCK:
        case SXN_SYS_PORT_CREATE:
        case SXN_SYS_PORT_CONNECT:
        case SXN_SYS_PORT_ACCEPT:
        case SXN_SYS_PIPE_SEND_HANDLE:
        case SXN_SYS_PIPE_RECEIVE_HANDLE:
        case SXN_SYS_PIPE_CREATE:
            return true;
        default:
            return false;
    }
}

SavedContext* dispatch_native_syscall(SavedContext* context) {
    Thread* const self = sx::current_thread();
    if (self == nullptr) {
        return context;
    }
    Process* const caller = self->process;

    self->context = context;
    self->state = State::running;

    // Prueba observable de que el camino nativo esta vivo. Se emite una
    // sola vez (la primera syscall nativa de todo el sistema); confirma que un
    // proceso marcado nativo entra por ESTE dispatcher y no por el de posix.
    static bool announced = false;
    if (!announced) {
        announced = true;
        console::printf(
            "native: dispatcher activo (pid=%u, syscall=%u)\n",
            static_cast<unsigned>(caller->pid),
            static_cast<unsigned>(context->rax)
        );
    }

    // The native subsystem's own calls (>= SXN_SYS_BASE); the contract lives
    // in include/abi/savanxp_native_abi.h. Other subsystems forward the
    // object calls here (native_call_open_to_all_subsystems); the lifecycle
    // ones exist only for native processes.
    //
    // Esta unidad solo usa la superficie nativa sx:: (current, memoria de
    // usuario, errores) mas display:: y console::. No accede a los internals
    // de process.cpp: es el patron que la rebanada POSIX va a seguir.
    switch (context->rax) {
        case SXN_SYS_INFO: {
            if (!sx::validate_user_range(context->rdi, sizeof(sxn_native_info), true)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            sxn_native_info info = {};
            info.abi_version = SXN_ABI_VERSION;
            info.subsystem_id = static_cast<uint32_t>(caller->subsystem_id);
            info.pid = caller->pid;
            if (!sx::copy_to_user(context->rdi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SXN_SYS_LOG: {
            char message[256] = {};
            if (!sx::copy_user_string(message, sizeof(message), context->rdi)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            console::printf("native[%u]: %s\n", static_cast<unsigned>(caller->pid), message);
            context->rax = 0;
            return context;
        }
        case SXN_SYS_EXIT:
            sx::exit_current(static_cast<int>(context->rdi));
            return nullptr;
        case SXN_SYS_SPAWN:
        case SXN_SYS_SPAWN_STDIO: {
            // Same register convention as the POSIX spawn: the child comes
            // from create_process_internal, and the EI_OSABI of its image,
            // not its parent's subsystem, decides which subsystem it runs in.
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            auto* arguments = static_cast<SpawnArguments*>(heap::allocate(sizeof(SpawnArguments)));
            if (arguments == nullptr) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_ENOMEM));
                return context;
            }
            if (!copy_spawn_arguments(*caller, *context, path, sizeof(path), *arguments) ||
                !resolve_path_from_root(path, resolved, sizeof(resolved))) {
                heap::free(arguments);
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_E2BIG));
                return context;
            }

            ImageFailure failure = ImageFailure::none;
            Process* child = create_process_internal(resolved, arguments->argc, arguments->argv, caller, -1, -1, -1, failure);
            // SPAWN_STDIO stops at the stdio create_process_internal already
            // gave the child, like the POSIX spawn.
            if (child != nullptr && context->rax == SXN_SYS_SPAWN && !clone_handles_for_spawn(*child, *caller)) {
                failure = ImageFailure::no_handles;
                child = nullptr;
            }
            heap::free(arguments);
            context->rax = static_cast<uint64_t>(child != nullptr
                ? static_cast<int>(child->pid)
                : sx::negative_error(image_failure_errno(failure)));
            return context;
        }
        case SXN_SYS_SPAWN_EX: {
            // The request carries what SPAWN takes in registers plus the
            // handles. Its path and argv go through the same copy as SPAWN's,
            // fed from a context built out of the request.
            sxn_spawn_request request = {};
            if (!sx::validate_user_range(context->rdi, sizeof(request), false) ||
                !sx::copy_from_user(&request, context->rdi, sizeof(request))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }
            if (request.flags != 0 || request.handle_count > SXN_SPAWN_MAX_HANDLES) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }
            for (int index = 0; index < 3; ++index) {
                const int32_t handle = request.std_handles[index];
                if (handle < -1 || (handle >= 0 && !handle_is_open(*caller, handle))) {
                    context->rax = static_cast<uint64_t>(sx::negative_error(SX_EBADF));
                    return context;
                }
            }
            for (uint32_t index = 0; index < request.handle_count; ++index) {
                if (request.handles[index] < 0 || !handle_is_open(*caller, request.handles[index])) {
                    context->rax = static_cast<uint64_t>(sx::negative_error(SX_EBADF));
                    return context;
                }
            }

            SavedContext spawn_context = *context;
            spawn_context.rdi = request.path;
            spawn_context.rsi = request.argv;
            spawn_context.rdx = static_cast<uint64_t>(static_cast<int64_t>(request.argc));
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            auto* arguments = static_cast<SpawnArguments*>(heap::allocate(sizeof(SpawnArguments)));
            if (arguments == nullptr) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_ENOMEM));
                return context;
            }
            if (!copy_spawn_arguments(*caller, spawn_context, path, sizeof(path), *arguments) ||
                !resolve_path_from_root(path, resolved, sizeof(resolved))) {
                heap::free(arguments);
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_E2BIG));
                return context;
            }

            ImageFailure failure = ImageFailure::none;
            Process* child = create_process_internal(resolved, arguments->argc, arguments->argv, caller,
                request.std_handles[0], request.std_handles[1], request.std_handles[2], failure);
            if (child != nullptr && !add_spawn_handles(*child, *caller, request.handles, request.handle_count)) {
                failure = ImageFailure::no_handles;
                child = nullptr;
            }
            heap::free(arguments);
            context->rax = static_cast<uint64_t>(child != nullptr
                ? static_cast<int>(child->pid)
                : sx::negative_error(image_failure_errno(failure)));
            return context;
        }
        case SXN_SYS_WAITPID: {
            const int result = waitpid_child(*self, static_cast<int64_t>(context->rdi), context->rsi);
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        // Sincronizacion y memoria compartida: los mismos nucleos publicados
        // que usa la traduccion POSIX (las banderas SXN_* coinciden en valor
        // con las del SDK; el contrato nativo es dueno de sus nombres).
        case SXN_SYS_EVENT_CREATE:
            context->rax = static_cast<uint64_t>(create_event_handle(*caller, static_cast<uint32_t>(context->rdi)));
            return context;
        case SXN_SYS_EVENT_SET:
            context->rax = static_cast<uint64_t>(set_event_handle(*caller, context->rdi));
            return context;
        case SXN_SYS_EVENT_RESET:
            context->rax = static_cast<uint64_t>(reset_event_handle(*caller, context->rdi));
            return context;
        case SXN_SYS_WAIT_ONE: {
            const int result = wait_on_handle(*self, context->rdi, static_cast<int64_t>(context->rsi));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SXN_SYS_WAIT_MANY: {
            object::Header* handles[kMaxWaitHandles] = {};
            const int checked = native_wait_targets(*caller, context->rdi, context->rsi, handles);
            if (checked < 0) {
                context->rax = static_cast<uint64_t>(checked);
                return context;
            }
            const int result = detail::begin_object_wait(*self, handles, static_cast<size_t>(checked), false,
                static_cast<int64_t>(context->rdx));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SXN_SYS_SECTION_CREATE:
            context->rax = static_cast<uint64_t>(create_section_handle(*caller, context->rdi, static_cast<uint32_t>(context->rsi)));
            return context;
        case SXN_SYS_MAP_VIEW:
            context->rax = static_cast<uint64_t>(map_view_handle(*caller, context->rdi, static_cast<uint32_t>(context->rsi)));
            return context;
        case SXN_SYS_UNMAP_VIEW:
            context->rax = static_cast<uint64_t>(unmap_view_address(*caller, context->rdi));
            return context;
        // Graficos: mismos internals (display::*) y misma sesion exclusiva por
        // pid que los ioctls GPU de posix (kernel/gpu_device.cpp). La sesion se
        // libera sola al morir el proceso (display::release_session_for en el
        // terminate).
        case SXN_SYS_GFX_INFO: {
            if (!sx::validate_user_range(context->rdi, sizeof(sxn_gfx_info), true)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            savanxp_gpu_info gpu_info = {};
            if (!display::get_info(gpu_info)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_ENODEV));
                return context;
            }

            sxn_gfx_info info = {};
            info.width = gpu_info.width;
            info.height = gpu_info.height;
            info.pitch = gpu_info.pitch;
            info.bpp = gpu_info.bpp;
            info.buffer_size = gpu_info.buffer_size;
            if (!sx::copy_to_user(context->rdi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SXN_SYS_GFX_ACQUIRE: {
            context->rax = static_cast<uint64_t>(
                display::acquire_session(caller->pid) ? 0 : sx::negative_error(SX_EBUSY));
            return context;
        }
        case SXN_SYS_GFX_RELEASE: {
            if (!display::owns_session(caller->pid)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EBUSY));
                return context;
            }
            display::release_session_resources();
            display::release_session(caller->pid);
            context->rax = 0;
            return context;
        }
        case SXN_SYS_GFX_PRESENT: {
            if (!display::owns_session(caller->pid)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EBUSY));
                return context;
            }

            sxn_gfx_present region = {};
            if (!sx::validate_user_range(context->rdi, sizeof(region), false) ||
                !sx::copy_from_user(&region, context->rdi, sizeof(region))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            const savanxp_fb_info& fb = display::framebuffer_info();
            if (region.pixels == 0 || region.source_pitch == 0 || region.width == 0 ||
                region.height == 0 || region.x >= fb.width || region.y >= fb.height ||
                region.width > (fb.width - region.x) ||
                region.height > (fb.height - region.y)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            const uint64_t row_bytes = static_cast<uint64_t>(region.width) * sizeof(uint32_t);
            const uint64_t last_row = static_cast<uint64_t>(region.y + region.height - 1);
            const uint64_t touched_bytes = (last_row * region.source_pitch) +
                (static_cast<uint64_t>(region.x) * sizeof(uint32_t)) + row_bytes;
            if (!sx::validate_user_range(region.pixels, touched_bytes, false)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            const bool presented = display::present_region(
                reinterpret_cast<const void*>(region.pixels),
                region.source_pitch,
                region.x,
                region.y,
                region.width,
                region.height
            );
            context->rax = static_cast<uint64_t>(presented ? 0 : sx::negative_error(SX_EIO));
            return context;
        }
        case SXN_SYS_CLOSE:
            context->rax = static_cast<uint64_t>(native_close(*caller, context->rdi));
            return context;
        case SXN_SYS_DUPLICATE:
            context->rax = static_cast<uint64_t>(native_duplicate(*caller, context->rdi));
            return context;
        case SXN_SYS_READ: {
            const int result = native_read(*self, context->rdi, context->rsi, static_cast<size_t>(context->rdx));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SXN_SYS_WRITE: {
            const int result = native_write(*self, context->rdi, context->rsi, static_cast<size_t>(context->rdx));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SXN_SYS_OPEN_DEVICE:
            context->rax = static_cast<uint64_t>(native_open_device(*caller, context->rdi, static_cast<uint32_t>(context->rsi)));
            return context;
        case SXN_SYS_DEVICE_CONTROL:
            context->rax = static_cast<uint64_t>(native_device_control(*caller, context->rdi, context->rsi, context->rdx));
            return context;
        case SXN_SYS_CLOCK:
            context->rax = timer::monotonic_ns();
            return context;
        case SXN_SYS_PORT_CREATE:
            context->rax = static_cast<uint64_t>(native_port_create(*caller, context->rdi));
            return context;
        case SXN_SYS_PORT_CONNECT:
            context->rax = static_cast<uint64_t>(native_port_connect(*caller, context->rdi, context->rsi));
            return context;
        case SXN_SYS_PORT_ACCEPT:
            context->rax = static_cast<uint64_t>(native_port_accept(*caller, context->rdi, context->rsi));
            return context;
        case SXN_SYS_PIPE_SEND_HANDLE:
            context->rax = static_cast<uint64_t>(native_pipe_send_handle(*caller, context->rdi, context->rsi));
            return context;
        case SXN_SYS_PIPE_RECEIVE_HANDLE:
            context->rax = static_cast<uint64_t>(native_pipe_receive_handle(*caller, context->rdi));
            return context;
        case SXN_SYS_TERMINATE:
            context->rax = static_cast<uint64_t>(native_terminate(*caller, context->rdi, context->rsi));
            return context;
        case SXN_SYS_THREAD_CREATE: {
            uint32_t tid = 0;
            const uint64_t tid_out = context->r10;
            if (tid_out != 0 && !sx::validate_user_range(tid_out, sizeof(tid), true)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }
            const int handle = create_thread(*caller, context->rdi, context->rsi, context->rdx, tid);
            if (handle >= 0 && tid_out != 0) {
                (void)sx::copy_to_user(tid_out, &tid, sizeof(tid));
            }
            context->rax = static_cast<uint64_t>(handle);
            return context;
        }
        case SXN_SYS_THREAD_EXIT:
            exit_current_thread(static_cast<int>(context->rdi));
        case SXN_SYS_THREAD_EXIT_CODE: {
            int exit_code = 0;
            if (!sx::validate_user_range(context->rsi, sizeof(int32_t), true)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }
            const int result = thread_exit_code(*caller, context->rdi, exit_code);
            if (result == 0) {
                const int32_t value = exit_code;
                (void)sx::copy_to_user(context->rsi, &value, sizeof(value));
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SXN_SYS_SET_THREAD_BASE:
            context->rax = static_cast<uint64_t>(
                set_thread_base(*self, static_cast<uint32_t>(context->rdi), context->rsi));
            return context;
        case SXN_SYS_PIPE_CREATE:
            context->rax = static_cast<uint64_t>(native_pipe_create(*caller, context->rdi));
            return context;
        case SXN_SYS_SET_NONBLOCKING:
            context->rax = static_cast<uint64_t>(native_set_nonblocking(*caller, context->rdi, context->rsi));
            return context;
        case SXN_SYS_PROCESS_QUERY:
            context->rax = static_cast<uint64_t>(native_process_query(context->rdi, context->rsi));
            return context;
        default:
            break;
    }

    // Standalone native: un proceso nativo solo habla SXN. Cualquier numero
    // fuera del contrato (incluido el rango posix) es ENOSYS, sin delegacion.
    context->rax = static_cast<uint64_t>(sx::negative_error(SX_ENOSYS));
    return context;
}

} // namespace process
