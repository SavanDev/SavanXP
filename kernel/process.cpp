#include "kernel/process.hpp"

#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/elf.hpp"
#include "kernel/panic.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/tty.hpp"
#include "shared/syscall.h"

namespace {

constexpr uint16_t kUserDataSelector = 0x1b;
constexpr uint16_t kUserCodeSelector = 0x23;
constexpr uint64_t kKernelStackPages = 4;
constexpr uint64_t kIdleCodeAddress = 0x0000000000800000ULL;
constexpr uint32_t kDefaultTimeSlice = 4;
constexpr size_t kMaxPipeCount = 16;
constexpr size_t kPipeCapacity = 4096;
constexpr uint8_t kIdleCode[] = {
    0xb8, static_cast<uint8_t>(SAVANXP_SYS_YIELD), 0x00, 0x00, 0x00,
    0xcd, 0x80,
    0xeb, 0xf7,
};

struct Pipe {
    bool in_use;
    uint32_t reader_count;
    uint32_t writer_count;
    size_t size;
    uint8_t buffer[kPipeCapacity];
};

process::Process g_processes[process::kMaxProcesses] = {};
process::Process* g_current = nullptr;
process::Process* g_idle = nullptr;
Pipe g_pipes[kMaxPipeCount] = {};
uint32_t g_next_pid = 1;
size_t g_schedule_cursor = 0;
bool g_ready = false;

extern "C" process::SavedContext* savanxp_handle_syscall(process::SavedContext* context);

uint64_t read_cr3() {
    uint64_t value = 0;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

void write_cr3(uint64_t value) {
    asm volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

void switch_to_process(process::Process* target) {
    if (target == nullptr) {
        return;
    }

    g_current = target;
    target->state = process::State::running;
    arch::x86_64::set_kernel_stack(target->kernel_stack_base + target->kernel_stack_size);
    write_cr3(target->address_space.pml4_physical);
}

size_t process_index(const process::Process* process) {
    return process != nullptr ? static_cast<size_t>(process - &g_processes[0]) : 0;
}

void reset_time_slice(process::Process& process) {
    process.time_slice = process.idle ? 1 : kDefaultTimeSlice;
}

process::Process* allocate_process_slot(bool idle) {
    for (process::Process& slot : g_processes) {
        if (slot.state == process::State::unused) {
            memset(&slot, 0, sizeof(slot));
            slot.pid = g_next_pid++;
            slot.state = process::State::runnable;
            slot.idle = idle;
            reset_time_slice(slot);
            return &slot;
        }
    }
    return nullptr;
}

void initialize_standard_handles(process::Process& proc) {
    for (size_t fd = 0; fd < process::kMaxFileHandles; ++fd) {
        memset(&proc.handles[fd], 0, sizeof(proc.handles[fd]));
    }

    for (size_t fd = 0; fd < 3; ++fd) {
        proc.handles[fd].in_use = true;
        proc.handles[fd].kind = process::HandleKind::tty;
    }
}

process::SavedContext* fabricate_initial_context(
    process::Process& proc,
    uint64_t entry_point,
    uint64_t stack_pointer,
    int argc,
    uint64_t argv_pointer
) {
    auto* stack_top = reinterpret_cast<uint8_t*>(proc.kernel_stack_base + proc.kernel_stack_size);
    auto* context = reinterpret_cast<process::SavedContext*>(stack_top - sizeof(process::SavedContext));
    memset(context, 0, sizeof(*context));
    context->rip = entry_point;
    context->cs = kUserCodeSelector;
    context->rflags = 0x202;
    context->rsp = stack_pointer;
    context->ss = kUserDataSelector;
    context->rdi = static_cast<uint64_t>(argc);
    context->rsi = argv_pointer;
    proc.context = context;
    return context;
}

bool copy_user_string(const char* user_text, char* kernel_buffer, size_t capacity) {
    if (user_text == nullptr || kernel_buffer == nullptr || capacity == 0) {
        return false;
    }

    size_t index = 0;
    while (index + 1 < capacity) {
        kernel_buffer[index] = user_text[index];
        if (kernel_buffer[index] == '\0') {
            return true;
        }
        ++index;
    }

    kernel_buffer[capacity - 1] = '\0';
    return false;
}

void write_to_process_memory(process::Process& target, uint64_t user_address, const void* source, size_t count) {
    if (user_address == 0 || source == nullptr || count == 0) {
        return;
    }

    const uint64_t previous_cr3 = read_cr3();
    write_cr3(target.address_space.pml4_physical);
    memcpy(reinterpret_cast<void*>(user_address), source, count);
    write_cr3(previous_cr3);
}

void write_int_to_process_memory(process::Process& target, uint64_t user_address, int value) {
    write_to_process_memory(target, user_address, &value, sizeof(value));
}

process::Process* child_of(uint32_t parent_pid, uint32_t child_pid) {
    for (process::Process& proc : g_processes) {
        if (proc.state == process::State::unused) {
            continue;
        }
        if (proc.pid == child_pid && proc.parent_pid == parent_pid) {
            return &proc;
        }
    }
    return nullptr;
}

process::Process* find_waiting_parent_for(uint32_t child_pid) {
    for (process::Process& proc : g_processes) {
        if (proc.state == process::State::blocked_wait && proc.waiting_for_pid == child_pid) {
            return &proc;
        }
    }
    return nullptr;
}

bool has_runnable_non_idle() {
    for (const process::Process& proc : g_processes) {
        if (!proc.idle && proc.state == process::State::runnable) {
            return true;
        }
    }
    return false;
}

process::Process* pick_next_runnable() {
    for (size_t offset = 1; offset <= process::kMaxProcesses; ++offset) {
        const size_t index = (g_schedule_cursor + offset) % process::kMaxProcesses;
        process::Process& proc = g_processes[index];
        if (proc.state == process::State::runnable && !proc.idle) {
            g_schedule_cursor = index;
            return &proc;
        }
    }

    if (g_idle != nullptr &&
        (g_idle->state == process::State::runnable || g_idle->state == process::State::running)) {
        g_schedule_cursor = process_index(g_idle);
        return g_idle;
    }

    return nullptr;
}

process::SavedContext* choose_next_context(process::SavedContext* current_context) {
    if (g_current != nullptr && current_context != nullptr) {
        g_current->context = current_context;
    }

    process::Process* next = pick_next_runnable();
    if (next == nullptr) {
        panic("scheduler: no runnable process");
    }

    reset_time_slice(*next);
    switch_to_process(next);
    return next->context;
}

Pipe* allocate_pipe() {
    for (Pipe& pipe : g_pipes) {
        if (!pipe.in_use) {
            memset(&pipe, 0, sizeof(pipe));
            pipe.in_use = true;
            return &pipe;
        }
    }
    return nullptr;
}

void free_pipe_if_unused(Pipe* pipe) {
    if (pipe == nullptr || !pipe->in_use) {
        return;
    }

    if (pipe->reader_count == 0 && pipe->writer_count == 0) {
        memset(pipe, 0, sizeof(*pipe));
    }
}

void reset_handle(process::FileHandle& handle) {
    memset(&handle, 0, sizeof(handle));
}

int allocate_fd(process::Process& proc) {
    for (size_t fd = 0; fd < process::kMaxFileHandles; ++fd) {
        if (!proc.handles[fd].in_use) {
            reset_handle(proc.handles[fd]);
            proc.handles[fd].in_use = true;
            return static_cast<int>(fd);
        }
    }
    return -1;
}

int copy_pending_tty_line(process::Process& proc, uint64_t buffer_address, size_t capacity) {
    tty::TtyDevice& device = tty::main();
    if (!device.line_ready || buffer_address == 0 || capacity == 0) {
        return -1;
    }

    const size_t to_copy = device.pending_length < capacity ? device.pending_length : capacity;
    write_to_process_memory(proc, buffer_address, device.pending_line, to_copy);

    memset(device.pending_line, 0, sizeof(device.pending_line));
    device.pending_length = 0;
    device.line_ready = false;
    return static_cast<int>(to_copy);
}

int read_from_pipe(process::Process& proc, process::FileHandle& handle, uint64_t buffer_address, size_t capacity) {
    if (handle.kind != process::HandleKind::pipe_read || handle.pipe == nullptr) {
        return -1;
    }

    Pipe& pipe = *static_cast<Pipe*>(handle.pipe);
    if (pipe.size == 0) {
        return pipe.writer_count == 0 ? 0 : -2;
    }

    const size_t to_copy = pipe.size < capacity ? pipe.size : capacity;
    if (&proc == g_current) {
        memcpy(reinterpret_cast<void*>(buffer_address), pipe.buffer, to_copy);
    } else {
        write_to_process_memory(proc, buffer_address, pipe.buffer, to_copy);
    }

    const size_t remaining = pipe.size - to_copy;
    if (remaining != 0) {
        memmove(pipe.buffer, pipe.buffer + to_copy, remaining);
    }
    pipe.size = remaining;
    return static_cast<int>(to_copy);
}

int satisfy_blocked_read(process::Process& proc) {
    if (proc.blocked_read_fd >= process::kMaxFileHandles || !proc.handles[proc.blocked_read_fd].in_use) {
        return -1;
    }

    process::FileHandle& handle = proc.handles[proc.blocked_read_fd];
    switch (handle.kind) {
        case process::HandleKind::tty:
            if (!tty::main().line_ready) {
                return -2;
            }
            return copy_pending_tty_line(proc, proc.blocked_read_buffer, static_cast<size_t>(proc.blocked_read_capacity));
        case process::HandleKind::pipe_read:
            return read_from_pipe(proc, handle, proc.blocked_read_buffer, static_cast<size_t>(proc.blocked_read_capacity));
        default:
            return -1;
    }
}

void wake_blocked_readers_for_pipe(Pipe& pipe) {
    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::blocked_read) {
            continue;
        }
        if (proc.blocked_read_fd >= process::kMaxFileHandles) {
            continue;
        }

        process::FileHandle& handle = proc.handles[proc.blocked_read_fd];
        if (!handle.in_use || handle.kind != process::HandleKind::pipe_read || handle.pipe != &pipe) {
            continue;
        }

        const int result = satisfy_blocked_read(proc);
        if (result == -2) {
            continue;
        }

        proc.blocked_read_buffer = 0;
        proc.blocked_read_capacity = 0;
        proc.blocked_read_fd = 0;
        proc.context->rax = result >= 0 ? static_cast<uint64_t>(result) : static_cast<uint64_t>(-1);
        proc.state = process::State::runnable;
        reset_time_slice(proc);
    }
}

void release_handle(process::FileHandle& handle) {
    if (!handle.in_use) {
        return;
    }

    if (handle.kind == process::HandleKind::pipe_read || handle.kind == process::HandleKind::pipe_write) {
        Pipe* pipe = static_cast<Pipe*>(handle.pipe);
        if (pipe != nullptr && pipe->in_use) {
            if (handle.kind == process::HandleKind::pipe_read) {
                if (pipe->reader_count != 0) {
                    pipe->reader_count -= 1;
                }
            } else {
                if (pipe->writer_count != 0) {
                    pipe->writer_count -= 1;
                }
                wake_blocked_readers_for_pipe(*pipe);
            }
            free_pipe_if_unused(pipe);
        }
    }

    reset_handle(handle);
}

void release_all_handles(process::Process& proc) {
    for (process::FileHandle& handle : proc.handles) {
        release_handle(handle);
    }
}

bool duplicate_handle(process::FileHandle& destination, const process::FileHandle& source) {
    reset_handle(destination);
    if (!source.in_use) {
        return false;
    }

    destination = source;
    if (destination.kind == process::HandleKind::pipe_read || destination.kind == process::HandleKind::pipe_write) {
        Pipe* pipe = static_cast<Pipe*>(destination.pipe);
        if (pipe == nullptr || !pipe->in_use) {
            reset_handle(destination);
            return false;
        }

        if (destination.kind == process::HandleKind::pipe_read) {
            pipe->reader_count += 1;
        } else {
            pipe->writer_count += 1;
        }
    }

    return true;
}

bool assign_standard_handle(process::Process& proc, size_t fd, const process::FileHandle& source) {
    if (fd >= process::kMaxFileHandles) {
        return false;
    }

    release_handle(proc.handles[fd]);
    return duplicate_handle(proc.handles[fd], source);
}

void wake_waiting_parent(process::Process& exiting) {
    process::Process* parent = find_waiting_parent_for(exiting.pid);
    if (parent == nullptr) {
        return;
    }

    if (parent->wait_status_address != 0) {
        write_int_to_process_memory(*parent, parent->wait_status_address, exiting.exit_code);
    }

    parent->waiting_for_pid = 0;
    parent->wait_status_address = 0;
    parent->context->rax = exiting.pid;
    parent->state = process::State::runnable;
    reset_time_slice(*parent);
}

void wake_sleepers(uint64_t current_tick) {
    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::sleeping) {
            continue;
        }

