#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/vfs.hpp"
#include "kernel/vmm.hpp"
#include "shared/syscall.h"

namespace process {

constexpr size_t kMaxProcesses = 16;
constexpr size_t kMaxFileHandles = 16;
constexpr size_t kMaxOpenFiles = 64;
constexpr size_t kProcessNameLength = 32;

enum class State : uint8_t {
    unused = 0,
    ready = 1,
    running = 2,
    blocked_read = 3,
    blocked_write = 4,
    blocked_wait = 5,
    sleeping = 6,
    zombie = 7,
};

struct SavedContext {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

enum class HandleKind : uint8_t {
    none = 0,
    tty = 1,
    vnode = 2,
    pipe = 3,
};

enum OpenFlags : uint32_t {
    open_read = 1u << 0,
    open_write = 1u << 1,
};

struct Pipe {
    bool in_use;
    uint32_t reader_refs;
    uint32_t writer_refs;
    size_t read_pos;
    size_t write_pos;
    size_t size;
    uint8_t* buffer;
};

struct OpenFile {
    bool in_use;
    HandleKind kind;
    uint32_t flags;
    uint32_t refcount;
    vfs::Vnode* node;
    Pipe* pipe;
    size_t offset;
    size_t iterator_index;
};

struct FdEntry {
    OpenFile* file;
};

struct Process {
    uint32_t pid;
    uint32_t parent_pid;
    State state;
    bool idle;
    int exit_code;
    uint32_t waiting_for_pid;
    uint64_t wait_status_address;
    uint64_t blocked_io_fd;
    uint64_t blocked_read_buffer;
    uint64_t blocked_read_capacity;
    uint64_t blocked_write_buffer;
    uint64_t blocked_write_length;
    uint64_t blocked_write_progress;
    uint64_t wake_tick;
    uint32_t time_slice;
    char name[kProcessNameLength];
    vm::VmSpace address_space;
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_size;
    SavedContext* context;
    FdEntry handles[kMaxFileHandles];
};

void initialize();
bool ready();
Process* current();
Process* find(uint32_t pid);
Process* create_user_process(const char* path, int argc, const char* const* argv, uint32_t parent_pid);
[[noreturn]] void start_init(const char* path);
void terminate_current(int exit_code);
void terminate_current_from_exception(uint8_t vector);
SavedContext* handle_syscall(SavedContext* context);
SavedContext* handle_timer_tick(SavedContext* context);
void notify_tty_line_ready();
bool snapshot_process(size_t index, savanxp_process_info& info);

} // namespace process
