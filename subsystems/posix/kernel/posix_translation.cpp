#include "kernel/process.hpp"

#include <stdarg.h>
#include <stdint.h>

#include "arch/cpu.hpp"
#include "kernel/audio_device.hpp"
#include "kernel/block.hpp"
#include "kernel/console.hpp"
#include "kernel/device.hpp"
#include "kernel/display.hpp"
#include "kernel/elf.hpp"
#include "kernel/heap.hpp"
#include "kernel/input.hpp"
#include "kernel/net.hpp"
#include "kernel/panic.hpp"
#include "kernel/pci.hpp"
#include "kernel/pcspeaker.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/process_detail.hpp"
#include "kernel/rtc.hpp"
#include "kernel/smp.hpp"
#include "kernel/string.hpp"
#include "kernel/sx.hpp"
#include "kernel/sxfs.hpp"
#include "kernel/timer.hpp"
#include "kernel/tty.hpp"
#include "kernel/ui.hpp"
// POSIX translation speaking the POSIX surface, like its dispatcher. The
// kernel proper never includes this header.
#include "savanxp/syscall.h"

#include "posix_process_state.hpp"
#include "posix_signals.hpp"

namespace {

// The scheduler's shorthand, same as in kernel/process.cpp: translation code
// says "this core" without repeating the namespace on every line.
smp::Cpu& this_cpu() {
    return smp::this_cpu();
}

// Numero de senal -> bit de POSIX. Antes vivia en el kernel (signal_flag_for),
// junto a un estado que nadie leia; con la rebanada D se vino aqui con el
// vocabulario al que pertenece (posix_signals.hpp).
uint32_t posix_signal_flag_for(int signal_number) {
    switch (signal_number) {
        case SAVANXP_SIGINT:
            return posix_signal_sigint;
        case SAVANXP_SIGKILL:
            return posix_signal_sigkill;
        case SAVANXP_SIGPIPE:
            return posix_signal_sigpipe;
        case SAVANXP_SIGTERM:
            return posix_signal_sigterm;
        case SAVANXP_SIGCHLD:
            return posix_signal_sigchld;
        default:
            return posix_signal_none;
    }
}

uint32_t exported_open_flags(const object::IoObject& file) {
    uint32_t flags = 0;
    if ((file.open_flags & process::open_read) != 0) {
        flags |= SAVANXP_OPEN_READ;
    }
    if ((file.open_flags & process::open_write) != 0) {
        flags |= SAVANXP_OPEN_WRITE;
    }
    if ((file.open_flags & process::open_nonblock) != 0) {
        flags |= SAVANXP_OPEN_NONBLOCK;
    }
    return flags;
}

// The vnode's recorded permissions with its kind's type bits (a device is a
// character device to POSIX, whatever the VFS stores). A volume mounted
// read-only drops the write bits, as before ownership existed.
void fill_stat_for_vnode(const vfs::Vnode& node, savanxp_stat_ex& stat) {
    memset(&stat, 0, sizeof(stat));
    const vfs::Ownership owner = vfs::ownership_of(node);
    uint32_t permissions = owner.mode & vfs::kModePermissions;
    if (node.backend == vfs::Backend::volume && !node.writable) {
        permissions &= ~0222u;
    }
    if (node.backend == vfs::Backend::device) {
        stat.st_mode = SAVANXP_S_IFCHR | permissions;
    } else if (node.type == vfs::NodeType::directory) {
        stat.st_mode = SAVANXP_S_IFDIR | permissions;
    } else {
        stat.st_mode = SAVANXP_S_IFREG | permissions;
        stat.st_size = static_cast<uint32_t>(node.size);
    }
    stat.st_uid = owner.uid;
    stat.st_gid = owner.gid;
    stat.st_mtime = node.mtime;
    stat.st_nlink = 1;
}

void fill_stat_for_io_object(const object::IoObject& file, savanxp_stat_ex& stat) {
    memset(&stat, 0, sizeof(stat));
    stat.st_nlink = 1;
    switch (file.kind) {
        case process::HandleKind::tty:
        case process::HandleKind::device:
            if (file.node != nullptr) {
                fill_stat_for_vnode(*file.node, stat);
            } else {
                stat.st_mode = SAVANXP_S_IFCHR | 0666u;
            }
            return;
        case process::HandleKind::pipe:
            stat.st_mode = SAVANXP_S_IFIFO | 0666u;
            return;
        case process::HandleKind::socket:
            stat.st_mode = SAVANXP_S_IFSOCK | 0666u;
            return;
        case process::HandleKind::vnode:
            if (file.node != nullptr) {
                fill_stat_for_vnode(*file.node, stat);
            }
            return;
        default:
            return;
    }
}

// The 8 bytes STAT and FSTAT have always written.
savanxp_stat short_stat(const savanxp_stat_ex& stat) {
    return {stat.st_mode, stat.st_size};
}


} // namespace

namespace process {

int fcntl_fd(process::Process& proc, uint64_t fd, uint64_t command, uint64_t value) {
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_query);
    if (file == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }

    switch (command) {
        case SAVANXP_F_GETFL:
            return static_cast<int>(exported_open_flags(*file));
        case SAVANXP_F_SETFL:
            file->open_flags &= ~process::open_nonblock;
            if ((value & SAVANXP_OPEN_NONBLOCK) != 0) {
                file->open_flags |= process::open_nonblock;
            }
            return 0;
        default:
            return sx::negative_error(SAVANXP_EINVAL);
    }
}

