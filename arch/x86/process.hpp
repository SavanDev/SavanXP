#pragma once

#include <stdint.h>
#include "context.hpp"
#include "kernel/object.hpp"
#include "kernel/io_types.hpp"
#include "savanxp/syscall.h"

namespace i386::process {

constexpr uint32_t kCapacity = 64;
enum class State { unused, runnable, running, sleeping, reading, writing, polling, waiting, zombie, object_wait };

// The process table is bounded; user images and page tables use the reference
// physical allocator. Kernel stacks and saved CPU/FPU contexts stay per slot.
struct alignas(4096) Process {
    State state;
    uint32_t pid, parent;
    bool native_subsystem;
    bool idle;
    uint32_t cpu;
    int exit_status;
    ContextFrame* frame;
    FpuState fpu;
    uint64_t wake_ms;
    int wait_pid;
    uint32_t wait_status;
    uint32_t io_fd, io_buffer, io_size, io_progress;
    uint32_t io_operation, io_peer;
    uint32_t object_count;
    bool wait_all;
    object::Header* wait_objects[16];
    uint32_t poll_buffer, poll_count;
    savanxp_pollfd poll_entries[64];
    ::process::HandleEntry handles[64];
    struct SectionView {
        uint32_t base, size, access;
        object::SectionObject* section;
        bool share_on_fork;
    };
    SectionView views[64];
    uint32_t next_view_base;
    char cwd[256], name[32];
    uint32_t preemptions;
    uint64_t cpu_ticks;
    uint32_t pending_signals;
    uint32_t last_signal;
    int pending_exit;
    // Retained C++ work stays pinned through syscall-return cleanup.
    bool in_syscall;
    bool kernel_kill_resumed;
    uint32_t wait_depth;
    uint32_t kernel_suspends, kernel_resumes;
    alignas(4096) uint32_t directory[1024];
    alignas(16) uint8_t kernel_stack[16384];
};

void initialize();
long spawn(const char* path, const char* const* argv, uint32_t argc, int stdin_fd = 0, int stdout_fd = 1);
long fork(const ContextFrame& frame);
[[noreturn]] void start();
[[noreturn]] void start_secondary();
ContextFrame* timer(ContextFrame* frame);
ContextFrame* reschedule(ContextFrame* frame, bool ipi = false);
uint64_t total_cpu_ticks();
void begin_syscall();
void note_woken(Process& task);
ContextFrame* finish_syscall(ContextFrame* frame, bool reschedule = false);
uint32_t current_pid();
uint32_t count();
bool user_range(uint32_t address, uint32_t size, bool writable = false);
ContextFrame* fault(uint32_t vector);
Process* current();
Process* idle_for_cpu(uint32_t index);
Process* at(uint32_t slot);
int replace_image(const char* path, const char* const* argv, uint32_t argc, ContextFrame& frame);
void sleep(uint32_t milliseconds);
void yield();
long wait(int pid, uint32_t status);
void exit(int status);
long kill(int pid, int signal);

} // namespace i386::process