        if (proc.wake_tick > current_tick) {
            continue;
        }

        proc.wake_tick = 0;
        proc.context->rax = 0;
        proc.state = process::State::runnable;
        reset_time_slice(proc);
    }
}

process::Process* create_process_internal(const char* path, int argc, const char* const* argv, uint32_t parent_pid) {
    const vfs::Vnode* image = vfs::resolve(path);
    if (image == nullptr || image->type != vfs::NodeType::file) {
        return nullptr;
    }

    process::Process* proc = allocate_process_slot(false);
    if (proc == nullptr) {
        return nullptr;
    }

    proc->parent_pid = parent_pid;
    if (!vm::create_address_space(proc->address_space)) {
        proc->state = process::State::unused;
        return nullptr;
    }

    memory::PageAllocation kernel_stack = {};
    if (!memory::allocate_contiguous_pages(kKernelStackPages, kernel_stack)) {
        proc->state = process::State::unused;
        return nullptr;
    }

    proc->kernel_stack_base = reinterpret_cast<uint64_t>(kernel_stack.virtual_address);
    proc->kernel_stack_size = kKernelStackPages * memory::kPageSize;
    initialize_standard_handles(*proc);

    elf::LoadResult load_result = {};
    if (!elf::load_user_image(image->data, image->size, proc->address_space, argc, argv, load_result)) {
        proc->state = process::State::unused;
        return nullptr;
    }

    fabricate_initial_context(*proc, load_result.entry_point, load_result.stack_pointer, argc, load_result.stack_pointer);
    return proc;
}

