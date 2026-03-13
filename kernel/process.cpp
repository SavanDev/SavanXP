#include "kernel/process.hpp"

#include <stdarg.h>
#include <stdint.h>

#include "kernel/block.hpp"
#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/device.hpp"
#include "kernel/elf.hpp"
#include "kernel/net.hpp"
#include "kernel/panic.hpp"
#include "kernel/pci.hpp"
#include "kernel/pcspeaker.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/ps2.hpp"
#include "kernel/string.hpp"
#include "kernel/svfs.hpp"
#include "kernel/timer.hpp"
#include "kernel/tty.hpp"
#include "kernel/ui.hpp"

namespace {

constexpr uint16_t kUserDataSelector = 0x1b;
constexpr uint16_t kUserCodeSelector = 0x23;
constexpr uint64_t kKernelStackPages = 4;
constexpr uint64_t kIdleCodeAddress = 0x0000000000800000ULL;
constexpr uint32_t kDefaultTimeSlice = 4;
constexpr size_t kMaxPipeCount = 16;
constexpr size_t kPipeCapacity = 4096;
constexpr size_t kPipeChunkSize = 256;
constexpr uint32_t kWaitAnyPid = 0xffffffffu;
constexpr int kBlockedResult = -0x70000000;
constexpr bool kLogProc = false;
constexpr uint8_t kIdleCode[] = {
    0xb8, static_cast<uint8_t>(SAVANXP_SYS_YIELD), 0x00, 0x00, 0x00,
    0xcd, 0x80,
    0xeb, 0xf7,
};

process::Process g_processes[process::kMaxProcesses] = {};
process::Process* g_current = nullptr;
process::Process* g_idle = nullptr;
process::Pipe g_pipes[kMaxPipeCount] = {};
uint8_t g_pipe_storage[kMaxPipeCount][kPipeCapacity] = {};
process::OpenFile g_open_files[process::kMaxOpenFiles] = {};
uint32_t g_next_pid = 1;
size_t g_schedule_cursor = 0;
bool g_ready = false;
savanxp_system_info g_boot_system_info = {};
bool g_boot_system_info_ready = false;

uint64_t read_cr3() {
    uint64_t value = 0;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

void write_cr3(uint64_t value) {
    asm volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

int negative_error(savanxp_error_code code) {
    return -static_cast<int>(code);
}

uint64_t current_uptime_ms() {
    return (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
}

uint32_t exported_timer_backend(timer::Backend backend) {
    switch (backend) {
        case timer::Backend::local_apic:
            return SAVANXP_TIMER_LOCAL_APIC;
        case timer::Backend::none:
        default:
            return SAVANXP_TIMER_NONE;
    }
}

void process_assert(bool condition, const char* message) {
    if (!condition) {
        panic(message);
    }
}

void proc_log(const char* format, ...) {
    if (!kLogProc) {
        return;
    }

    va_list args;
    va_start(args, format);
    console::vprintf(format, args);
    va_end(args);
}

uint32_t exported_state(process::State state) {
    switch (state) {
        case process::State::unused: return SAVANXP_PROC_UNUSED;
        case process::State::ready: return SAVANXP_PROC_READY;
        case process::State::running: return SAVANXP_PROC_RUNNING;
        case process::State::blocked_read: return SAVANXP_PROC_BLOCKED_READ;
        case process::State::blocked_write: return SAVANXP_PROC_BLOCKED_WRITE;
        case process::State::blocked_wait: return SAVANXP_PROC_BLOCKED_WAIT;
        case process::State::sleeping: return SAVANXP_PROC_SLEEPING;
        case process::State::zombie: return SAVANXP_PROC_ZOMBIE;
        default: return SAVANXP_PROC_UNUSED;
    }
}

size_t process_index(const process::Process* proc) {
    return proc != nullptr ? static_cast<size_t>(proc - &g_processes[0]) : 0;
}

void reset_time_slice(process::Process& proc) {
    proc.time_slice = proc.idle ? 1 : kDefaultTimeSlice;
}

void clear_fd_entry(process::FdEntry& entry) {
    entry.file = nullptr;
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

void set_process_name(process::Process& proc, const char* path) {
    memset(proc.name, 0, sizeof(proc.name));
    if (path == nullptr) {
        strcpy(proc.name, "task");
        return;
    }

    const char* name = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            name = cursor + 1;
        }
    }

    size_t index = 0;
    while (name[index] != '\0' && index + 1 < sizeof(proc.name)) {
        proc.name[index] = name[index];
        ++index;
    }
    proc.name[index] = '\0';
}

void set_process_cwd_root(process::Process& proc) {
    memset(proc.cwd, 0, sizeof(proc.cwd));
    strcpy(proc.cwd, "/");
}

bool resolve_process_path(const process::Process& proc, const char* input, char* output, size_t capacity) {
    const char* cwd = proc.cwd[0] != '\0' ? proc.cwd : "/";
    return vfs::normalize_path(cwd, input, output, capacity);
}

void fill_stat_for_vnode(const vfs::Vnode& node, savanxp_stat& stat) {
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

void fill_stat_for_open_file(const process::OpenFile& file, savanxp_stat& stat) {
    memset(&stat, 0, sizeof(stat));
    switch (file.kind) {
        case process::HandleKind::tty:
        case process::HandleKind::device:
            stat.st_mode = SAVANXP_S_IFCHR | 0666u;
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

process::Process* allocate_process_slot(bool idle) {
    for (process::Process& slot : g_processes) {
        if (slot.state == process::State::unused) {
            memset(&slot, 0, sizeof(slot));
            slot.pid = g_next_pid++;
            slot.state = process::State::ready;
            slot.idle = idle;
            reset_time_slice(slot);
            return &slot;
        }
    }
    return nullptr;
}

void reset_process_slot(process::Process& proc) {
    const bool idle = proc.idle;
    memset(&proc, 0, sizeof(proc));
    proc.idle = idle;
    proc.state = process::State::unused;
}

void release_kernel_stack(process::Process& proc) {
    if (proc.kernel_stack_base == 0 || proc.kernel_stack_size == 0) {
        return;
    }

    const uint64_t physical_base = proc.kernel_stack_base - vm::hhdm_offset();
    const uint64_t page_count = proc.kernel_stack_size / memory::kPageSize;
    (void)memory::free_pages(physical_base, page_count);
    proc.kernel_stack_base = 0;
    proc.kernel_stack_size = 0;
    proc.context = nullptr;
}

void release_address_space(process::Process& proc) {
    vm::destroy_address_space(proc.address_space);
    proc.context = nullptr;
}

void release_process_resources(process::Process& proc) {
    release_address_space(proc);
    release_kernel_stack(proc);
}

bool read_from_process_memory(const process::Process& proc, void* destination, uint64_t user_address, size_t count, bool require_write = false) {
    if (destination == nullptr || count == 0) {
        return false;
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_address, count, require_write)) {
        return false;
    }

    if (&proc == g_current) {
        memcpy(destination, reinterpret_cast<const void*>(user_address), count);
        return true;
    }

    const uint64_t previous_cr3 = read_cr3();
    write_cr3(proc.address_space.pml4_physical);
    memcpy(destination, reinterpret_cast<const void*>(user_address), count);
    write_cr3(previous_cr3);
    return true;
}

bool write_to_process_memory(process::Process& proc, uint64_t user_address, const void* source, size_t count) {
    if (source == nullptr || count == 0) {
        return false;
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_address, count, true)) {
        return false;
    }

    if (&proc == g_current) {
        memcpy(reinterpret_cast<void*>(user_address), source, count);
        return true;
    }

    const uint64_t previous_cr3 = read_cr3();
    write_cr3(proc.address_space.pml4_physical);
    memcpy(reinterpret_cast<void*>(user_address), source, count);
    write_cr3(previous_cr3);
    return true;
}

bool read_user_pointer(const process::Process& proc, uint64_t user_address, uint64_t& value) {
    return read_from_process_memory(proc, &value, user_address, sizeof(value));
}

bool copy_user_string(const process::Process& proc, uint64_t user_address, char* buffer, size_t capacity) {
    if (buffer == nullptr || capacity == 0 || user_address == 0) {
        return false;
    }

    size_t index = 0;
    while (index + 1 < capacity) {
        if (!read_from_process_memory(proc, &buffer[index], user_address + index, 1)) {
            buffer[0] = '\0';
            return false;
        }
        if (buffer[index] == '\0') {
            return true;
        }
        ++index;
    }

    buffer[capacity - 1] = '\0';
    return false;
}

void write_int_to_process_memory(process::Process& proc, uint64_t user_address, int value) {
    if (user_address != 0) {
        (void)write_to_process_memory(proc, user_address, &value, sizeof(value));
    }
}

process::OpenFile* allocate_open_file() {
    for (process::OpenFile& file : g_open_files) {
        if (!file.in_use) {
            memset(&file, 0, sizeof(file));
            file.in_use = true;
            return &file;
        }
    }
    return nullptr;
}

process::Pipe* allocate_pipe() {
    for (size_t index = 0; index < kMaxPipeCount; ++index) {
        process::Pipe& pipe = g_pipes[index];
        if (!pipe.in_use) {
            memset(&pipe, 0, sizeof(pipe));
            pipe.in_use = true;
            pipe.buffer = &g_pipe_storage[index][0];
            return &pipe;
        }
    }
    return nullptr;
}

void free_pipe_if_unused(process::Pipe* pipe) {
    if (pipe == nullptr || !pipe->in_use) {
        return;
    }
    if (pipe->reader_refs == 0 && pipe->writer_refs == 0) {
        memset(pipe, 0, sizeof(*pipe));
    }
}

void retain_open_file(process::OpenFile* file) {
    if (file != nullptr) {
        process_assert(file->in_use, "process: retain dead open file");
        file->refcount += 1;
    }
}

void discard_open_file(process::OpenFile*& file_ptr) {
    if (file_ptr == nullptr) {
        return;
    }
    process_assert(file_ptr->refcount == 0, "process: discard referenced open file");
    memset(file_ptr, 0, sizeof(*file_ptr));
    file_ptr = nullptr;
}

int install_fd(process::Process& proc, int fd, process::OpenFile* file);
void wake_blocked_readers_for_pipe(process::Pipe& pipe);
void wake_blocked_writers_for_pipe(process::Pipe& pipe);
void release_all_handles(process::Process& proc);

void release_open_file(process::OpenFile*& file_ptr) {
    process::OpenFile* file = file_ptr;
    file_ptr = nullptr;
    if (file == nullptr) {
        return;
    }

    process_assert(file->refcount != 0, "process: open file refcount underflow");
    file->refcount -= 1;
    if (file->refcount != 0) {
        return;
    }

    if (file->kind == process::HandleKind::pipe && file->pipe != nullptr) {
        if ((file->flags & process::open_read) != 0) {
            process_assert(file->pipe->reader_refs != 0, "process: pipe reader underflow");
            file->pipe->reader_refs -= 1;
            wake_blocked_writers_for_pipe(*file->pipe);
        }
        if ((file->flags & process::open_write) != 0) {
            process_assert(file->pipe->writer_refs != 0, "process: pipe writer underflow");
            file->pipe->writer_refs -= 1;
            wake_blocked_readers_for_pipe(*file->pipe);
        }
        free_pipe_if_unused(file->pipe);
    } else if (file->kind == process::HandleKind::device && file->device != nullptr) {
        device::close(file->device);
    } else if (file->kind == process::HandleKind::socket && file->socket != nullptr) {
        net::close_socket(file->socket);
    }

    memset(file, 0, sizeof(*file));
}

int allocate_fd_slot(process::Process& proc) {
    for (size_t fd = 0; fd < process::kMaxFileHandles; ++fd) {
        if (proc.handles[fd].file == nullptr) {
            return static_cast<int>(fd);
        }
    }
    return negative_error(SAVANXP_EBADF);
}

int install_fd(process::Process& proc, int fd, process::OpenFile* file) {
    if (fd < 0 || fd >= static_cast<int>(process::kMaxFileHandles) || file == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }

    if (proc.handles[fd].file != nullptr) {
        release_open_file(proc.handles[fd].file);
    }
    retain_open_file(file);
    proc.handles[fd].file = file;
    return fd;
}

int allocate_fd(process::Process& proc, process::OpenFile* file) {
    const int fd = allocate_fd_slot(proc);
    if (fd < 0) {
        return fd;
    }
    return install_fd(proc, fd, file);
}

process::OpenFile* create_tty_open_file(uint32_t flags) {
    process::OpenFile* file = allocate_open_file();
    if (file == nullptr) {
        return nullptr;
    }
    file->kind = process::HandleKind::tty;
    file->flags = flags;
    return file;
}

bool initialize_standard_handles(process::Process& proc) {
    for (size_t index = 0; index < process::kMaxFileHandles; ++index) {
        clear_fd_entry(proc.handles[index]);
    }

    process::OpenFile* stdin_file = create_tty_open_file(process::open_read);
    process::OpenFile* stdout_file = create_tty_open_file(process::open_write);
    process::OpenFile* stderr_file = create_tty_open_file(process::open_write);
    if (stdin_file == nullptr || stdout_file == nullptr || stderr_file == nullptr) {
        discard_open_file(stdin_file);
        discard_open_file(stdout_file);
        discard_open_file(stderr_file);
        return false;
    }

    if (install_fd(proc, 0, stdin_file) < 0) {
        discard_open_file(stdin_file);
        discard_open_file(stdout_file);
        discard_open_file(stderr_file);
        return false;
    }
    stdin_file = nullptr;

    if (install_fd(proc, 1, stdout_file) < 0) {
        release_all_handles(proc);
        discard_open_file(stdout_file);
        discard_open_file(stderr_file);
        return false;
    }
    stdout_file = nullptr;

    if (install_fd(proc, 2, stderr_file) < 0) {
        release_all_handles(proc);
        discard_open_file(stdin_file);
        discard_open_file(stdout_file);
        discard_open_file(stderr_file);
        return false;
    }
    stderr_file = nullptr;

    return true;
}

bool inherit_handle(process::Process& child, int child_fd, const process::Process& parent, int parent_fd) {
    if (parent_fd < 0 || parent_fd >= static_cast<int>(process::kMaxFileHandles)) {
        return false;
    }
    process::OpenFile* file = parent.handles[parent_fd].file;
    if (file == nullptr) {
        return false;
    }
    return install_fd(child, child_fd, file) >= 0;
}

void release_all_handles(process::Process& proc) {
    for (process::FdEntry& entry : proc.handles) {
        if (entry.file != nullptr) {
            release_open_file(entry.file);
        }
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

bool load_image_bytes(const vfs::Vnode& image, const void*& data, size_t& size, memory::PageAllocation& backing) {
    data = nullptr;
    size = 0;
    memset(&backing, 0, sizeof(backing));

    if (image.type != vfs::NodeType::file || image.size == 0) {
        return false;
    }

    if (image.backend == vfs::Backend::memory && image.data != nullptr) {
        data = image.data;
        size = image.size;
        return true;
    }

    const uint64_t page_count = (image.size + memory::kPageSize - 1) / memory::kPageSize;
    if (!memory::allocate_contiguous_pages(page_count, backing)) {
        return false;
    }

    memset(backing.virtual_address, 0, page_count * memory::kPageSize);
    const size_t copied = vfs::read(*const_cast<vfs::Vnode*>(&image), 0, backing.virtual_address, image.size);
    if (copied != image.size) {
        (void)memory::free_allocation(backing);
        memset(&backing, 0, sizeof(backing));
        return false;
    }

    data = backing.virtual_address;
    size = image.size;
    return true;
}

bool prepare_exec_image(process::Process& proc, const char* path, int argc, const char* const* argv) {
    const vfs::Vnode* image = vfs::resolve(path);
    if (image == nullptr || image->type != vfs::NodeType::file) {
        return false;
    }

    const void* image_bytes = nullptr;
    size_t image_size = 0;
    memory::PageAllocation image_backing = {};
    if (!load_image_bytes(*image, image_bytes, image_size, image_backing)) {
        return false;
    }

    vm::VmSpace new_space = {};
    if (!vm::create_address_space(new_space)) {
        if (image_backing.physical_address != 0) {
            (void)memory::free_allocation(image_backing);
        }
        return false;
    }

    elf::LoadResult load_result = {};
    if (!elf::load_user_image(image_bytes, image_size, new_space, argc, argv, load_result)) {
        vm::destroy_address_space(new_space);
        if (image_backing.physical_address != 0) {
            (void)memory::free_allocation(image_backing);
        }
        return false;
    }
    if (image_backing.physical_address != 0) {
        (void)memory::free_allocation(image_backing);
    }

    vm::VmSpace old_space = proc.address_space;
    proc.address_space = new_space;
    vm::destroy_address_space(old_space);
    set_process_name(proc, path);
    proc.blocked_io_fd = 0;
    proc.blocked_read_buffer = 0;
    proc.blocked_read_capacity = 0;
    proc.blocked_write_buffer = 0;
    proc.blocked_write_length = 0;
    proc.blocked_write_progress = 0;
    proc.waiting_for_pid = 0;
    proc.wait_status_address = 0;
    proc.wake_tick = 0;
    reset_time_slice(proc);
    fabricate_initial_context(proc, load_result.entry_point, load_result.stack_pointer, argc, load_result.stack_pointer);
    return true;
}

process::Process* create_idle_process() {
    process::Process* proc = allocate_process_slot(true);
    if (proc == nullptr) {
        return nullptr;
    }

    if (!vm::create_address_space(proc->address_space)) {
        reset_process_slot(*proc);
        return nullptr;
    }

    memory::PageAllocation kernel_stack = {};
    if (!memory::allocate_contiguous_pages(kKernelStackPages, kernel_stack)) {
        release_address_space(*proc);
        reset_process_slot(*proc);
        return nullptr;
    }

    proc->kernel_stack_base = reinterpret_cast<uint64_t>(kernel_stack.virtual_address);
    proc->kernel_stack_size = kKernelStackPages * memory::kPageSize;
    set_process_name(*proc, "idle");
    set_process_cwd_root(*proc);

    memory::PageAllocation code_page = {};
    memory::PageAllocation stack_page = {};
    if (!memory::allocate_page(code_page) || !memory::allocate_page(stack_page)) {
        if (code_page.physical_address != 0) {
            (void)memory::free_allocation(code_page);
        }
        if (stack_page.physical_address != 0) {
            (void)memory::free_allocation(stack_page);
        }
        release_process_resources(*proc);
        reset_process_slot(*proc);
        return nullptr;
    }

    memset(code_page.virtual_address, 0, memory::kPageSize);
    memcpy(code_page.virtual_address, kIdleCode, sizeof(kIdleCode));
    memset(stack_page.virtual_address, 0, memory::kPageSize);

    const bool code_mapped = vm::map_page(proc->address_space, kIdleCodeAddress, code_page.physical_address, vm::kPageUser);
    const bool stack_mapped = code_mapped &&
        vm::map_page(proc->address_space,
            vm::kUserStackTop - memory::kPageSize,
            stack_page.physical_address,
            vm::kPageUser | vm::kPageWrite);
    if (!code_mapped || !stack_mapped) {
        if (!code_mapped) {
            (void)memory::free_allocation(code_page);
            (void)memory::free_allocation(stack_page);
        } else if (!stack_mapped) {
            (void)memory::free_allocation(stack_page);
        }
        release_process_resources(*proc);
        reset_process_slot(*proc);
        return nullptr;
    }

    fabricate_initial_context(*proc, kIdleCodeAddress, vm::kUserStackTop, 0, 0);
    return proc;
}

process::Process* create_process_internal(
    const char* path,
    int argc,
    const char* const* argv,
    process::Process* parent,
    int stdin_fd,
    int stdout_fd
) {
    const vfs::Vnode* image = vfs::resolve(path);
    if (image == nullptr || image->type != vfs::NodeType::file) {
        return nullptr;
    }

    const void* image_bytes = nullptr;
    size_t image_size = 0;
    memory::PageAllocation image_backing = {};
    if (!load_image_bytes(*image, image_bytes, image_size, image_backing)) {
        return nullptr;
    }

    process::Process* proc = allocate_process_slot(false);
    if (proc == nullptr) {
        if (image_backing.physical_address != 0) {
            (void)memory::free_allocation(image_backing);
        }
        return nullptr;
    }

    proc->parent_pid = parent != nullptr ? parent->pid : 0;
    set_process_name(*proc, path);
    if (parent != nullptr && parent->cwd[0] != '\0') {
        strcpy(proc->cwd, parent->cwd);
    } else {
        set_process_cwd_root(*proc);
    }
    if (!vm::create_address_space(proc->address_space)) {
        if (image_backing.physical_address != 0) {
            (void)memory::free_allocation(image_backing);
        }
        reset_process_slot(*proc);
        return nullptr;
    }

    memory::PageAllocation kernel_stack = {};
    if (!memory::allocate_contiguous_pages(kKernelStackPages, kernel_stack)) {
        if (image_backing.physical_address != 0) {
            (void)memory::free_allocation(image_backing);
        }
        release_address_space(*proc);
        reset_process_slot(*proc);
        return nullptr;
    }

    proc->kernel_stack_base = reinterpret_cast<uint64_t>(kernel_stack.virtual_address);
    proc->kernel_stack_size = kKernelStackPages * memory::kPageSize;

    if (parent == nullptr) {
        if (!initialize_standard_handles(*proc)) {
            if (image_backing.physical_address != 0) {
                (void)memory::free_allocation(image_backing);
            }
            release_process_resources(*proc);
            reset_process_slot(*proc);
            return nullptr;
        }
    } else {
        if (!inherit_handle(*proc, 0, *parent, stdin_fd >= 0 ? stdin_fd : 0) ||
            !inherit_handle(*proc, 1, *parent, stdout_fd >= 0 ? stdout_fd : 1) ||
            !inherit_handle(*proc, 2, *parent, 2)) {
            release_all_handles(*proc);
            if (image_backing.physical_address != 0) {
                (void)memory::free_allocation(image_backing);
            }
            release_process_resources(*proc);
            reset_process_slot(*proc);
            return nullptr;
        }
    }

    elf::LoadResult load_result = {};
    if (!elf::load_user_image(image_bytes, image_size, proc->address_space, argc, argv, load_result)) {
        release_all_handles(*proc);
        if (image_backing.physical_address != 0) {
            (void)memory::free_allocation(image_backing);
        }
        release_process_resources(*proc);
        reset_process_slot(*proc);
        return nullptr;
    }
    if (image_backing.physical_address != 0) {
        (void)memory::free_allocation(image_backing);
    }

    fabricate_initial_context(*proc, load_result.entry_point, load_result.stack_pointer, argc, load_result.stack_pointer);
    return proc;
}

process::Process* child_of(uint32_t parent_pid, uint32_t pid) {
    for (process::Process& proc : g_processes) {
        if (proc.state == process::State::unused) {
            continue;
        }
        if (proc.parent_pid == parent_pid && proc.pid == pid) {
            return &proc;
        }
    }
    return nullptr;
}

bool has_child(uint32_t parent_pid) {
    for (const process::Process& proc : g_processes) {
        if (proc.state != process::State::unused && proc.parent_pid == parent_pid) {
            return true;
        }
    }
    return false;
}

process::Process* find_zombie_child(uint32_t parent_pid, uint32_t waited_pid) {
    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::zombie || proc.parent_pid != parent_pid) {
            continue;
        }
        if (waited_pid == kWaitAnyPid || proc.pid == waited_pid) {
            return &proc;
        }
    }
    return nullptr;
}

process::Process* find_waiting_parent_for(uint32_t child_pid) {
    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::blocked_wait) {
            continue;
        }
        if (proc.waiting_for_pid == kWaitAnyPid || proc.waiting_for_pid == child_pid) {
            return &proc;
        }
    }
    return nullptr;
}

void reap_process(process::Process& proc) {
    proc_log("proc: reap pid=%u\n", proc.pid);
    release_all_handles(proc);
    release_process_resources(proc);
    reset_process_slot(proc);
}

void reparent_orphaned_children(uint32_t parent_pid) {
    for (process::Process& proc : g_processes) {
        if (proc.state == process::State::unused || proc.parent_pid != parent_pid) {
            continue;
        }
        if (proc.state == process::State::zombie) {
            reap_process(proc);
            continue;
        }
        proc.parent_pid = 0;
    }
}

bool has_runnable_non_idle() {
    for (const process::Process& proc : g_processes) {
        if (!proc.idle && proc.state == process::State::ready) {
            return true;
        }
    }
    return false;
}

process::Process* pick_next_runnable() {
    for (size_t offset = 1; offset <= process::kMaxProcesses; ++offset) {
        const size_t index = (g_schedule_cursor + offset) % process::kMaxProcesses;
        process::Process& proc = g_processes[index];
        if (proc.state == process::State::ready && !proc.idle) {
            g_schedule_cursor = index;
            return &proc;
        }
    }

    if (g_idle != nullptr &&
        (g_idle->state == process::State::ready || g_idle->state == process::State::running)) {
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

int complete_blocked_read(process::Process& proc, int result) {
    proc.blocked_io_fd = 0;
    proc.blocked_read_buffer = 0;
    proc.blocked_read_capacity = 0;
    proc.context->rax = static_cast<uint64_t>(result);
    proc.state = process::State::ready;
    reset_time_slice(proc);
    return result;
}

int complete_blocked_write(process::Process& proc, int result) {
    proc.blocked_io_fd = 0;
    proc.blocked_write_buffer = 0;
    proc.blocked_write_length = 0;
    proc.blocked_write_progress = 0;
    proc.context->rax = static_cast<uint64_t>(result);
    proc.state = process::State::ready;
    reset_time_slice(proc);
    return result;
}

size_t pipe_copy_out(process::Process& proc, process::Pipe& pipe, uint64_t user_address, size_t count) {
    const size_t to_copy = count < pipe.size ? count : pipe.size;
    if (to_copy == 0) {
        return 0;
    }

    const size_t first = (pipe.read_pos + to_copy <= kPipeCapacity) ? to_copy : (kPipeCapacity - pipe.read_pos);
    if (!write_to_process_memory(proc, user_address, pipe.buffer + pipe.read_pos, first)) {
        return 0;
    }
    if (to_copy > first) {
        if (!write_to_process_memory(proc, user_address + first, pipe.buffer, to_copy - first)) {
            return 0;
        }
    }

    pipe.read_pos = (pipe.read_pos + to_copy) % kPipeCapacity;
    pipe.size -= to_copy;
    return to_copy;
}

int try_pipe_read(process::Process& proc, process::OpenFile& file, uint64_t user_address, size_t count) {
    process::Pipe* pipe = file.pipe;
    if (pipe == nullptr || !pipe->in_use) {
        return negative_error(SAVANXP_EBADF);
    }
    if (count == 0) {
        return 0;
    }
    if (pipe->size == 0) {
        return pipe->writer_refs == 0 ? 0 : kBlockedResult;
    }

    const size_t copied = pipe_copy_out(proc, *pipe, user_address, count);
    if (copied == 0 && count != 0) {
        return negative_error(SAVANXP_EINVAL);
    }
    wake_blocked_writers_for_pipe(*pipe);
    return static_cast<int>(copied);
}

int pipe_write_chunk(process::Process& proc, process::Pipe& pipe, uint64_t user_address, size_t count, size_t progress) {
    if (pipe.reader_refs == 0) {
        return negative_error(SAVANXP_EPIPE);
    }
    if (count == 0) {
        return 0;
    }
    if (pipe.size == kPipeCapacity) {
        return kBlockedResult;
    }

    const size_t remaining = count - progress;
    const size_t writable = kPipeCapacity - pipe.size;
    const size_t total = writable < remaining ? writable : remaining;
    uint8_t scratch[kPipeChunkSize] = {};
    size_t written = 0;

    while (written < total) {
        const size_t step = (total - written) < sizeof(scratch) ? (total - written) : sizeof(scratch);
        if (!read_from_process_memory(proc, scratch, user_address + progress + written, step)) {
            return negative_error(SAVANXP_EINVAL);
        }

        size_t copied = 0;
        while (copied < step) {
            const size_t contiguous = pipe.write_pos + (step - copied) <= kPipeCapacity
                ? (step - copied)
                : (kPipeCapacity - pipe.write_pos);
            memcpy(pipe.buffer + pipe.write_pos, scratch + copied, contiguous);
            pipe.write_pos = (pipe.write_pos + contiguous) % kPipeCapacity;
            pipe.size += contiguous;
            copied += contiguous;
        }

        written += step;
    }

    wake_blocked_readers_for_pipe(pipe);
    return static_cast<int>(written);
}

void wake_waiting_parent(process::Process& child) {
    process::Process* parent = find_waiting_parent_for(child.pid);
    if (parent == nullptr) {
        return;
    }

    write_int_to_process_memory(*parent, parent->wait_status_address, child.exit_code);
    parent->waiting_for_pid = 0;
    parent->wait_status_address = 0;
    parent->context->rax = child.pid;
    parent->state = process::State::ready;
    reset_time_slice(*parent);
    reap_process(child);
}

void wake_sleepers(uint64_t current_tick) {
    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::sleeping || proc.wake_tick > current_tick) {
            continue;
        }

        proc.wake_tick = 0;
        proc.context->rax = 0;
        proc.state = process::State::ready;
        reset_time_slice(proc);
    }
}

void wake_blocked_readers_for_pipe(process::Pipe& pipe) {
    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::blocked_read) {
            continue;
        }
        if (proc.blocked_io_fd >= process::kMaxFileHandles) {
            continue;
        }

        process::OpenFile* file = proc.handles[proc.blocked_io_fd].file;
        if (file == nullptr || file->kind != process::HandleKind::pipe || file->pipe != &pipe) {
            continue;
        }

        const int result = try_pipe_read(proc, *file, proc.blocked_read_buffer, static_cast<size_t>(proc.blocked_read_capacity));
        if (result == kBlockedResult) {
            continue;
        }
        complete_blocked_read(proc, result);
    }
}

