#include "process.hpp"
#include "waits.hpp"
#include "kernel/display.hpp"
#include "kernel/audio_device.hpp"

#include "cpu.hpp"
#include "smp.hpp"
#include "kernel/smp.hpp"
#include "lapic.hpp"
#include "kernel/string.hpp"
#include "paging.hpp"
#include "kernel/timer.hpp"
#include "ps2_32.hpp"
#include "savanxp/syscall.h"
#include "serial.hpp"
#include "user32.hpp"
#include "syscall.hpp"
#include "io.hpp"
#include "sections.hpp"
#include "kernel/physical_memory.hpp"

namespace i386::process {
namespace {
constexpr uint32_t kQuantumTicks = 20;
constexpr uint32_t kStackCanary = 0x51a7c0de;
Process g_processes[kCapacity];
struct CpuState {
    Process* current;
    uint32_t ticks, slice_tick;
    Process* idle;
    bool idle_entered;
    bool resched_pending;
};
CpuState g_cpus[cpu::kMaxCpus];
CpuState& local() { return g_cpus[cpu::index()]; }
// Callers hold the global kernel lock; a kernel continuation never migrates.
Process*& current_slot() { return local().current; }
uint64_t g_total_cpu_ticks;
uint32_t g_next_pid = 1;
uint32_t g_init_pid;
uint32_t g_last_slot = kCapacity - 1;

// Same MOV EAX,YIELD / INT 80 / JMP loop as v0.4.0, placed inside the
// native user window. Each CPU owns a private code/stack/address space.
constexpr uint8_t kIdleCode[] = {0xb8, SAVANXP_SYS_YIELD, 0, 0, 0, 0xcd, 0x80, 0xeb, 0xf7};
Process* create_idle(uint32_t index) {
    for (auto& task : g_processes) {
        if (task.state != State::unused) continue;
        memset(&task, 0, sizeof(task));
        paging::initialize_process_directory(task.directory);
        if (!paging::map_user_range(task.directory, paging::kUserWindowBase, 4096, false) ||
            !paging::map_user_range(task.directory, paging::kUserStackTop - 4096, 4096, true)) {
            paging::destroy_process_directory(task.directory);
            return nullptr;
        }
        memcpy(paging::user_pointer(task.directory, paging::kUserWindowBase), kIdleCode, sizeof(kIdleCode));
        *reinterpret_cast<uint32_t*>(task.kernel_stack) = kStackCanary;
        task.frame = reinterpret_cast<ContextFrame*>(task.kernel_stack + sizeof(task.kernel_stack) - sizeof(ContextFrame));
        initialize_user_frame(*task.frame, paging::kUserWindowBase, paging::kUserStackTop, 0, 0, 0);
        initialize_fpu_state(task.fpu);
        strcpy(task.cwd, "/"); strcpy(task.name, "idle");
        task.idle = true;
        task.cpu = index;
        task.pid = g_next_pid++;
        task.state = State::runnable;
        serial::write("idle: ready cpu="); serial::write_dec(index);
        serial::write(" pid="); serial::write_dec(task.pid); serial::write("\n");
        return &task;
    }
    return nullptr;
}

uint32_t slot(const Process& task) {
    return static_cast<uint32_t>(&task - g_processes);
}

void wake_tasks() {
    const uint64_t now = timer::monotonic_ms();
    for (auto& task : g_processes) {
        if (task.state == State::sleeping && task.wake_ms <= now) {
            task.state = State::runnable;
        }
    }
    waits::service();
    io::service();
}

bool runnable_here(const Process& candidate, uint32_t here) {
    if (candidate.idle || candidate.state != State::runnable) return false;
    if (candidate.in_syscall && candidate.cpu != here) return false;
    for (uint32_t n = 0; n < cpu::kMaxCpus; ++n)
        if (n != here && g_cpus[n].current == &candidate) return false;
    return true;
}

ContextFrame* schedule(ContextFrame* frame) {
    Process* previous = current_slot();
    if (previous != nullptr &&
        *reinterpret_cast<const uint32_t*>(previous->kernel_stack) != kStackCanary) {
        serial::write("process: kernel stack overflow\n");
        cpu::halt_forever();
    }
    if (previous != nullptr && frame) {
        previous->frame = frame;
        if (previous->state == State::running) {
            previous->state = State::runnable;
        }
    }
    waits::service();
    io::service();
    Process* next = nullptr;
    // This scheduling decision consumes all wakeups serviced above.
    local().resched_pending = false;
    const uint32_t here = cpu::index();
    for (uint32_t offset = 1; offset <= kCapacity; ++offset) {
        Process& candidate = g_processes[(g_last_slot + offset) % kCapacity];
        if (runnable_here(candidate, here)) {
            next = &candidate;
            break;
        }
    }
    if (!next) next = local().idle;
    if (!next) { serial::write("scheduler: no idle process\n"); cpu::halt_forever(); }
    if (previous != next) {
        if (previous && frame && (frame->cs & 3u) == 0) {
            ++previous->kernel_suspends;
            if (!previous->idle && previous->kernel_suspends == 1) {
                serial::write("process: kernel suspended pid="); serial::write_dec(previous->pid);
                serial::write(" cpu="); serial::write_dec(here); serial::write("\n");
            }
        }
        if (previous != nullptr && previous->state != State::unused &&
            previous->state != State::zombie) {
            save_fpu_state(previous->fpu);
        }
        if (next != nullptr) {
            restore_fpu_state(next->fpu);
            paging::activate(next->directory);
            cpu::set_ring0_stack(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                next->kernel_stack + sizeof(next->kernel_stack))));
        }
    }
    // The exiting process keeps its kernel stack, but no user pages survive
    // exit (even while its small wait status remains a zombie). CR3 is safe now.
    if (previous != nullptr && (previous->state == State::unused || previous->state == State::zombie)) {
        paging::destroy_process_directory(previous->directory);
    }
    current_slot() = next;
    local().slice_tick = local().ticks;
    next->state = State::running;
    next->cpu = here;
    g_last_slot = slot(*next);
    return next->frame;
}