process::Process* create_idle_process() {
    process::Process* proc = allocate_process_slot(true);
    if (proc == nullptr) {
        return nullptr;
    }

    if (!vm::create_address_space(proc->address_space)) {
        proc->state = process::State::unused;
        return nullptr;
    }

    memory::PageAllocation kernel_stack = {};
    if (!memory::allocate_contiguous_pages(kKernelStackPages, kernel_stack)) {
        proc->state = process::State::unused;
        return nullptr;
    }

    proc->kernel_stack_base = reinterpret_cast<uint64_t>(kernel_stack.virtual_address);
    proc->kernel_stack_size = kKernelStackPages * memory::kPageSize;

    memory::PageAllocation code_page = {};
    memory::PageAllocation stack_page = {};
    if (!memory::allocate_page(code_page) || !memory::allocate_page(stack_page)) {
        proc->state = process::State::unused;
        return nullptr;
    }

    memset(code_page.virtual_address, 0, memory::kPageSize);
    memcpy(code_page.virtual_address, kIdleCode, sizeof(kIdleCode));
    memset(stack_page.virtual_address, 0, memory::kPageSize);

    if (!vm::map_page(proc->address_space, kIdleCodeAddress, code_page.physical_address, vm::kPageUser) ||
        !vm::map_page(proc->address_space, vm::kUserStackTop - memory::kPageSize, stack_page.physical_address, vm::kPageUser | vm::kPageWrite)) {
        proc->state = process::State::unused;
        return nullptr;
    }

    fabricate_initial_context(*proc, kIdleCodeAddress, vm::kUserStackTop, 0, 0);
    return proc;
}

