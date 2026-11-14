#include "io.hpp"

#include "fb32.hpp"
#include "kernel/string.hpp"
#include "kernel/tty.hpp"
#include "kernel/vfs.hpp"
#include "paging.hpp"
#include "pit.hpp"
#include "ps2_32.hpp"
#include "serial.hpp"

// Adapted from v0.4.0 kernel/process.cpp: shared IoObject lifetimes, per-process
// handles, first-free descriptors and bounded ring pipes. Only the memory-copy,
// scheduler and device/console boundaries differ in the protected-mode port.
namespace i386::io {
namespace {
using ::process::HandleKind;
constexpr uint32_t kPipeCount = 64, kPipeCapacity = 8192, kChunk = 256;
object::IoObject g_objects[object::kMaxIoObjects];
::process::Pipe g_pipes[kPipeCount];
uint8_t g_pipe_storage[kPipeCount][kPipeCapacity];
object::IoObject* g_gpu;
vfs::Vnode* g_gpu_node;
vfs::Vnode* g_input_node;
tty::TtyDevice g_tty;

object::IoObject* lookup(const process::Process& task, uint32_t fd, uint32_t access = object::access_query) {
    if (fd >= kMaxHandles || (task.handles[fd].granted_access & access) != access) return nullptr;
    return object::as_io(task.handles[fd].object);
}

void destroy(object::Header* header) {
    auto* file = object::as_io(header);
    if (file->kind == HandleKind::pipe) {
        auto& pipe = *file->pipe;
        if (file->open_flags & SAVANXP_OPEN_READ) --pipe.reader_refs;
        if (file->open_flags & SAVANXP_OPEN_WRITE) --pipe.writer_refs;
        if (!pipe.reader_refs && !pipe.writer_refs) memset(&pipe, 0, sizeof(pipe));
    }
    if (file == g_gpu) {
        g_gpu = nullptr;
        fb32::console_set_enabled(true);
    }
    memset(file, 0, sizeof(*file));
}

object::IoObject* allocate(HandleKind kind, uint32_t flags) {
    for (auto& file : g_objects) {
        if (file.in_use) continue;
        memset(&file, 0, sizeof(file));
        file.in_use = true;
        file.header.type = object::Type::io;
        file.header.destroy = destroy;
        file.kind = kind;
        file.open_flags = (flags & (SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE)) |
            ((flags & SAVANXP_OPEN_NONBLOCK) ? ::process::open_nonblock : 0);
        return &file;
    }
    return nullptr;
}

uint32_t access_for(uint32_t flags) {
    return object::access_query | ((flags & SAVANXP_OPEN_READ) ? object::access_read : 0) |
        ((flags & SAVANXP_OPEN_WRITE) ? object::access_write : 0);
}

int install(process::Process& task, object::IoObject* file, int fd = -1) {
    if (fd < 0) {
        for (uint32_t index = 0; index < kMaxHandles; ++index)
            if (!task.handles[index].object) { fd = static_cast<int>(index); break; }
    }
    if (fd < 0 || fd >= static_cast<int>(kMaxHandles)) return -SAVANXP_EBADF;
    object::retain(&file->header);
    object::release(task.handles[fd].object);
    task.handles[fd] = {&file->header, access_for(file->open_flags), object::handle_none};
    return fd;
}

bool valid(process::Process& task, uint32_t buffer, uint32_t count, bool writable) {
    return paging::user_range(task.directory, buffer, count, writable);
}

void complete(process::Process& task, int result) {
    task.frame->eax = static_cast<uint32_t>(result);
    task.state = process::State::runnable;
}

int tty_read(process::Process& task, uint32_t buffer, uint32_t count) {
    if (!g_tty.line_ready) return kBlocked;
    const uint32_t bytes = count < g_tty.pending_length ? count : g_tty.pending_length;
    paging::copy_to_user(task.directory, buffer, g_tty.pending_line, bytes);
    // Keep the reference line discipline: a read consumes the pending line.
    memset(g_tty.pending_line, 0, sizeof(g_tty.pending_line));
    g_tty.pending_length = 0;
    g_tty.line_ready = false;
    return bytes;
}

void feed_tty(const savanxp_input_event& event) {
    if (event.type != SAVANXP_INPUT_EVENT_KEY_DOWN || g_tty.line_ready) return;
    if (event.key == SAVANXP_KEY_BACKSPACE) {
        if (g_tty.pending_length) {
            g_tty.pending_line[--g_tty.pending_length] = 0;
            serial::write_char('\b');
        }
    } else if (event.key == SAVANXP_KEY_ENTER) {
        serial::write_char('\n');
        g_tty.pending_line[g_tty.pending_length] = 0;
        g_tty.line_ready = true;
    } else if ((event.modifiers & SAVANXP_KEY_MOD_CTRL) && (event.ascii == 'l' || event.ascii == 'L')) {
        clear_console();
    } else if (event.ascii && g_tty.pending_length + 1 < sizeof(g_tty.pending_line)) {
        g_tty.pending_line[g_tty.pending_length++] = static_cast<char>(event.ascii);
        serial::write_char(static_cast<char>(event.ascii));
    }
}

int try_read(process::Process& task, object::IoObject& file, uint32_t buffer, uint32_t count) {
    if (file.kind == HandleKind::tty) return tty_read(task, buffer, count);
    if (file.kind == HandleKind::device && file.node == g_input_node) {
        if (count < sizeof(savanxp_input_event)) return -SAVANXP_EINVAL;
        savanxp_input_event event = {};
        if (!ps2_32::pop_event(event)) return kBlocked;
        paging::copy_to_user(task.directory, buffer, &event, sizeof(event));
        return sizeof(event);
    }
    if (file.kind == HandleKind::pipe) {
        auto& pipe = *file.pipe;
        if (!pipe.size) return pipe.writer_refs ? kBlocked : 0;
        uint32_t bytes = count < pipe.size ? count : pipe.size;
        const uint32_t first = bytes < kPipeCapacity - pipe.read_pos ? bytes : kPipeCapacity - pipe.read_pos;
        paging::copy_to_user(task.directory, buffer, pipe.buffer + pipe.read_pos, first);
        if (first < bytes) paging::copy_to_user(task.directory, buffer + first, pipe.buffer, bytes - first);
        pipe.read_pos = (pipe.read_pos + bytes) % kPipeCapacity;
        pipe.size -= bytes;
        return bytes;
    }
    if (file.kind != HandleKind::vnode || file.node->type != vfs::NodeType::file) return -SAVANXP_EBADF;
    uint8_t scratch[kChunk];
    uint32_t total = 0;
    while (total < count) {
        uint32_t step = count - total < kChunk ? count - total : kChunk;
        const uint32_t consumed = vfs::read(*file.node, file.offset, scratch, step);
        if (!consumed) break;
        paging::copy_to_user(task.directory, buffer + total, scratch, consumed);
        total += consumed; file.offset += consumed;
        if (consumed < step) break;
    }
    return total;
}

int pipe_write(process::Process& task, object::IoObject& file, uint32_t buffer, uint32_t count) {
    auto& pipe = *file.pipe;
    if (!pipe.reader_refs) return -SAVANXP_EPIPE;
    if (pipe.size == kPipeCapacity) return kBlocked;
    if ((file.open_flags & ::process::open_nonblock) && count <= kPipeCapacity &&
        count > kPipeCapacity - pipe.size) return kBlocked;
    const uint32_t bytes = count < kPipeCapacity - pipe.size ? count : kPipeCapacity - pipe.size;
    const uint32_t first = bytes < kPipeCapacity - pipe.write_pos ? bytes : kPipeCapacity - pipe.write_pos;
    paging::copy_from_user(task.directory, pipe.buffer + pipe.write_pos, buffer, first);
    if (first < bytes) paging::copy_from_user(task.directory, pipe.buffer, buffer + first, bytes - first);
    pipe.write_pos = (pipe.write_pos + bytes) % kPipeCapacity;
    pipe.size += bytes;
    return bytes;
}

int evaluate_poll(process::Process& task) {
    int ready = 0;
    for (uint32_t index = 0; index < task.poll_count; ++index) {
        auto& entry = task.poll_entries[index];
        entry.revents = 0;
        if (entry.fd < 0) continue;
        auto* file = lookup(task, static_cast<uint32_t>(entry.fd));
        if (!file) { entry.revents = SAVANXP_POLLNVAL; ++ready; continue; }
        bool readable = false, writable = false;
        if (file->kind == HandleKind::pipe) {
            const auto& pipe = *file->pipe;
            if (file->open_flags & SAVANXP_OPEN_READ) {
                readable = pipe.size || !pipe.writer_refs;
                if (!pipe.writer_refs) entry.revents |= SAVANXP_POLLHUP;
            }
            if (file->open_flags & SAVANXP_OPEN_WRITE) {
                writable = pipe.size < kPipeCapacity && pipe.reader_refs;
                if (!pipe.reader_refs) entry.revents |= SAVANXP_POLLERR;
            }
        } else if (file->kind == HandleKind::tty) {
            readable = g_tty.line_ready;
            writable = true;
        } else if (file->kind == HandleKind::device && file->node == g_input_node) {
            readable = ps2_32::has_pending_event();
        } else if (file->kind == HandleKind::vnode) {
            readable = writable = true;
        }
        if (readable && (file->open_flags & SAVANXP_OPEN_READ)) entry.revents |= entry.events & SAVANXP_POLLIN;
        if (writable && (file->open_flags & SAVANXP_OPEN_WRITE)) entry.revents |= entry.events & SAVANXP_POLLOUT;
        if (entry.revents) ++ready;
    }
    return ready;
}

void fill_stat(const vfs::Vnode& node, savanxp_stat& stat) {
    memset(&stat, 0, sizeof(stat));
    if (node.backend == vfs::Backend::device) {
        stat.st_mode = SAVANXP_S_IFCHR | 0666u;
        return;
    }

    if (node.type == vfs::NodeType::directory) {
        stat.st_mode = SAVANXP_S_IFDIR | (node.writable ? 0755u : 0555u);
        return;
    }

    stat.st_mode = SAVANXP_S_IFREG | (node.writable ? 0644u : 0444u);
    stat.st_size = static_cast<uint32_t>(node.size);
}

} // namespace

void initialize() {
    g_input_node = vfs::install_external_file("/dev/input0", vfs::Backend::device, nullptr, 0, true);
    g_gpu_node = vfs::install_external_file("/dev/gpu0", vfs::Backend::device, nullptr, 0, true);
}

bool initialize_handles(process::Process& child, const process::Process* parent, int stdin_fd, int stdout_fd) {
    for (int fd = 0; fd < 3; ++fd) {
        object::IoObject* file = parent ? lookup(*parent, fd == 0 ? stdin_fd : (fd == 1 ? stdout_fd : 2)) :
            allocate(HandleKind::tty, fd == 0 ? SAVANXP_OPEN_READ : SAVANXP_OPEN_WRITE);
        if (!file || install(child, file, fd) < 0) { release_handles(child); return false; }
    }
    return true;
}

void release_handles(process::Process& task) {
    for (auto& entry : task.handles) {
        object::release(entry.object);
        entry.granted_access = entry.flags = 0;
    }
}

int close(process::Process& task, uint32_t fd) {
    if (!lookup(task, fd)) return -SAVANXP_EBADF;
    object::release(task.handles[fd].object);
    task.handles[fd].granted_access = task.handles[fd].flags = 0;
    return 0;
}

int duplicate(process::Process& task, uint32_t oldfd, int newfd) {
    auto* file = lookup(task, oldfd);
    if (!file || newfd < -1 || newfd >= static_cast<int>(kMaxHandles)) return -SAVANXP_EBADF;
    if (newfd == static_cast<int>(oldfd)) return newfd;
    return install(task, file, newfd);
}

int open(process::Process& task, const char* path, uint32_t flags) {
    auto* node = vfs::open(path, flags);
    if (!node) return -vfs::last_error();
    if (node == g_gpu_node) {
        if (!fb32::ready()) return -SAVANXP_ENODEV;
        if (g_gpu) return -SAVANXP_EBUSY;
    }
    if (node->type == vfs::NodeType::directory && (flags & SAVANXP_OPEN_WRITE)) return -SAVANXP_EISDIR;
    if (!(flags & (SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE))) flags |= SAVANXP_OPEN_READ;
    auto* file = allocate(node->backend == vfs::Backend::device ? HandleKind::device : HandleKind::vnode, flags);
    if (!file) return -SAVANXP_ENOMEM;
    file->node = node;
    if (flags & SAVANXP_OPEN_APPEND) file->offset = node->size;
    const int result = install(task, file);
    if (result < 0) { memset(file, 0, sizeof(*file)); return result; }
    if (node == g_gpu_node) { g_gpu = file; serial::write("gpu0: opened by userland\n"); }
    return result;
}

int read(process::Process& task, uint32_t fd, uint32_t buffer, uint32_t count) {
    auto* file = lookup(task, fd, object::access_read);
    if (!file) return -SAVANXP_EBADF;
    if (!count) return 0;
    if (!valid(task, buffer, count, true)) return -SAVANXP_EFAULT;
    const int result = try_read(task, *file, buffer, count);
    if (result != kBlocked) return result;
    if (file->open_flags & ::process::open_nonblock) return -SAVANXP_EAGAIN;
    task.io_fd = fd; task.io_buffer = buffer; task.io_size = count;
    task.state = process::State::reading;
    return kBlocked;
}

int write(process::Process& task, uint32_t fd, uint32_t buffer, uint32_t count) {
    auto* file = lookup(task, fd, object::access_write);
    if (!file) return -SAVANXP_EBADF;
    if (!count) return 0;
    if (!valid(task, buffer, count, false)) return -SAVANXP_EFAULT;
    if (file->kind == HandleKind::pipe) {
        const int result = pipe_write(task, *file, buffer, count);
        if (file->open_flags & ::process::open_nonblock) return result == kBlocked ? -SAVANXP_EAGAIN : result;
        if (result < 0 && result != kBlocked) return result;
        if (result == static_cast<int>(count)) return result;
        task.io_fd = fd; task.io_buffer = buffer; task.io_size = count;
        task.io_progress = result == kBlocked ? 0 : result;
        task.state = process::State::writing;
        return kBlocked;
    }
    if (file->kind != HandleKind::tty &&
        (file->kind != HandleKind::vnode || file->node->type != vfs::NodeType::file)) return -SAVANXP_EBADF;
    uint8_t scratch[kChunk];
    uint32_t total = 0;
    while (total < count) {
        const uint32_t step = count - total < kChunk ? count - total : kChunk;
        paging::copy_from_user(task.directory, scratch, buffer + total, step);
        uint32_t produced = step;
        if (file->kind == HandleKind::tty) {
            for (uint32_t index = 0; index < step; ++index) serial::write_char(scratch[index]);
        } else {
            produced = vfs::write(*file->node, file->offset, scratch, step, false);
            if (!produced) return total ? static_cast<int>(total) : -(vfs::last_error() ? vfs::last_error() : SAVANXP_EIO);
            file->offset += produced;
        }
        total += produced;
        if (produced < step) break;
    }
    return total;
}

int pipe(process::Process& task, uint32_t array) {
    if (!valid(task, array, 8, true)) return -SAVANXP_EFAULT;
    ::process::Pipe* pipe = nullptr;
    for (uint32_t index = 0; index < kPipeCount; ++index) {
        if (g_pipes[index].in_use) continue;
        pipe = &g_pipes[index]; memset(pipe, 0, sizeof(*pipe));
        pipe->in_use = true; pipe->buffer = g_pipe_storage[index]; break;
    }
    if (!pipe) return -SAVANXP_ENOMEM;
    auto* reader = allocate(HandleKind::pipe, SAVANXP_OPEN_READ);
    auto* writer = allocate(HandleKind::pipe, SAVANXP_OPEN_WRITE);
    if (!reader || !writer) {
        if (reader) memset(reader, 0, sizeof(*reader));
        if (writer) memset(writer, 0, sizeof(*writer));
        memset(pipe, 0, sizeof(*pipe)); return -SAVANXP_ENOMEM;
    }
    reader->pipe = writer->pipe = pipe;
    pipe->reader_refs = pipe->writer_refs = 1;
    const int read_fd = install(task, reader);
    if (read_fd < 0) {
        destroy(&reader->header); destroy(&writer->header); return read_fd;
    }
    const int write_fd = install(task, writer);
    if (write_fd < 0) {
        close(task, read_fd); destroy(&writer->header); return write_fd;
    }
    const int values[] = {read_fd, write_fd};
    paging::copy_to_user(task.directory, array, values, sizeof(values));
    return 0;
}

int readdir(process::Process& task, uint32_t fd, uint32_t buffer, uint32_t capacity) {
    auto* file = lookup(task, fd, object::access_read);
    if (!file || file->kind != HandleKind::vnode || file->node->type != vfs::NodeType::directory || !capacity)
        return -SAVANXP_EBADF;
    if (!valid(task, buffer, capacity, true)) return -SAVANXP_EFAULT;
    const auto* child = vfs::child_at(*file->node, file->iterator_index++);
    uint32_t bytes = child ? strlen(child->name) : 0;
    if (bytes >= capacity) bytes = capacity - 1;
    if (bytes) paging::copy_to_user(task.directory, buffer, child->name, bytes);
    const char zero = 0;
    paging::copy_to_user(task.directory, buffer + bytes, &zero, 1);
    return bytes;
}

int seek(process::Process& task, uint32_t fd, int32_t offset, uint32_t whence) {
    auto* file = lookup(task, fd);
    if (!file || file->kind != HandleKind::vnode) return -SAVANXP_EBADF;
    if (file->node->type != vfs::NodeType::file) return -SAVANXP_EISDIR;
    int64_t base = 0;
    if (whence == SAVANXP_SEEK_CUR) base = file->offset;
    else if (whence == SAVANXP_SEEK_END) base = file->node->size;
    else if (whence != SAVANXP_SEEK_SET) return -SAVANXP_EINVAL;
    const int64_t position = base + offset;
    if (position < 0 || position > INT32_MAX) return -SAVANXP_EINVAL;
    file->offset = static_cast<uint32_t>(position);
    return static_cast<int>(position);
}

int fcntl(process::Process& task, uint32_t fd, uint32_t command, uint32_t value) {
    auto* file = lookup(task, fd);
    if (!file) return -SAVANXP_EBADF;
    if (command == SAVANXP_F_GETFL) return (file->open_flags & (SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE)) |
        ((file->open_flags & ::process::open_nonblock) ? SAVANXP_OPEN_NONBLOCK : 0);
    if (command != SAVANXP_F_SETFL) return -SAVANXP_EINVAL;
    file->open_flags = (file->open_flags & ~::process::open_nonblock) |
        ((value & SAVANXP_OPEN_NONBLOCK) ? ::process::open_nonblock : 0);
    return 0;
}

int poll(process::Process& task, uint32_t array, uint32_t count, int32_t timeout) {
    if (count > kMaxHandles) return -SAVANXP_EINVAL;
    if (count && !valid(task, array, count * sizeof(savanxp_pollfd), true)) return -SAVANXP_EFAULT;
    task.poll_buffer = array; task.poll_count = count;
    if (count) paging::copy_from_user(task.directory, task.poll_entries, array, count * sizeof(savanxp_pollfd));
    const int ready = evaluate_poll(task);
    if (ready || timeout == 0) {
        if (count) paging::copy_to_user(task.directory, array, task.poll_entries, count * sizeof(savanxp_pollfd));
        return ready;
    }
    task.wake_ms = timeout < 0 ? 0 : pit::uptime_ms() + static_cast<uint32_t>(timeout);
    task.state = process::State::polling;
    return kBlocked;
}

void service() {
    bool raw_reader = false;
    for (uint32_t index = 0; index < process::kCapacity; ++index) {
        auto& task = *process::at(index);
        // A raw-input client owns keyboard delivery while it is the consumer
        // of stdin; an ancestor waiting for it must not echo/consume its events.
        if (task.state == process::State::unused || task.state == process::State::zombie ||
            task.state == process::State::waiting) continue;
        auto* input = lookup(task, 0);
        if (input && input->node == g_input_node) raw_reader = true;
        if (task.state == process::State::reading && input_descriptor(task, task.io_fd)) raw_reader = true;
        if (task.state == process::State::polling) {
            for (uint32_t poll = 0; poll < task.poll_count; ++poll)
                if (input_descriptor(task, static_cast<uint32_t>(task.poll_entries[poll].fd))) raw_reader = true;
        }
    }
    if (!raw_reader) {
        savanxp_input_event event;
        while (!g_tty.line_ready && ps2_32::pop_event(event)) feed_tty(event);
    }
    for (uint32_t index = 0; index < process::kCapacity; ++index) {
        auto& task = *process::at(index);
        if (task.state == process::State::reading) {
            auto* file = lookup(task, task.io_fd, object::access_read);
            const int result = file ? try_read(task, *file, task.io_buffer, task.io_size) : -SAVANXP_EBADF;
            if (result != kBlocked) complete(task, result);
        } else if (task.state == process::State::writing) {
            auto* file = lookup(task, task.io_fd, object::access_write);
            const int result = file ? pipe_write(task, *file, task.io_buffer + task.io_progress,
                                                task.io_size - task.io_progress) : -SAVANXP_EBADF;
            if (result == kBlocked) continue;
            if (result < 0) { complete(task, result); continue; }
            task.io_progress += result;
            if (task.io_progress == task.io_size) complete(task, task.io_progress);
        } else if (task.state == process::State::polling) {
            const int ready = evaluate_poll(task);
            if (ready || (task.wake_ms && task.wake_ms <= pit::uptime_ms())) {
                if (task.poll_count) paging::copy_to_user(task.directory, task.poll_buffer, task.poll_entries,
                                                        task.poll_count * sizeof(savanxp_pollfd));
                complete(task, ready);
            }
        }
    }
}

int stat(process::Process& task, const char* path, uint32_t buffer) {
    if (!valid(task, buffer, sizeof(savanxp_stat), true)) return -SAVANXP_EFAULT;
    const auto* node = vfs::resolve(path);
    if (!node) return -SAVANXP_ENOENT;
    savanxp_stat info = {}; fill_stat(*node, info);
    paging::copy_to_user(task.directory, buffer, &info, sizeof(info)); return 0;
}

int fstat(process::Process& task, uint32_t fd, uint32_t buffer) {
    auto* file = lookup(task, fd);
    if (!file) return -SAVANXP_EBADF;
    if (!valid(task, buffer, sizeof(savanxp_stat), true)) return -SAVANXP_EFAULT;
    savanxp_stat info = {};
    if (file->node) fill_stat(*file->node, info);
    else info.st_mode = file->kind == HandleKind::pipe ? SAVANXP_S_IFIFO | 0666 : SAVANXP_S_IFCHR | 0666;
    paging::copy_to_user(task.directory, buffer, &info, sizeof(info)); return 0;
}

int mutate(const char* path, uint32_t operation, uint32_t value, const char* new_path) {
    const auto* node = vfs::resolve(path);
    if (operation == SAVANXP_SYS_MKDIR) return vfs::mkdir(path) ? 0 : -vfs::last_error();
    if (!node) return -SAVANXP_ENOENT;
    if (operation == SAVANXP_SYS_UNLINK && node->type == vfs::NodeType::directory) return -SAVANXP_EISDIR;
    if (operation == SAVANXP_SYS_RMDIR && node->type != vfs::NodeType::directory) return -SAVANXP_ENOTDIR;
    if (operation == SAVANXP_SYS_TRUNCATE && node->type != vfs::NodeType::file) return -SAVANXP_EISDIR;
    if (operation == SAVANXP_SYS_UNLINK || operation == SAVANXP_SYS_RMDIR ||
        (operation == SAVANXP_SYS_RENAME && node->type == vfs::NodeType::directory)) {
        for (const auto& file : g_objects)
            if (file.in_use && file.node == node) return -SAVANXP_EBUSY;
    }
    bool success = operation == SAVANXP_SYS_UNLINK ? vfs::unlink(path) :
        operation == SAVANXP_SYS_RMDIR ? vfs::rmdir(path) :
        operation == SAVANXP_SYS_TRUNCATE ? vfs::truncate(path, value) : vfs::rename(path, new_path);
    return success ? 0 : -vfs::last_error();
}

bool input_descriptor(process::Process& task, uint32_t fd, bool writable) {
    const auto* file = lookup(task, fd, writable ? object::access_write : object::access_query);
    return file && file->node == g_input_node;
}

bool gpu_descriptor(process::Process& task, uint32_t fd) {
    const auto* file = lookup(task, fd);
    return file && file == g_gpu;
}
void clear_console() {
    memset(&g_tty, 0, sizeof(g_tty));
    fb32::console_clear();
    serial::write("\x1b[2J\x1b[H");
}
uint32_t handle_count(const process::Process& task) {
    uint32_t count = 0;
    for (const auto& entry : task.handles) if (entry.object) ++count;
    return count;
}
} // namespace i386::io