bool matches(const Process& child, const Process& parent, int pid) {
    return child.state != State::unused && child.parent == parent.pid &&
           (pid == SAVANXP_WAIT_ANY || child.pid == static_cast<uint32_t>(pid));
}

Process* find(uint32_t pid) {
    for (auto& task : g_processes) {
        if (task.state != State::unused && task.pid == pid) return &task;
    }
    return nullptr;
}

enum SignalFlags : uint32_t {
    signal_sigint = 1u << 0,
    signal_sigkill = 1u << 1,
    signal_sigpipe = 1u << 2,
    signal_sigterm = 1u << 3,
    signal_sigchld = 1u << 4,
};

uint32_t signal_flag(int signal) {
    switch (signal) {
        case SAVANXP_SIGINT: return signal_sigint;
        case SAVANXP_SIGKILL: return signal_sigkill;
        case SAVANXP_SIGPIPE: return signal_sigpipe;
        case SAVANXP_SIGTERM: return signal_sigterm;
        case SAVANXP_SIGCHLD: return signal_sigchld;
        default: return 0;
    }
}

void record_signal(Process& task, uint32_t flag, int signal) {
    task.pending_signals |= flag;
    task.last_signal = static_cast<uint32_t>(signal);
}

} // namespace

void initialize() {
    cpu::enable_fpu();
    for (uint32_t index = 0; index < cpu::kMaxCpus; ++index) {
        if (!smp::may_schedule(index)) continue;
        g_cpus[index].idle = create_idle(index);
        if (!g_cpus[index].idle) {
            serial::write("idle: allocation failed cpu="); serial::write_dec(index); serial::write("\n");
            if (!index) cpu::halt_forever();
        }
    }
    serial::write("process: private stacks, address spaces and FXSAVE ready\n");
}

bool user_range(uint32_t address, uint32_t size, bool writable) {
    return current_slot() != nullptr && paging::user_range(current_slot()->directory, address, size, writable);
}

long spawn(const char* path, const char* const* argv, uint32_t argc, int stdin_fd, int stdout_fd) {
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
        paging::initialize_process_directory(task.directory);
        const int error = user32::load(path, task.directory, *task.frame, argc, argv, &task.native_subsystem);
        if (error != 0) {
            paging::destroy_process_directory(task.directory);
            return -error;
        }
        if (!io::initialize_handles(task, current_slot(), stdin_fd, stdout_fd)) {
            paging::destroy_process_directory(task.directory);
            return -SAVANXP_EBADF;
        }
        strcpy(task.cwd, current_slot() ? current_slot()->cwd : "/");
        const char* name = path;
        for (const char* cursor = path; *cursor; ++cursor) if (*cursor == '/') name = cursor + 1;
        uint32_t length = strlen(name);
        if (length >= sizeof(task.name)) length = sizeof(task.name) - 1;
        memcpy(task.name, name, length);
        initialize_fpu_state(task.fpu);
        task.pid = g_next_pid++;
        sections::randomize_base(task);
        task.parent = current_pid();
        if (!current_slot() && !g_init_pid) g_init_pid = task.pid;
        task.state = State::runnable;
        return task.pid;
    }
    return -SAVANXP_EAGAIN;
}