int sys_write_tty(const void* buffer, size_t count) {
    const auto* text = static_cast<const char*>(buffer);
    for (size_t index = 0; index < count; ++index) {
        tty::write_char(text[index]);
    }
    return static_cast<int>(count);
}

int sys_write_handle(process::Process& proc, uint64_t fd, const void* buffer, size_t count) {
    if (fd >= process::kMaxFileHandles || !proc.handles[fd].in_use || buffer == nullptr) {
        return -1;
    }

    process::FileHandle& handle = proc.handles[fd];
    switch (handle.kind) {
        case process::HandleKind::tty:
            return sys_write_tty(buffer, count);
        case process::HandleKind::vnode: {
            if (handle.node == nullptr) {
                return -1;
            }
            const size_t written = vfs::write(*handle.node, handle.offset, buffer, count, false);
            handle.offset += written;
            return static_cast<int>(written);
        }
        case process::HandleKind::pipe_write: {
            Pipe* pipe = static_cast<Pipe*>(handle.pipe);
            if (pipe == nullptr || !pipe->in_use || pipe->reader_count == 0) {
                return -1;
            }

            const size_t available = kPipeCapacity - pipe->size;
            const size_t to_copy = available < count ? available : count;
            if (to_copy == 0) {
                return 0;
            }

            memcpy(pipe->buffer + pipe->size, buffer, to_copy);
            pipe->size += to_copy;
            wake_blocked_readers_for_pipe(*pipe);
            return static_cast<int>(to_copy);
        }
        default:
            return -1;
    }
}

