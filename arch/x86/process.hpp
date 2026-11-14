#pragma once

#include <stdint.h>
#include "context.hpp"

namespace i386::process {

constexpr uint32_t kCapacity = 6;
enum class State { unused, runnable, running, sleeping, reading, waiting, zombie };

// The process table is bounded; user images and page tables use the reference
// physical allocator. Kernel stacks and saved CPU/FPU contexts stay per slot.
struct alignas(4096) Process {
    State state;
    uint32_t pid, parent;
    int exit_status;
    ContextFrame* frame;
    FpuState fpu;
    uint64_t wake_ms;
    int wait_pid;
    uint32_t wait_status;
    uint32_t input_buffer;
    uint32_t preemptions;
    alignas(4096) uint32_t directory[1024];
    alignas(16) uint8_t kernel_stack[16384];
};

void initialize();
long spawn(const char* path, const char* const* argv, uint32_t argc);
[[noreturn]] void start();
ContextFrame* timer(ContextFrame* frame);
ContextFrame* finish_syscall(ContextFrame* frame, bool reschedule = false);
uint32_t current_pid();
uint32_t count();
bool user_range(uint32_t address, uint32_t size, bool writable = false);
ContextFrame* fault(uint32_t vector);
void read_input(uint32_t buffer);
void sleep(uint32_t milliseconds);
void yield();
long wait(int pid, uint32_t status);
void exit(int status);

} // namespace i386::process