int poll_fds(process::Thread& self, uint64_t user_fds, size_t count, int timeout_ms) {
    process::Process& proc = *self.process;
    posix::ThreadState* const state = posix::thread_state(self);
    if (state == nullptr) {
        return sx::negative_error(SAVANXP_ENOMEM);
    }
    if (count > posix::POSIX_MAX_POLL_ENTRIES) {
        return sx::negative_error(SAVANXP_EINVAL);
    }
    if (count != 0 && !vm::is_user_range_accessible(proc.address_space, user_fds, count * sizeof(savanxp_pollfd), true)) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    clear_wait_state(self);
    state->poll_user_fds = user_fds;
    state->poll_count = static_cast<uint32_t>(count);
    state->poll_active = 0;
    if (count != 0 &&
        !read_from_process_memory(proc, state->poll_entries, user_fds, count * sizeof(savanxp_pollfd))) {
        state->poll_user_fds = 0;
        state->poll_count = 0;
        return sx::negative_error(SAVANXP_EINVAL);
    }

    // Primera pasada en el contexto del que llama, con interrupciones
    // habilitadas: es la que atiende el caso comun de "ya hay algo" sin pagar un
    // cambio de proceso, y la unica que corre cuando el timeout es 0.
    device::service_background();
    input::poll();
    net::poll();

    const int ready = posix::evaluate_poll_entries(*state, proc);
    if (ready != 0 || timeout_ms == 0) {
        const bool committed = posix::commit_poll_entries(*state, proc);
        state->poll_user_fds = 0;
        state->poll_count = 0;
        if (!committed) {
            return sx::negative_error(SAVANXP_EINVAL);
        }
        return ready;
    }

    // It parks. The kernel only knows a thread is stopped with a deadline:
    // that what it waits for is a poll is POSIX's business (slice C).
    state->poll_active = 1;
    self.wake_deadline_ms = timeout_ms > 0 ? (detail::now_ms() + static_cast<uint64_t>(timeout_ms)) : 0;
    self.state = process::State::blocked_wait;
    return process::kBlockedResult;
}

/*
 * Re-evaluating a parked thread (isolation slice C).
 *
 * This used to live in the kernel: the tick looked for WaitReason::poll and
 * re-evaluated the descriptors. Now the kernel only hands over the parked
 * thread (subsystem::recheck_parked_thread) and POSIX decides whether it was
 * a poll, whether something is ready, or whether its deadline passed.
 *
 * The request lives in a COPY IN THE KERNEL (posix::ThreadState::poll_entries),
 * not behind a pointer to user memory: the per-tick re-evaluation cannot
 * switch CR3 a thousand times a second for every parked thread. User memory
 * is touched once, on completion, to return the revents.
 */

// fork copies the calling thread only: the child is a process with one
// thread, resuming where the caller made the call (THREADS_ROADMAP.md).
process::Process* fork_process(process::Thread& parent_thread, const process::SavedContext& parent_context) {
    process::Process& parent = *parent_thread.process;
    process::Process* child = detail::allocate_process_slot(false);
    if (child == nullptr) {
        return nullptr;
    }

    child->parent_pid = parent.pid;
    // A fork acts as its parent did (USERS_AND_PERMISSIONS.md, "The token").
    child->credentials = parent.credentials;
    memcpy(child->name, parent.name, sizeof(child->name));
    // fork hereda el estado privado, cwd incluido: lo copia el nucleo, que no
    // sabe que hay un cwd dentro.
    process::copy_subsystem_state(*child, parent);

    if (!vm::clone_address_space(parent.address_space, child->address_space)) {
        process::free_subsystem_state(*child);
        detail::reset_process_slot(*child);
        return nullptr;
    }

    process::Thread* const child_thread = detail::allocate_thread(*child);
    if (child_thread == nullptr) {
        detail::release_process_resources(*child);
        detail::reset_process_slot(*child);
        return nullptr;
    }

    // The child inherits the floating-point registers the way it inherits the
    // integer ones. While the parent runs, its FPU lives in the registers and
    // the saved area is stale (switch_to_thread only dumps it on a real
    // switch), so it has to be dumped before it is copied.
    if (this_cpu().current == &parent_thread) {
        arch::fpu_save(parent_thread.fpu_state);
    }
    memcpy(child_thread->fpu_state, parent_thread.fpu_state, sizeof(child_thread->fpu_state));
    // So do the segment bases: the child's thread-local storage is the
    // parent's, at the same addresses of the copied address space.
    process::thread_bases(parent_thread, child_thread->fs_base, child_thread->gs_base);

    if (!detail::inherit_all_handles(*child, parent)) {
        detail::release_process_resources(*child);
        detail::reset_process_slot(*child);
        return nullptr;
    }

    auto* stack_top = reinterpret_cast<uint8_t*>(child_thread->kernel_stack_base + child_thread->kernel_stack_size);
    auto* child_context = reinterpret_cast<process::SavedContext*>(stack_top - sizeof(process::SavedContext));
    memcpy(child_context, &parent_context, sizeof(*child_context));
    child_context->rax = 0;
    child_thread->context = child_context;
    detail::proc_log("proc: fork parent=%u child=%u\n", parent.pid, child->pid);
    return child;
}

int kill_process(process::Process& sender, int pid, int signal_number) {
    const uint32_t flag = posix_signal_flag_for(signal_number);
    if (flag == posix_signal_none) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    if (pid == 0) {
        pid = static_cast<int>(sender.pid);
    }
    if (pid < 0) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    process::Process* target = process::find(static_cast<uint32_t>(pid));
    if (target == nullptr) {
        return sx::negative_error(SAVANXP_ENOENT);
    }
    if (target->idle || target->pid == detail::init_pid()) {
        return sx::negative_error(SAVANXP_EACCES);
    }
    // Another person's process, or the system's, is not the caller's to
    // end (docs/kernel/USERS_AND_PERMISSIONS.md#what-requires-elevation):
    // POSIX's rule, the caller's real or effective uid against the target's
    // real one.
    if (!cred::acts_as_system(sender.credentials) &&
        sender.credentials.uid != target->credentials.uid &&
        sender.credentials.euid != target->credentials.uid) {
        return sx::negative_error(SAVANXP_EPERM);
    }

    // No hay entrega de senales a userland (posix_signals.hpp): SIGCHLD es un
    // no-op que devuelve exito, y el resto termina el proceso con 128+n.
    if (signal_number == SAVANXP_SIGCHLD) {
        return 0;
    }
    if (target == process::current()) {
        process::terminate_current(128 + signal_number);
        return 0;
    }

    detail::terminate_process(*target, 128 + signal_number);
    return 0;
}