int sys_read_handle(process::Process& proc, uint64_t fd, uint64_t buffer_address, size_t count, process::SavedContext* context) {
    if (fd >= process::kMaxFileHandles || !proc.handles[fd].in_use || buffer_address == 0 || count == 0) {
        return -1;
    }

    process::FileHandle& handle = proc.handles[fd];
    switch (handle.kind) {
        case process::HandleKind::tty:
            if (tty::main().line_ready) {
                return copy_pending_tty_line(proc, buffer_address, count);
            }

            proc.blocked_read_buffer = buffer_address;
            proc.blocked_read_capacity = count;
            proc.blocked_read_fd = fd;
            proc.state = process::State::blocked_read;
            context->rax = 0;
            return -2;
        case process::HandleKind::vnode: {
            if (handle.node == nullptr) {
                return -1;
            }
            const size_t copied = vfs::read(*handle.node, handle.offset, reinterpret_cast<void*>(buffer_address), count);
            handle.offset += copied;
            return static_cast<int>(copied);
        }
        case process::HandleKind::pipe_read: {
            const int copied = read_from_pipe(proc, handle, buffer_address, count);
            if (copied == -2) {
                proc.blocked_read_buffer = buffer_address;
                proc.blocked_read_capacity = count;
                proc.blocked_read_fd = fd;
                proc.state = process::State::blocked_read;
                context->rax = 0;
            }
            return copied;
        }
        default:
            return -1;
    }
}

