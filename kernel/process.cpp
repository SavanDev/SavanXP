#include "kernel/process.hpp"
#include "kernel/process_detail.hpp"

#include "abi/savanxp_native_abi.h"

#include <stdarg.h>
#include <stdint.h>

#include "arch/cpu.hpp"
#include "kernel/audio_device.hpp"
#include "kernel/block.hpp"
#include "kernel/console.hpp"
#include "kernel/device.hpp"
#include "kernel/display.hpp"
#include "kernel/elf.hpp"
#include "kernel/fs.hpp"
#include "kernel/heap.hpp"
#include "kernel/input.hpp"
#include "kernel/net.hpp"
#include "kernel/panic.hpp"
#include "kernel/pci.hpp"
#include "kernel/pcspeaker.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/rtc.hpp"
#include "kernel/smp.hpp"
#include "kernel/string.hpp"
#include "kernel/sx.hpp"
#include "kernel/timer.hpp"
#include "kernel/tty.hpp"
#include "kernel/ui.hpp"

namespace {

constexpr uint16_t kUserDataSelector = 0x1b;
constexpr uint16_t kUserCodeSelector = 0x23;
// Clase PCI de un controlador de red, para reportar el hardware que existe
// aunque ningun driver lo haya reclamado.
constexpr uint8_t kPciClassNetwork = 0x02;
constexpr uint64_t kIdleCodeAddress = 0x0000000000800000ULL;
// Round-robin quantum in timer ticks. Sized to ~20 ms of wall clock at the
// 1000 Hz tick rate so raising the tick frequency does not multiply the
// preemptive context-switch rate for CPU-bound threads.
constexpr uint32_t kDefaultTimeSlice = 20;
constexpr bool kLogProc = false;
constexpr uint8_t kIdleCode[] = {
    0xb8, static_cast<uint8_t>(SAVANXP_SYS_YIELD), 0x00, 0x00, 0x00,
    0xcd, 0x80,
    0xeb, 0xf7,
};

// (process::ImageFailure vive en include/kernel/process.hpp: es parte de la frontera de
// traduccion POSIX que usa el dispatcher en su propia unidad.)

process::Process g_processes[process::kMaxProcesses] = {};
process::Thread g_threads[process::kMaxThreads] = {};
// One past the highest thread slot ever handed out. The table is sized for
// many threads per process, but the scheduler and every waker walk it under
// the big kernel lock on each tick and each pipe write: stopping here keeps
// those walks as long as the live population, not as the table.
size_t g_thread_bound = 0;

struct ThreadSlots {
    process::Thread* first;
    process::Thread* last;
    process::Thread* begin() const { return first; }
    process::Thread* end() const { return last; }
};

ThreadSlots used_thread_slots() {
    return {&g_threads[0], &g_threads[0] + g_thread_bound};
}
process::Pipe g_pipes[process::detail::kMaxPipeCount] = {};
uint8_t g_pipe_storage[process::detail::kMaxPipeCount][process::detail::kPipeCapacity] = {};
object::IoObject g_io_objects[object::kMaxIoObjects] = {};
// One counter for pids and tids: a process's first thread takes the pid as
// its tid, and every other thread draws a fresh id, so no tid ever equals
// another process's pid (docs/kernel/THREADS_ROADMAP.md).
uint32_t g_next_pid = 1;
uint32_t g_init_pid = 0;
// Shared between cores on purpose: it is the cursor of the single ready queue
// (an index into g_threads), which lives under the big kernel lock.
size_t g_schedule_cursor = 0;
// Ticks that found a thread running, on any core. It is the denominator of
// CPU accounting (cpu_ticks_total): each one increments exactly one process's
// cpu_ticks, so both sides add up the same with N cores. It is not
// timer::ticks(), which counts only the BSP's because it is a clock.
uint64_t g_cpu_ticks_total = 0;
bool g_ready = false;
savanxp_system_info g_boot_system_info = {};
bool g_boot_system_info_ready = false;

// The current thread, the idle and the reschedule request belong to each core
// and live in smp::Cpu (kernel/smp.hpp). This shortcut lets the scheduler and
// the syscalls say "this core's" without repeating the namespace every line.
smp::Cpu& this_cpu() {
    return smp::this_cpu();
}

int negative_error(sx_status code) {
    return -static_cast<int>(code);
}

/*
 * Reloj de pared del kernel, en milisegundos desde el arranque.
 *
 * Sale del TSC y NO del contador de interrupciones del timer. La diferencia es
 * la que hizo que Doom se arrastrara en VirtualBox: con la maquina halteando,
 * la entrega del tick del APIC se vuelve a rafagas -- medido entre 151 Hz y
 * 4000 Hz con el timer programado a 1000 --, y el total se promedia bien pero
 * el ritmo instantaneo no. Todo lo que pacea tiempo contra eso se traba y
 * despues se adelanta. El TSC, medido contra el RTC en los dos hipervisores,
 * no hace eso.
 *
 * timer::ticks() se queda para lo unico que mide bien: la contabilidad de CPU,
 * donde un tick ES un proceso (docs/userland/SYSTEM_MONITORING.md).
 *
 * El contador de interrupciones queda de reserva por si el TSC no llego a
 * calibrarse; como el calibrado corre al principio del arranque, eso solo pasa
 * antes de que exista userland.
 */

void process_assert(bool condition, const char* message) {
    if (!condition) {
        panic(message);
    }
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

uint32_t exported_timer_backend(timer::Backend backend) {
    switch (backend) {
        case timer::Backend::local_apic:
            return SAVANXP_TIMER_LOCAL_APIC;
        case timer::Backend::pit:
            return SAVANXP_TIMER_PIT;
        case timer::Backend::none:
        default:
            return SAVANXP_TIMER_NONE;
    }
}



size_t thread_index(const process::Thread* thread) {
    return thread != nullptr ? static_cast<size_t>(thread - &g_threads[0]) : 0;
}



uint32_t object_access_for_section_flags(uint32_t flags, uint32_t default_access) {
    uint32_t access = object::access_none;
    if ((flags & SAVANXP_SECTION_READ) != 0) {
        access |= object::access_read;
    }
    if ((flags & SAVANXP_SECTION_WRITE) != 0) {
        access |= object::access_write;
    }
    if ((flags & SAVANXP_SECTION_EXEC) != 0) {
        access |= object::access_execute;
    }
    return access != object::access_none ? access : default_access;
}

void clear_handle_entry(process::HandleEntry& entry) {
    entry.object = nullptr;
    entry.granted_access = object::access_none;
    entry.flags = object::handle_none;
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

/*
 * Estado privado del subsistema (rebanada B de aislamiento).
 *
 * El kernel reserva y libera estos bytes y NO mira dentro: lo unico que sabe
 * es cuanto pide cada subsistema, declarado en
 * subsystem::register_process_state_size. Por eso Process ya no tiene campo
 * cwd: el directorio actual es estado de resolucion de rutas de POSIX y vive
 * en el estado que POSIX declara (subsystems/posix/kernel/
 * posix_process_state.hpp). Un kernel sin POSIX no gasta un byte en esto.
 *
 * ensure_subsystem_state, free_subsystem_state y copy_subsystem_state viven
 * en namespace process, mas abajo, con el resto de la superficie publicada.
 */

void release_kernel_stack(process::Thread& thread) {
    if (thread.kernel_stack_base == 0 || thread.kernel_stack_size == 0) {
        return;
    }

    const uint64_t physical_base = thread.kernel_stack_base - vm::hhdm_offset();
    const uint64_t page_count = thread.kernel_stack_size / memory::kPageSize;
    (void)memory::free_pages(physical_base, page_count);
    thread.kernel_stack_base = 0;
    thread.kernel_stack_size = 0;
    thread.context = nullptr;
}



bool read_user_pointer(const process::Process& proc, uint64_t user_address, uint64_t& value) {
    return read_from_process_memory(proc, &value, user_address, sizeof(value));
}



// Suelta los handles que quedaron viajando. Se llama cuando ya nadie los puede
// recibir: sin lectores, o con el pipe muerto. Los objetos encolados nunca son
// IoObjects (pipe_send_handle los rechaza), asi que soltar uno no puede volver a
// entrar aca por el destroy de otro pipe.
void drain_pipe_handles(process::Pipe& pipe) {
    while (pipe.handle_count != 0) {
        object::Header*& queued = pipe.handles[pipe.handle_head];
        object::release(queued);
        pipe.handle_access[pipe.handle_head] = object::access_none;
        pipe.handle_head = (pipe.handle_head + 1u) % process::kPipeHandleQueueCapacity;
        pipe.handle_count -= 1u;
    }
    pipe.handle_head = 0;
}

void free_pipe_if_unused(process::Pipe* pipe) {
    if (pipe == nullptr || !pipe->in_use) {
        return;
    }
    if (pipe->reader_refs == 0) {
        drain_pipe_handles(*pipe);
    }
    if (pipe->reader_refs == 0 && pipe->writer_refs == 0) {
        memset(pipe, 0, sizeof(*pipe));
    }
}

void wake_blocked_readers_for_pipe(process::Pipe& pipe);
void wake_blocked_writers_for_pipe(process::Pipe& pipe);
void recheck_parked_threads(uint64_t current_tick, bool honour_deadline);
void release_all_handles(process::Process& proc);




int allocate_fd_slot(process::Process& proc) {
    for (size_t fd = 0; fd < process::kMaxFileHandles; ++fd) {
        if (proc.handles[fd].object == nullptr) {
            return static_cast<int>(fd);
        }
    }
    return negative_error(SX_EBADF);
}



object::IoObject* create_tty_io_object(uint32_t flags) {
    object::IoObject* file = process::detail::allocate_io_object();
    if (file == nullptr) {
        return nullptr;
    }
    file->kind = process::HandleKind::tty;
    file->open_flags = flags;
    file->header.destroy = process::detail::destroy_io_object;
    return file;
}

bool initialize_standard_handles(process::Process& proc) {
    for (size_t index = 0; index < process::kMaxFileHandles; ++index) {
        clear_handle_entry(proc.handles[index]);
    }

    object::IoObject* stdin_file = create_tty_io_object(process::open_read);
    object::IoObject* stdout_file = create_tty_io_object(process::open_write);
    object::IoObject* stderr_file = create_tty_io_object(process::open_write);
    if (stdin_file == nullptr || stdout_file == nullptr || stderr_file == nullptr) {
        process::detail::discard_io_object(stdin_file);
        process::detail::discard_io_object(stdout_file);
        process::detail::discard_io_object(stderr_file);
        return false;
    }

    if (process::detail::install_handle(proc, 0, &stdin_file->header, process::detail::object_access_for_open_flags(stdin_file->open_flags), object::handle_none) < 0) {
        process::detail::discard_io_object(stdin_file);
        process::detail::discard_io_object(stdout_file);
        process::detail::discard_io_object(stderr_file);
        return false;
    }
    stdin_file = nullptr;

    if (process::detail::install_handle(proc, 1, &stdout_file->header, process::detail::object_access_for_open_flags(stdout_file->open_flags), object::handle_none) < 0) {
        release_all_handles(proc);
        process::detail::discard_io_object(stdout_file);
        process::detail::discard_io_object(stderr_file);
        return false;
    }
    stdout_file = nullptr;

    if (process::detail::install_handle(proc, 2, &stderr_file->header, process::detail::object_access_for_open_flags(stderr_file->open_flags), object::handle_none) < 0) {
        release_all_handles(proc);
        process::detail::discard_io_object(stdin_file);
        process::detail::discard_io_object(stdout_file);
        process::detail::discard_io_object(stderr_file);
        return false;
    }
    stderr_file = nullptr;

    return true;
}

bool inherit_handle(process::Process& child, int child_fd, const process::Process& parent, int parent_fd) {
    if (parent_fd < 0 || parent_fd >= static_cast<int>(process::kMaxFileHandles)) {
        return false;
    }
    object::Header* handle_object = parent.handles[parent_fd].object;
    if (handle_object == nullptr) {
        return false;
    }
    return process::detail::install_handle(
        child,
        child_fd,
        handle_object,
        parent.handles[parent_fd].granted_access,
        parent.handles[parent_fd].flags) >= 0;
}

void release_all_handles(process::Process& proc) {
    for (process::HandleEntry& entry : proc.handles) {
        if (entry.object != nullptr) {
            object::release(entry.object);
            entry.granted_access = object::access_none;
            entry.flags = object::handle_none;
        }
    }
}

uint64_t initial_stack_guard() {
    uint64_t value = arch::random_u64();
    // The stack-protector ABI reserves the least-significant byte for string
    // termination, so a canary must never contain a zero there.
    if ((value & 0xffu) == 0) {
        value ^= 0xa5u;
    }
    return value;
}

process::SavedContext* fabricate_initial_context(
    process::Thread& thread,
    uint64_t entry_point,
    uint64_t stack_pointer,
    int argc,
    uint64_t argv_pointer,
    uint64_t interpreter_address,
    uint64_t image_base
) {
    auto* stack_top = reinterpret_cast<uint8_t*>(thread.kernel_stack_base + thread.kernel_stack_size);
    auto* context = reinterpret_cast<process::SavedContext*>(stack_top - sizeof(process::SavedContext));
    memset(context, 0, sizeof(*context));
    context->rip = entry_point;
    context->cs = kUserCodeSelector;
    context->rflags = 0x202;
    context->rsp = stack_pointer;
    context->ss = kUserDataSelector;
    context->rdi = static_cast<uint64_t>(argc);
    context->rsi = argv_pointer;
    // crt0 consumes this otherwise-unused startup register to install a fresh
    // process canary before calling main. Fork copies the full context and
    // address space, so children inherit their parent's guard.
    context->rdx = initial_stack_guard();
    // rcx queda libre para la ruta del interprete (PT_INTERP), que el loader
    // copio al stack inicial. rcx no forma parte del ABI de entrada a main.
    context->rcx = interpreter_address;
    // r8 lleva la direccion de la cabecera ELF de la imagen. crt0 la guarda y el
    // cargador la usa en vez de buscarla: el recorrido hacia atras que se hacia
    // antes leia paginas que un PT_LOAD puede dejar sin mapear.
    context->r8 = image_base;
    thread.context = context;
    return context;
}

// Origen de la imagen de un ejecutable para elf::load_user_image.
//
// Antes la imagen se copiaba ENTERA a un bloque de paginas fisicamente
// contiguas y de ahi el loader la volvia a copiar segmento por segmento. Eso
// costaba el doble de memoria durante el exec y, sobre todo, pedia N paginas
// seguidas: un binario de 5 MB son 1280, y con la memoria fragmentada eso falla
// aunque haya de sobra. Ahora el loader lee de aca directo sobre las paginas
// del proceso.
struct ImageSource {
    const uint8_t* bytes; // != nullptr si la imagen ya vive en memoria
    vfs::Vnode* node;     // si no, se lee del vnode
    size_t size;
};

bool read_image_from_memory(void* context, uint64_t offset, void* destination, size_t size) {
    const auto* source = static_cast<const ImageSource*>(context);
    if (offset > source->size || size > source->size - offset) {
        return false;
    }
    memcpy(destination, source->bytes + offset, size);
    return true;
}

bool read_image_from_vnode(void* context, uint64_t offset, void* destination, size_t size) {
    auto* source = static_cast<ImageSource*>(context);
    if (offset > source->size || size > source->size - offset) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    return vfs::read(*source->node, offset, destination, size) == size;
}

// Prepara el origen. No reserva memoria: el camino de disco lee por demanda y
// el de memoria (initramfs) usa los bytes donde ya estan.
bool open_image_source(const vfs::Vnode& image, ImageSource& source, elf::ImageReader& reader, process::ImageFailure& failure) {
    source = {};
    reader = nullptr;
    failure = process::ImageFailure::none;

    if (image.type != vfs::NodeType::file || image.size == 0) {
        failure = process::ImageFailure::not_a_file;
        return false;
    }

    source.size = image.size;
    if (image.backend == vfs::Backend::memory && image.data != nullptr) {
        source.bytes = static_cast<const uint8_t*>(image.data);
        reader = read_image_from_memory;
        return true;
    }

    source.node = const_cast<vfs::Vnode*>(&image);
    reader = read_image_from_vnode;
    return true;
}

// Traduce el motivo de fallo a un errno con sentido. Sin esto, exec/spawn
// devolvian ENOENT ("no such file or directory") para cualquier fallo, lo que
// hacia indistinguible un binario ausente de uno que no entra en memoria.
const char* image_failure_string(process::ImageFailure failure) {
    switch (failure) {
        case process::ImageFailure::none:
            return "ok";
        case process::ImageFailure::not_found:
            return "path not found";
        case process::ImageFailure::not_a_file:
            return "not a regular file";
        case process::ImageFailure::permission_denied:
            return "permission denied";
        case process::ImageFailure::read_failed:
            return "image read failed";
        case process::ImageFailure::bad_image:
            return "bad elf image";
        case process::ImageFailure::no_staging_memory:
            return "no contiguous memory to stage the image";
        case process::ImageFailure::no_address_space:
            return "no memory for the address space";
        case process::ImageFailure::no_load_memory:
            return "no memory to map the image";
        case process::ImageFailure::no_process_slot:
            return "no free process slot";
        case process::ImageFailure::no_handles:
            return "handle setup failed";
        case process::ImageFailure::threads_running:
            return "other threads still running";
    }
    return "unknown";
}

// Un fallo de carga se loguea siempre: es la unica traza de por que un binario
// que existe no arranca (el errno llega al proceso, pero el detalle no).
void report_image_failure(const char* what, const char* path, process::ImageFailure failure, size_t image_size) {
    console::printf(
        "process: %s de '%s' fallo: %s (imagen %u bytes, %u paginas libres)\n",
        what,
        path,
        image_failure_string(failure),
        static_cast<unsigned>(image_size),
        static_cast<unsigned>(memory::free_page_count()));
}

process::Thread* create_idle_process() {
    process::Process* proc = process::detail::allocate_process_slot(true);
    if (proc == nullptr) {
        return nullptr;
    }

    if (!vm::create_address_space(proc->address_space)) {
        process::detail::reset_process_slot(*proc);
        return nullptr;
    }

    process::Thread* thread = process::detail::allocate_thread(*proc);
    if (thread == nullptr) {
        process::detail::release_address_space(*proc);
        process::detail::reset_process_slot(*proc);
        return nullptr;
    }
    set_process_name(*proc, "idle");
    process::ensure_subsystem_state(*proc);

    memory::PageAllocation code_page = {};
    memory::PageAllocation stack_page = {};
    if (!memory::allocate_page(code_page) || !memory::allocate_page(stack_page)) {
        if (code_page.physical_address != 0) {
            (void)memory::free_allocation(code_page);
        }
        if (stack_page.physical_address != 0) {
            (void)memory::free_allocation(stack_page);
        }
        process::detail::release_process_resources(*proc);
        process::detail::reset_process_slot(*proc);
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
            vm::kPageUser | vm::kPageWrite | vm::kPageNoExecute);
    if (!code_mapped || !stack_mapped) {
        if (!code_mapped) {
            (void)memory::free_allocation(code_page);
            (void)memory::free_allocation(stack_page);
        } else if (!stack_mapped) {
            (void)memory::free_allocation(stack_page);
        }
        process::detail::release_process_resources(*proc);
        process::detail::reset_process_slot(*proc);
        return nullptr;
    }

    fabricate_initial_context(*thread, kIdleCodeAddress, vm::kUserStackTop, 0, 0, 0, 0);
    return thread;
}

// A thread of the child's own parent, if it is blocked in a waitpid that this
// child satisfies. Only the parent: a waitpid(-1) somewhere else (init and
// sessiond sit in one all the time) must not take a child that is not theirs,
// or the real parent waits forever for a pid that was reaped under it.
process::Thread* find_waiting_parent_for(const process::Process& child) {
    for (process::Thread& thread : used_thread_slots()) {
        if (thread.state != process::State::blocked_wait || thread.wait_reason != process::WaitReason::child) {
            continue;
        }
        if (thread.process->pid != child.parent_pid) {
            continue;
        }
        if (thread.waiting_for_pid == process::kWaitAnyPid || thread.waiting_for_pid == child.pid) {
            return &thread;
        }
    }
    return nullptr;
}

void reparent_orphaned_children(uint32_t parent_pid) {
    for (process::Process& proc : g_processes) {
        if (proc.life == process::Lifecycle::unused || proc.parent_pid != parent_pid) {
            continue;
        }
        if (proc.life == process::Lifecycle::zombie) {
            reap_process(proc);
            continue;
        }
        proc.parent_pid = g_init_pid;
        proc.orphaned = true;
    }
}

// Ready to run on THIS core. There is a single queue and any core takes from
// it, with one exception: a thread preempted while asleep inside the kernel
// (smp::wait_for_interrupt) has its context in ring 0, halfway through a
// syscall, and only the core where it went to sleep resumes it.
//
// Threads running on another core need no filtering: they are in
// State::running, not ready. Siblings of a running thread are not filtered
// either: threads of one process run on several cores at once, which the user
// TLB shootdown makes safe (THREADS_ROADMAP.md, phase 2). And idles never
// enter, each core returns to its own separately.
//
// A thread whose process is ending would only run user code it must not run:
// it waits in ready until the process is torn down. Unless it is halfway
// through a syscall in ring 0, which must finish to release what it holds;
// handle_syscall then ends it.
// Preempted while asleep inside a syscall (smp::wait_for_interrupt): ready,
// but with its context in ring 0, halfway through kernel code.
bool stopped_in_kernel(const process::Thread& thread) {
    return thread.state == process::State::ready && thread.context != nullptr &&
        (thread.context->cs & 0x3) == 0;
}

bool runnable_here(const process::Thread& thread) {
    if (thread.state != process::State::ready || thread.process->idle) {
        return false;
    }
    const bool in_kernel = thread.context != nullptr && (thread.context->cs & 0x3) == 0;
    if (in_kernel && thread.cpu != this_cpu().index) {
        return false;
    }
    return in_kernel || (!thread.process->exit_requested && !thread.end_requested);
}

process::Thread* pick_next_runnable() {
    for (size_t offset = 1; offset <= g_thread_bound; ++offset) {
        const size_t index = (g_schedule_cursor + offset) % g_thread_bound;
        process::Thread& thread = g_threads[index];
        if (runnable_here(thread)) {
            g_schedule_cursor = index;
            return &thread;
        }
    }

    if (this_cpu().idle != nullptr &&
        (this_cpu().idle->state == process::State::ready || this_cpu().idle->state == process::State::running)) {
        g_schedule_cursor = thread_index(this_cpu().idle);
        return this_cpu().idle;
    }

    return nullptr;
}

void note_woken(process::Thread& thread) {
    if (&thread == this_cpu().current) {
        return;
    }
    this_cpu().resched_pending = true;
}

// Los tres complete_blocked_* piden el mismo wakeup preemptivo. Al principio
// solo lo hacia complete_blocked_wait, y eso dejaba a los PIPES sin el: un RPC
// sincrono -- escribir la request y bloquearse leyendo la reply, que es como
// habla windowd con compositord -- despertaba al que esperaba pero lo dejaba
// esperando su turno del round-robin. Medido en el camino de display eso era el
// 99% del costo de poner un frame en pantalla (ver docs/desktop/GRAPHICS_PERF.md), con
// el trabajo de GPU real en dos ordenes de magnitud menos.
int complete_blocked_read(process::Thread& thread, int result) {
    thread.blocked_io_fd = 0;
    thread.blocked_read_buffer = 0;
    thread.blocked_read_capacity = 0;
    thread.context->rax = static_cast<uint64_t>(result);
    thread.state = process::State::ready;
    process::detail::reset_time_slice(thread);
    note_woken(thread);
    return result;
}

int complete_blocked_write(process::Thread& thread, int result) {
    thread.blocked_io_fd = 0;
    thread.blocked_write_buffer = 0;
    thread.blocked_write_length = 0;
    thread.blocked_write_progress = 0;
    thread.context->rax = static_cast<uint64_t>(result);
    thread.state = process::State::ready;
    process::detail::reset_time_slice(thread);
    note_woken(thread);
    return result;
}


size_t pipe_copy_out(process::Process& proc, process::Pipe& pipe, uint64_t user_address, size_t count) {
    const size_t to_copy = count < pipe.size ? count : pipe.size;
    if (to_copy == 0) {
        return 0;
    }

    const size_t first = (pipe.read_pos + to_copy <= process::detail::kPipeCapacity) ? to_copy : (process::detail::kPipeCapacity - pipe.read_pos);
    if (!write_to_process_memory(proc, user_address, pipe.buffer + pipe.read_pos, first)) {
        return 0;
    }
    if (to_copy > first) {
        if (!write_to_process_memory(proc, user_address + first, pipe.buffer, to_copy - first)) {
            return 0;
        }
    }

    pipe.read_pos = (pipe.read_pos + to_copy) % process::detail::kPipeCapacity;
    pipe.size -= to_copy;
    return to_copy;
}



void wake_waiting_parent(process::Process& child) {
    process::Thread* parent = find_waiting_parent_for(child);
    if (parent == nullptr) {
        if (child.orphaned) {
            reap_process(child);
        }
        return;
    }

    write_int_to_process_memory(*parent->process, parent->wait_status_address, child.exit_code);
    clear_wait_state(*parent);
    parent->context->rax = child.pid;
    parent->state = process::State::ready;
    process::detail::reset_time_slice(*parent);
    reap_process(child);
}

void wake_sleepers(uint64_t current_tick) {
    for (process::Thread& thread : used_thread_slots()) {
        if (thread.state != process::State::sleeping || thread.wake_deadline_ms > current_tick) {
            continue;
        }

        thread.wake_deadline_ms = 0;
        thread.context->rax = 0;
        thread.state = process::State::ready;
        process::detail::reset_time_slice(thread);
    }
}

void wake_wait_timeouts(uint64_t current_tick) {
    for (process::Thread& thread : used_thread_slots()) {
        if (thread.state != process::State::blocked_wait ||
            (thread.wait_reason != process::WaitReason::object &&
             thread.wait_reason != process::WaitReason::address) ||
            thread.wake_deadline_ms == 0 ||
            thread.wake_deadline_ms > current_tick) {
            continue;
        }

        process::detail::complete_blocked_wait(thread, negative_error(SX_ETIMEDOUT));
    }
}


void wake_blocked_readers_for_pipe(process::Pipe& pipe) {
    for (process::Thread& thread : used_thread_slots()) {
        if (thread.state != process::State::blocked_read) {
            continue;
        }
        if (thread.blocked_io_fd >= process::kMaxFileHandles) {
            continue;
        }

        process::Process& proc = *thread.process;
        object::IoObject* file = object::as_io(proc.handles[thread.blocked_io_fd].object);
        if (file == nullptr || file->kind != process::HandleKind::pipe || file->pipe != &pipe) {
            continue;
        }

        const int result = process::detail::try_pipe_read(proc, *file, thread.blocked_read_buffer, static_cast<size_t>(thread.blocked_read_capacity));
        if (result == process::kBlockedResult) {
            continue;
        }
        complete_blocked_read(thread, result);
    }

    // Y los que esperaban ese pipe por poll en vez de por read.
    recheck_parked_threads(0, false);
}

void wake_blocked_writers_for_pipe(process::Pipe& pipe) {
    for (process::Thread& thread : used_thread_slots()) {
        if (thread.state != process::State::blocked_write) {
            continue;
        }
        if (thread.blocked_io_fd >= process::kMaxFileHandles) {
            continue;
        }

        process::Process& proc = *thread.process;
        object::IoObject* file = object::as_io(proc.handles[thread.blocked_io_fd].object);
        if (file == nullptr || file->kind != process::HandleKind::pipe || file->pipe != &pipe) {
            continue;
        }

        const int step = process::detail::pipe_write_chunk(
            proc,
            pipe,
            thread.blocked_write_buffer,
            static_cast<size_t>(thread.blocked_write_length),
            static_cast<size_t>(thread.blocked_write_progress));

        if (step == process::kBlockedResult) {
            continue;
        }
        if (step < 0) {
            complete_blocked_write(thread, step);
            continue;
        }

        thread.blocked_write_progress += static_cast<uint64_t>(step);
        if (thread.blocked_write_progress >= thread.blocked_write_length) {
            complete_blocked_write(thread, static_cast<int>(thread.blocked_write_length));
        }
    }

    // Hacer lugar en el pipe es lo que un poll por POLLOUT esta esperando.
    recheck_parked_threads(0, false);
}



bool open_file_can_read(const object::IoObject& file) {
    switch (file.kind) {
        case process::HandleKind::tty:
            return tty::main().line_ready;
        case process::HandleKind::vnode:
            return file.node != nullptr;
        case process::HandleKind::pipe:
            return file.pipe != nullptr && (file.pipe->size != 0 || file.pipe->writer_refs == 0);
        case process::HandleKind::device:
            return device::can_read(file.device);
        case process::HandleKind::socket:
            return net::socket_can_read(file.socket);
        default:
            return false;
    }
}

bool open_file_can_write(const object::IoObject& file) {
    switch (file.kind) {
        case process::HandleKind::tty:
            return true;
        case process::HandleKind::vnode:
            return file.node != nullptr;
        case process::HandleKind::pipe:
            return file.pipe != nullptr && file.pipe->reader_refs != 0 && file.pipe->size < process::detail::kPipeCapacity;
        case process::HandleKind::device:
            return true;
        case process::HandleKind::socket:
            return net::socket_can_write(file.socket);
        default:
            return false;
    }
}

bool open_file_has_hangup(const object::IoObject& file) {
    switch (file.kind) {
        case process::HandleKind::pipe:
            return file.pipe != nullptr && (file.pipe->reader_refs == 0 || file.pipe->writer_refs == 0);
        case process::HandleKind::socket:
            return net::socket_has_hangup(file.socket);
        default:
            return false;
    }
}





/*
 * poll() bloquea al proceso, no gira en su contexto.
 *
 * Hasta sep 2026 este syscall era un bucle con hlt: el proceso nunca salia de
 * State::running, asi que cada tick del timer de la espera se le cargaba a EL
 * -- cualquier programa que hiciera poll se veia al 100% de CPU, y el proceso
 * ocioso, lo unico que hace visible la ociosidad, casi no corria. windowd daba
 * 97-99% en QEMU y en VirtualBox mientras su propia instrumentacion media ~1%
 * de trabajo real. Ver docs/userland/SYSTEM_MONITORING.md.
 *
 * Ahora se prueba una vez y, si no hay nada listo, el proceso se estaciona con
 * su subsistema y lo despierta wake_parked_waiters() desde el tick. La
 * granularidad de la espera no cambia: el bucle viejo tampoco miraba mas
 * seguido que eso, porque entre chequeo y chequeo estaba halteado.
 */
void recheck_parked_threads(uint64_t current_tick, bool honour_deadline) {
    for (process::Thread& thread : used_thread_slots()) {
        // The kernel only knows the thread is parked with a deadline: WHAT it
        // waits for belongs to the layer that parked it (slice C). It used to
        // say wait_reason == poll and re-evaluate the descriptors right here.
        if (thread.state != process::State::blocked_wait) {
            continue;
        }
        // Only a wake or its deadline ends an address wait; it is no
        // subsystem's park either.
        if (thread.wait_reason == process::WaitReason::address) {
            continue;
        }
        // An object wait that names a stream has no signal to wake it: the
        // stream becoming readable is noticed here, where pipe writes, object
        // signals and the tick already come to look.
        if (thread.wait_reason == process::WaitReason::object && sx::can_complete_object_wait(thread)) {
            const int result = sx::consume_object_wait(thread);
            if (result >= 0) {
                process::detail::complete_blocked_wait(thread, result);
                continue;
            }
        }
        subsystem::recheck_parked_thread(thread, current_tick, honour_deadline);
    }
}

/*
 * Despertar por EVENTO: lo llaman los mismos lugares donde el kernel ya
 * despierta a un lector o a un waiter -- un pipe que recibe datos, un objeto
 * que se senaliza --, en el contexto del que provoco el cambio.
 *
 * Sin esto, un proceso parado en poll no se entera hasta el proximo tick, y esa
 * latencia entra ENTERA en cada vuelta del compositor: el cliente publica su
 * frame, windowd lo espera por poll, y el frame se queda esperando el tick. El
 * bucle viejo no tenia el problema porque su hlt se despertaba con CUALQUIER
 * interrupcion y volvia a mirar en el acto; un tick de 1 ms nominal, ademas, es
 * mucho mas que 1 ms de reloj real cuando el hipervisor junta interrupciones.
 * Esto lo devuelve, y de paso mejor que antes: se despierta por el evento y no
 * por la siguiente interrupcion cualquiera.
 */
void wake_parked_waiters(uint64_t current_tick) {
    bool any_waiting = false;

    for (const process::Thread& thread : used_thread_slots()) {
        if (thread.state == process::State::blocked_wait) {
            any_waiting = true;
            break;
        }
    }
    if (!any_waiting) {
        return;
    }

    net::poll();
    recheck_parked_threads(current_tick, true);
}

/*
 * Re-intenta los read() estacionados sobre sockets. Es el gemelo de
 * wake_parked_waiters() y tiene el mismo motivo para llamar a net::poll(): el
 * pump de red no tiene latido propio, y el que espera datos no puede ser el que
 * los va a buscar si esta dormido.
 *
 * Que este despertador exista es ademas lo que hace honesto el plazo por
 * defecto de SO_RCVTIMEO. POSIX dice que 0 es esperar indefinidamente; con el
 * bucle viejo, un programa parado para siempre en un read se veia al 100% de
 * CPU para siempre.
 */
// Igual que el tope por segmento de net::, y por el mismo motivo: es lo que
// entra en un scratch del stack del kernel.
constexpr size_t kSocketReadChunk = 1024;

void wake_blocked_socket_readers(uint64_t current_tick) {
    bool any_waiting = false;

    for (const process::Thread& thread : used_thread_slots()) {
        if (thread.state != process::State::blocked_read || thread.blocked_io_fd >= process::kMaxFileHandles) {
            continue;
        }
        const object::IoObject* file = object::as_io(thread.process->handles[thread.blocked_io_fd].object);
        if (file != nullptr && file->kind == process::HandleKind::socket) {
            any_waiting = true;
            break;
        }
    }
    if (!any_waiting) {
        return;
    }

    net::poll();

    for (process::Thread& thread : used_thread_slots()) {
        if (thread.state != process::State::blocked_read || thread.blocked_io_fd >= process::kMaxFileHandles) {
            continue;
        }
        process::Process& proc = *thread.process;
        object::IoObject* file = object::as_io(proc.handles[thread.blocked_io_fd].object);
        if (file == nullptr || file->kind != process::HandleKind::socket || file->socket == nullptr) {
            continue;
        }

        uint8_t scratch[kSocketReadChunk] = {};
        const size_t want = thread.blocked_read_capacity < sizeof(scratch)
            ? static_cast<size_t>(thread.blocked_read_capacity)
            : sizeof(scratch);
        int result = net::read_socket_bytes(file->socket, scratch, want, true);
        if (result > 0 &&
            !write_to_process_memory(proc, thread.blocked_read_buffer, scratch, static_cast<size_t>(result))) {
            result = negative_error(SX_EINVAL);
        }
        if (result == negative_error(SX_EAGAIN)) {
            if (thread.wake_deadline_ms != 0 && thread.wake_deadline_ms <= current_tick) {
                thread.wake_deadline_ms = 0;
                complete_blocked_read(thread, negative_error(SX_ETIMEDOUT));
            }
            continue;
        }
        thread.wake_deadline_ms = 0;
        complete_blocked_read(thread, result);
    }
}


/*
 * Aviso al padre de que su hijo termino (rebanada D de aislamiento).
 *
 * Antes esto marcaba los bits de senal pendiente del proceso con SIGCHLD y
 * nada mas: ningun camino del kernel los leia, no hay entrega de senales a
 * userland (no hay sigaction ni frame de senal), asi que eran estado muerto
 * que solo servia para nombrar senales POSIX en el kernel. Lo que de verdad
 * despierta a un padre esperando a su hijo es wake_waiting_parent(), que es
 * neutro y se queda.
 *
 * El aviso de que un hijo termino, para el que espera, sigue siendo
 * waitpid -- que el subsistema nativo tambien usa (SXN_SYS_WAITPID).
 */
void note_child_exited(process::Process& child) {
    (void)child;
}




} // namespace

namespace process {
namespace detail {
// Forward declarations; definitions follow in file order.
uint64_t now_ms();
void proc_log(const char* format, ...);
void reset_time_slice(Thread& thread);
uint32_t init_pid();
Process* allocate_process_slot(bool idle);
Thread* allocate_thread(Process& proc);
void release_thread(Thread& thread);
void reset_process_slot(Process& proc);
void release_address_space(Process& proc);
void release_process_resources(Process& proc);
bool inherit_all_handles(Process& child, const Process& parent);
object::IoObject* allocate_io_object();
void discard_io_object(object::IoObject*& file_ptr);
uint32_t object_access_for_open_flags(uint32_t open_flags);
int allocate_fd(Process& proc, object::Header* handle_object, uint32_t access, uint32_t flags);
int install_handle(Process& proc, int fd, object::Header* handle_object, uint32_t access, uint32_t flags);
object::IoObject* fd_to_io_object(Process& proc, uint64_t fd, uint32_t desired_access);
object::Header* lookup_handle(Process& proc, uint64_t fd, uint32_t desired_access);
Pipe* allocate_pipe();
int try_pipe_read(Process& proc, object::IoObject& file, uint64_t user_address, size_t count);
int pipe_write_chunk(Process& proc, Pipe& pipe, uint64_t user_address, size_t count, size_t progress);
int begin_object_wait(Thread& self, object::Header* const* handles, size_t count, bool wait_all, int64_t timeout_ms);
// The POSIX translation uses these too: the end of a poll and the evaluation
// of a descriptor (slice C).
int complete_blocked_wait(Thread& thread, int result);
short poll_open_file(const object::IoObject* file, short events);
void wake_waiters_for_object(object::Header* handle_object);
void terminate_process(Process& proc, int exit_code);
bool vnode_has_open_references(const vfs::Vnode& node);
void destroy_io_object(object::Header* header);

uint64_t now_ms() {
    const uint64_t monotonic = timer::monotonic_ms();
    if (monotonic != 0) {
        return monotonic;
    }
    return (timer::ticks() * 1000ULL) / (timer::frequency_hz() != 0 ? timer::frequency_hz() : 1);
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


void reset_time_slice(process::Thread& thread) {
    thread.time_slice = thread.process->idle ? 1 : kDefaultTimeSlice;
}

uint32_t object_access_for_open_flags(uint32_t open_flags) {
    uint32_t access = object::access_query | object::access_synchronize;
    if ((open_flags & process::open_read) != 0) {
        access |= object::access_read;
    }
    if ((open_flags & process::open_write) != 0) {
        access |= object::access_write;
    }
    return access;
}

process::Process* allocate_process_slot(bool idle) {
    for (process::Process& slot : g_processes) {
        if (slot.life == process::Lifecycle::unused) {
            memset(&slot, 0, sizeof(slot));
            slot.pid = g_next_pid++;
            slot.life = process::Lifecycle::alive;
            slot.idle = idle;
            return &slot;
        }
    }
    return nullptr;
}

void reset_process_slot(process::Process& proc) {
    const bool idle = proc.idle;
    memset(&proc, 0, sizeof(proc));
    proc.idle = idle;
    proc.life = process::Lifecycle::unused;
}

process::Thread* allocate_thread(process::Process& proc) {
    for (process::Thread& slot : g_threads) {
        if (slot.state != process::State::unused) {
            continue;
        }

        const uint32_t tid = proc.thread_count == 0 ? proc.pid : g_next_pid;
        object::ThreadObject* const thread_object = object::create_thread_object(tid, proc.pid);
        if (thread_object == nullptr) {
            return nullptr;
        }
        memory::PageAllocation kernel_stack = {};
        if (!memory::allocate_contiguous_pages(kKernelStackPages, kernel_stack)) {
            // Nobody holds a reference yet: destroy it the way the last
            // release would.
            thread_object->header.destroy(&thread_object->header);
            return nullptr;
        }
        if (tid == g_next_pid) {
            g_next_pid += 1;
        }
        memset(&slot, 0, sizeof(slot));
        slot.tid = tid;
        slot.object = thread_object;
        object::retain(&thread_object->header);
        slot.state = process::State::ready;
        slot.process = &proc;
        slot.kernel_stack_base = reinterpret_cast<uint64_t>(kernel_stack.virtual_address);
        slot.kernel_stack_size = kKernelStackPages * memory::kPageSize;
        reset_time_slice(slot);
        arch::fpu_init_area(slot.fpu_state);
        proc.thread_count += 1;
        const size_t index = thread_index(&slot);
        if (index >= g_thread_bound) {
            g_thread_bound = index + 1;
        }
        return &slot;
    }
    return nullptr;
}

// The thread running here can be released too: a process that exits reaps
// itself when it is an orphan or its parent is already waiting. Its kernel
// stack stays in use until the scheduler switches away, which is safe only
// because nothing allocates in between and the big kernel lock is held until
// resume_context has left this stack. A thread that exits on its own while
// its process lives on must not rely on that (THREADS_ROADMAP.md, phase 1).
void release_thread(process::Thread& thread) {
    // Another core still running on it would be resuming freed memory: a
    // thread is released only once every core has left it.
    for (uint32_t index = 0; index < smp::kMaxCpus; ++index) {
        process_assert(index == this_cpu().index || smp::g_cpu_state[index].current != &thread,
            "process: releasing a thread another core is running");
    }
    if (thread.sleep_timer != nullptr) {
        object::Header* header = &thread.sleep_timer->header;
        object::release(header);
        thread.sleep_timer = nullptr;
    }
    release_kernel_stack(thread);
    process::free_thread_subsystem_state(thread);
    if (thread.object != nullptr) {
        object::Header* header = &thread.object->header;
        object::release(header);
        thread.object = nullptr;
    }
    for (smp::Cpu& cpu : smp::g_cpu_state) {
        if (cpu.retired == &thread) {
            cpu.retired = nullptr;
        }
    }
    if (thread.process != nullptr && thread.process->thread_count != 0) {
        thread.process->thread_count -= 1;
    }
    memset(&thread, 0, sizeof(thread));
    thread.state = process::State::unused;
}

void release_address_space(process::Process& proc) {
    vm::destroy_address_space(proc.address_space);
}

void release_process_resources(process::Process& proc) {
    for (process::Thread& thread : used_thread_slots()) {
        if (thread.state != process::State::unused && thread.process == &proc) {
            release_thread(thread);
        }
    }
    free_subsystem_state(proc);
    process::detail::release_address_space(proc);
}

object::IoObject* allocate_io_object() {
    for (object::IoObject& file : g_io_objects) {
        if (!file.in_use) {
            memset(&file, 0, sizeof(file));
            file.in_use = true;
            file.header.type = object::Type::io;
            return &file;
        }
    }
    return nullptr;
}

process::Pipe* allocate_pipe() {
    for (size_t index = 0; index < process::detail::kMaxPipeCount; ++index) {
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

/* A pipe and its two ends, each end holding one reference that the caller
 * owns: install them in a handle table (which takes its own) and release, or
 * just release. Shared by every caller that needs a fresh channel without
 * going through a subsystem's notion of descriptors -- a port connection, for
 * one. */
bool create_pipe_pair(object::IoObject*& reader, object::IoObject*& writer) {
    process::Pipe* pipe = allocate_pipe();

    reader = nullptr;
    writer = nullptr;
    if (pipe == nullptr) {
        return false;
    }
    reader = allocate_io_object();
    writer = allocate_io_object();
    if (reader == nullptr || writer == nullptr) {
        discard_io_object(reader);
        discard_io_object(writer);
        memset(pipe, 0, sizeof(*pipe));
        return false;
    }
    reader->kind = process::HandleKind::pipe;
    reader->open_flags = process::open_read;
    reader->pipe = pipe;
    reader->header.destroy = destroy_io_object;
    writer->kind = process::HandleKind::pipe;
    writer->open_flags = process::open_write;
    writer->pipe = pipe;
    writer->header.destroy = destroy_io_object;
    pipe->reader_refs = 1;
    pipe->writer_refs = 1;
    object::retain(&reader->header);
    object::retain(&writer->header);
    return true;
}

void discard_io_object(object::IoObject*& file_ptr) {
    if (file_ptr == nullptr) {
        return;
    }
    process_assert(file_ptr->header.refcount == 0, "process: discard referenced io object");
    memset(file_ptr, 0, sizeof(*file_ptr));
    file_ptr = nullptr;
}

int install_handle(process::Process& proc, int fd, object::Header* handle_object, uint32_t access, uint32_t flags) {
    if (fd < 0 || fd >= static_cast<int>(process::kMaxFileHandles) || handle_object == nullptr) {
        return negative_error(SX_EBADF);
    }

    if (proc.handles[fd].object != nullptr) {
        object::release(proc.handles[fd].object);
    }
    object::retain(handle_object);
    proc.handles[fd].object = handle_object;
    proc.handles[fd].granted_access = access;
    proc.handles[fd].flags = flags;
    return fd;
}

int allocate_fd(process::Process& proc, object::Header* handle_object, uint32_t access, uint32_t flags) {
    const int fd = allocate_fd_slot(proc);
    if (fd < 0) {
        return fd;
    }
    return install_handle(proc, fd, handle_object, access, flags);
}

int try_pipe_read(process::Process& proc, object::IoObject& file, uint64_t user_address, size_t count) {
    process::Pipe* pipe = file.pipe;
    if (pipe == nullptr || !pipe->in_use) {
        return negative_error(SX_EBADF);
    }
    if (count == 0) {
        return 0;
    }
    if (pipe->size == 0) {
        return pipe->writer_refs == 0 ? 0 : process::kBlockedResult;
    }

    const size_t copied = pipe_copy_out(proc, *pipe, user_address, count);
    if (copied == 0 && count != 0) {
        return negative_error(SX_EINVAL);
    }
    wake_blocked_writers_for_pipe(*pipe);
    return static_cast<int>(copied);
}

int pipe_write_chunk(process::Process& proc, process::Pipe& pipe, uint64_t user_address, size_t count, size_t progress) {
    if (pipe.reader_refs == 0) {
        return negative_error(SX_EPIPE);
    }
    if (count == 0) {
        return 0;
    }
    if (pipe.size == process::detail::kPipeCapacity) {
        return process::kBlockedResult;
    }

    const size_t remaining = count - progress;
    const size_t writable = process::detail::kPipeCapacity - pipe.size;
    const size_t total = writable < remaining ? writable : remaining;
    uint8_t scratch[process::detail::kPipeChunkSize] = {};
    size_t written = 0;

    while (written < total) {
        const size_t step = (total - written) < sizeof(scratch) ? (total - written) : sizeof(scratch);
        if (!read_from_process_memory(proc, scratch, user_address + progress + written, step)) {
            return negative_error(SX_EINVAL);
        }

        size_t copied = 0;
        while (copied < step) {
            const size_t contiguous = pipe.write_pos + (step - copied) <= process::detail::kPipeCapacity
                ? (step - copied)
                : (process::detail::kPipeCapacity - pipe.write_pos);
            memcpy(pipe.buffer + pipe.write_pos, scratch + copied, contiguous);
            pipe.write_pos = (pipe.write_pos + contiguous) % process::detail::kPipeCapacity;
            pipe.size += contiguous;
            copied += contiguous;
        }

        written += step;
    }

    wake_blocked_readers_for_pipe(pipe);
    return static_cast<int>(written);
}

void wake_waiters_for_object(object::Header* handle_object) {
    if (!sx::is_waitable(handle_object)) {
        return;
    }

    for (process::Thread& thread : used_thread_slots()) {
        if (thread.state != process::State::blocked_wait ||
            thread.wait_reason != process::WaitReason::object) {
            continue;
        }
        bool references_object = false;
        for (size_t index = 0; index < thread.wait_handle_count; ++index) {
            if (thread.waited_objects[index] == handle_object) {
                references_object = true;
                break;
            }
        }
        if (!references_object || !sx::can_complete_object_wait(thread)) {
            continue;
        }

        const int result = sx::consume_object_wait(thread);
        if (result < 0) {
            continue;
        }
        process::detail::complete_blocked_wait(thread, result);
    }

    // Un objeto senalizado tambien puede ser lo que espera un poll. El evento de
    // submit de un cliente grafico entra por aca, que es el camino por el que el
    // compositor se entera de que hay un frame.
    recheck_parked_threads(0, false);
}

object::Header* lookup_handle(process::Process& proc, uint64_t fd, uint32_t desired_access) {
    if (fd >= process::kMaxFileHandles) {
        return nullptr;
    }
    process::HandleEntry& entry = proc.handles[fd];
    if (entry.object == nullptr) {
        return nullptr;
    }
    if ((entry.granted_access & desired_access) != desired_access) {
        return nullptr;
    }
    return entry.object;
}

object::IoObject* fd_to_io_object(process::Process& proc, uint64_t fd, uint32_t desired_access) {
    return object::as_io(process::detail::lookup_handle(proc, fd, desired_access));
}


bool inherit_all_handles(process::Process& child, const process::Process& parent) {
    for (size_t fd = 0; fd < process::kMaxFileHandles; ++fd) {
        clear_handle_entry(child.handles[fd]);
    }
    for (size_t fd = 0; fd < process::kMaxFileHandles; ++fd) {
        object::Header* handle_object = parent.handles[fd].object;
        if (handle_object == nullptr) {
            continue;
        }
        if (process::detail::install_handle(
                child,
                static_cast<int>(fd),
                handle_object,
                parent.handles[fd].granted_access,
                parent.handles[fd].flags) < 0) {
            release_all_handles(child);
            return false;
        }
    }
    return true;
}

void terminate_process(process::Process& proc, int exit_code) {
    if (proc.idle || proc.life != process::Lifecycle::alive) {
        return;
    }
    // The first request decides the code, whichever thread finishes the job.
    if (!proc.exit_requested) {
        proc.exit_requested = true;
        proc.exit_code = exit_code;
    }
    exit_code = proc.exit_code;

    // A thread running on another core: its kernel stack and the process's
    // page tables are in use right there, and releasing them now would pull
    // them out from under it. That core stops it at its next entry to the
    // kernel (tick, IPI or end of syscall) and calls back here; the last
    // thread to stop finds none running and finishes the process.
    //
    // So does one preempted while asleep inside a syscall (ready, with its
    // context in ring 0): it resumes on its own core, gives up the call
    // (current_thread_must_stop) and releases what the call held -- a socket
    // call holds a reference to its handle's object -- before it stops.
    bool deferred = false;
    for (process::Thread& thread : used_thread_slots()) {
        if (thread.process != &proc || &thread == this_cpu().current ||
            (thread.state != process::State::running && !stopped_in_kernel(thread))) {
            continue;
        }
        smp::kick(thread.cpu);
        deferred = true;
    }
    if (deferred) {
        return;
    }

    const uint32_t exiting_pid = proc.pid;
    process::detail::proc_log("proc: terminate pid=%u status=%d\n", proc.pid, exit_code);
    note_child_exited(proc);
    release_all_handles(proc);
    /* A clean exit releases the GPU via the device close handler in the owner's
       own context; a kill runs that handler in the killer's context, where the
       owner check fails. Reclaim the session here by pid so a crashed compositor
       does not leave the GPU acquired forever. */
    display::release_session_for(exiting_pid);
    /* Y lo mismo con el audio: /dev/audio0 tiene un unico dueno y windowd mata
       al cliente cuando cierran la ventana, asi que sin esto la sesion queda
       tomada por un pid muerto y el proximo programa con sonido recibe EBUSY. */
    audio_device::release_session_for(exiting_pid);
    // Every thread ends with the process. Their slots and kernel stacks stay
    // until the reap: one of them may be the stack this code is running on.
    for (process::Thread& thread : used_thread_slots()) {
        if (thread.state == process::State::unused || thread.process != &proc) {
            continue;
        }
        clear_wait_state(thread);
        thread.state = process::State::zombie;
        object::end_thread_object(thread.object, exit_code);
    }
    proc.life = process::Lifecycle::zombie;
    // A handle to one of these threads may be waited on elsewhere: another
    // process that inherited it, or nobody yet. Only after every thread is a
    // zombie, so a sibling cannot be woken by its own process's end.
    for (process::Thread& thread : used_thread_slots()) {
        if (thread.state == process::State::zombie && thread.process == &proc && thread.object != nullptr) {
            wake_waiters_for_object(&thread.object->header);
        }
    }
    wake_waiting_parent(proc);
    reparent_orphaned_children(exiting_pid);
}

int begin_object_wait(process::Thread& self, object::Header* const* handles, size_t count, bool wait_all, int64_t timeout_ms) {
    const int consumed = sx::try_consume_objects(self, handles, count, wait_all);
    if (consumed != sx::negative_error(SX_EAGAIN)) {
        return consumed;
    }
    if (timeout_ms == 0) {
        clear_wait_state(self);
        return negative_error(SX_ETIMEDOUT);
    }

    self.wake_deadline_ms = timeout_ms > 0 ? (process::detail::now_ms() + static_cast<uint64_t>(timeout_ms)) : 0;
    self.state = process::State::blocked_wait;
    return process::kBlockedResult;
}

int complete_blocked_wait(process::Thread& thread, int result) {
    clear_wait_state(thread);
    thread.context->rax = static_cast<uint64_t>(result);
    thread.state = process::State::ready;
    process::detail::reset_time_slice(thread);
    note_woken(thread);
    return result;
}

short poll_open_file(const object::IoObject* file, short events) {
    if (file == nullptr) {
        return static_cast<short>(SAVANXP_POLLNVAL);
    }

    short revents = 0;
    if ((events & SAVANXP_POLLIN) != 0 && open_file_can_read(*file)) {
        revents = static_cast<short>(revents | SAVANXP_POLLIN);
    }
    if ((events & SAVANXP_POLLOUT) != 0 && open_file_can_write(*file)) {
        revents = static_cast<short>(revents | SAVANXP_POLLOUT);
    }
    if (open_file_has_hangup(*file)) {
        revents = static_cast<short>(revents | SAVANXP_POLLHUP);
    }
    return revents;
}

uint32_t init_pid() {
    return g_init_pid;
}
void destroy_io_object(object::Header* header) {
    object::IoObject* file = object::as_io(header);
    if (file == nullptr) {
        return;
    }

    if (file->kind == process::HandleKind::pipe && file->pipe != nullptr) {
        if ((file->open_flags & process::open_read) != 0) {
            process_assert(file->pipe->reader_refs != 0, "process: pipe reader underflow");
            file->pipe->reader_refs -= 1;
            wake_blocked_writers_for_pipe(*file->pipe);
        }
        if ((file->open_flags & process::open_write) != 0) {
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
bool vnode_has_open_references(const vfs::Vnode& node) {
    for (const object::IoObject& file : g_io_objects) {
        if (file.in_use && file.kind == process::HandleKind::vnode && file.node == &node) {
            return true;
        }
    }
    return false;
}
} // namespace detail
} // namespace process

namespace process {
uint64_t current_uptime_ms() {
    return process::detail::now_ms();
}

/*
 * Estado privado del subsistema: el kernel reserva, copia y libera estos bytes
 * sin interpretarlos (isolation slice B). Ver el comentario de la seccion
 * anonima de arriba y subsystems/posix/kernel/posix_process_state.hpp.
 *
 * Las tres se llaman entre si y se declaran en process.hpp, asi que no hace
 * falta adelantar nada aqui.
 */

void ensure_subsystem_state(Process& proc) {
    const size_t needed = subsystem::process_state_size(proc.subsystem_id);
    if (needed == 0) {
        free_subsystem_state(proc);
        return;
    }
    if (proc.subsystem_state != nullptr && proc.subsystem_state_bytes == needed) {
        return;
    }

    uint8_t* fresh = static_cast<uint8_t*>(heap::allocate(needed));
    // Sin memoria no es motivo para no crear el proceso: el subsistema ve
    // estado vacio (nullptr), que POSIX trata igual que "/".
    if (fresh != nullptr) {
        memset(fresh, 0, needed);
    }
    free_subsystem_state(proc);
    proc.subsystem_state = fresh;
    proc.subsystem_state_bytes = fresh != nullptr ? needed : 0;
}

void free_subsystem_state(Process& proc) {
    if (proc.subsystem_state != nullptr) {
        heap::free(proc.subsystem_state);
        proc.subsystem_state = nullptr;
    }
    proc.subsystem_state_bytes = 0;
}

uint8_t* thread_subsystem_state(Thread& thread) {
    const size_t needed = subsystem::thread_state_size(thread.process->subsystem_id);
    if (needed == 0) {
        free_thread_subsystem_state(thread);
        return nullptr;
    }
    if (thread.subsystem_state != nullptr && thread.subsystem_state_bytes == needed) {
        return thread.subsystem_state;
    }
    free_thread_subsystem_state(thread);
    uint8_t* fresh = static_cast<uint8_t*>(heap::allocate(needed));
    if (fresh != nullptr) {
        memset(fresh, 0, needed);
        thread.subsystem_state = fresh;
        thread.subsystem_state_bytes = needed;
    }
    return fresh;
}

void free_thread_subsystem_state(Thread& thread) {
    if (thread.subsystem_state != nullptr) {
        heap::free(thread.subsystem_state);
        thread.subsystem_state = nullptr;
    }
    thread.subsystem_state_bytes = 0;
}

// fork hereda el estado privado tal cual: el hijo arranca donde estaba el
// padre, que para POSIX significa el mismo cwd.
void copy_subsystem_state(Process& child, const process::Process& parent) {
    child.subsystem_id = parent.subsystem_id;
    ensure_subsystem_state(child);
    if (child.subsystem_state != nullptr && parent.subsystem_state != nullptr &&
        child.subsystem_state_bytes == parent.subsystem_state_bytes) {
        memcpy(child.subsystem_state, parent.subsystem_state, parent.subsystem_state_bytes);
    }
}

void clear_wait_state(process::Thread& thread) {
    thread.wait_reason = process::WaitReason::none;
    thread.wait_handle_count = 0;
    thread.wait_all = 0;
    thread.wait_reserved = 0;
    thread.waiting_for_pid = 0;
    thread.wait_status_address = 0;
    thread.wait_address = 0;
    thread.wait_ticket = 0;
    for (size_t index = 0; index < process::kMaxWaitHandles; ++index) {
        thread.waited_objects[index] = nullptr;
    }
    thread.wake_deadline_ms = 0;
}

void switch_to_thread(process::Thread* target) {
    if (target == nullptr) {
        return;
    }

    // A real thread switch: save the outgoing thread's FPU/SSE state and
    // restore the incoming one's. If target already is this_cpu().current, it
    // keeps running and its FPU is still live in the registers -- an FXRSTOR
    // here would overwrite it with a stale copy, so it is skipped. The kernel
    // is -mno-sse and does not touch the FPU in between.
    // The user FS/GS bases go the same way, for the same reason.
    if (this_cpu().current != target) {
        if (this_cpu().current != nullptr) {
            arch::fpu_save(this_cpu().current->fpu_state);
            arch::save_user_segment_bases(this_cpu().current->fs_base, this_cpu().current->gs_base);
        }
        arch::fpu_restore(target->fpu_state);
        arch::load_user_segment_bases(target->fs_base, target->gs_base);
    }

    this_cpu().current = target;
    target->state = process::State::running;
    target->cpu = this_cpu().index;
    arch::set_kernel_stack(target->kernel_stack_base + target->kernel_stack_size);
    // Two threads of one process share the address space: switching between
    // them keeps CR3, and with it the TLB. load_address_space compares against
    // the live CR3, not against the previous thread's process, which also
    // covers an exec swapping the space under the same thread; and it records
    // the space for the TLB shootdown.
    vm::load_address_space(target->process->address_space);
}

// Resolucion NEUTRAL de una ruta: absoluta, o relativa a la raiz. No hay cwd
// en el kernel (rebanada B), asi que esta es la unica resolucion que el
// kernel ofrece, y la que usa el spawn nativo. La version con directorio
// actual es de POSIX y vive en subsystems/posix/kernel/.
bool resolve_path_from_root(const char* input, char* output, size_t capacity) {
    return vfs::normalize_path("/", input, output, capacity);
}

bool read_from_process_memory(const process::Process& proc, void* destination, uint64_t user_address, size_t count, bool require_write) {
    return sx::read_user_memory(proc, destination, user_address, count, require_write);
}

bool write_to_process_memory(process::Process& proc, uint64_t user_address, const void* source, size_t count) {
    return sx::write_user_memory(proc, user_address, source, count);
}

bool copy_user_string(const process::Process& proc, uint64_t user_address, char* buffer, size_t capacity) {
    return sx::copy_user_string_from(proc, user_address, buffer, capacity);
}

void write_int_to_process_memory(process::Process& proc, uint64_t user_address, int value) {
    if (user_address != 0) {
        (void)write_to_process_memory(proc, user_address, &value, sizeof(value));
    }
}

sx_status image_failure_errno(process::ImageFailure failure) {
    switch (failure) {
        case process::ImageFailure::none:
            return SX_EIO;
        case process::ImageFailure::not_found:
            return SX_ENOENT;
        case process::ImageFailure::not_a_file:
        case process::ImageFailure::permission_denied:
            return SX_EACCES;
        case process::ImageFailure::read_failed:
            return SX_EIO;
        case process::ImageFailure::bad_image:
            return SX_ENOEXEC;
        case process::ImageFailure::no_staging_memory:
        case process::ImageFailure::no_address_space:
        case process::ImageFailure::no_load_memory:
        case process::ImageFailure::no_process_slot:
            return SX_ENOMEM;
        case process::ImageFailure::no_handles:
            return SX_EBADF;
        case process::ImageFailure::threads_running:
            return SX_EAGAIN;
    }
    return SX_EIO;
}

bool prepare_exec_image(
    process::Thread& self,
    const char* path,
    int argc,
    const char* const* argv,
    process::ImageFailure& failure
) {
    failure = process::ImageFailure::none;
    process::Process& proc = *self.process;

    const vfs::Vnode* image = vfs::resolve(path);
    if (image == nullptr) {
        failure = process::ImageFailure::not_found;
        report_image_failure("exec", path, failure, 0);
        return false;
    }
    if (image->type != vfs::NodeType::file) {
        failure = process::ImageFailure::not_a_file;
        report_image_failure("exec", path, failure, image->size);
        return false;
    }
    if (!vfs::may_execute(proc.credentials, path)) {
        failure = process::ImageFailure::permission_denied;
        report_image_failure("exec", path, failure, image->size);
        return false;
    }

    // Every other thread ends before the image goes, so none can be running
    // on the address space about to be destroyed. After the cheap checks, so
    // a path that does not exist leaves the threads alone; a load that fails
    // past this point returns to a process left with only the caller.
    if (!end_other_threads(self)) {
        failure = process::ImageFailure::threads_running;
        return false;
    }

    const size_t reported_size = image->size;
    ImageSource image_source = {};
    elf::ImageReader image_reader = nullptr;
    if (!open_image_source(*image, image_source, image_reader, failure)) {
        report_image_failure("exec", path, failure, reported_size);
        return false;
    }

    vm::VmSpace new_space = {};
    if (!vm::create_address_space(new_space)) {
        failure = process::ImageFailure::no_address_space;
        report_image_failure("exec", path, failure, reported_size);
        return false;
    }
    vm::randomize_section_base(new_space, arch::random_u64() ^ proc.pid);

    elf::LoadResult load_result = {};
    elf::LoadFailure load_failure = elf::LoadFailure::none;
    if (!elf::load_user_image(image_reader, &image_source, image_source.size, new_space, argc, argv, load_result, load_failure)) {
        vm::destroy_address_space(new_space);
        failure = load_failure == elf::LoadFailure::out_of_memory
            ? process::ImageFailure::no_load_memory
            : process::ImageFailure::bad_image;
        console::printf("process: elf de '%s': %s (phdr %u)\n", path, elf::load_failure_string(load_failure), load_result.fail_index);
        report_image_failure("exec", path, failure, reported_size);
        return false;
    }

    vm::VmSpace old_space = proc.address_space;
    proc.address_space = new_space;
    vm::destroy_address_space(old_space);
    // El exec reemplaza la imagen: el subsistema pasa a ser el del binario nuevo.
    proc.subsystem_id = load_result.os_abi == elf::kOsAbiNative
        ? subsystem::Id::native
        : subsystem::Id::posix;
    // El exec puede cambiar de subsistema, asi que el estado privado se
    // revisa: si el tamano no cambio se conserva (exec no cambia el cwd de
    // POSIX), y si cambio llega a cero, que POSIX lee como "/".
    ensure_subsystem_state(proc);
    set_process_name(proc, path);
    // The image is replaced under the calling thread, which starts over at the
    // new entry point; end_other_threads already ended the rest.
    process::free_thread_subsystem_state(self);
    self.blocked_io_fd = 0;
    self.blocked_read_buffer = 0;
    self.blocked_read_capacity = 0;
    self.blocked_write_buffer = 0;
    self.blocked_write_length = 0;
    self.blocked_write_progress = 0;
    clear_wait_state(self);
    process::detail::reset_time_slice(self);
    // The new image starts without thread-local storage, as a new thread does.
    self.fs_base = 0;
    self.gs_base = 0;
    if (this_cpu().current == &self) {
        arch::load_user_segment_bases(0, 0);
    }
    fabricate_initial_context(self, load_result.entry_point, load_result.stack_pointer, load_result.accepted_argc, load_result.stack_pointer, load_result.interpreter_address, load_result.image_base);
    return true;
}

process::Process* create_process_internal(
    const char* path,
    int argc,
    const char* const* argv,
    process::Process* parent,
    int stdin_fd,
    int stdout_fd,
    int stderr_fd,
    process::ImageFailure& failure
) {
    failure = process::ImageFailure::none;

    const vfs::Vnode* image = vfs::resolve(path);
    if (image == nullptr) {
        failure = process::ImageFailure::not_found;
        report_image_failure("spawn", path, failure, 0);
        return nullptr;
    }
    if (image->type != vfs::NodeType::file) {
        failure = process::ImageFailure::not_a_file;
        report_image_failure("spawn", path, failure, image->size);
        return nullptr;
    }
    // Whoever spawns has to be allowed to run it; the first process (init) is
    // the kernel's, and the kernel may.
    const cred::Credentials spawner = parent != nullptr ? parent->credentials : cred::system();
    if (!vfs::may_execute(spawner, path)) {
        failure = process::ImageFailure::permission_denied;
        report_image_failure("spawn", path, failure, image->size);
        return nullptr;
    }

    const size_t reported_size = image->size;
    ImageSource image_source = {};
    elf::ImageReader image_reader = nullptr;
    if (!open_image_source(*image, image_source, image_reader, failure)) {
        report_image_failure("spawn", path, failure, reported_size);
        return nullptr;
    }

    process::Process* proc = process::detail::allocate_process_slot(false);
    if (proc == nullptr) {
        failure = process::ImageFailure::no_process_slot;
        report_image_failure("spawn", path, failure, reported_size);
        return nullptr;
    }

    proc->parent_pid = parent != nullptr ? parent->pid : 0;
    proc->subsystem_id = parent != nullptr ? parent->subsystem_id : subsystem::Id::posix;
    proc->credentials = spawner;
    set_process_name(*proc, path);
    if (parent != nullptr) {
        // El hijo hereda el estado privado del padre, que para POSIX incluye
        // el directorio actual.
        process::copy_subsystem_state(*proc, *parent);
    }
    else
    {
        process::ensure_subsystem_state(*proc);
    }
    if (!vm::create_address_space(proc->address_space)) {
        process::detail::reset_process_slot(*proc);
        failure = process::ImageFailure::no_address_space;
        report_image_failure("spawn", path, failure, reported_size);
        return nullptr;
    }
    vm::randomize_section_base(proc->address_space,
        arch::random_u64() ^ static_cast<uint64_t>(proc->pid));

    process::Thread* thread = process::detail::allocate_thread(*proc);
    if (thread == nullptr) {
        process::detail::release_process_resources(*proc);
        process::detail::reset_process_slot(*proc);
        failure = process::ImageFailure::no_address_space;
        report_image_failure("spawn", path, failure, reported_size);
        return nullptr;
    }

    if (parent == nullptr) {
        if (!initialize_standard_handles(*proc)) {
            process::detail::release_process_resources(*proc);
            process::detail::reset_process_slot(*proc);
            failure = process::ImageFailure::no_handles;
            report_image_failure("spawn", path, failure, reported_size);
            return nullptr;
        }
    } else {
        if (!inherit_handle(*proc, 0, *parent, stdin_fd >= 0 ? stdin_fd : 0) ||
            !inherit_handle(*proc, 1, *parent, stdout_fd >= 0 ? stdout_fd : 1) ||
            !inherit_handle(*proc, 2, *parent, stderr_fd >= 0 ? stderr_fd : 2)) {
            release_all_handles(*proc);
            process::detail::release_process_resources(*proc);
            process::detail::reset_process_slot(*proc);
            failure = process::ImageFailure::no_handles;
            report_image_failure("spawn", path, failure, reported_size);
            return nullptr;
        }
    }

    elf::LoadResult load_result = {};
    elf::LoadFailure load_failure = elf::LoadFailure::none;
    if (!elf::load_user_image(image_reader, &image_source, image_source.size, proc->address_space, argc, argv, load_result, load_failure)) {
        release_all_handles(*proc);
        process::detail::release_process_resources(*proc);
        process::detail::reset_process_slot(*proc);
        failure = load_failure == elf::LoadFailure::out_of_memory
            ? process::ImageFailure::no_load_memory
            : process::ImageFailure::bad_image;
        console::printf("process: elf de '%s': %s (phdr %u)\n", path, elf::load_failure_string(load_failure), load_result.fail_index);
        report_image_failure("spawn", path, failure, reported_size);
        return nullptr;
    }

    // El subsistema lo define el ABI del binario (e_ident[EI_OSABI]), no la
    // herencia: el valor de allocate_process_slot/parent era solo el default.
    proc->subsystem_id = load_result.os_abi == elf::kOsAbiNative
        ? subsystem::Id::native
        : subsystem::Id::posix;
    // The state copied from the parent belongs to the parent's subsystem. A
    // POSIX program started by a native one (sessiond starting windowd) would
    // otherwise run with no POSIX state at all, and poll fails on it. Same
    // subsystem: kept as copied, which for POSIX is the parent's cwd.
    process::ensure_subsystem_state(*proc);
    if (proc->subsystem_id == subsystem::Id::native) {
        console::printf("process: pid=%u marcado nativo\n", static_cast<unsigned>(proc->pid));
    }

    fabricate_initial_context(*thread, load_result.entry_point, load_result.stack_pointer, load_result.accepted_argc, load_result.stack_pointer, load_result.interpreter_address, load_result.image_base);
    return proc;
}

process::Process* child_of(uint32_t parent_pid, uint32_t pid) {
    for (process::Process& proc : g_processes) {
        if (proc.life == process::Lifecycle::unused) {
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
        if (proc.life != process::Lifecycle::unused && proc.parent_pid == parent_pid) {
            return true;
        }
    }
    return false;
}

process::Process* find_zombie_child(uint32_t parent_pid, uint32_t waited_pid) {
    for (process::Process& proc : g_processes) {
        if (proc.life != process::Lifecycle::zombie || proc.parent_pid != parent_pid) {
            continue;
        }
        if (waited_pid == process::kWaitAnyPid || proc.pid == waited_pid) {
            return &proc;
        }
    }
    return nullptr;
}

void reap_process(process::Process& proc) {
    process::detail::proc_log("proc: reap pid=%u\n", proc.pid);
    release_all_handles(proc);
    process::detail::release_process_resources(proc);
    process::detail::reset_process_slot(proc);
}

// Shared child-wait core for the POSIX and native WAITPID paths: reports a
// zombie child, rejects an unrelated pid, or parks the caller until a child
// appears. Returns the child pid (or negative errno), or kBlockedResult when
// the caller was parked and the scheduler already picked the next context.
int waitpid_child(process::Thread& self, int64_t waited, uint64_t status_address) {
    process::Process& proc = *self.process;
    const uint32_t waited_pid = waited == -1 ? process::kWaitAnyPid : static_cast<uint32_t>(waited);
    process::Process* child = find_zombie_child(proc.pid, waited_pid);
    if (child != nullptr) {
        write_int_to_process_memory(proc, status_address, child->exit_code);
        const int pid = static_cast<int>(child->pid);
        reap_process(*child);
        return pid;
    }

    if (waited_pid != process::kWaitAnyPid && child_of(proc.pid, waited_pid) == nullptr) {
        return negative_error(SX_ECHILD);
    }
    if (waited_pid == process::kWaitAnyPid && !has_child(proc.pid)) {
        return negative_error(SX_ECHILD);
    }

    clear_wait_state(self);
    self.wait_reason = process::WaitReason::child;
    self.waiting_for_pid = waited_pid;
    self.wait_status_address = status_address;
    self.state = process::State::blocked_wait;
    return process::kBlockedResult;
}

// Native spawn clones the caller's whole handle table into the child (the
// POSIX spawn only takes stdio): without a native fork this is the only
// channel that carries handles across, and per-handle opt-in stays future
// work next to FD_CLOEXEC (see docs/kernel/KERNEL_SECURITY.md). On failure the
// fully created child is reaped here, so the caller only sees ENOMEM.
bool clone_handles_for_spawn(process::Process& proc, const process::Process& parent) {
    if (!process::detail::inherit_all_handles(proc, parent)) {
        reap_process(proc);
        return false;
    }
    return true;
}

bool add_spawn_handles(process::Process& proc, const process::Process& parent, const int32_t* handles, uint32_t count) {
    for (uint32_t index = 0; index < count; ++index) {
        if (!inherit_handle(proc, static_cast<int>(3 + index), parent, handles[index])) {
            reap_process(proc);
            return false;
        }
    }
    return true;
}

bool has_runnable_non_idle() {
    for (const process::Thread& thread : used_thread_slots()) {
        if (runnable_here(thread)) {
            return true;
        }
    }
    return false;
}

process::SavedContext* choose_next_context(process::SavedContext* current_context) {
    if (this_cpu().current != nullptr && current_context != nullptr) {
        this_cpu().current->context = current_context;
    }
    // A thread that ended here earlier is off its kernel stack now, unless it
    // is the one still running this very code (exit_current_thread).
    if (this_cpu().retired != nullptr && this_cpu().retired != this_cpu().current) {
        process::detail::release_thread(*this_cpu().retired);
    }

    // Pasar por aca atiende cualquier IPI de replanificar que ande en vuelo:
    // el que llegue despues encuentra la decision ya tomada y no hace nada.
    this_cpu().kicked = false;

    process::Thread* next = pick_next_runnable();
    if (next == nullptr) {
        panic("scheduler: no runnable thread");
    }

    process::detail::reset_time_slice(*next);
    switch_to_thread(next);
    return next->context;
}

// Un proceso paso a listo: este core le entrega la CPU a la vuelta de la
// syscall, en vez de dejarlo esperando al proximo tick. Es lo mismo que con un
// solo core, y a proposito no se le pasa el despertado a un core ocioso por IPI:
// en un RPC sincrono el que despierta se bloquea enseguida esperando la
// respuesta, y cederle este core es el camino mas corto. El que queda
// desplazado lo toma cualquier core ocioso en su proximo tick.
int create_event_handle(process::Process& proc, uint32_t flags) {
    object::EventObject* event_object = sx::create_event(
        (flags & SAVANXP_EVENT_MANUAL_RESET) != 0,
        (flags & SAVANXP_EVENT_INITIAL_SET) != 0);
    if (event_object == nullptr) {
        return negative_error(SX_ENOMEM);
    }

    const uint32_t access = object::access_query | object::access_modify | object::access_synchronize;
    const int handle = process::detail::allocate_fd(proc, &event_object->header, access, object::handle_none);
    if (handle < 0) {
        object::Header* header = &event_object->header;
        object::release(header);
        return handle;
    }
    return handle;
}

int set_event_handle(process::Process& proc, uint64_t fd) {
    object::Header* handle_object = process::detail::lookup_handle(proc, fd, object::access_modify);
    object::EventObject* event_object = object::as_event(handle_object);
    if (event_object == nullptr) {
        return negative_error(SX_EBADF);
    }

    sx::set_event(event_object);
    process::detail::wake_waiters_for_object(handle_object);
    return 0;
}

int reset_event_handle(process::Process& proc, uint64_t fd) {
    object::Header* handle_object = process::detail::lookup_handle(proc, fd, object::access_modify);
    object::EventObject* event_object = object::as_event(handle_object);
    if (event_object == nullptr) {
        return negative_error(SX_EBADF);
    }

    sx::reset_event(event_object);
    return 0;
}

int create_section_handle(process::Process& proc, uint64_t size, uint32_t flags) {
    if (size == 0) {
        return negative_error(SX_EINVAL);
    }

    const uint32_t section_access = object_access_for_section_flags(
        flags,
        object::access_read | object::access_write);
    object::SectionObject* section_object = sx::create_section(size, section_access);
    if (section_object == nullptr) {
        return negative_error(SX_ENOMEM);
    }

    const uint32_t handle_access = object::access_query | object::access_synchronize | section_access;
    const int handle = process::detail::allocate_fd(proc, &section_object->header, handle_access, object::handle_none);
    if (handle < 0) {
        object::Header* header = &section_object->header;
        object::release(header);
        return handle;
    }
    return handle;
}

int open_range_section_handle(process::Process& proc, uint64_t fd, uint64_t offset, uint64_t length, uint32_t flags) {
    object::Header* handle_object = process::detail::lookup_handle(proc, fd, object::access_read);
    object::IoObject* file = object::as_io(handle_object);
    if (file == nullptr || file->node == nullptr) {
        return negative_error(SX_EBADF);
    }
    if (file->node->type != vfs::NodeType::file) {
        return negative_error(SX_EINVAL);
    }
    if ((flags & SAVANXP_SECTION_WRITE) != 0) {
        return negative_error(SX_EINVAL);
    }
    uint32_t section_access = object_access_for_section_flags(flags, object::access_read);
    section_access |= object::access_read;

    object::SectionObject* section_object =
        sx::open_file_section(*file->node, offset, length, section_access);
    if (section_object == nullptr) {
        return negative_error(SX_ENOMEM);
    }
    const uint32_t handle_access = object::access_query | object::access_synchronize | section_access;
    const int handle = process::detail::allocate_fd(proc, &section_object->header, handle_access, object::handle_none);
    if (handle < 0) {
        object::Header* header = &section_object->header;
        object::release(header);
        return handle;
    }
    return handle;
}

int open_section_handle(process::Process& proc, uint64_t fd, uint32_t flags) {
    return open_range_section_handle(proc, fd, 0, 0, flags);
}

int64_t map_view_base_handle(process::Process& proc, uint64_t fd, uint64_t base, uint32_t flags) {
    object::Header* handle_object = process::detail::lookup_handle(proc, fd, object::access_query);
    object::SectionObject* section_object = object::as_section(handle_object);
    if (section_object == nullptr) {
        return negative_error(SX_EBADF);
    }

    const uint32_t requested_access = object_access_for_section_flags(flags, section_object->access_mask);
    const bool share_on_fork = (flags & SAVANXP_VIEW_PRIVATE) == 0;
    const process::HandleEntry& entry = proc.handles[fd];
    return sx::map_view(proc, *section_object, requested_access, entry.granted_access, base, share_on_fork);
}

int64_t map_view_handle(process::Process& proc, uint64_t fd, uint32_t flags) {
    return map_view_base_handle(proc, fd, 0, flags);
}

int unmap_view_address(process::Process& proc, uint64_t base_address) {
    return sx::unmap_view(proc, base_address);
}

int wait_on_handle(process::Thread& self, uint64_t fd, int64_t timeout_ms) {
    object::Header* handle_object = process::detail::lookup_handle(*self.process, fd, object::access_synchronize);
    if (handle_object == nullptr) {
        return negative_error(SX_EBADF);
    }
    if (!sx::is_wait_target(handle_object)) {
        return negative_error(SX_EINVAL);
    }

    object::Header* handles[1] = {handle_object};
    return process::detail::begin_object_wait(self, handles, 1, false, timeout_ms);
}


/* Los argumentos de spawn/exec se copian a un buffer del heap del kernel, no
 * al stack.
 *
 * Antes eran `char argv_storage[16][64]` sobre el stack de kernel, que son 4
 * paginas en total: de ahi salian los dos topes que habia, 15 argumentos y 63
 * caracteres por argumento. El segundo era el peor de los dos -- una ruta un
 * poco larga hacia fallar el spawn entero -- y ninguno se podia levantar sin
 * comerse el stack. Con el buffer afuera, el limite pasa a ser el total de
 * bytes, que es lo que de verdad importa, y ya no lo paga cada llamada.
 * (Los topes y el struct viven en include/kernel/process.hpp, junto al resto
 * de la frontera de traduccion.) */

bool copy_spawn_arguments(
    const process::Process& proc,
    const process::SavedContext& context,
    char* path,
    size_t path_capacity,
    process::SpawnArguments& arguments
) {
    size_t used = 0;
    const int requested = static_cast<int>(static_cast<int64_t>(context.rdx));

    arguments.argc = 0;
    arguments.argv[0] = nullptr;

    if (requested < 0 || requested > process::kMaxSpawnArguments) {
        return false;
    }
    if (!copy_user_string(proc, context.rdi, path, path_capacity)) {
        return false;
    }
    if (requested != 0 && !vm::is_user_range_accessible(proc.address_space, context.rsi, static_cast<size_t>(requested) * sizeof(uint64_t), false)) {
        return false;
    }

    for (int index = 0; index < requested; ++index) {
        uint64_t arg_address = 0;
        if (!read_user_pointer(proc, context.rsi + static_cast<uint64_t>(index) * sizeof(uint64_t), arg_address)) {
            return false;
        }
        /* Si no entra se falla, no se trunca: un argumento cortado es una ruta
         * distinta, y el programa no tiene como enterarse. */
        if (used >= sizeof(arguments.storage) ||
            !copy_user_string(proc, arg_address, &arguments.storage[used],
                              sizeof(arguments.storage) - used)) {
            return false;
        }
        arguments.argv[index] = &arguments.storage[used];
        used += strlen(&arguments.storage[used]) + 1;
    }

    arguments.argc = requested;
    arguments.argv[requested] = nullptr;
    return true;
}


void initialize() {
    memset(g_processes, 0, sizeof(g_processes));
    memset(g_threads, 0, sizeof(g_threads));
    g_thread_bound = 0;
    memset(g_pipes, 0, sizeof(g_pipes));
    memset(g_pipe_storage, 0, sizeof(g_pipe_storage));
    memset(g_io_objects, 0, sizeof(g_io_objects));
    memset(&g_boot_system_info, 0, sizeof(g_boot_system_info));
    this_cpu().current = nullptr;
    this_cpu().idle = nullptr;
    g_next_pid = 1;
    g_schedule_cursor = 0;
    this_cpu().resched_pending = false;
    g_boot_system_info_ready = false;

    subsystem::reset();
    subsystem::register_dispatcher(subsystem::Id::posix, &dispatch_posix_syscall);
    subsystem::register_dispatcher(subsystem::Id::native, &dispatch_native_syscall);
    // Cada subsistema declara cuanto estado privado por proceso necesita; el
    // kernel lo reserva y lo libera sin mirar dentro (isolation slice B).
    initialize_posix_subsystem();
    initialize_native_subsystem();

    g_ready = true;
}

bool ready() {
    return g_ready;
}

Process* current() {
    return this_cpu().current != nullptr ? this_cpu().current->process : nullptr;
}

Thread* current_thread() {
    return this_cpu().current;
}

uint32_t current_pid() {
    Process* const proc = current();
    return proc != nullptr ? proc->pid : 0;
}

Thread* primary_thread(const Process& proc) {
    Thread* ended = nullptr;
    for (Thread& thread : used_thread_slots()) {
        if (thread.state == State::unused || thread.process != &proc) {
            continue;
        }
        if (thread.state != State::zombie) {
            return &thread;
        }
        if (ended == nullptr) {
            ended = &thread;
        }
    }
    return ended;
}

Process* find(uint32_t pid) {
    for (Process& proc : g_processes) {
        if (proc.life != Lifecycle::unused && proc.pid == pid) {
            return &proc;
        }
    }
    return nullptr;
}

bool snapshot_process(size_t index, savanxp_process_info& info) {
    size_t visible = 0;
    for (const Process& proc : g_processes) {
        if (proc.life == Lifecycle::unused) {
            continue;
        }
        if (visible++ != index) {
            continue;
        }

        memset(&info, 0, sizeof(info));
        info.pid = proc.pid;
        info.parent_pid = proc.parent_pid;
        info.exit_code = proc.exit_code;
        // A live process shows what its first thread is doing.
        const Thread* const thread = primary_thread(proc);
        info.state = exported_state(proc.life == Lifecycle::zombie || thread == nullptr
            ? State::zombie
            : thread->state);
        memcpy(info.name, proc.name, sizeof(info.name));
        info.cpu_ticks = proc.cpu_ticks;
        // A zombie already let go of its address space: asking it for memory
        // would return 0 anyway, but walking tables that are no longer its own
        // is not worth even trying.
        info.memory_bytes = proc.life == Lifecycle::zombie
            ? 0
            : vm::resident_user_bytes(proc.address_space);
        for (const HandleEntry& handle : proc.handles) {
            if (handle.object != nullptr) {
                info.handle_count += 1;
            }
        }
        info.flags = proc.idle ? SAVANXP_PROC_FLAG_IDLE : 0u;
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

    info.input_ready = input::keyboard_available() ? 1u : 0u;
    info.framebuffer_ready = ui::framebuffer_available() ? 1u : 0u;
    info.net_present = net::present() ? 1u : 0u;
    info.speaker_ready = pcspeaker::ready() ? 1u : 0u;
    info.block_ready = block::ready() ? 1u : 0u;
    // The sxfs_* fields describe the system disk, whatever its format.
    fs::VolumeStats root_stats = {};
    info.sxfs_mounted = fs::stats(fs::find(fs::kRootMountPoint), root_stats) ? 1u : 0u;
    info.timer_backend = exported_timer_backend(timer::backend());
    info.timer_frequency_hz = timer::frequency_hz();
    info.pci_device_count = static_cast<uint32_t>(pci::device_count());
    info.sxfs_file_count = static_cast<uint32_t>(root_stats.file_count);
    object::section_live_counts(info.sections_live, info.file_sections_live);
    info.last_view_failure = vm::last_view_failure();
    info.memory_total_pages = memory::total_page_count();
    info.sxfs_total_bytes = root_stats.total_bytes;
    info.sxfs_used_bytes = root_stats.used_bytes;
    info.sxfs_free_bytes = root_stats.free_bytes;
    info.uptime_ms = current_uptime_ms();

    info.memory_free_bytes = memory::free_page_count() * memory::kPageSize;
    info.cpu_ticks_total = g_cpu_ticks_total;
    info.process_count = 0;
    for (const Process& proc : g_processes) {
        if (proc.life != Lifecycle::unused) {
            info.process_count += 1;
        }
    }
    info.cpu_count = smp::cpu_count();
    info.cpu_online = smp::scheduling_count();
    info.cpu_khz = timer::tsc_khz();

    // Placa de red enchufada, la reclame un driver o no: clase PCI 0x02. Se
    // busca aca y no en nic:: porque nic:: solo conoce el hardware que alguno
    // de sus drivers reclamo, y justamente lo que falta saber es lo otro.
    info.net_hardware = 0;
    info.net_hardware_vendor = 0;
    info.net_hardware_device = 0;
    for (size_t index = 0; index < pci::device_count(); ++index) {
        pci::DeviceInfo device = {};
        if (!pci::device_info(index, device) || device.class_code != kPciClassNetwork) {
            continue;
        }
        info.net_hardware = 1;
        info.net_hardware_vendor = device.vendor_id;
        info.net_hardware_device = device.device_id;
        break;
    }

    arch::CpuIdentity identity = {};
    arch::query_cpu_identity(identity);
    memcpy(info.cpu_vendor, identity.vendor, sizeof(info.cpu_vendor));
    memcpy(info.cpu_brand, identity.brand, sizeof(info.cpu_brand));
    info.cpu_vendor[sizeof(info.cpu_vendor) - 1] = '\0';
    info.cpu_brand[sizeof(info.cpu_brand) - 1] = '\0';
    info.cpu_features = 0;
    if ((identity.features & arch::kCpuFeatureSse2) != 0) {
        info.cpu_features |= SAVANXP_CPU_FEATURE_SSE2;
    }
    if ((identity.features & arch::kCpuFeaturePae) != 0) {
        info.cpu_features |= SAVANXP_CPU_FEATURE_PAE;
    }
    if ((identity.features & arch::kCpuFeatureNx) != 0) {
        info.cpu_features |= SAVANXP_CPU_FEATURE_NX;
    }
    if ((identity.features & arch::kCpuFeatureLongMode) != 0) {
        info.cpu_features |= SAVANXP_CPU_FEATURE_LONG_MODE;
    }
    if ((identity.features & arch::kCpuFeatureX2Apic) != 0) {
        info.cpu_features |= SAVANXP_CPU_FEATURE_X2APIC;
    }
    if ((identity.features & arch::kCpuFeatureHypervisor) != 0) {
        info.cpu_features |= SAVANXP_CPU_FEATURE_HYPERVISOR;
    }
    return true;
}

bool copy_from_user(void* destination, uint64_t user_address, size_t count) {
    return sx::copy_from_user(destination, user_address, count);
}

bool copy_to_user(uint64_t user_address, const void* source, size_t count) {
    return sx::copy_to_user(user_address, source, count);
}

bool validate_user_range(uint64_t user_address, size_t count, bool require_write) {
    return sx::validate_user_range(user_address, count, require_write);
}

int export_handle(object::Header* handle_object, uint32_t access, uint32_t flags) {
    Process* const proc = current();
    if (proc == nullptr || handle_object == nullptr) {
        return negative_error(SX_EBADF);
    }
    return process::detail::allocate_fd(*proc, handle_object, access, flags);
}

void notify_object_signal(object::Header* handle_object) {
    process::detail::wake_waiters_for_object(handle_object);
}

Process* create_user_process(const char* path, int argc, const char* const* argv, uint32_t parent_pid) {
    process::ImageFailure failure = process::ImageFailure::none;
    return create_process_internal(path, argc, argv, find(parent_pid), -1, -1, -1, failure);
}

[[noreturn]] void start_init(const char* path) {
    // Desde aca el kernel deja de ser solo del BSP. El arranque corrio sin el
    // lock porque no habia con quien competir; lo que sigue ya no. IF=0 antes
    // de tomarlo: un tick con el lock tomado por este mismo core no entra.
    arch::disable_interrupts();
    smp::lock_kernel();

    // Un idle por core que pueda planificar. El del BSP es imprescindible; el
    // de un AP que no se pudo crear deja a ese AP estacionado, nada mas.
    for (uint32_t index = 0; index < smp::kMaxCpus; ++index) {
        if (!smp::may_schedule(index)) {
            continue;
        }
        Thread* idle = create_idle_process();
        if (idle == nullptr) {
            if (index == 0) {
                panic("scheduler: unable to create idle task");
            }
            console::printf("smp: sin idle para el core %u, queda estacionado\n", static_cast<unsigned>(index));
            continue;
        }
        idle->cpu = index;
        smp::g_cpu_state[index].idle = idle;
    }

    const char* argv[] = {path, nullptr};
    process::ImageFailure init_failure = process::ImageFailure::none;
    Process* init = create_process_internal(path, 1, argv, nullptr, -1, -1, -1, init_failure);
    if (init == nullptr) {
        panic("process: unable to start init");
    }
    g_init_pid = init->pid;

    // Los APs salen a buscar trabajo en cuanto el lock se libere, que es al
    // retomar init.
    smp::start_scheduling();

    Thread* const init_thread = primary_thread(*init);
    g_schedule_cursor = thread_index(init_thread);
    switch_to_thread(init_thread);
    arch::resume_context(init_thread->context, init->address_space.pml4_physical);
}

[[noreturn]] void run_secondary_core() {
    SavedContext* next = choose_next_context(nullptr);
    arch::resume_context(next, this_cpu().current->process->address_space.pml4_physical);
}

void terminate_current(int exit_code) {
    Thread* const self = this_cpu().current;
    Process* const proc = self != nullptr ? self->process : nullptr;
    if (proc == nullptr || proc->idle) {
        panic("process: invalid current task on exit");
    }

    process::detail::terminate_process(*proc, exit_code);
    // A sibling still runs on another core, so the process lives on until it
    // stops. This thread stops here: it is a zombie whose object is signaled
    // with the others when the last one finishes the process, and it never
    // runs again. Its slot and stack stay until the reap.
    if (proc->life == Lifecycle::alive) {
        clear_wait_state(*self);
        self->state = State::zombie;
    }

    SavedContext* next = choose_next_context(nullptr);
    arch::resume_context(next, this_cpu().current->process->address_space.pml4_physical);
}

bool exit_pending(const Process& proc, int& exit_code) {
    if (proc.life != Lifecycle::alive || !proc.exit_requested) {
        return false;
    }
    exit_code = proc.exit_code;
    return true;
}

/*
 * Wait on an address: the primitive user-space locks park on (a futex,
 * WaitOnAddress). The fast path never enters the kernel; a thread comes here
 * only to sleep until another one changes the word and says so.
 *
 * No wake is lost because both calls run under the kernel lock: a waker
 * stores the new value and then takes the lock, so either it finds this
 * thread already parked, or this thread reads the word after the store and
 * does not park. Parked threads are a scan of the thread table keyed by
 * (process, address): one address space per process, so that is the
 * (address space, virtual address) of the roadmap. Threads of two processes
 * sharing a section never meet here.
 */
namespace {

uint64_t g_address_wait_tickets = 0;

bool read_wait_word(const Process& proc, uint64_t address, uint64_t size, uint64_t& value) {
    if (!vm::is_user_range_accessible(proc.address_space, address, static_cast<size_t>(size), false)) {
        return false;
    }
    // The caller's address space is the loaded one, and the kernel lock keeps
    // the page mapped while it is read. One load of the word's own size, the
    // same access the user side makes.
    switch (size) {
        case 1: value = __atomic_load_n(reinterpret_cast<const volatile uint8_t*>(address), __ATOMIC_SEQ_CST); return true;
        case 2: value = __atomic_load_n(reinterpret_cast<const volatile uint16_t*>(address), __ATOMIC_SEQ_CST); return true;
        case 4: value = __atomic_load_n(reinterpret_cast<const volatile uint32_t*>(address), __ATOMIC_SEQ_CST); return true;
        default: value = __atomic_load_n(reinterpret_cast<const volatile uint64_t*>(address), __ATOMIC_SEQ_CST); return true;
    }
}

} // namespace

int wait_on_address(Thread& self, uint64_t address, uint64_t expected, uint64_t size, int64_t timeout_ms) {
    if ((size != 1 && size != 2 && size != 4 && size != 8) || (address & (size - 1)) != 0) {
        return negative_error(SX_EINVAL);
    }
    uint64_t value = 0;
    if (&self != this_cpu().current || !read_wait_word(*self.process, address, size, value)) {
        return negative_error(SX_EINVAL);
    }
    const uint64_t mask = size == 8 ? ~0ULL : ((1ULL << (size * 8)) - 1);
    if (value != (expected & mask)) {
        return negative_error(SX_EAGAIN);
    }
    if (timeout_ms == 0) {
        return negative_error(SX_ETIMEDOUT);
    }

    clear_wait_state(self);
    self.wait_reason = WaitReason::address;
    self.wait_address = address;
    self.wait_ticket = ++g_address_wait_tickets;
    self.wake_deadline_ms = timeout_ms > 0 ? (process::detail::now_ms() + static_cast<uint64_t>(timeout_ms)) : 0;
    self.state = State::blocked_wait;
    return kBlockedResult;
}

int wake_address(Process& proc, uint64_t address, uint32_t count) {
    uint32_t woken = 0;
    // Releasing them all needs no order: one pass.
    if (count == SXN_WAKE_ALL) {
        for (Thread& thread : used_thread_slots()) {
            if (thread.state == State::blocked_wait && thread.wait_reason == WaitReason::address &&
                thread.process == &proc && thread.wait_address == address) {
                process::detail::complete_blocked_wait(thread, 0);
                ++woken;
            }
        }
        return static_cast<int>(woken);
    }
    while (woken < count) {
        Thread* oldest = nullptr;
        for (Thread& thread : used_thread_slots()) {
            if (thread.state != State::blocked_wait || thread.wait_reason != WaitReason::address ||
                thread.process != &proc || thread.wait_address != address) {
                continue;
            }
            if (oldest == nullptr || thread.wait_ticket < oldest->wait_ticket) {
                oldest = &thread;
            }
        }
        if (oldest == nullptr) {
            break;
        }
        process::detail::complete_blocked_wait(*oldest, 0);
        ++woken;
    }
    return static_cast<int>(woken);
}

bool end_other_threads(Thread& self) {
    // Two threads calling exec at once: the one asked to end first loses,
    // and ends on its way out of the call instead of taking the process.
    if (self.end_requested) {
        return false;
    }
    bool waiting = false;
    for (Thread& thread : used_thread_slots()) {
        if (&thread == &self || thread.process != self.process ||
            thread.state == State::unused || thread.state == State::zombie) {
            continue;
        }
        // Running elsewhere, or asleep in a syscall: only it can stop itself.
        if (thread.state == State::running || stopped_in_kernel(thread)) {
            thread.end_requested = true;
            smp::kick(thread.cpu);
            waiting = true;
            continue;
        }
        // Stopped in user mode, or parked by a syscall that blocked the
        // kBlockedResult way: nothing of it is on any core, so it ends here.
        clear_wait_state(thread);
        thread.state = State::zombie;
        object::end_thread_object(thread.object, 0);
        process::detail::wake_waiters_for_object(&thread.object->header);
        process::detail::release_thread(thread);
    }
    return !waiting;
}

bool current_thread_must_stop() {
    const Thread* const thread = this_cpu().current;
    return thread != nullptr && (thread->process->exit_requested || thread->end_requested);
}

uint32_t live_thread_count(const Process& proc) {
    uint32_t live = 0;
    for (const Thread& thread : used_thread_slots()) {
        if (thread.process == &proc && thread.state != State::unused && thread.state != State::zombie) {
            live += 1;
        }
    }
    return live;
}

int create_thread(Process& proc, uint64_t entry, uint64_t argument, uint64_t stack_top, uint32_t& tid) {
    // The entry is reached as if by a call: rsp is 8 below a 16-byte boundary,
    // where the return address would be. A thread that returns from its entry
    // has nowhere to go; it ends with SXN_SYS_THREAD_EXIT.
    const uint64_t stack_pointer = (stack_top & ~static_cast<uint64_t>(0xf)) - sizeof(uint64_t);

    if (proc.idle || proc.life != Lifecycle::alive || entry == 0 || stack_top < 16) {
        return negative_error(SX_EINVAL);
    }
    if (!vm::is_user_range_accessible(proc.address_space, entry, 1, false) ||
        !vm::is_user_range_accessible(proc.address_space, stack_pointer, sizeof(uint64_t), true)) {
        return negative_error(SX_EINVAL);
    }

    Thread* const thread = process::detail::allocate_thread(proc);
    if (thread == nullptr) {
        return negative_error(SX_ENOMEM);
    }
    const int handle = process::detail::allocate_fd(
        proc, &thread->object->header, object::access_synchronize | object::access_query, object::handle_none);
    if (handle < 0) {
        process::detail::release_thread(*thread);
        return handle;
    }

    SavedContext* const context = fabricate_initial_context(*thread, entry, stack_pointer, 0, 0, 0, 0);
    context->rdi = argument;
    context->rdx = 0;
    tid = thread->tid;
    process::detail::proc_log("proc: thread %u of pid %u created\n", thread->tid, proc.pid);
    return handle;
}

void exit_current_thread(int exit_code) {
    Thread* const self = this_cpu().current;
    if (self == nullptr || self->process->idle) {
        panic("process: invalid current thread on thread exit");
    }

    // A process already ending decides the code. Otherwise the last live
    // thread takes the process with it, as returning from main or ExitThread
    // on the last thread does.
    if (self->process->exit_requested) {
        terminate_current(self->process->exit_code);
    }
    if (live_thread_count(*self->process) <= 1) {
        terminate_current(exit_code);
    }

    clear_wait_state(*self);
    self->state = State::zombie;
    object::end_thread_object(self->object, exit_code);
    process::detail::wake_waiters_for_object(&self->object->header);
    process::detail::proc_log("proc: thread %u of pid %u exited %d\n", self->tid, self->process->pid, exit_code);

    // Its kernel stack is the one in use: the next pass through the scheduler
    // on this core frees it, from another stack.
    if (this_cpu().retired != nullptr) {
        process::detail::release_thread(*this_cpu().retired);
    }
    this_cpu().retired = self;
    SavedContext* next = choose_next_context(nullptr);
    arch::resume_context(next, this_cpu().current->process->address_space.pml4_physical);
}

int set_thread_base(Thread& self, uint32_t which, uint64_t address) {
    // wrmsr of a non-canonical base is a #GP in the kernel; a kernel address
    // would be harmless (user accesses through it fault) but means nothing.
    if (address >= (1ULL << 47) || (which != SXN_THREAD_BASE_FS && which != SXN_THREAD_BASE_GS)) {
        return negative_error(SX_EINVAL);
    }
    if (which == SXN_THREAD_BASE_FS) {
        self.fs_base = address;
    } else {
        self.gs_base = address;
    }
    if (this_cpu().current == &self) {
        arch::load_user_segment_bases(self.fs_base, self.gs_base);
    }
    return 0;
}

void thread_bases(const Thread& thread, uint64_t& fs_base, uint64_t& gs_base) {
    if (this_cpu().current == &thread) {
        arch::save_user_segment_bases(fs_base, gs_base);
        return;
    }
    fs_base = thread.fs_base;
    gs_base = thread.gs_base;
}

int thread_exit_code(Process& proc, uint64_t handle, int& exit_code) {
    object::ThreadObject* const thread_object =
        object::as_thread(process::detail::lookup_handle(proc, handle, object::access_query));
    if (thread_object == nullptr) {
        return negative_error(SX_EBADF);
    }
    if (!thread_object->ended) {
        return negative_error(SX_EAGAIN);
    }
    exit_code = thread_object->exit_code;
    return 0;
}

/* Crecimiento por demanda del stack.
 *
 * La region esta reservada entera (vm::kUserStackBottom..kUserStackTop) pero al
 * arrancar el proceso solo se mapea la punta. El primer acceso mas abajo llega
 * aca como #PF de no-presente y se resuelve mapeando esa pagina.
 *
 * Lo que NO entra: la pagina de guarda, que queda debajo de kUserStackBottom.
 * Un desborde de verdad cae ahi, no lo atiende nadie y el proceso muere con un
 * fault legible en vez de pisar en silencio lo que hubiera abajo.
 */
bool grow_user_stack(uint64_t fault_address) {
    process::Process* proc = current();
    uint64_t page_address = 0;

    if (proc == nullptr || proc->life == process::Lifecycle::unused) {
        return false;
    }
    if (fault_address < vm::kUserStackBottom || fault_address >= vm::kUserStackTop) {
        return false;
    }

    page_address = fault_address & ~(memory::kPageSize - 1);
    /* map_page ya invalida la TLB cuando el espacio destino es el activo, que
     * es siempre el caso aca. */
    return vm::ensure_user_stack_page(proc->address_space, page_address);
}

void terminate_current_from_exception(uint8_t vector) {
    console::printf(
        "user: exception #%u killed pid=%u\n",
        static_cast<unsigned>(vector),
        current_pid()
    );
    terminate_current(128 + vector);
}

SavedContext* handle_syscall(SavedContext* context) {
    Thread* const caller = this_cpu().current;
    const subsystem::Id id = caller != nullptr ? caller->process->subsystem_id : subsystem::Id::posix;
    SavedContext* result = subsystem::dispatch(id, context);

    // Its process was asked to end while it was in this syscall. If it is
    // still the thread running here, it stops now, before returning to ring
    // 3. If the syscall blocked it or gave the CPU to someone else, it no
    // longer runs anywhere, which may be what the process was waiting for.
    if (caller != nullptr && caller->state != State::unused && caller->process->exit_requested) {
        if (caller == this_cpu().current && caller->state == State::running) {
            terminate_current(caller->process->exit_code);
        }
        process::detail::terminate_process(*caller->process, caller->process->exit_code);
        // The syscall blocked the caller and handed this core to a sibling of
        // the same process, which just ended with it (or was released with it,
        // if the parent was already waiting). Resuming it would run a dead
        // thread in user mode, and its process may be reaped from another core
        // while it is still the one on this core.
        if (this_cpu().current != nullptr && this_cpu().current->state != State::running) {
            return choose_next_context(nullptr);
        }
    }

    // An exec of a sibling asked this thread to end while it was in this
    // syscall. Running here: it exits now. Blocked or parked by the call: it
    // runs nowhere, so it ends in place and the next thread here goes on.
    if (caller != nullptr && caller->state != State::unused && caller->state != State::zombie &&
        caller->end_requested) {
        if (caller == this_cpu().current && caller->state == State::running) {
            exit_current_thread(0);
        }
        clear_wait_state(*caller);
        caller->state = State::zombie;
        object::end_thread_object(caller->object, 0);
        process::detail::wake_waiters_for_object(&caller->object->header);
        process::detail::release_thread(*caller);
    }

    // Preemptive event wakeup: if this syscall made another process runnable
    // and we are still the running caller (i.e. the syscall did not itself
    // block), hand the CPU over now so the woken waiter runs without waiting
    // for the next timer tick. Mirrors the SYS_YIELD path.
    if (this_cpu().resched_pending) {
        this_cpu().resched_pending = false;
        if (this_cpu().current != nullptr && this_cpu().current->state == State::running) {
            this_cpu().current->context = result;
            this_cpu().current->state = State::ready;
            return choose_next_context(result);
        }
    }
    return result;
}

SavedContext* handle_timer_tick(SavedContext* context) {
    if (this_cpu().current == nullptr) {
        return context;
    }

    this_cpu().current->context = context;
    // This tick was consumed by whoever was running when it arrived, before
    // anything below can replace it.
    this_cpu().current->process->cpu_ticks += 1;
    g_cpu_ticks_total += 1;

    // Los vencimientos se miden con el reloj monotono, no con el contador de
    // ticks: ver process::detail::now_ms(). El tick sigue siendo el que los revisa -- es el
    // latido que ya existe -- pero no es la regla con la que se miden. Y los
    // revisa uno solo, el del BSP: son del sistema, no de cada core.
    if (this_cpu().index == 0) {
        const uint64_t current_tick = process::detail::now_ms();
        object::poll_timers(current_tick, process::detail::wake_waiters_for_object);
        wake_sleepers(current_tick);
        wake_wait_timeouts(current_tick);
        wake_parked_waiters(current_tick);
        wake_blocked_socket_readers(current_tick);
        // The writeback queue's clock, and its drain of last resort when no
        // core has gone idle for a while.
        block::writeback_tick(current_tick);
        // Sections of views unmapped while another core shared the address
        // space, once every such core has flushed.
        vm::reclaim_deferred_views();
    }
    // The tick makes its own reschedule decision below, so any wakeups above
    // must not leak a preemptive-resched request into the next syscall return.
    this_cpu().resched_pending = false;

    if (this_cpu().current->process->exit_requested && (context->cs & 0x3) == 0x3) {
        terminate_current(this_cpu().current->process->exit_code);
    }
    if (this_cpu().current->end_requested && (context->cs & 0x3) == 0x3) {
        exit_current_thread(0);
    }

    if (this_cpu().current->state != State::running) {
        return choose_next_context(context);
    }

    if (this_cpu().current->process->idle) {
        if (has_runnable_non_idle()) {
            this_cpu().current->state = State::ready;
            return choose_next_context(context);
        }
        return context;
    }

    if (this_cpu().current->time_slice > 1) {
        this_cpu().current->time_slice -= 1;
        return context;
    }

    this_cpu().current->state = State::ready;
    return choose_next_context(context);
}

SavedContext* handle_reschedule_ipi(SavedContext* context) {
    this_cpu().kicked = false;
    if (this_cpu().current == nullptr) {
        return context;
    }

    this_cpu().current->context = context;
    // Only from ring 3: in ring 0 it is asleep inside a syscall, and the end
    // of that syscall is what stops it.
    if (this_cpu().current->process->exit_requested && (context->cs & 0x3) == 0x3) {
        terminate_current(this_cpu().current->process->exit_code);
    }
    if (this_cpu().current->end_requested && (context->cs & 0x3) == 0x3) {
        exit_current_thread(0);
    }
    if (this_cpu().current->process->idle && has_runnable_non_idle()) {
        this_cpu().current->state = State::ready;
        return choose_next_context(context);
    }
    return context;
}

void notify_tty_line_ready() {
    for (Thread& thread : used_thread_slots()) {
        if (thread.state != State::blocked_read) {
            continue;
        }
        if (thread.blocked_io_fd >= kMaxFileHandles) {
            continue;
        }

        Process& proc = *thread.process;
        object::IoObject* file = object::as_io(proc.handles[thread.blocked_io_fd].object);
        if (file == nullptr || file->kind != HandleKind::tty) {
            continue;
        }

        const size_t to_copy = tty::main().pending_length < thread.blocked_read_capacity
            ? tty::main().pending_length
            : static_cast<size_t>(thread.blocked_read_capacity);
        if (!write_to_process_memory(proc, thread.blocked_read_buffer, tty::main().pending_line, to_copy)) {
            complete_blocked_read(thread, negative_error(SX_EINVAL));
        } else {
            memset(tty::main().pending_line, 0, sizeof(tty::main().pending_line));
            tty::main().pending_length = 0;
            tty::main().line_ready = false;
            complete_blocked_read(thread, static_cast<int>(to_copy));
        }
        break;
    }
}

} // namespace process