long fork(const ContextFrame& frame) {
    for (auto& child : g_processes) {
        if (child.state != State::unused) continue;
        if (g_next_pid >= 0x80000000u) return -SAVANXP_EAGAIN;
        memset(&child, 0, sizeof(child));
        *reinterpret_cast<uint32_t*>(child.kernel_stack) = kStackCanary;
        child.frame = reinterpret_cast<ContextFrame*>(child.kernel_stack + sizeof(child.kernel_stack) - sizeof(ContextFrame));
        paging::initialize_process_directory(child.directory);
        if (!paging::clone_private_pages(current_slot()->directory, child.directory) ||
            !sections::clone_views(*current_slot(), child)) {
            sections::release_views(child);
            paging::destroy_process_directory(child.directory);
            return -SAVANXP_ENOMEM;
        }
        for (uint32_t fd = 0; fd < io::kMaxHandles; ++fd) {
            child.handles[fd] = current_slot()->handles[fd];
            object::retain(child.handles[fd].object);
        }
        child.native_subsystem = current_slot()->native_subsystem;
        strcpy(child.cwd, current_slot()->cwd);
        strcpy(child.name, current_slot()->name);
        *child.frame = frame;
        child.frame->eax = 0;
        save_fpu_state(current_slot()->fpu);
        memcpy(&child.fpu, &current_slot()->fpu, sizeof(child.fpu));
        child.pid = g_next_pid++;
        child.parent = current_pid();
        child.state = State::runnable;
        return child.pid;
    }
    return -SAVANXP_EAGAIN;
}

[[noreturn]] void start() {
    ps2_32::clear_queue();
    smp::lock_kernel();
    smp::start_scheduling();
    serial::write("scheduler: round-robin, 20 ms quantum\n");
    i386_resume(schedule(nullptr));
}

[[noreturn]] void start_secondary() {
    smp::lock_kernel();
    (void)timer::start_on_secondary();
    i386_resume(schedule(nullptr));
}

ContextFrame* reschedule(ContextFrame* frame, bool ipi) {
    // Ring-0 continuations belong to this CPU until their syscall returns.
    if (current_slot() && (frame->cs & 3u) == 0) return frame;
    if (current_slot() && current_slot()->pending_exit) {
        if (ipi) {
            serial::write("smp: reschedule IPI terminated pid=");
            serial::write_dec(current_slot()->pid);
            serial::write(" cpu="); serial::write_dec(cpu::index()); serial::write("\n");
        }
        exit(current_slot()->pending_exit);
    }
    return schedule(frame);
}

extern "C" uint8_t i386_wait_interrupt_resume[];
bool at_interruptible_wait(const ContextFrame& frame) {
    return current_slot() && current_slot()->state == State::running &&
        current_slot()->in_syscall && current_slot()->wait_depth &&
        (frame.eflags & 0x200u) && frame.eip == reinterpret_cast<uintptr_t>(i386_wait_interrupt_resume);
}

ContextFrame* timer(ContextFrame* frame) {
    ++local().ticks;
    if (current_slot()) { ++g_total_cpu_ticks; ++current_slot()->cpu_ticks; }
    if (!cpu::index()) wake_tasks();
    // The timer owns its scheduling decision; do not carry its wakeups into
    // an unrelated syscall return (the same policy as v0.4.0).
    local().resched_pending = false;
    const bool kernel = (frame->cs & 3u) == 0;
    // Interrupt gates keep ordinary kernel work atomic. Only the exact wait
    // window runs with IF set and the lock released, so it may be suspended.
    if (kernel && !at_interruptible_wait(*frame)) return frame;
    if (!kernel && current_slot() && current_slot()->pending_exit) return reschedule(frame);
    if (current_slot() == nullptr || current_slot()->idle || local().ticks - local().slice_tick >= kQuantumTicks) {
        if (current_slot() != nullptr) ++current_slot()->preemptions;
        return schedule(frame);
    }
    return frame;
}

void begin_syscall() {
    current_slot()->in_syscall = true;
}

void note_woken(Process& task) {
    // v0.4.0 hands off on the waker's CPU, without an idle-CPU wake IPI.
    if (&task != current_slot()) local().resched_pending = true;
}