void wake_blocked_writers_for_pipe(process::Pipe& pipe) {
    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::blocked_write) {
            continue;
        }
        if (proc.blocked_io_fd >= process::kMaxFileHandles) {
            continue;
        }

        process::OpenFile* file = proc.handles[proc.blocked_io_fd].file;
        if (file == nullptr || file->kind != process::HandleKind::pipe || file->pipe != &pipe) {
            continue;
        }

        const int step = pipe_write_chunk(
            proc,
            pipe,
            proc.blocked_write_buffer,
            static_cast<size_t>(proc.blocked_write_length),
            static_cast<size_t>(proc.blocked_write_progress));

        if (step == kBlockedResult) {
            continue;
        }
        if (step < 0) {
            complete_blocked_write(proc, step);
            continue;
        }

        proc.blocked_write_progress += static_cast<uint64_t>(step);
        if (proc.blocked_write_progress >= proc.blocked_write_length) {
            complete_blocked_write(proc, static_cast<int>(proc.blocked_write_length));
        }
    }
}

process::OpenFile* fd_to_open(process::Process& proc, uint64_t fd) {
    if (fd >= process::kMaxFileHandles) {
        return nullptr;
    }
    return proc.handles[fd].file;
}

bool vnode_has_open_references(const vfs::Vnode& node) {
    for (const process::OpenFile& file : g_open_files) {
        if (file.in_use && file.kind == process::HandleKind::vnode && file.node == &node) {
            return true;
        }
    }
    return false;
}

