#include "process.hpp"

#include "cpu.hpp"
#include "kernel/string.hpp"
#include "paging.hpp"
#include "pit.hpp"
#include "ps2_32.hpp"
#include "savanxp/syscall.h"
#include "serial.hpp"
#include "user32.hpp"

namespace i386::process {
namespace {
constexpr uint32_t kImageBytes = paging::kUserWindowEnd - paging::kUserWindowBase;
constexpr uint32_t kQuantumTicks = 2;
constexpr uint32_t kStackCanary = 0x51a7c0de;
alignas(4194304) uint8_t g_images[kCapacity][kImageBytes];
Process g_processes[kCapacity];
Process* g_current = nullptr;
uint32_t g_next_pid = 1;
uint32_t g_last_slot = kCapacity - 1;
uint32_t g_slice_tick = 0;
alignas(16) uint8_t g_idle_stack[16384];
ContextFrame* g_idle_frame;

[[noreturn]] void idle() {
    for (;;) {
        asm volatile("sti; hlt" ::: "memory");
    }
}

uint32_t slot(const Process& task) {
    return static_cast<uint32_t>(&task - g_processes);
}

void wake_tasks() {
    const uint64_t now = pit::uptime_ms();
    for (auto& task : g_processes) {
        if (task.state == State::sleeping && task.wake_ms <= now) {
            task.state = State::runnable;
        }
        if (task.state == State::reading) {
            savanxp_input_event event = {};
            if (ps2_32::pop_event(event)) {
                memcpy(user_pointer(task, task.input_buffer), &event, sizeof(event));
                task.frame->eax = sizeof(event);
                task.state = State::runnable;
            }
        }
    }
}

ContextFrame* schedule(ContextFrame* frame) {
    Process* previous = g_current;
    if (previous != nullptr &&
        *reinterpret_cast<const uint32_t*>(previous->kernel_stack) != kStackCanary) {
        serial::write("process: kernel stack overflow\n");
        cpu::halt_forever();
    }
    if (previous != nullptr) {
        previous->frame = frame;
        if (previous->state == State::running) {
            previous->state = State::runnable;
        }
    } else {
        g_idle_frame = frame;
    }
    Process* next = nullptr;
    for (uint32_t offset = 1; offset <= kCapacity; ++offset) {
        Process& candidate = g_processes[(g_last_slot + offset) % kCapacity];
        if (candidate.state == State::runnable) {
            next = &candidate;
            break;
        }
    }
    if (previous != next) {
        if (previous != nullptr && previous->state != State::unused &&
            previous->state != State::zombie) {
            save_fpu_state(previous->fpu);
        }
        if (next != nullptr) {
            restore_fpu_state(next->fpu);
            paging::activate(next->directory);
            cpu::set_ring0_stack(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                next->kernel_stack + sizeof(next->kernel_stack))));
        } else {
            paging::activate_kernel();
        }
    }
    g_current = next;
    g_slice_tick = pit::ticks();
    if (next == nullptr) {
        return g_idle_frame;
    }
    next->state = State::running;
    g_last_slot = slot(*next);
    return next->frame;
}

bool matches(const Process& child, const Process& parent, int pid) {
    return child.state != State::unused && child.parent == parent.pid &&
           (pid == SAVANXP_WAIT_ANY || child.pid == static_cast<uint32_t>(pid));
}

} // namespace

void initialize() {
    const uintptr_t end = reinterpret_cast<uintptr_t>(g_images) + sizeof(g_images);
    if (end > paging::kUserWindowBase) {
        serial::write("process: image storage overlaps the user window\n");
        cpu::halt_forever();
    }
    cpu::enable_fpu();
    g_idle_frame = reinterpret_cast<ContextFrame*>(g_idle_stack + sizeof(g_idle_stack) -
                                                  sizeof(ContextFrame));
    *g_idle_frame = {};
    g_idle_frame->gs = g_idle_frame->fs = g_idle_frame->es = g_idle_frame->ds =
        cpu::kKernelDataSelector;
    g_idle_frame->eip = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&idle));
    g_idle_frame->cs = cpu::kKernelCodeSelector;
    g_idle_frame->eflags = 0x202;
    serial::write("process: private stacks, address spaces and FXSAVE ready\n");
}

void* user_pointer(Process& task, uint32_t address) {
    return g_images[slot(task)] + (address - paging::kUserWindowBase);
}

bool user_range(uint32_t address, uint32_t size) {
    return g_current != nullptr && address >= paging::kUserWindowBase &&
        address < paging::kUserWindowEnd &&
        static_cast<uint64_t>(address) + size <= paging::kUserWindowEnd;
}