ContextFrame* finish_syscall(ContextFrame* frame, bool reschedule) {
    if (current_slot()->idle && !local().idle_entered) {
        if ((frame->cs & 3u) != 3 || frame->eip != paging::kUserWindowBase + 7 ||
            current_slot()->cpu != cpu::index() || cpu::ring0_stack_top() !=
                reinterpret_cast<uintptr_t>(current_slot()->kernel_stack + sizeof(current_slot()->kernel_stack))) {
            serial::write("idle: invalid CPU/user frame/TSS\n"); cpu::halt_forever();
        }
        local().idle_entered = true;
        serial::write("idle: ring3 cpu="); serial::write_dec(cpu::index());
        serial::write(" pid="); serial::write_dec(current_slot()->pid); serial::write("\n");
    }
    if (current_slot()->pending_exit && current_slot()->state != State::unused &&
        current_slot()->state != State::zombie) exit(current_slot()->pending_exit);
    // Cleanup can itself wait; keep the continuation alive/pinned until all
    // C++ work is complete and the user frame is ready to publish.
    current_slot()->frame = frame;
    io::service_pending();
    current_slot()->in_syscall = false;
    if (reschedule || local().resched_pending || current_slot()->state != State::running) {
        return schedule(frame);
    }
    return frame;
}

uint64_t total_cpu_ticks() { return g_total_cpu_ticks; }

uint32_t current_pid() { return current_slot() == nullptr ? 0 : current_slot()->pid; }

uint32_t count() {
    uint32_t total = 0;
    for (const auto& task : g_processes) {
        if (task.state != State::unused) {
            ++total;
        }
    }
    return total;
}

Process* current() { return current_slot(); }
Process* idle_for_cpu(uint32_t index) { return index < cpu::kMaxCpus ? g_cpus[index].idle : nullptr; }
Process* at(uint32_t index) { return index < kCapacity ? &g_processes[index] : nullptr; }

int replace_image(const char* path, const char* const* argv, uint32_t argc, ContextFrame& frame) {
    memory::PageAllocation staging = {};
    if (!memory::allocate_page(staging)) return SAVANXP_ENOMEM;
    auto* directory = static_cast<uint32_t*>(staging.virtual_address);
    paging::initialize_process_directory(directory);
    ContextFrame replacement = {};
    bool native_subsystem = false;
    const int error = user32::load(path, directory, replacement, argc, argv, &native_subsystem);
    if (error) {
        paging::destroy_process_directory(directory);
        memory::free_allocation(staging);
        return error;
    }
    // Commit only after the new image is complete; failures preserve the old
    // program, descriptors and CWD. Kernel stacks do not depend on user CR3.
    paging::activate_kernel();
    sections::release_views(*current_slot());
    sections::randomize_base(*current_slot());
    paging::destroy_process_directory(current_slot()->directory);
    memcpy(current_slot()->directory, directory, 4096);
    memory::free_allocation(staging);
    paging::activate(current_slot()->directory);
    frame = replacement;
    current_slot()->native_subsystem = native_subsystem;
    initialize_fpu_state(current_slot()->fpu);
    restore_fpu_state(current_slot()->fpu);
    const char* name = path;
    for (const char* cursor = path; *cursor; ++cursor) if (*cursor == '/') name = cursor + 1;
    memset(current_slot()->name, 0, sizeof(current_slot()->name));
    uint32_t length = strlen(name);
    if (length >= sizeof(current_slot()->name)) length = sizeof(current_slot()->name) - 1;
    memcpy(current_slot()->name, name, length);
    return 0;
}

void sleep(uint32_t milliseconds) {
    if (milliseconds != 0) {
        current_slot()->wake_ms = timer::monotonic_ms() + milliseconds;
        current_slot()->state = State::sleeping;
    } else {
        yield();
    }
}

void yield() {
    if (current_slot()->idle) {
        bool ready = false;
        const uint32_t here = cpu::index();
        for (auto& task : g_processes) if (runnable_here(task, here)) { ready = true; break; }
        // Match v0.4.0: halt on behalf of the ring-3 idle, with the lock
        // released. The BSP tick must service deadlines before returning here.
        if (!ready) ::smp::wait_for_interrupt();
    }
    current_slot()->state = State::runnable;
}

long wait(int pid, uint32_t status) {
    if (pid != SAVANXP_WAIT_ANY && pid <= 0) {
        return -SAVANXP_EINVAL;
    }
    bool child_exists = false;
    for (auto& child : g_processes) {
        if (!matches(child, *current_slot(), pid)) {
            continue;
        }
        child_exists = true;
        if (child.state == State::zombie) {
            if (status != 0) {
                paging::copy_to_user(current_slot()->directory, status, &child.exit_status, sizeof(int));
            }
            const uint32_t result = child.pid;
            child.state = State::unused;
            return result;
        }
    }
    if (!child_exists) {
        return -SAVANXP_ECHILD;
    }
    current_slot()->wait_pid = pid;
    current_slot()->wait_status = status;
    current_slot()->state = State::waiting;
    return 0; // The exit path fills in the actual child PID before resuming.
}