int duplicate_fd(process::Process& proc, uint64_t oldfd) {
    process::OpenFile* file = fd_to_open(proc, oldfd);
    if (file == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }
    return allocate_fd(proc, file);
}

int duplicate_fd_to(process::Process& proc, uint64_t oldfd, uint64_t newfd) {
    process::OpenFile* file = fd_to_open(proc, oldfd);
    if (file == nullptr || newfd >= process::kMaxFileHandles) {
        return negative_error(SAVANXP_EBADF);
    }
    if (oldfd == newfd) {
        return static_cast<int>(newfd);
    }
    return install_fd(proc, static_cast<int>(newfd), file);
}

int close_fd(process::Process& proc, uint64_t fd) {
    if (fd >= process::kMaxFileHandles || proc.handles[fd].file == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }
    release_open_file(proc.handles[fd].file);
    return 0;
}

int open_node(process::Process& proc, const char* path, uint32_t flags) {
    vfs::Vnode* node = vfs::open(path, flags);
    if (node == nullptr) {
        return negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    if (node->type == vfs::NodeType::directory && (flags & SAVANXP_OPEN_WRITE) != 0) {
        return negative_error(SAVANXP_EISDIR);
    }

    process::OpenFile* file = allocate_open_file();
    if (file == nullptr) {
        return negative_error(SAVANXP_ENOMEM);
    }

    file->kind = node->backend == vfs::Backend::device ? process::HandleKind::device : process::HandleKind::vnode;
    file->flags =
        ((flags & SAVANXP_OPEN_READ) != 0 ? process::open_read : 0) |
        ((flags & SAVANXP_OPEN_WRITE) != 0 ? process::open_write : 0);
    if (file->flags == 0) {
        file->flags = process::open_read;
    }
    file->node = node;
    file->device = node->backend == vfs::Backend::device ? static_cast<device::Device*>(node->data) : nullptr;
    if ((flags & SAVANXP_OPEN_APPEND) != 0 && node->type == vfs::NodeType::file) {
        file->offset = node->size;
    }
    const int fd = allocate_fd(proc, file);
    if (fd < 0) {
        discard_open_file(file);
    }
    return fd;
}

int create_socket_fd(process::Process& proc, uint64_t domain, uint64_t type, uint64_t protocol) {
    net::Socket* socket = nullptr;
    const int create_result = net::create_socket(static_cast<uint32_t>(domain), static_cast<uint32_t>(type), static_cast<uint32_t>(protocol), socket);
    if (create_result < 0 || socket == nullptr) {
        return create_result < 0 ? create_result : negative_error(SAVANXP_ENOMEM);
    }

    process::OpenFile* file = allocate_open_file();
    if (file == nullptr) {
        net::close_socket(socket);
        return negative_error(SAVANXP_ENOMEM);
    }

    file->kind = process::HandleKind::socket;
    file->flags = process::open_read | process::open_write;
    file->socket = socket;
    const int fd = allocate_fd(proc, file);
    if (fd < 0) {
        net::close_socket(socket);
        discard_open_file(file);
    }
    return fd;
}

int bind_fd(process::Process& proc, uint64_t fd, uint64_t user_address) {
    process::OpenFile* file = fd_to_open(proc, fd);
    if (file == nullptr || file->kind != process::HandleKind::socket || file->socket == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }
    return net::bind_socket(file->socket, user_address);
}

int connect_fd(process::Process& proc, uint64_t fd, uint64_t user_address, uint32_t timeout_ms) {
    process::OpenFile* file = fd_to_open(proc, fd);
    if (file == nullptr || file->kind != process::HandleKind::socket || file->socket == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }
    return net::connect_socket(file->socket, user_address, timeout_ms);
}

int sendto_fd(process::Process& proc, uint64_t fd, uint64_t user_buffer, size_t count, uint64_t user_address) {
    process::OpenFile* file = fd_to_open(proc, fd);
    if (file == nullptr || file->kind != process::HandleKind::socket || file->socket == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }
    return net::sendto_socket(file->socket, user_buffer, count, user_address);
}

int recvfrom_fd(process::Process& proc, uint64_t fd, uint64_t user_buffer, size_t count, uint64_t user_address, uint32_t timeout_ms) {
    process::OpenFile* file = fd_to_open(proc, fd);
    if (file == nullptr || file->kind != process::HandleKind::socket || file->socket == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }
    return net::recvfrom_socket(file->socket, user_buffer, count, user_address, timeout_ms);
}

int seek_fd(process::Process& proc, uint64_t fd, int64_t offset, uint64_t whence) {
    process::OpenFile* file = fd_to_open(proc, fd);
    if (file == nullptr || file->kind != process::HandleKind::vnode || file->node == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }
    if (file->node->type != vfs::NodeType::file) {
        return negative_error(SAVANXP_EISDIR);
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
            return negative_error(SAVANXP_EINVAL);
    }

    const int64_t position = base + offset;
    if (position < 0) {
        return negative_error(SAVANXP_EINVAL);
    }

    file->offset = static_cast<size_t>(position);
    return static_cast<int>(file->offset);
}

int unlink_path(const char* path) {
    const vfs::Vnode* node = vfs::resolve(path);
    if (node == nullptr) {
        return negative_error(SAVANXP_ENOENT);
    }
    if (node->type == vfs::NodeType::directory) {
        return negative_error(SAVANXP_EISDIR);
    }
    if (vnode_has_open_references(*node)) {
        return negative_error(SAVANXP_EBUSY);
    }
    if (!vfs::unlink(path)) {
        return negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int mkdir_path(const char* path) {
    if (path == nullptr || *path == '\0') {
        return negative_error(SAVANXP_EINVAL);
    }
    if (vfs::mkdir(path) == nullptr) {
        return negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int rmdir_path(const char* path) {
    const vfs::Vnode* node = vfs::resolve(path);
    if (node == nullptr) {
        return negative_error(SAVANXP_ENOENT);
    }
    if (node->type != vfs::NodeType::directory) {
        return negative_error(SAVANXP_ENOTDIR);
    }
    if (vnode_has_open_references(*node)) {
        return negative_error(SAVANXP_EBUSY);
    }
    if (!vfs::rmdir(path)) {
        return negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int truncate_path(const char* path, uint64_t size) {
    const vfs::Vnode* node = vfs::resolve(path);
    if (node == nullptr) {
        return negative_error(SAVANXP_ENOENT);
    }
    if (node->type != vfs::NodeType::file) {
        return negative_error(SAVANXP_EISDIR);
    }
    if (!vfs::truncate(path, static_cast<size_t>(size))) {
        return negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int rename_path(const char* old_path, const char* new_path) {
    const vfs::Vnode* node = vfs::resolve(old_path);
    if (node == nullptr) {
        return negative_error(SAVANXP_ENOENT);
    }
    if (node->type == vfs::NodeType::directory && vnode_has_open_references(*node)) {
        return negative_error(SAVANXP_EBUSY);
    }
    if (!vfs::rename(old_path, new_path)) {
        return negative_error(static_cast<savanxp_error_code>(vfs::last_error()));
    }
    return 0;
}

int stat_path(process::Process& proc, const char* path, uint64_t user_stat_address) {
    if (!vm::is_user_range_accessible(proc.address_space, user_stat_address, sizeof(savanxp_stat), true)) {
        return negative_error(SAVANXP_EINVAL);
    }

    const vfs::Vnode* node = vfs::resolve(path);
    if (node == nullptr) {
        return negative_error(SAVANXP_ENOENT);
    }

    savanxp_stat stat = {};
    fill_stat_for_vnode(*node, stat);
    return write_to_process_memory(proc, user_stat_address, &stat, sizeof(stat))
        ? 0
        : negative_error(SAVANXP_EINVAL);
}

int fstat_fd(process::Process& proc, uint64_t fd, uint64_t user_stat_address) {
    process::OpenFile* file = fd_to_open(proc, fd);
    if (file == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_stat_address, sizeof(savanxp_stat), true)) {
        return negative_error(SAVANXP_EINVAL);
    }

    savanxp_stat stat = {};
    fill_stat_for_open_file(*file, stat);
    return write_to_process_memory(proc, user_stat_address, &stat, sizeof(stat))
        ? 0
        : negative_error(SAVANXP_EINVAL);
}

int chdir_path(process::Process& proc, const char* path) {
    const vfs::Vnode* node = vfs::resolve(path);
    if (node == nullptr) {
        return negative_error(SAVANXP_ENOENT);
    }
    if (node->type != vfs::NodeType::directory) {
        return negative_error(SAVANXP_ENOTDIR);
    }
    if (strlen(path) >= sizeof(proc.cwd)) {
        return negative_error(SAVANXP_EINVAL);
    }
    strcpy(proc.cwd, path);
    return 0;
}

int getcwd_path(process::Process& proc, uint64_t user_buffer, size_t capacity) {
    const size_t length = strlen(proc.cwd) + 1;
    if (capacity < length || !vm::is_user_range_accessible(proc.address_space, user_buffer, capacity, true)) {
        return negative_error(SAVANXP_EINVAL);
    }
    return write_to_process_memory(proc, user_buffer, proc.cwd, length)
        ? static_cast<int>(length - 1)
        : negative_error(SAVANXP_EINVAL);
}

int readdir_node(process::Process& proc, uint64_t fd, uint64_t user_buffer, size_t capacity) {
    process::OpenFile* file = fd_to_open(proc, fd);
    if (file == nullptr || file->kind != process::HandleKind::vnode || file->node == nullptr ||
        file->node->type != vfs::NodeType::directory || capacity == 0) {
        return negative_error(SAVANXP_EBADF);
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_buffer, capacity, true)) {
        return negative_error(SAVANXP_EINVAL);
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
        return negative_error(SAVANXP_EINVAL);
    }
    char zero = '\0';
    (void)write_to_process_memory(proc, user_buffer + to_copy, &zero, 1);
    return static_cast<int>(to_copy);
}

int ioctl_handle(process::Process& proc, uint64_t fd, uint64_t request, uint64_t argument) {
    (void)proc;
    process::OpenFile* file = fd_to_open(proc, fd);
    if (file == nullptr || file->kind != process::HandleKind::device || file->device == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }
    return device::ioctl(file->device, request, argument);
}

int read_handle(process::Process& proc, uint64_t fd, uint64_t user_buffer, size_t count) {
    process::OpenFile* file = fd_to_open(proc, fd);
    if (file == nullptr || count == 0) {
        return file == nullptr ? negative_error(SAVANXP_EBADF) : 0;
    }
    if ((file->flags & process::open_read) == 0) {
        return negative_error(SAVANXP_EBADF);
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_buffer, count, true)) {
        return negative_error(SAVANXP_EINVAL);
    }

    switch (file->kind) {
        case process::HandleKind::tty:
            if (tty::main().line_ready) {
                const size_t to_copy = tty::main().pending_length < count ? tty::main().pending_length : count;
                if (!write_to_process_memory(proc, user_buffer, tty::main().pending_line, to_copy)) {
                    return negative_error(SAVANXP_EINVAL);
                }
                memset(tty::main().pending_line, 0, sizeof(tty::main().pending_line));
                tty::main().pending_length = 0;
                tty::main().line_ready = false;
                return static_cast<int>(to_copy);
            }
            proc.blocked_io_fd = fd;
            proc.blocked_read_buffer = user_buffer;
            proc.blocked_read_capacity = count;
            proc.state = process::State::blocked_read;
            return kBlockedResult;
        case process::HandleKind::vnode: {
            if (file->node == nullptr || file->node->type != vfs::NodeType::file) {
                return negative_error(SAVANXP_EBADF);
            }
            uint8_t scratch[kPipeChunkSize] = {};
            size_t total = 0;
            while (total < count) {
                const size_t step = (count - total) < sizeof(scratch) ? (count - total) : sizeof(scratch);
                const size_t consumed = vfs::read(*file->node, file->offset, scratch, step);
                if (consumed == 0) {
                    break;
                }
                if (!write_to_process_memory(proc, user_buffer + total, scratch, consumed)) {
                    return negative_error(SAVANXP_EINVAL);
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
            int result = try_pipe_read(proc, *file, user_buffer, count);
            if (result == kBlockedResult) {
                proc.blocked_io_fd = fd;
                proc.blocked_read_buffer = user_buffer;
                proc.blocked_read_capacity = count;
                proc.state = process::State::blocked_read;
            }
            return result;
        }
        case process::HandleKind::device:
            return device::read(file->device, user_buffer, count);
        case process::HandleKind::socket:
            return net::read_socket(file->socket, user_buffer, count);
        default:
            return negative_error(SAVANXP_EBADF);
    }
}

int write_handle(process::Process& proc, uint64_t fd, uint64_t user_buffer, size_t count) {
    process::OpenFile* file = fd_to_open(proc, fd);
    if (file == nullptr || count == 0) {
        return file == nullptr ? negative_error(SAVANXP_EBADF) : 0;
    }
    if ((file->flags & process::open_write) == 0) {
        return negative_error(SAVANXP_EBADF);
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_buffer, count, false)) {
        return negative_error(SAVANXP_EINVAL);
    }

    switch (file->kind) {
        case process::HandleKind::tty: {
            char scratch[kPipeChunkSize] = {};
            size_t written = 0;
            while (written < count) {
                const size_t step = (count - written) < sizeof(scratch) ? (count - written) : sizeof(scratch);
                if (!read_from_process_memory(proc, scratch, user_buffer + written, step)) {
                    return negative_error(SAVANXP_EINVAL);
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
                return negative_error(SAVANXP_EBADF);
            }

            uint8_t scratch[kPipeChunkSize] = {};
            size_t written = 0;
            while (written < count) {
                const size_t step = (count - written) < sizeof(scratch) ? (count - written) : sizeof(scratch);
                if (!read_from_process_memory(proc, scratch, user_buffer + written, step)) {
                    return negative_error(SAVANXP_EINVAL);
                }
                const size_t produced = vfs::write(*file->node, file->offset, scratch, step, false);
                if (produced == 0 && step != 0) {
                    const int error = vfs::last_error();
                    return written != 0
                        ? static_cast<int>(written)
                        : negative_error(static_cast<savanxp_error_code>(error != 0 ? error : SAVANXP_EIO));
                }
                file->offset += produced;
                written += produced;
                if (produced < step) {
                    break;
                }
            }
            return static_cast<int>(written);
        }
        case process::HandleKind::pipe: {
            const int step = pipe_write_chunk(proc, *file->pipe, user_buffer, count, 0);
            if (step == kBlockedResult) {
                proc.blocked_io_fd = fd;
                proc.blocked_write_buffer = user_buffer;
                proc.blocked_write_length = count;
                proc.blocked_write_progress = 0;
                proc.state = process::State::blocked_write;
                return kBlockedResult;
            }
            if (step < 0) {
                return step;
            }

            if (static_cast<size_t>(step) == count) {
                return step;
            }

            proc.blocked_io_fd = fd;
            proc.blocked_write_buffer = user_buffer;
            proc.blocked_write_length = count;
            proc.blocked_write_progress = static_cast<uint64_t>(step);
            proc.state = process::State::blocked_write;
            return kBlockedResult;
        }
        case process::HandleKind::device:
            return device::write(file->device, user_buffer, count);
        case process::HandleKind::socket:
            return net::write_socket(file->socket, user_buffer, count);
        default:
            return negative_error(SAVANXP_EBADF);
    }
}

int create_pipe(process::Process& proc, uint64_t user_fd_array) {
    if (!vm::is_user_range_accessible(proc.address_space, user_fd_array, sizeof(int) * 2, true)) {
        return negative_error(SAVANXP_EINVAL);
    }

    process::Pipe* pipe = allocate_pipe();
    if (pipe == nullptr) {
        return negative_error(SAVANXP_ENOMEM);
    }

    process::OpenFile* reader = allocate_open_file();
    process::OpenFile* writer = allocate_open_file();
    if (reader == nullptr || writer == nullptr) {
        discard_open_file(reader);
        discard_open_file(writer);
        return negative_error(SAVANXP_ENOMEM);
    }

    reader->kind = process::HandleKind::pipe;
    reader->flags = process::open_read;
    reader->pipe = pipe;
    writer->kind = process::HandleKind::pipe;
    writer->flags = process::open_write;
    writer->pipe = pipe;
    pipe->reader_refs = 1;
    pipe->writer_refs = 1;

    const int read_fd = allocate_fd(proc, reader);
    if (read_fd < 0) {
        discard_open_file(reader);
        discard_open_file(writer);
        memset(pipe, 0, sizeof(*pipe));
        return read_fd;
    }
    const int write_fd = allocate_fd(proc, writer);
    if (write_fd < 0) {
        (void)close_fd(proc, read_fd);
        discard_open_file(writer);
        return write_fd;
    }

    const int values[2] = {read_fd, write_fd};
    if (!write_to_process_memory(proc, user_fd_array, values, sizeof(values))) {
        (void)close_fd(proc, read_fd);
        (void)close_fd(proc, write_fd);
        return negative_error(SAVANXP_EINVAL);
    }
    return 0;
}

bool copy_spawn_arguments(
    const process::Process& proc,
    const process::SavedContext& context,
    char* path,
    size_t path_capacity,
    char argv_storage[16][64],
    const char* argv_local[16],
    int& argc
) {
    argc = static_cast<int>(context.rdx < 15 ? context.rdx : 15);
    if (!copy_user_string(proc, context.rdi, path, path_capacity)) {
        return false;
    }

    if (argc != 0 && !vm::is_user_range_accessible(proc.address_space, context.rsi, static_cast<size_t>(argc) * sizeof(uint64_t), false)) {
        return false;
    }

    for (int index = 0; index < argc; ++index) {
        uint64_t arg_address = 0;
        if (!read_user_pointer(proc, context.rsi + static_cast<uint64_t>(index) * sizeof(uint64_t), arg_address)) {
            return false;
        }
        if (!copy_user_string(proc, arg_address, argv_storage[index], sizeof(argv_storage[index]))) {
            return false;
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
    memset(g_pipe_storage, 0, sizeof(g_pipe_storage));
    memset(g_open_files, 0, sizeof(g_open_files));
    memset(&g_boot_system_info, 0, sizeof(g_boot_system_info));
    g_current = nullptr;
    g_idle = nullptr;
    g_next_pid = 1;
    g_schedule_cursor = 0;
    g_boot_system_info_ready = false;
    g_ready = true;
}

bool ready() {
    return g_ready;
}

Process* current() {
    return g_current;
}

uint32_t current_pid() {
    return g_current != nullptr ? g_current->pid : 0;
}

Process* find(uint32_t pid) {
    for (Process& proc : g_processes) {
        if (proc.state != State::unused && proc.pid == pid) {
            return &proc;
        }
    }
    return nullptr;
}

bool snapshot_process(size_t index, savanxp_process_info& info) {
    size_t visible = 0;
    for (const Process& proc : g_processes) {
        if (proc.state == State::unused) {
            continue;
        }
        if (visible++ != index) {
            continue;
        }

        memset(&info, 0, sizeof(info));
        info.pid = proc.pid;
        info.parent_pid = proc.parent_pid;
        info.exit_code = proc.exit_code;
        info.state = exported_state(proc.state);
        memcpy(info.name, proc.name, sizeof(info.name));
        return true;
    }
    return false;
}

void set_boot_system_info(const savanxp_system_info& info) {
    memcpy(&g_boot_system_info, &info, sizeof(g_boot_system_info));
    g_boot_system_info_ready = true;
}

bool snapshot_system_info(savanxp_system_info& info) {
    memset(&info, 0, sizeof(info));
    if (g_boot_system_info_ready) {
        memcpy(&info, &g_boot_system_info, sizeof(info));
    }

    info.input_ready = ps2::ready() ? 1u : 0u;
    info.framebuffer_ready = ui::framebuffer_available() ? 1u : 0u;
    info.net_present = net::present() ? 1u : 0u;
    info.speaker_ready = pcspeaker::ready() ? 1u : 0u;
    info.block_ready = block::ready() ? 1u : 0u;
    info.svfs_mounted = svfs::mounted() ? 1u : 0u;
    info.timer_backend = exported_timer_backend(timer::backend());
    info.timer_frequency_hz = timer::frequency_hz();
    info.pci_device_count = static_cast<uint32_t>(pci::device_count());
    info.svfs_file_count = static_cast<uint32_t>(svfs::file_count());
    info.memory_total_pages = memory::total_page_count();
    info.uptime_ms = current_uptime_ms();
    return true;
}

bool copy_from_user(void* destination, uint64_t user_address, size_t count) {
    if (g_current == nullptr) {
        return false;
    }
    return read_from_process_memory(*g_current, destination, user_address, count);
}

bool copy_to_user(uint64_t user_address, const void* source, size_t count) {
    if (g_current == nullptr) {
        return false;
    }
    return write_to_process_memory(*g_current, user_address, source, count);
}

bool validate_user_range(uint64_t user_address, size_t count, bool require_write) {
    if (g_current == nullptr) {
        return false;
    }
    return vm::is_user_range_accessible(g_current->address_space, user_address, count, require_write);
}

Process* create_user_process(const char* path, int argc, const char* const* argv, uint32_t parent_pid) {
    return create_process_internal(path, argc, argv, find(parent_pid), -1, -1);
}

[[noreturn]] void start_init(const char* path) {
    g_idle = create_idle_process();
    if (g_idle == nullptr) {
        panic("scheduler: unable to create idle task");
    }

    const char* argv[] = {path, nullptr};
    Process* init = create_process_internal(path, 1, argv, nullptr, -1, -1);
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
    const uint32_t exiting_pid = exiting->pid;
    proc_log("proc: exit pid=%u status=%d\n", exiting->pid, exit_code);
    exiting->exit_code = exit_code;
    release_all_handles(*exiting);
    exiting->state = State::zombie;
    wake_waiting_parent(*exiting);
    reparent_orphaned_children(exiting_pid);

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
            const int result = read_handle(*g_current, context->rdi, context->rsi, static_cast<size_t>(context->rdx));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_WRITE: {
            const int result = write_handle(*g_current, context->rdi, context->rsi, static_cast<size_t>(context->rdx));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_OPEN: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(open_node(*g_current, resolved, static_cast<uint32_t>(context->rsi)));
            return context;
        }
        case SAVANXP_SYS_CLOSE:
            context->rax = static_cast<uint64_t>(close_fd(*g_current, context->rdi));
            return context;
        case SAVANXP_SYS_READDIR:
            context->rax = static_cast<uint64_t>(readdir_node(*g_current, context->rdi, context->rsi, static_cast<size_t>(context->rdx)));
            return context;
        case SAVANXP_SYS_SPAWN:
        case SAVANXP_SYS_SPAWN_FD: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            char argv_storage[16][64] = {};
            const char* argv_local[16] = {};
            int argc = 0;
            if (!copy_spawn_arguments(*g_current, *context, path, sizeof(path), argv_storage, argv_local, argc) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            int stdin_fd = -1;
            int stdout_fd = -1;
            if (context->rax == SAVANXP_SYS_SPAWN_FD) {
                stdin_fd = static_cast<int>(context->r10);
                stdout_fd = static_cast<int>(context->r8);
            }

            Process* child = create_process_internal(resolved, argc, argv_local, g_current, stdin_fd, stdout_fd);
            context->rax = static_cast<uint64_t>(child != nullptr ? static_cast<int>(child->pid) : negative_error(SAVANXP_ENOENT));
            return context;
        }
        case SAVANXP_SYS_EXEC: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            char argv_storage[16][64] = {};
            const char* argv_local[16] = {};
            int argc = 0;
            if (!copy_spawn_arguments(*g_current, *context, path, sizeof(path), argv_storage, argv_local, argc) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            if (!prepare_exec_image(*g_current, resolved, argc, argv_local)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ENOENT));
                return context;
            }

            switch_to_process(g_current);
            return g_current->context;
        }
        case SAVANXP_SYS_WAITPID: {
            const uint32_t waited_pid = static_cast<int64_t>(context->rdi) == -1 ? kWaitAnyPid : static_cast<uint32_t>(context->rdi);
            Process* child = find_zombie_child(g_current->pid, waited_pid);
            if (child != nullptr) {
                write_int_to_process_memory(*g_current, context->rsi, child->exit_code);
                const int pid = static_cast<int>(child->pid);
                reap_process(*child);
                context->rax = static_cast<uint64_t>(pid);
                return context;
            }

            if (waited_pid != kWaitAnyPid && child_of(g_current->pid, waited_pid) == nullptr) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ECHILD));
                return context;
            }
            if (waited_pid == kWaitAnyPid && !has_child(g_current->pid)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ECHILD));
                return context;
            }

            g_current->waiting_for_pid = waited_pid;
            g_current->wait_status_address = context->rsi;
            g_current->state = State::blocked_wait;
            return choose_next_context(context);
        }
        case SAVANXP_SYS_EXIT:
            terminate_current(static_cast<int>(context->rdi));
            return nullptr;
        case SAVANXP_SYS_YIELD:
            g_current->state = State::ready;
            context->rax = 0;
            return choose_next_context(context);
        case SAVANXP_SYS_UPTIME_MS:
            context->rax = current_uptime_ms();
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
            context->rax = static_cast<uint64_t>(create_pipe(*g_current, context->rdi));
            return context;
        case SAVANXP_SYS_DUP:
            context->rax = static_cast<uint64_t>(duplicate_fd(*g_current, context->rdi));
            return context;
        case SAVANXP_SYS_DUP2:
            context->rax = static_cast<uint64_t>(duplicate_fd_to(*g_current, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_PROC_INFO: {
            if (!vm::is_user_range_accessible(g_current->address_space, context->rsi, sizeof(savanxp_process_info), true)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            savanxp_process_info info = {};
            if (!snapshot_process(static_cast<size_t>(context->rdi), info)) {
                context->rax = 0;
                return context;
            }
            if (!write_to_process_memory(*g_current, context->rsi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = 1;
            return context;
        }
        case SAVANXP_SYS_SEEK:
            context->rax = static_cast<uint64_t>(seek_fd(*g_current, context->rdi, static_cast<int64_t>(context->rsi), context->rdx));
            return context;
        case SAVANXP_SYS_UNLINK: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(unlink_path(resolved));
            return context;
        }
        case SAVANXP_SYS_MKDIR: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(mkdir_path(resolved));
            return context;
        }
        case SAVANXP_SYS_RMDIR: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(rmdir_path(resolved));
            return context;
        }
        case SAVANXP_SYS_TRUNCATE: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(truncate_path(resolved, context->rsi));
            return context;
        }
        case SAVANXP_SYS_RENAME: {
            char old_path[process::kProcessPathLength] = {};
            char new_path[process::kProcessPathLength] = {};
            char resolved_old[process::kProcessPathLength] = {};
            char resolved_new[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, old_path, sizeof(old_path)) ||
                !copy_user_string(*g_current, context->rsi, new_path, sizeof(new_path)) ||
                !resolve_process_path(*g_current, old_path, resolved_old, sizeof(resolved_old)) ||
                !resolve_process_path(*g_current, new_path, resolved_new, sizeof(resolved_new))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(rename_path(resolved_old, resolved_new));
            return context;
        }
        case SAVANXP_SYS_IOCTL:
            context->rax = static_cast<uint64_t>(ioctl_handle(*g_current, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_SOCKET:
            context->rax = static_cast<uint64_t>(create_socket_fd(*g_current, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_BIND:
            context->rax = static_cast<uint64_t>(bind_fd(*g_current, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_SENDTO:
            context->rax = static_cast<uint64_t>(sendto_fd(*g_current, context->rdi, context->rsi, static_cast<size_t>(context->rdx), context->r10));
            return context;
        case SAVANXP_SYS_RECVFROM:
            context->rax = static_cast<uint64_t>(recvfrom_fd(
                *g_current,
                context->rdi,
                context->rsi,
                static_cast<size_t>(context->rdx),
                context->r10,
                static_cast<uint32_t>(context->r8)));
            return context;
        case SAVANXP_SYS_CONNECT:
            context->rax = static_cast<uint64_t>(connect_fd(*g_current, context->rdi, context->rsi, static_cast<uint32_t>(context->rdx)));
            return context;
        case SAVANXP_SYS_GETPID:
            context->rax = current_pid();
            return context;
        case SAVANXP_SYS_STAT: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(stat_path(*g_current, resolved, context->rsi));
            return context;
        }
        case SAVANXP_SYS_FSTAT:
            context->rax = static_cast<uint64_t>(fstat_fd(*g_current, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_CHDIR: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(chdir_path(*g_current, resolved));
            return context;
        }
        case SAVANXP_SYS_GETCWD:
            context->rax = static_cast<uint64_t>(getcwd_path(*g_current, context->rdi, static_cast<size_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_SYSTEM_INFO: {
            if (!vm::is_user_range_accessible(g_current->address_space, context->rdi, sizeof(savanxp_system_info), true)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            savanxp_system_info info = {};
            if (!snapshot_system_info(info) ||
                !write_to_process_memory(*g_current, context->rdi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SAVANXP_SYS_SYNC:
            context->rax = static_cast<uint64_t>(svfs::sync() ? 0 : negative_error(SAVANXP_EIO));
            return context;
        default:
            context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ENOSYS));
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
            g_current->state = State::ready;
            return choose_next_context(context);
        }
        return context;
    }

    if (g_current->time_slice > 1) {
        g_current->time_slice -= 1;
        return context;
    }

    g_current->state = State::ready;
    return choose_next_context(context);
}

void notify_tty_line_ready() {
    for (Process& proc : g_processes) {
        if (proc.state != State::blocked_read) {
            continue;
        }
        if (proc.blocked_io_fd >= kMaxFileHandles) {
            continue;
        }

        OpenFile* file = proc.handles[proc.blocked_io_fd].file;
        if (file == nullptr || file->kind != HandleKind::tty) {
            continue;
        }

        const size_t to_copy = tty::main().pending_length < proc.blocked_read_capacity
            ? tty::main().pending_length
            : static_cast<size_t>(proc.blocked_read_capacity);
        if (!write_to_process_memory(proc, proc.blocked_read_buffer, tty::main().pending_line, to_copy)) {
            complete_blocked_read(proc, negative_error(SAVANXP_EINVAL));
        } else {
            memset(tty::main().pending_line, 0, sizeof(tty::main().pending_line));
            tty::main().pending_length = 0;
            tty::main().line_ready = false;
            complete_blocked_read(proc, static_cast<int>(to_copy));
        }
        break;
    }
}

} // namespace process

extern "C" process::SavedContext* savanxp_handle_syscall(process::SavedContext* context) {
    return process::handle_syscall(context);
}