int sys_open(process::Process& proc, const char* path, uint32_t flags) {
    vfs::Vnode* node = vfs::open(path, flags);
    if (node == nullptr) {
        return -1;
    }

    const int fd = allocate_fd(proc);
    if (fd < 0) {
        return -1;
    }

    proc.handles[fd].kind = process::HandleKind::vnode;
    proc.handles[fd].node = node;
    return fd;
}

int sys_close(process::Process& proc, uint64_t fd) {
    if (fd >= process::kMaxFileHandles || !proc.handles[fd].in_use) {
        return -1;
    }

    release_handle(proc.handles[fd]);
    return 0;
}

int sys_readdir(process::Process& proc, uint64_t fd, char* buffer, size_t capacity) {
    if (fd >= process::kMaxFileHandles || !proc.handles[fd].in_use || buffer == nullptr || capacity == 0) {
        return -1;
    }

    process::FileHandle& handle = proc.handles[fd];
    if (handle.kind != process::HandleKind::vnode || handle.node == nullptr || handle.node->type != vfs::NodeType::directory) {
        return -1;
    }

    const vfs::Vnode* child = vfs::child_at(*handle.node, handle.iterator_index++);
    if (child == nullptr) {
        buffer[0] = '\0';
        return 0;
    }

    const size_t length = strlen(child->name);
    const size_t to_copy = length < (capacity - 1) ? length : (capacity - 1);
    memcpy(buffer, child->name, to_copy);
    buffer[to_copy] = '\0';
    return static_cast<int>(to_copy);
}

int sys_pipe(process::Process& proc, uint64_t user_fd_array) {
    Pipe* pipe = allocate_pipe();
    if (pipe == nullptr) {
        return -1;
    }

    pipe->reader_count = 1;
    pipe->writer_count = 1;

    const int read_fd = allocate_fd(proc);
    if (read_fd < 0) {
        memset(pipe, 0, sizeof(*pipe));
        return -1;
    }

    const int write_fd = allocate_fd(proc);
    if (write_fd < 0) {
        release_handle(proc.handles[read_fd]);
        memset(pipe, 0, sizeof(*pipe));
        return -1;
    }

    proc.handles[read_fd].kind = process::HandleKind::pipe_read;
    proc.handles[read_fd].pipe = pipe;
    proc.handles[write_fd].kind = process::HandleKind::pipe_write;
    proc.handles[write_fd].pipe = pipe;

    const int values[2] = {read_fd, write_fd};
    write_to_process_memory(proc, user_fd_array, values, sizeof(values));
    return 0;
}

bool copy_spawn_arguments(
    const process::SavedContext& context,
    char* path,
    size_t path_capacity,
    char argv_storage[16][64],
    const char* argv_local[16],
    int& argc
) {
    const auto* user_argv = reinterpret_cast<const char* const*>(context.rsi);
    argc = static_cast<int>(context.rdx < 15 ? context.rdx : 15);

    if (!copy_user_string(reinterpret_cast<const char*>(context.rdi), path, path_capacity)) {
        return false;
    }

    for (int index = 0; index < argc; ++index) {
        if (!copy_user_string(user_argv[index], argv_storage[index], sizeof(argv_storage[index]))) {
            argc = index;
            break;
        }
        argv_local[index] = argv_storage[index];
    }
    argv_local[argc] = nullptr;
    return true;
}

uint64_t milliseconds_to_ticks(uint64_t milliseconds) {
    const uint32_t hz = timer::frequency_hz() != 0 ? timer::frequency_hz() : 1;
    const uint64_t ticks = (milliseconds * hz + 999ULL) / 1000ULL;
    return ticks != 0 ? ticks : 1;
}

} // namespace