long spawn(const char* path, const char* const* argv, uint32_t argc) {
    for (auto& task : g_processes) {
        if (task.state != State::unused) {
            continue;
        }
        if (g_next_pid >= 0x80000000u) {
            return -SAVANXP_EAGAIN;
        }
        memset(&task, 0, sizeof(task));
        *reinterpret_cast<uint32_t*>(task.kernel_stack) = kStackCanary;
        task.frame = reinterpret_cast<ContextFrame*>(task.kernel_stack + sizeof(task.kernel_stack) -
                                                     sizeof(ContextFrame));
        const int error = user32::load(path, g_images[slot(task)], *task.frame, argc, argv);
        if (error != 0) {
            return -error;
        }
        paging::initialize_process_directory(task.directory,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_images[slot(task)])));
        initialize_fpu_state(task.fpu);
        task.pid = g_next_pid++;
        task.parent = current_pid();
        task.state = State::runnable;
        return task.pid;
    }
    return -SAVANXP_EAGAIN;
}

[[noreturn]] void start() {
    ps2_32::clear_queue();
    serial::write("scheduler: PIT round-robin, 20 ms quantum\n");
    i386_resume(schedule(g_idle_frame));
}

ContextFrame* timer(ContextFrame* frame) {
    // Kernel execution is atomic with respect to scheduling. Blocking syscalls
    // return to this scheduler instead of retaining C++ continuations on stacks.
    if (g_current != nullptr && (frame->cs & 3u) == 0) {
        return frame;
    }
    wake_tasks();
    if (g_current == nullptr || pit::ticks() - g_slice_tick >= kQuantumTicks) {
        if (g_current != nullptr) {
            ++g_current->preemptions;
        }
        return schedule(frame);
    }
    return frame;
}

ContextFrame* finish_syscall(ContextFrame* frame, bool reschedule) {
    g_current->frame = frame;
    if (reschedule || g_current->state != State::running) {
        return schedule(frame);
    }
    return frame;
}

uint32_t current_pid() { return g_current == nullptr ? 0 : g_current->pid; }

uint32_t count() {
    uint32_t total = 0;
    for (const auto& task : g_processes) {
        if (task.state != State::unused && task.state != State::zombie) {
            ++total;
        }
    }
    return total;
}

void read_input(uint32_t buffer) {
    g_current->input_buffer = buffer;
    g_current->state = State::reading;
}

void sleep(uint32_t milliseconds) {
    if (milliseconds != 0) {
        g_current->wake_ms = pit::uptime_ms() + milliseconds;
        g_current->state = State::sleeping;
    } else {
        yield();
    }
}

void yield() { g_current->state = State::runnable; }

long wait(int pid, uint32_t status) {
    if (pid != SAVANXP_WAIT_ANY && pid <= 0) {
        return -SAVANXP_EINVAL;
    }
    bool child_exists = false;
    for (auto& child : g_processes) {
        if (!matches(child, *g_current, pid)) {
            continue;
        }
        child_exists = true;
        if (child.state == State::zombie) {
            if (status != 0) {
                memcpy(user_pointer(*g_current, status), &child.exit_status, sizeof(int));
            }
            const uint32_t result = child.pid;
            child.state = State::unused;
            return result;
        }
    }
    if (!child_exists) {
        return -SAVANXP_ECHILD;
    }
    g_current->wait_pid = pid;
    g_current->wait_status = status;
    g_current->state = State::waiting;
    return 0; // The exit path fills in the actual child PID before resuming.
}

void exit(int status) {
    Process& child = *g_current;
    serial::write("process: pid ");
    serial::write_dec(child.pid);
    serial::write(" preemptions ");
    serial::write_dec(child.preemptions);
    serial::write("\n");
    child.exit_status = status;
    child.state = child.parent == 0 ? State::unused : State::zombie;
    for (auto& parent : g_processes) {
        if (parent.state == State::waiting && matches(child, parent, parent.wait_pid)) {
            if (parent.wait_status != 0) {
                memcpy(user_pointer(parent, parent.wait_status), &status, sizeof(status));
            }
            parent.frame->eax = child.pid;
            parent.state = State::runnable;
            child.state = State::unused;
            break;
        }
    }
    // Detached children are reaped when they exit; already-dead orphans are
    // released here so an exiting parent cannot exhaust the bounded table.
    for (auto& orphan : g_processes) {
        if (orphan.state != State::unused && orphan.parent == child.pid) {
            orphan.parent = 0;
            if (orphan.state == State::zombie) {
                orphan.state = State::unused;
            }
        }
    }
}

} // namespace i386::process
