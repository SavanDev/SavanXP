#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/vfs.hpp"
#include "kernel/vmm.hpp"

namespace process {

constexpr size_t kMaxProcesses = 16;
constexpr size_t kMaxFileHandles = 16;

enum class State : uint8_t {
    unused = 0,
    runnable = 1,
    running = 2,
    blocked_read = 3,
    blocked_wait = 4,
    sleeping = 5,
    exited = 6,
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
    pipe_read = 3,
    pipe_write = 4,
};

struct FileHandle {
    bool in_use;
    HandleKind kind;
    vfs::Vnode* node;
    void* pipe;
    size_t offset;
    size_t iterator_index;
};

struct Process {
    uint32_t pid;
    uint32_t parent_pid;
    State state;
    bool idle;
    int exit_code;
    uint32_t waiting_for_pid;
    uint64_t wait_status_address;
    uint64_t blocked_read_buffer;
    uint64_t blocked_read_capacity;
    uint64_t blocked_read_fd;
    uint64_t wake_tick;
    uint32_t time_slice;
    vm::VmSpace address_space;
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_size;
    SavedContext* context;
    FileHandle handles[kMaxFileHandles];
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

} // namespace process