namespace process {

void initialize() {
    memset(g_processes, 0, sizeof(g_processes));
    memset(g_pipes, 0, sizeof(g_pipes));
    g_current = nullptr;
    g_idle = nullptr;
    g_next_pid = 1;
    g_schedule_cursor = 0;
    g_ready = true;
}

bool ready() {
    return g_ready;
}

Process* current() {
    return g_current;
}

Process* find(uint32_t pid) {
    for (Process& proc : g_processes) {
        if (proc.state != State::unused && proc.pid == pid) {
            return &proc;
        }
    }
    return nullptr;
}

Process* create_user_process(const char* path, int argc, const char* const* argv, uint32_t parent_pid) {
    return create_process_internal(path, argc, argv, parent_pid);
}

[[noreturn]] void start_init(const char* path) {
    g_idle = create_idle_process();
    if (g_idle == nullptr) {
        panic("scheduler: unable to create idle task");
    }

    const char* argv[] = {path, nullptr};
    Process* init = create_process_internal(path, 1, argv, 0);
    if (init == nullptr) {
        panic("process: unable to start init");
    }

    g_schedule_cursor = process_index(init);
    switch_to_process(init);
    arch::x86_64::resume_context(init->context, init->address_space.pml4_physical);
}

void terminate_current(int exit_code) {
    if (g_current == nullptr || g_current->idle) {
        panic("process: invalid current task on exit");
    }

    Process* exiting = g_current;
    exiting->exit_code = exit_code;
    release_all_handles(*exiting);
    exiting->state = State::exited;
    wake_waiting_parent(*exiting);

    SavedContext* next = choose_next_context(nullptr);
    arch::x86_64::resume_context(next, g_current->address_space.pml4_physical);
}

void terminate_current_from_exception(uint8_t vector) {
    console::printf(
        "user: exception #%u killed pid=%u\n",
        static_cast<unsigned>(vector),
        g_current != nullptr ? g_current->pid : 0
    );
    terminate_current(128 + vector);
}

SavedContext* handle_syscall(SavedContext* context) {
    if (g_current == nullptr) {
        return context;
    }

    g_current->context = context;
    g_current->state = State::running;

    switch (context->rax) {
        case SAVANXP_SYS_READ: {
            const int result = sys_read_handle(
                *g_current,
                context->rdi,
                context->rsi,
                static_cast<size_t>(context->rdx),
                context);
            if (result == -2) {
                return choose_next_context(context);
            }
            context->rax = result >= 0 ? static_cast<uint64_t>(result) : static_cast<uint64_t>(-1);
            return context;
        }
        case SAVANXP_SYS_WRITE:
            context->rax = static_cast<uint64_t>(
                sys_write_handle(*g_current, context->rdi, reinterpret_cast<const void*>(context->rsi), static_cast<size_t>(context->rdx)));
            return context;
        case SAVANXP_SYS_OPEN: {
            char path[128] = {};
            if (!copy_user_string(reinterpret_cast<const char*>(context->rdi), path, sizeof(path))) {
                context->rax = static_cast<uint64_t>(-1);
                return context;
            }
            context->rax = static_cast<uint64_t>(sys_open(*g_current, path, static_cast<uint32_t>(context->rsi)));
            return context;
        }
        case SAVANXP_SYS_CLOSE:
            context->rax = static_cast<uint64_t>(sys_close(*g_current, context->rdi));
            return context;
        case SAVANXP_SYS_READDIR:
            context->rax = static_cast<uint64_t>(
                sys_readdir(*g_current, context->rdi, reinterpret_cast<char*>(context->rsi), static_cast<size_t>(context->rdx)));
            return context;
        case SAVANXP_SYS_SPAWN:
        case SAVANXP_SYS_SPAWN_FD: {
            char path[128] = {};
            char argv_storage[16][64] = {};
            const char* argv_local[16] = {};
            int argc = 0;
            if (!copy_spawn_arguments(*context, path, sizeof(path), argv_storage, argv_local, argc)) {
                context->rax = static_cast<uint64_t>(-1);
                return context;
            }

            Process* child = create_process_internal(path, argc, argv_local, g_current->pid);
            if (child == nullptr) {
                context->rax = static_cast<uint64_t>(-1);
                return context;
            }

            if (context->rax == SAVANXP_SYS_SPAWN_FD) {
                const uint64_t stdin_fd = context->r10;
                const uint64_t stdout_fd = context->r8;
                if (stdin_fd >= kMaxFileHandles || stdout_fd >= kMaxFileHandles ||
                    !g_current->handles[stdin_fd].in_use || !g_current->handles[stdout_fd].in_use ||
                    !assign_standard_handle(*child, 0, g_current->handles[stdin_fd]) ||
                    !assign_standard_handle(*child, 1, g_current->handles[stdout_fd])) {
                    release_all_handles(*child);
                    child->state = State::exited;
                    child->exit_code = 127;
                    context->rax = static_cast<uint64_t>(-1);
                    return context;
                }
            }

            context->rax = child->pid;
            return context;
        }
        case SAVANXP_SYS_WAITPID: {
            Process* child = child_of(g_current->pid, static_cast<uint32_t>(context->rdi));
            if (child == nullptr) {
                context->rax = static_cast<uint64_t>(-1);
                return context;
            }

            if (child->state == State::exited) {
                if (context->rsi != 0) {
                    write_int_to_process_memory(*g_current, context->rsi, child->exit_code);
                }
                context->rax = child->pid;
                return context;
            }

            g_current->waiting_for_pid = child->pid;
            g_current->wait_status_address = context->rsi;
            g_current->state = State::blocked_wait;
            return choose_next_context(context);
        }
        case SAVANXP_SYS_EXIT:
            terminate_current(static_cast<int>(context->rdi));
            return nullptr;
        case SAVANXP_SYS_YIELD:
            g_current->state = State::runnable;
            context->rax = 0;
            return choose_next_context(context);
        case SAVANXP_SYS_UPTIME_MS:
            context->rax = (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
            return context;
        case SAVANXP_SYS_CLEAR:
            tty::clear();
            context->rax = 0;
            return context;
        case SAVANXP_SYS_SLEEP_MS:
            g_current->wake_tick = timer::ticks() + milliseconds_to_ticks(context->rdi);
            g_current->state = State::sleeping;
            return choose_next_context(context);
        case SAVANXP_SYS_PIPE:
            context->rax = static_cast<uint64_t>(sys_pipe(*g_current, context->rdi));
            return context;
        default:
            context->rax = static_cast<uint64_t>(-1);
            return context;
    }
}

SavedContext* handle_timer_tick(SavedContext* context) {
    if (g_current == nullptr) {
        return context;
    }

    g_current->context = context;
    wake_sleepers(timer::ticks());

    if (g_current->state != State::running) {
        return choose_next_context(context);
    }

    if (g_current->idle) {
        if (has_runnable_non_idle()) {
            g_current->state = State::runnable;
            return choose_next_context(context);
        }
        return context;
    }

    if (g_current->time_slice > 1) {
        g_current->time_slice -= 1;
        return context;
    }

    g_current->state = State::runnable;
    return choose_next_context(context);
}

void notify_tty_line_ready() {
    for (Process& proc : g_processes) {
        if (proc.state != State::blocked_read) {
            continue;
        }
        if (proc.blocked_read_fd >= kMaxFileHandles || !proc.handles[proc.blocked_read_fd].in_use) {
            continue;
        }
        if (proc.handles[proc.blocked_read_fd].kind != HandleKind::tty) {
            continue;
        }

        const int copied = satisfy_blocked_read(proc);
        if (copied == -2) {
            continue;
        }

        proc.blocked_read_buffer = 0;
        proc.blocked_read_capacity = 0;
        proc.blocked_read_fd = 0;
        proc.context->rax = copied >= 0 ? static_cast<uint64_t>(copied) : static_cast<uint64_t>(-1);
        proc.state = State::runnable;
        reset_time_slice(proc);
        break;
    }
}

} // namespace process

extern "C" process::SavedContext* savanxp_handle_syscall(process::SavedContext* context) {
    return process::handle_syscall(context);
}