int duplicate_fd(process::Process& proc, uint64_t oldfd) {
    object::Header* handle_object = detail::lookup_handle(proc, oldfd, object::access_query);
    if (handle_object == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    process::HandleEntry& source = proc.handles[oldfd];
    return detail::allocate_fd(proc, handle_object, source.granted_access, source.flags);
}

int duplicate_fd_to(process::Process& proc, uint64_t oldfd, uint64_t newfd) {
    object::Header* handle_object = detail::lookup_handle(proc, oldfd, object::access_query);
    if (handle_object == nullptr || newfd >= process::kMaxFileHandles) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    if (oldfd == newfd) {
        return static_cast<int>(newfd);
    }
    process::HandleEntry& source = proc.handles[oldfd];
    return detail::install_handle(proc, static_cast<int>(newfd), handle_object, source.granted_access, source.flags);
}

int close_fd(process::Process& proc, uint64_t fd) {
    if (fd >= process::kMaxFileHandles || proc.handles[fd].object == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    object::release(proc.handles[fd].object);
    proc.handles[fd].granted_access = object::access_none;
    proc.handles[fd].flags = object::handle_none;
    return 0;
}

int open_node(process::Process& proc, const char* path, uint32_t flags, uint16_t create_permissions) {
    vfs::Vnode* node = vfs::open(proc.credentials, path, flags, create_permissions);
    if (node == nullptr) {
        return sx::negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    if (node->type == vfs::NodeType::directory && (flags & SAVANXP_OPEN_WRITE) != 0) {
        return sx::negative_error(SAVANXP_EISDIR);
    }

    object::IoObject* file = detail::allocate_io_object();
    if (file == nullptr) {
        return sx::negative_error(SAVANXP_ENOMEM);
    }

    file->kind = node->backend == vfs::Backend::device ? process::HandleKind::device : process::HandleKind::vnode;
    file->open_flags =
        ((flags & SAVANXP_OPEN_READ) != 0 ? process::open_read : 0) |
        ((flags & SAVANXP_OPEN_WRITE) != 0 ? process::open_write : 0) |
        ((flags & SAVANXP_OPEN_NONBLOCK) != 0 ? process::open_nonblock : 0);
    if ((file->open_flags & (process::open_read | process::open_write)) == 0) {
        file->open_flags = process::open_read;
        if ((flags & SAVANXP_OPEN_NONBLOCK) != 0) {
            file->open_flags |= process::open_nonblock;
        }
    }
    file->node = node;
    file->device = node->backend == vfs::Backend::device ? static_cast<device::Device*>(node->data) : nullptr;
    file->header.destroy = detail::destroy_io_object;
    if ((flags & SAVANXP_OPEN_APPEND) != 0 && node->type == vfs::NodeType::file) {
        file->offset = node->size;
    }
    const int fd = detail::allocate_fd(proc, &file->header, detail::object_access_for_open_flags(file->open_flags), object::handle_none);
    if (fd < 0) {
        detail::discard_io_object(file);
    }
    return fd;
}

int create_socket_fd(process::Process& proc, uint64_t domain, uint64_t type, uint64_t protocol) {
    net::Socket* socket = nullptr;
    const int create_result = net::create_socket(static_cast<uint32_t>(domain), static_cast<uint32_t>(type), static_cast<uint32_t>(protocol), socket);
    if (create_result < 0 || socket == nullptr) {
        return create_result < 0 ? create_result : sx::negative_error(SAVANXP_ENOMEM);
    }

    object::IoObject* file = detail::allocate_io_object();
    if (file == nullptr) {
        net::close_socket(socket);
        return sx::negative_error(SAVANXP_ENOMEM);
    }

    file->kind = process::HandleKind::socket;
    file->open_flags = process::open_read | process::open_write;
    file->socket = socket;
    file->header.destroy = detail::destroy_io_object;
    const int fd = detail::allocate_fd(proc, &file->header, detail::object_access_for_open_flags(file->open_flags), object::handle_none);
    if (fd < 0) {
        net::close_socket(socket);
        detail::discard_io_object(file);
    }
    return fd;
}

namespace {

// A socket call that may sleep in ring 0 (connect, send, UDP receive) keeps a
// pointer to the socket across the sleep. A sibling thread may close the
// handle meanwhile; holding a reference to the handle's object for the length
// of the call keeps the socket alive until the call is done with it.
class HeldObject {
public:
    explicit HeldObject(object::IoObject* file) : header_(file != nullptr ? &file->header : nullptr) {
        if (header_ != nullptr) {
            object::retain(header_);
        }
    }
    ~HeldObject() {
        if (header_ != nullptr) {
            object::release(header_);
        }
    }
    HeldObject(const HeldObject&) = delete;
    HeldObject& operator=(const HeldObject&) = delete;

private:
    object::Header* header_;
};

} // namespace

int bind_fd(process::Process& proc, uint64_t fd, uint64_t user_address) {
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_write);
    if (file == nullptr || file->kind != process::HandleKind::socket || file->socket == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    return net::bind_socket(file->socket, user_address);
}

int setsockopt_fd(process::Process& proc, uint64_t fd, uint64_t option, uint64_t value) {
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_write);
    if (file == nullptr || file->kind != process::HandleKind::socket || file->socket == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    return net::set_socket_option(file->socket, static_cast<uint32_t>(option), value);
}

int connect_fd(process::Process& proc, uint64_t fd, uint64_t user_address, uint32_t timeout_ms) {
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_write);
    if (file == nullptr || file->kind != process::HandleKind::socket || file->socket == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    const HeldObject held(file);
    return net::connect_socket(file->socket, user_address, timeout_ms);
}

int sendto_fd(process::Process& proc, uint64_t fd, uint64_t user_buffer, size_t count, uint64_t user_address) {
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_write);
    if (file == nullptr || file->kind != process::HandleKind::socket || file->socket == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    const HeldObject held(file);
    return net::sendto_socket(file->socket, user_buffer, count, user_address, (file->open_flags & process::open_nonblock) != 0);
}

int recvfrom_fd(process::Process& proc, uint64_t fd, uint64_t user_buffer, size_t count, uint64_t user_address, uint32_t timeout_ms) {
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_read);
    if (file == nullptr || file->kind != process::HandleKind::socket || file->socket == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    const HeldObject held(file);
    return net::recvfrom_socket(
        file->socket,
        user_buffer,
        count,
        user_address,
        timeout_ms,
        (file->open_flags & process::open_nonblock) != 0);
}

int seek_fd(process::Process& proc, uint64_t fd, int64_t offset, uint64_t whence) {
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_query);
    if (file == nullptr || file->kind != process::HandleKind::vnode || file->node == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    if (file->node->type != vfs::NodeType::file) {
        return sx::negative_error(SAVANXP_EISDIR);
    }

    int64_t base = 0;
    switch (whence) {
        case SAVANXP_SEEK_SET:
            break;
        case SAVANXP_SEEK_CUR:
            base = static_cast<int64_t>(file->offset);
            break;
        case SAVANXP_SEEK_END:
            base = static_cast<int64_t>(file->node->size);
            break;
        default:
            return sx::negative_error(SAVANXP_EINVAL);
    }

    const int64_t position = base + offset;
    if (position < 0) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    file->offset = static_cast<size_t>(position);
    return static_cast<int>(file->offset);
}

int unlink_path(process::Process& proc, const char* path) {
    const vfs::Vnode* node = vfs::resolve(path);
    if (node == nullptr) {
        return sx::negative_error(SAVANXP_ENOENT);
    }
    if (node->type == vfs::NodeType::directory) {
        return sx::negative_error(SAVANXP_EISDIR);
    }
    if (detail::vnode_has_open_references(*node)) {
        return sx::negative_error(SAVANXP_EBUSY);
    }
    if (!vfs::unlink(proc.credentials, path)) {
        return sx::negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int mkdir_path(process::Process& proc, const char* path, uint16_t permissions) {
    if (path == nullptr || *path == '\0') {
        return sx::negative_error(SAVANXP_EINVAL);
    }
    if (vfs::mkdir(proc.credentials, path, permissions) == nullptr) {
        return sx::negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int rmdir_path(process::Process& proc, const char* path) {
    const vfs::Vnode* node = vfs::resolve(path);
    if (node == nullptr) {
        return sx::negative_error(SAVANXP_ENOENT);
    }
    if (node->type != vfs::NodeType::directory) {
        return sx::negative_error(SAVANXP_ENOTDIR);
    }
    if (detail::vnode_has_open_references(*node)) {
        return sx::negative_error(SAVANXP_EBUSY);
    }
    if (!vfs::rmdir(proc.credentials, path)) {
        return sx::negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int truncate_path(process::Process& proc, const char* path, uint64_t size) {
    const vfs::Vnode* node = vfs::resolve(path);
    if (node == nullptr) {
        return sx::negative_error(SAVANXP_ENOENT);
    }
    if (node->type != vfs::NodeType::file) {
        return sx::negative_error(SAVANXP_EISDIR);
    }
    if (!vfs::truncate(proc.credentials, path, static_cast<size_t>(size))) {
        return sx::negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int rename_path(process::Process& proc, const char* old_path, const char* new_path) {
    const vfs::Vnode* node = vfs::resolve(old_path);
    if (node == nullptr) {
        return sx::negative_error(SAVANXP_ENOENT);
    }
    if (node->type == vfs::NodeType::directory && detail::vnode_has_open_references(*node)) {
        return sx::negative_error(SAVANXP_EBUSY);
    }
    if (!vfs::rename(proc.credentials, old_path, new_path)) {
        return sx::negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

namespace {

// STAT writes the short struct, STAT_EX the whole one.
int write_stat(process::Process& proc, uint64_t user_stat_address, const savanxp_stat_ex& stat, bool extended) {
    const savanxp_stat short_form = short_stat(stat);
    const void* source = extended ? static_cast<const void*>(&stat) : static_cast<const void*>(&short_form);
    const size_t size = extended ? sizeof(stat) : sizeof(short_form);
    if (!vm::is_user_range_accessible(proc.address_space, user_stat_address, size, true)) {
        return sx::negative_error(SAVANXP_EINVAL);
    }
    return write_to_process_memory(proc, user_stat_address, source, size)
        ? 0
        : sx::negative_error(SAVANXP_EINVAL);
}

} // namespace

int stat_path(process::Process& proc, const char* path, uint64_t user_stat_address, bool extended) {
    const vfs::Vnode* node = vfs::resolve(path);
    if (node == nullptr) {
        return sx::negative_error(SAVANXP_ENOENT);
    }
    // stat needs no permission on the file, only to reach it.
    if (!vfs::may_access(proc.credentials, path, 0)) {
        return sx::negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }

    savanxp_stat_ex stat = {};
    fill_stat_for_vnode(*node, stat);
    return write_stat(proc, user_stat_address, stat, extended);
}

int fstat_fd(process::Process& proc, uint64_t fd, uint64_t user_stat_address, bool extended) {
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_query);
    if (file == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }

    savanxp_stat_ex stat = {};
    fill_stat_for_io_object(*file, stat);
    return write_stat(proc, user_stat_address, stat, extended);
}

int chdir_path(process::Process& proc, const char* path) {
    const vfs::Vnode* node = vfs::resolve(path);
    if (node == nullptr) {
        return sx::negative_error(SAVANXP_ENOENT);
    }
    if (node->type != vfs::NodeType::directory) {
        return sx::negative_error(SAVANXP_ENOTDIR);
    }
    if (!vfs::may_access(proc.credentials, path, vfs::kAccessExecute)) {
        return sx::negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    posix::ProcessState* const owned = posix::state(proc);
    if (owned == nullptr) {
        // Sin estado no hay donde guardar el cwd: el proceso no tiene donde
        // estar, y quedarse en "/" es la respuesta que no inventa.
        return sx::negative_error(SAVANXP_ENOMEM);
    }
    if (strlen(path) >= sizeof(owned->cwd)) {
        return sx::negative_error(SAVANXP_EINVAL);
    }
    strcpy(owned->cwd, path);
    return 0;
}

// Lowering only (USERS_AND_PERMISSIONS.md, "POSIX, as a consequence"). Acting
// as the system, setuid becomes the user for good and seteuid takes any euid;
// otherwise the only move is back to the real uid (dropping elevation).
int set_identity(process::Process& proc, uint32_t field, uint32_t value) {
    cred::Credentials& who = proc.credentials;
    const bool system = cred::acts_as_system(who);
    switch (field) {
        case SAVANXP_IDENTITY_UID:
            if (system) {
                who.uid = value;
                who.euid = value;
                return 0;
            }
            if (value == who.uid) {
                who.euid = value;
                return 0;
            }
            return sx::negative_error(SAVANXP_EPERM);
        case SAVANXP_IDENTITY_EUID:
            if (system || value == who.uid) {
                who.euid = value;
                return 0;
            }
            return sx::negative_error(SAVANXP_EPERM);
        case SAVANXP_IDENTITY_GID:
            if (system || value == who.gid) {
                who.gid = value;
                return 0;
            }
            return sx::negative_error(SAVANXP_EPERM);
        default:
            return sx::negative_error(SAVANXP_EINVAL);
    }
}

int chmod_path(process::Process& proc, const char* path, uint16_t permissions) {
    if (!vfs::chmod(proc.credentials, path, permissions)) {
        return sx::negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int chown_path(process::Process& proc, const char* path, uint32_t uid, uint32_t gid) {
    if (!vfs::chown(proc.credentials, path, uid, gid)) {
        return sx::negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int fchmod_fd(process::Process& proc, uint64_t fd, uint16_t permissions) {
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_query);
    if (file == nullptr || file->node == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    if (!vfs::chmod_node(proc.credentials, *file->node, permissions)) {
        return sx::negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int access_path(process::Process& proc, const char* path, uint32_t want) {
    if ((want & ~(vfs::kAccessRead | vfs::kAccessWrite | vfs::kAccessExecute)) != 0) {
        return sx::negative_error(SAVANXP_EINVAL);
    }
    if (!vfs::may_access(cred::as_real(proc.credentials), path, want)) {
        return sx::negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int getcwd_path(process::Process& proc, uint64_t user_buffer, size_t capacity) {
    const char* const cwd = posix::cwd_or_root(proc);
    const size_t length = strlen(cwd) + 1;
    if (capacity < length || !vm::is_user_range_accessible(proc.address_space, user_buffer, capacity, true)) {
        return sx::negative_error(SAVANXP_EINVAL);
    }
    return write_to_process_memory(proc, user_buffer, cwd, length)
        ? static_cast<int>(length - 1)
        : sx::negative_error(SAVANXP_EINVAL);
}

// Resolucion POSIX: relativa al directorio actual del proceso. La del kernel
// (resolve_path_from_root) es la que se usa cuando no hay cwd que consultar.
bool resolve_process_path(const process::Process& proc, const char* input, char* output, size_t capacity) {
    return vfs::normalize_path(posix::cwd_or_root(proc), input, output, capacity);
}

int readdir_node(process::Process& proc, uint64_t fd, uint64_t user_buffer, size_t capacity) {
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_read);
    if (file == nullptr || file->kind != process::HandleKind::vnode || file->node == nullptr ||
        file->node->type != vfs::NodeType::directory || capacity == 0) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_buffer, capacity, true)) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    const vfs::Vnode* child = vfs::child_at(*file->node, file->iterator_index++);
    if (child == nullptr) {
        char zero = '\0';
        (void)write_to_process_memory(proc, user_buffer, &zero, 1);
        return 0;
    }

    const size_t length = strlen(child->name);
    const size_t to_copy = length < (capacity - 1) ? length : (capacity - 1);
    if (!write_to_process_memory(proc, user_buffer, child->name, to_copy)) {
        return sx::negative_error(SAVANXP_EINVAL);
    }
    char zero = '\0';
    (void)write_to_process_memory(proc, user_buffer + to_copy, &zero, 1);
    return static_cast<int>(to_copy);
}

int ioctl_handle(process::Process& proc, uint64_t fd, uint64_t request, uint64_t argument) {
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_query);
    if (file == nullptr || file->kind != process::HandleKind::device || file->device == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    return device::ioctl(file->device, request, argument, proc.handles[fd].granted_access);
}

int read_handle(process::Thread& self, uint64_t fd, uint64_t user_buffer, size_t count) {
    process::Process& proc = *self.process;
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_read);
    if (file == nullptr || count == 0) {
        return file == nullptr ? sx::negative_error(SAVANXP_EBADF) : 0;
    }
    if ((file->open_flags & process::open_read) == 0) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_buffer, count, true)) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    switch (file->kind) {
        case process::HandleKind::tty:
            if (tty::main().line_ready) {
                const size_t to_copy = tty::main().pending_length < count ? tty::main().pending_length : count;
                if (!write_to_process_memory(proc, user_buffer, tty::main().pending_line, to_copy)) {
                    return sx::negative_error(SAVANXP_EINVAL);
                }
                memset(tty::main().pending_line, 0, sizeof(tty::main().pending_line));
                tty::main().pending_length = 0;
                tty::main().line_ready = false;
                return static_cast<int>(to_copy);
            }
            if ((file->open_flags & process::open_nonblock) != 0) {
                return sx::negative_error(SAVANXP_EAGAIN);
            }
            self.blocked_io_fd = fd;
            self.blocked_read_buffer = user_buffer;
            self.blocked_read_capacity = count;
            self.state = process::State::blocked_read;
            return process::kBlockedResult;
        case process::HandleKind::vnode: {
            if (file->node == nullptr || file->node->type != vfs::NodeType::file) {
                return sx::negative_error(SAVANXP_EBADF);
            }
            uint8_t scratch[detail::kPipeChunkSize] = {};
            size_t total = 0;
            while (total < count) {
                const size_t step = (count - total) < sizeof(scratch) ? (count - total) : sizeof(scratch);
                const size_t consumed = vfs::read(*file->node, file->offset, scratch, step);
                if (consumed == 0) {
                    break;
                }
                if (!write_to_process_memory(proc, user_buffer + total, scratch, consumed)) {
                    return sx::negative_error(SAVANXP_EINVAL);
                }
                file->offset += consumed;
                total += consumed;
                if (consumed < step) {
                    break;
                }
            }
            return static_cast<int>(total);
        }
        case process::HandleKind::pipe: {
            int result = detail::try_pipe_read(proc, *file, user_buffer, count);
            if (result == process::kBlockedResult && (file->open_flags & process::open_nonblock) != 0) {
                return sx::negative_error(SAVANXP_EAGAIN);
            }
            if (result == process::kBlockedResult) {
                self.blocked_io_fd = fd;
                self.blocked_read_buffer = user_buffer;
                self.blocked_read_capacity = count;
                self.state = process::State::blocked_read;
            }
            return result;
        }
        case process::HandleKind::device:
            return device::read(file->device, user_buffer, count);
        case process::HandleKind::socket: {
            // Siempre se intenta sin bloquear y, si no hay nada, se estaciona el
            // proceso: el camino bloqueante de net:: gira con hlt en el contexto
            // del que llama, que es el mismo defecto que tenia poll() -- el
            // proceso nunca sale de running y se le carga cada tick de la
            // espera. Ver docs/userland/SYSTEM_MONITORING.md.
            const int result = net::read_socket(file->socket, user_buffer, count, true);
            if (result != sx::negative_error(SAVANXP_EAGAIN)) {
                return result;
            }
            if ((file->open_flags & process::open_nonblock) != 0) {
                return result;
            }
            const uint32_t timeout_ms = net::socket_recv_timeout_ms(file->socket);
            self.blocked_io_fd = fd;
            self.blocked_read_buffer = user_buffer;
            self.blocked_read_capacity = count;
            // wake_deadline_ms 0 = sin vencimiento, igual que en poll y en la espera
            // por objetos; asi SO_RCVTIMEO en 0 (esperar para siempre, que es
            // lo que manda POSIX) no necesita un caso aparte.
            self.wake_deadline_ms = timeout_ms != 0 ? (detail::now_ms() + static_cast<uint64_t>(timeout_ms)) : 0;
            self.state = process::State::blocked_read;
            return process::kBlockedResult;
        }
        default:
            return sx::negative_error(SAVANXP_EBADF);
    }
}

// Largest piece of a write handed to a file in one vfs::write.
constexpr size_t kFileWriteChunk = 64u * 1024u;

int write_handle(process::Thread& self, uint64_t fd, uint64_t user_buffer, size_t count) {
    process::Process& proc = *self.process;
    object::IoObject* file = detail::fd_to_io_object(proc, fd, object::access_write);
    if (file == nullptr || count == 0) {
        return file == nullptr ? sx::negative_error(SAVANXP_EBADF) : 0;
    }
    if ((file->open_flags & process::open_write) == 0) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_buffer, count, false)) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    switch (file->kind) {
        case process::HandleKind::tty: {
            char scratch[detail::kPipeChunkSize] = {};
            size_t written = 0;
            while (written < count) {
                const size_t step = (count - written) < sizeof(scratch) ? (count - written) : sizeof(scratch);
                if (!read_from_process_memory(proc, scratch, user_buffer + written, step)) {
                    return sx::negative_error(SAVANXP_EINVAL);
                }
                for (size_t index = 0; index < step; ++index) {
                    tty::write_char(scratch[index]);
                }
                written += step;
            }
            return static_cast<int>(count);
        }
        case process::HandleKind::vnode: {
            if (file->node == nullptr || file->node->type != vfs::NodeType::file) {
                return sx::negative_error(SAVANXP_EBADF);
            }

            // The file sees one vfs::write per chunk, and on SxFS every write
            // that grows a file is a metadata commit: with the 256-byte stack
            // scratch, a 3 KB file was 12 commits. The heap buffer makes it
            // one per kFileWriteChunk; the stack scratch is the fallback when
            // the heap cannot spare one.
            uint8_t stack_scratch[detail::kPipeChunkSize] = {};
            const size_t heap_size = count < kFileWriteChunk ? count : kFileWriteChunk;
            uint8_t* heap_scratch = static_cast<uint8_t*>(heap::allocate(heap_size));
            uint8_t* scratch = heap_scratch != nullptr ? heap_scratch : stack_scratch;
            const size_t scratch_size = heap_scratch != nullptr ? heap_size : sizeof(stack_scratch);
            size_t written = 0;
            int result = 0;
            while (written < count) {
                const size_t step = (count - written) < scratch_size ? (count - written) : scratch_size;
                if (!read_from_process_memory(proc, scratch, user_buffer + written, step)) {
                    result = sx::negative_error(SAVANXP_EINVAL);
                    break;
                }
                const size_t produced = vfs::write(*file->node, file->offset, scratch, step, false);
                if (produced == 0 && step != 0) {
                    const int error = vfs::last_error();
                    result = written != 0
                        ? static_cast<int>(written)
                        : sx::negative_error(static_cast<savanxp_error_code>(error != 0 ? error : SAVANXP_EIO));
                    break;
                }
                file->offset += produced;
                written += produced;
                result = static_cast<int>(written);
                if (produced < step) {
                    break;
                }
            }
            heap::free(heap_scratch);
            return result;
        }
        case process::HandleKind::pipe: {
            const bool nonblocking = (file->open_flags & process::open_nonblock) != 0;
            /* No-bloqueante y el registro no entra entero: EAGAIN sin escribir
             * nada. Escribir la parte que cabe desincronizaria al lector, que
             * lee registros de tamano fijo, asi que para lo que entra en el pipe
             * la escritura es atomica (semantica POSIX hasta PIPE_BUF). */
            if (nonblocking && file->pipe != nullptr && count <= detail::kPipeCapacity &&
                file->pipe->reader_refs != 0 &&
                (detail::kPipeCapacity - file->pipe->size) < count) {
                return sx::negative_error(SAVANXP_EAGAIN);
            }
            const int step = detail::pipe_write_chunk(proc, *file->pipe, user_buffer, count, 0);
            if (step == process::kBlockedResult && nonblocking) {
                return sx::negative_error(SAVANXP_EAGAIN);
            }
            if (step == process::kBlockedResult) {
                self.blocked_io_fd = fd;
                self.blocked_write_buffer = user_buffer;
                self.blocked_write_length = count;
                self.blocked_write_progress = 0;
                self.state = process::State::blocked_write;
                return process::kBlockedResult;
            }
            if (step < 0) {
                return step;
            }

            if (static_cast<size_t>(step) == count) {
                return step;
            }
            /* Un fd no-bloqueante nunca debe quedar en blocked_write: se informa
             * lo escrito y el llamador decide. (Con el chequeo atomico de arriba
             * esto solo alcanza a writes mas grandes que el pipe entero.) */
            if (nonblocking) {
                return step;
            }

            self.blocked_io_fd = fd;
            self.blocked_write_buffer = user_buffer;
            self.blocked_write_length = count;
            self.blocked_write_progress = static_cast<uint64_t>(step);
            self.state = process::State::blocked_write;
            return process::kBlockedResult;
        }
        case process::HandleKind::device:
            return device::write(file->device, user_buffer, count);
        case process::HandleKind::socket: {
            const HeldObject held(file);
            return net::write_socket(file->socket, user_buffer, count, (file->open_flags & process::open_nonblock) != 0);
        }
        default:
            return sx::negative_error(SAVANXP_EBADF);
    }
}

int create_pipe(process::Process& proc, uint64_t user_fd_array) {
    if (!vm::is_user_range_accessible(proc.address_space, user_fd_array, sizeof(int) * 2, true)) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    process::Pipe* pipe = detail::allocate_pipe();
    if (pipe == nullptr) {
        return sx::negative_error(SAVANXP_ENOMEM);
    }

    object::IoObject* reader = detail::allocate_io_object();
    object::IoObject* writer = detail::allocate_io_object();
    if (reader == nullptr || writer == nullptr) {
        detail::discard_io_object(reader);
        detail::discard_io_object(writer);
        return sx::negative_error(SAVANXP_ENOMEM);
    }

    reader->kind = process::HandleKind::pipe;
    reader->open_flags = process::open_read;
    reader->pipe = pipe;
    writer->kind = process::HandleKind::pipe;
    writer->open_flags = process::open_write;
    writer->pipe = pipe;
    reader->header.destroy = detail::destroy_io_object;
    writer->header.destroy = detail::destroy_io_object;
    pipe->reader_refs = 1;
    pipe->writer_refs = 1;

    const int read_fd = detail::allocate_fd(proc, &reader->header, detail::object_access_for_open_flags(reader->open_flags), object::handle_none);
    if (read_fd < 0) {
        detail::discard_io_object(reader);
        detail::discard_io_object(writer);
        memset(pipe, 0, sizeof(*pipe));
        return read_fd;
    }
    reader = nullptr;
    const int write_fd = detail::allocate_fd(proc, &writer->header, detail::object_access_for_open_flags(writer->open_flags), object::handle_none);
    if (write_fd < 0) {
        (void)close_fd(proc, read_fd);
        detail::discard_io_object(writer);
        return write_fd;
    }
    writer = nullptr;

    const int values[2] = {read_fd, write_fd};
    if (!write_to_process_memory(proc, user_fd_array, values, sizeof(values))) {
        (void)close_fd(proc, read_fd);
        (void)close_fd(proc, write_fd);
        return sx::negative_error(SAVANXP_EINVAL);
    }
    return 0;
}

/*
 * Paso de handles por pipe, al estilo de I_SENDFD de System V: quien tiene el
 * extremo de ESCRITURA le deja un handle a quien tenga el de LECTURA. El permiso
 * es tener el canal -- no hace falta un handle al proceso destino ni una regla de
 * quien puede meterle objetos a quien.
 *
 * Solo viajan objetos que no son IoObjects (secciones, eventos, semaforos,
 * timers): un IoObject compartido arrastraria su offset y, si fuera otro pipe, la
 * cuenta de lectores y escritores. El receptor recibe exactamente el acceso que
 * tenia el emisor, nunca mas.
 */
int pipe_send_handle(process::Process& proc, uint64_t pipe_fd, uint64_t handle_fd) {
    object::IoObject* file = detail::fd_to_io_object(proc, pipe_fd, object::access_write);
    if (file == nullptr || file->kind != process::HandleKind::pipe || file->pipe == nullptr ||
        (file->open_flags & process::open_write) == 0) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    object::Header* handle_object = detail::lookup_handle(proc, handle_fd, object::access_none);
    if (handle_object == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    if (handle_object->type == object::Type::io) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    process::Pipe& pipe = *file->pipe;
    if (pipe.reader_refs == 0) {
        return sx::negative_error(SAVANXP_EPIPE);
    }
    if (pipe.handle_count >= process::kPipeHandleQueueCapacity) {
        return sx::negative_error(SAVANXP_EAGAIN);
    }

    const uint32_t tail = (pipe.handle_head + pipe.handle_count) % process::kPipeHandleQueueCapacity;
    object::retain(handle_object);
    pipe.handles[tail] = handle_object;
    pipe.handle_access[tail] = proc.handles[handle_fd].granted_access;
    pipe.handle_count += 1u;
    return 0;
}

// Nunca bloquea: el protocolo que usa el pipe anuncia el handle con sus propios
// registros, asi que una cola vacia es un error del que llama, no una espera.
int pipe_receive_handle(process::Process& proc, uint64_t pipe_fd) {
    object::IoObject* file = detail::fd_to_io_object(proc, pipe_fd, object::access_read);
    if (file == nullptr || file->kind != process::HandleKind::pipe || file->pipe == nullptr ||
        (file->open_flags & process::open_read) == 0) {
        return sx::negative_error(SAVANXP_EBADF);
    }

    process::Pipe& pipe = *file->pipe;
    if (pipe.handle_count == 0) {
        return sx::negative_error(SAVANXP_EAGAIN);
    }

    object::Header* handle_object = pipe.handles[pipe.handle_head];
    const int fd = detail::allocate_fd(proc, handle_object, pipe.handle_access[pipe.handle_head], object::handle_none);
    if (fd < 0) {
        // Sin lugar en la tabla el handle se queda en la cola: el receptor puede
        // cerrar algo y reintentar.
        return fd;
    }
    // allocate_fd tomo su propia referencia; la de la cola se suelta.
    object::release(pipe.handles[pipe.handle_head]);
    pipe.handle_access[pipe.handle_head] = object::access_none;
    pipe.handle_head = (pipe.handle_head + 1u) % process::kPipeHandleQueueCapacity;
    pipe.handle_count -= 1u;
    return fd;
}

object::TimerObject* ensure_sleep_timer(process::Thread& thread) {
    if (thread.sleep_timer != nullptr) {
        return thread.sleep_timer;
    }

    object::TimerObject* timer_object = object::create_timer(false);
    if (timer_object == nullptr) {
        return nullptr;
    }
    object::retain(&timer_object->header);
    thread.sleep_timer = timer_object;
    return timer_object;
}

int create_timer_handle(process::Process& proc, uint32_t flags) {
    object::TimerObject* timer_object = sx::create_timer((flags & SAVANXP_TIMER_MANUAL_RESET) != 0);
    if (timer_object == nullptr) {
        return sx::negative_error(SAVANXP_ENOMEM);
    }

    const uint32_t access = object::access_query | object::access_modify | object::access_synchronize;
    const int handle = detail::allocate_fd(proc, &timer_object->header, access, object::handle_none);
    if (handle < 0) {
        object::Header* header = &timer_object->header;
        object::release(header);
        return handle;
    }
    return handle;
}

int set_timer_handle(process::Process& proc, uint64_t fd, uint64_t due_ms, uint64_t period_ms) {
    object::Header* handle_object = detail::lookup_handle(proc, fd, object::access_modify);
    object::TimerObject* timer_object = object::as_timer(handle_object);
    if (timer_object == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }

    const uint64_t now = detail::now_ms();
    sx::set_timer(timer_object, now + due_ms, period_ms);
    if (due_ms == 0) {
        sx::poll_timers(now, detail::wake_waiters_for_object);
    }
    return 0;
}

int cancel_timer_handle(process::Process& proc, uint64_t fd) {
    object::Header* handle_object = detail::lookup_handle(proc, fd, object::access_modify);
    object::TimerObject* timer_object = object::as_timer(handle_object);
    if (timer_object == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }

    sx::cancel_timer(timer_object);
    return 0;
}

int create_semaphore_handle(process::Process& proc, int32_t initial_count, int32_t max_count) {
    if (max_count <= 0 || initial_count < 0 || initial_count > max_count) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    object::SemaphoreObject* semaphore_object = sx::create_semaphore(initial_count, max_count);
    if (semaphore_object == nullptr) {
        return sx::negative_error(SAVANXP_ENOMEM);
    }

    const uint32_t access = object::access_query | object::access_modify | object::access_synchronize;
    const int handle = detail::allocate_fd(proc, &semaphore_object->header, access, object::handle_none);
    if (handle < 0) {
        object::Header* header = &semaphore_object->header;
        object::release(header);
        return handle;
    }
    return handle;
}

int release_semaphore_handle(process::Process& proc, uint64_t fd, uint32_t release_count) {
    object::Header* handle_object = detail::lookup_handle(proc, fd, object::access_modify);
    object::SemaphoreObject* semaphore_object = object::as_semaphore(handle_object);
    if (semaphore_object == nullptr) {
        return sx::negative_error(SAVANXP_EBADF);
    }
    if (release_count == 0 || release_count > 0x7fffffffu) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    int32_t previous_count = 0;
    if (!sx::release_semaphore(semaphore_object, static_cast<int32_t>(release_count), &previous_count)) {
        return sx::negative_error(SAVANXP_EINVAL);
    }
    detail::wake_waiters_for_object(handle_object);
    return previous_count;
}

int wait_on_many_handles(process::Thread& self, uint64_t user_handles, size_t count, bool wait_all, int64_t timeout_ms) {
    process::Process& proc = *self.process;
    if (count == 0 || count > process::kMaxWaitHandles) {
        return sx::negative_error(SAVANXP_EINVAL);
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_handles, count * sizeof(int32_t), false)) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    int32_t raw_handles[process::kMaxWaitHandles] = {};
    if (!read_from_process_memory(proc, raw_handles, user_handles, count * sizeof(int32_t))) {
        return sx::negative_error(SAVANXP_EINVAL);
    }

    object::Header* handles[process::kMaxWaitHandles] = {};
    for (size_t index = 0; index < count; ++index) {
        if (raw_handles[index] < 0) {
            return sx::negative_error(SAVANXP_EBADF);
        }
        handles[index] = detail::lookup_handle(proc, static_cast<uint64_t>(raw_handles[index]), object::access_synchronize);
        if (handles[index] == nullptr) {
            return sx::negative_error(SAVANXP_EBADF);
        }
        if (!sx::is_wait_target(handles[index])) {
            return sx::negative_error(SAVANXP_EINVAL);
        }
    }

    return detail::begin_object_wait(self, handles, count, wait_all, timeout_ms);
}

int sleep_via_timer(process::Thread& self, uint64_t milliseconds) {
    object::TimerObject* timer_object = ensure_sleep_timer(self);
    if (timer_object == nullptr) {
        return sx::negative_error(SAVANXP_ENOMEM);
    }

    const uint64_t now = detail::now_ms();
    object::set_timer(timer_object, now + milliseconds, 0);
    object::Header* handle_object = &timer_object->header;
    if (milliseconds == 0) {
        object::poll_timers(now, detail::wake_waiters_for_object);
    }
    return detail::begin_object_wait(self, &handle_object, 1, false, -1);
}

} // namespace process

// --- poll() (rebanada C de aislamiento) --------------------------------------
//
// El pedido de poll es estado de POSIX y la reevaluacion tambien: el kernel
// solo entrega cualquier proceso estacionado (subsystem::recheck_parked_process)
// y aqui se decide si era un poll y si ya hay algo o se vencio el plazo.

namespace posix {

// Devuelve los revents al arreglo del proceso. Es el unico momento en que el
// camino de poll toca memoria de usuario de un proceso que no esta corriendo.
bool commit_poll_entries(ThreadState& state, process::Process& proc) {
    if (state.poll_count == 0) {
        return true;
    }
    return write_to_process_memory(
        proc, state.poll_user_fds, state.poll_entries, state.poll_count * sizeof(savanxp_pollfd)) != 0;
}

// Recorre la copia en kernel del pedido y deja los revents ahi mismo. No toca
// memoria de usuario ni el estado del proceso: sirve igual para el que esta
// llamando a poll que para uno ya parado, que es de donde lo llama el tick.
int evaluate_poll_entries(ThreadState& state, process::Process& proc) {
    int ready = 0;

    for (size_t index = 0; index < state.poll_count; ++index) {
        savanxp_pollfd& entry = state.poll_entries[index];

        entry.revents = 0;
        if (entry.fd < 0) {
            continue;
        }

        object::IoObject* file = process::detail::fd_to_io_object(proc, static_cast<uint64_t>(entry.fd), object::access_synchronize);
        if (file != nullptr) {
            entry.revents = process::detail::poll_open_file(file, entry.events);
        } else if ((entry.events & SAVANXP_POLLIN) != 0) {
            // Waitable kernel objects (events, timers) are not IoObjects but
            // can still be polled: report POLLIN when the object is signaled.
            object::Header* waitable =
                process::detail::lookup_handle(proc, static_cast<uint64_t>(entry.fd), object::access_synchronize);
            if (waitable != nullptr && object::can_satisfy_wait(waitable)) {
                entry.revents = static_cast<short>(SAVANXP_POLLIN);
            }
        }
        if (entry.revents != 0) {
            ++ready;
        }
    }
    return ready;
}

// The hook the kernel calls on the tick and on every wakeup by event.
// `honour_deadline` tells the two entries apart: the tick also has to expire
// deadlines, an event only looks for data.
void recheck_parked_poll(process::Thread& thread, uint64_t now_ms, bool honour_deadline) {
    process::Process& proc = *thread.process;
    ThreadState* const owned = posix::existing_thread_state(thread);
    if (owned == nullptr || owned->poll_active == 0) {
        return;
    }

    const int ready = posix::evaluate_poll_entries(*owned, proc);
    const bool expired = honour_deadline && thread.wake_deadline_ms != 0 && thread.wake_deadline_ms <= now_ms;
    if (ready == 0 && !expired) {
        return;
    }

    // El commit puede fallar solo si el proceso dejo de tener mapeado su propio
    // arreglo, que estando parado no puede pasar; si pasara, se lo despierta con
    // el error en vez de dejarlo esperando para siempre.
    const bool committed = posix::commit_poll_entries(*owned, proc);
    owned->poll_user_fds = 0;
    owned->poll_count = 0;
    owned->poll_active = 0;
    process::detail::complete_blocked_wait(thread, committed ? ready : sx::negative_error(SAVANXP_EINVAL));
}

} // namespace posix