static void notify_parent(Process& child) {
    for (auto& parent : g_processes) {
        if (parent.state != State::waiting || !matches(child, parent, parent.wait_pid)) continue;
        if (parent.wait_status)
            paging::copy_to_user(parent.directory, parent.wait_status, &child.exit_status, sizeof(int));
        parent.frame->eax = child.pid;
        parent.state = State::runnable;
        child.state = State::unused;
        return;
    }
}

static void terminate(Process& child, int status) {
    if (auto* parent = find(child.parent))
        record_signal(*parent, signal_sigchld, SAVANXP_SIGCHLD);
    waits::clear(child);
    display::release_session_for(child.pid);
    audio_device::release_session_for(child.pid);
    sections::release_views(child);
    io::release_handles(child);
    serial::write("process: pid ");
    serial::write_dec(child.pid);
    serial::write(" preemptions ");
    serial::write_dec(child.preemptions);
    serial::write("\n");
    if (child.kernel_suspends) {
        serial::write("process: kernel continuations pid="); serial::write_dec(child.pid);
        serial::write(" suspended="); serial::write_dec(child.kernel_suspends);
        serial::write(" resumed="); serial::write_dec(child.kernel_resumes);
        serial::write(" cpu="); serial::write_dec(child.cpu); serial::write("\n");
    }
    child.exit_status = status;
    child.state = child.parent == 0 ? State::unused : State::zombie;
    notify_parent(child);
    if (child.orphaned && child.state == State::zombie) child.state = State::unused;
    // Reap dead children and let init supervise the live ones.
    Process* init = nullptr;
    for (auto& candidate : g_processes)
        if (candidate.pid == g_init_pid && &candidate != &child &&
            candidate.state != State::unused && candidate.state != State::zombie) init = &candidate;
    for (auto& orphan : g_processes) {
        if (orphan.state == State::unused || orphan.parent != child.pid) continue;
        if (orphan.state == State::zombie) {
            orphan.state = State::unused;
            continue;
        }
        orphan.parent = init ? init->pid : 0;
        orphan.orphaned = true;
    }
}

void exit(int status) {
    if (!current_slot() || current_slot()->idle) {
        serial::write("process: invalid idle exit\n");
        cpu::halt_forever();
    }
    terminate(*current_slot(), status);
}

long kill(int pid, int signal) {
    const uint32_t flag = signal_flag(signal);
    if (!flag || pid < 0) return -SAVANXP_EINVAL;
    Process* target = find(pid == 0 ? current_pid() : static_cast<uint32_t>(pid));
    if (!target) return -SAVANXP_ENOENT;
    Process& task = *target;
    if (task.idle || task.pid == g_init_pid) return -SAVANXP_EACCES;
    record_signal(task, flag, signal);
    if (signal == SAVANXP_SIGCHLD || task.state == State::zombie) return 0;

    if (task.in_syscall) {
        if (!task.pending_exit) task.pending_exit = 128 + signal;
        if (&task != current_slot()) {
            bool owned = false;
            for (const auto& state : g_cpus) if (state.current == &task) owned = true;
            serial::write("process: kernel kill deferred pid="); serial::write_dec(task.pid);
            serial::write(" cpu="); serial::write_dec(task.cpu);
            serial::write(" suspended="); serial::write_dec(!owned); serial::write("\n");
            smp::kick(task.cpu);
        }
        return 0;
    }
    for (uint32_t n = 0; n < cpu::kMaxCpus; ++n) {
        if (n != cpu::index() && g_cpus[n].current == &task) {
            if (!task.pending_exit) task.pending_exit = 128 + signal;
            serial::write("smp: remote termination requested pid=");
            serial::write_dec(task.pid);
            serial::write(" owner="); serial::write_dec(n);
            serial::write(" requester="); serial::write_dec(cpu::index());
            serial::write("\n");
            smp::kick(n);
            return 0;
        }
    }
    terminate(task, 128 + signal);
    if (&task != current_slot()) paging::destroy_process_directory(task.directory);
    return 0;
}

ContextFrame* fault(uint32_t vector) {
    // Match v0.4.0's terminate_current_from_exception status convention.
    exit(128 + static_cast<int>(vector));
    return schedule(current_slot()->frame);
}

} // namespace i386::process
