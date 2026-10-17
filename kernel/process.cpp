#include "kernel/process.hpp"
#include "kernel/process_detail.hpp"

#include <stdarg.h>
#include <stdint.h>

#include "kernel/audio_device.hpp"
#include "kernel/block.hpp"
#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/device.hpp"
#include "kernel/display.hpp"
#include "kernel/elf.hpp"
#include "kernel/heap.hpp"
#include "kernel/input.hpp"
#include "kernel/net.hpp"
#include "kernel/panic.hpp"
#include "kernel/pci.hpp"
#include "kernel/pcspeaker.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/ps2.hpp"
#include "kernel/rtc.hpp"
#include "kernel/smp.hpp"
#include "kernel/string.hpp"
#include "kernel/sx.hpp"
#include "kernel/sxfs.hpp"
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
process::Pipe g_pipes[process::detail::kMaxPipeCount] = {};
uint8_t g_pipe_storage[process::detail::kMaxPipeCount][process::detail::kPipeCapacity] = {};
object::IoObject g_io_objects[object::kMaxIoObjects] = {};
uint32_t g_next_pid = 1;
uint32_t g_init_pid = 0;
// Compartido entre cores a proposito: es el cursor de la cola de listos unica,
// que vive bajo el lock grande del kernel.
size_t g_schedule_cursor = 0;
// Ticks que encontraron a un proceso corriendo, en cualquier core. Es el
// denominador de la contabilidad de CPU (cpu_ticks_total): cada uno incrementa
// exactamente un cpu_ticks, asi que los dos lados suman igual con N cores. No es
// timer::ticks(), que cuenta solo los del BSP porque es un reloj.
uint64_t g_cpu_ticks_total = 0;
bool g_ready = false;
savanxp_system_info g_boot_system_info = {};
bool g_boot_system_info_ready = false;

// El proceso en curso, el idle y el pedido de replanificar son de cada core y
// viven en smp::Cpu (kernel/smp.hpp). Este atajo deja que el scheduler y los
// syscalls digan "el de este core" sin repetir el namespace en cada linea.
smp::Cpu& this_cpu() {
    return smp::this_cpu();
}

void write_cr3(uint64_t value) {
    asm volatile("mov %0, %%cr3" : : "r"(value) : "memory");
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
 * donde un tick ES un proceso (docs/SYSTEM_MONITORING.md).
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



size_t process_index(const process::Process* proc) {
    return proc != nullptr ? static_cast<size_t>(proc - &g_processes[0]) : 0;
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
void recheck_parked_processes(uint64_t current_tick, bool honour_deadline);
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
    uint64_t value = arch::x86_64::random_u64();
    // The stack-protector ABI reserves the least-significant byte for string
    // termination, so a canary must never contain a zero there.
    if ((value & 0xffu) == 0) {
        value ^= 0xa5u;
    }
    return value;
}

process::SavedContext* fabricate_initial_context(
    process::Process& proc,
    uint64_t entry_point,
    uint64_t stack_pointer,
    int argc,
    uint64_t argv_pointer,
    uint64_t interpreter_address,
    uint64_t image_base
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
    proc.context = context;
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

process::Process* create_idle_process() {
    process::Process* proc = process::detail::allocate_process_slot(true);
    if (proc == nullptr) {
        return nullptr;
    }

    if (!vm::create_address_space(proc->address_space)) {
        process::detail::reset_process_slot(*proc);
        return nullptr;
    }

    memory::PageAllocation kernel_stack = {};
    if (!memory::allocate_contiguous_pages(process::detail::kKernelStackPages, kernel_stack)) {
        process::detail::release_address_space(*proc);
        process::detail::reset_process_slot(*proc);
        return nullptr;
    }

    proc->kernel_stack_base = reinterpret_cast<uint64_t>(kernel_stack.virtual_address);
    proc->kernel_stack_size = process::detail::kKernelStackPages * memory::kPageSize;
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

    fabricate_initial_context(*proc, kIdleCodeAddress, vm::kUserStackTop, 0, 0, 0, 0);
    return proc;
}

process::Process* find_waiting_parent_for(uint32_t child_pid) {
    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::blocked_wait || proc.wait_reason != process::WaitReason::child) {
            continue;
        }
        if (proc.waiting_for_pid == process::kWaitAnyPid || proc.waiting_for_pid == child_pid) {
            return &proc;
        }
    }
    return nullptr;
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
        proc.parent_pid = g_init_pid;
        proc.orphaned = true;
    }
}

// Listo para correr en ESTE core. La cola es una sola y cualquier core toma de
// ella, con una excepcion: un proceso desalojado mientras dormia adentro del
// kernel (smp::wait_for_interrupt) tiene el contexto en ring 0, en la mitad de
// una syscall, y solo lo retoma el core donde se durmio.
//
// Los que corren en otro core no hace falta filtrarlos: estan en
// State::running, no en ready. Y los idles no entran nunca, cada core vuelve al
// suyo por separado.
bool runnable_here(const process::Process& proc) {
    if (proc.idle || proc.state != process::State::ready) {
        return false;
    }
    if (proc.context != nullptr && (proc.context->cs & 0x3) == 0) {
        return proc.cpu == this_cpu().index;
    }
    return true;
}

process::Process* pick_next_runnable() {
    for (size_t offset = 1; offset <= process::kMaxProcesses; ++offset) {
        const size_t index = (g_schedule_cursor + offset) % process::kMaxProcesses;
        process::Process& proc = g_processes[index];
        if (runnable_here(proc)) {
            g_schedule_cursor = index;
            return &proc;
        }
    }

    if (this_cpu().idle != nullptr &&
        (this_cpu().idle->state == process::State::ready || this_cpu().idle->state == process::State::running)) {
        g_schedule_cursor = process_index(this_cpu().idle);
        return this_cpu().idle;
    }

    return nullptr;
}

void note_woken(process::Process& proc) {
    if (&proc == this_cpu().current) {
        return;
    }
    this_cpu().resched_pending = true;
}

// Los tres complete_blocked_* piden el mismo wakeup preemptivo. Al principio
// solo lo hacia complete_blocked_wait, y eso dejaba a los PIPES sin el: un RPC
// sincrono -- escribir la request y bloquearse leyendo la reply, que es como
// habla windowd con compositord -- despertaba al que esperaba pero lo dejaba
// esperando su turno del round-robin. Medido en el camino de display eso era el
// 99% del costo de poner un frame en pantalla (ver docs/GRAPHICS_PERF.md), con
// el trabajo de GPU real en dos ordenes de magnitud menos.
int complete_blocked_read(process::Process& proc, int result) {
    proc.blocked_io_fd = 0;
    proc.blocked_read_buffer = 0;
    proc.blocked_read_capacity = 0;
    proc.context->rax = static_cast<uint64_t>(result);
    proc.state = process::State::ready;
    process::detail::reset_time_slice(proc);
    note_woken(proc);
    return result;
}

int complete_blocked_write(process::Process& proc, int result) {
    proc.blocked_io_fd = 0;
    proc.blocked_write_buffer = 0;
    proc.blocked_write_length = 0;
    proc.blocked_write_progress = 0;
    proc.context->rax = static_cast<uint64_t>(result);
    proc.state = process::State::ready;
    process::detail::reset_time_slice(proc);
    note_woken(proc);
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
    process::Process* parent = find_waiting_parent_for(child.pid);
    if (parent == nullptr) {
        if (child.orphaned) {
            reap_process(child);
        }
        return;
    }

    write_int_to_process_memory(*parent, parent->wait_status_address, child.exit_code);
    clear_wait_state(*parent);
    parent->context->rax = child.pid;
    parent->state = process::State::ready;
    process::detail::reset_time_slice(*parent);
    reap_process(child);
}

void wake_sleepers(uint64_t current_tick) {
    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::sleeping || proc.wake_deadline_ms > current_tick) {
            continue;
        }

        proc.wake_deadline_ms = 0;
        proc.context->rax = 0;
        proc.state = process::State::ready;
        process::detail::reset_time_slice(proc);
    }
}

void wake_wait_timeouts(uint64_t current_tick) {
    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::blocked_wait ||
            proc.wait_reason != process::WaitReason::object ||
            proc.wake_deadline_ms == 0 ||
            proc.wake_deadline_ms > current_tick) {
            continue;
        }

        process::detail::complete_blocked_wait(proc, negative_error(SX_ETIMEDOUT));
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

        object::IoObject* file = object::as_io(proc.handles[proc.blocked_io_fd].object);
        if (file == nullptr || file->kind != process::HandleKind::pipe || file->pipe != &pipe) {
            continue;
        }

        const int result = process::detail::try_pipe_read(proc, *file, proc.blocked_read_buffer, static_cast<size_t>(proc.blocked_read_capacity));
        if (result == process::kBlockedResult) {
            continue;
        }
        complete_blocked_read(proc, result);
    }

    // Y los que esperaban ese pipe por poll en vez de por read.
    recheck_parked_processes(0, false);
}

void wake_blocked_writers_for_pipe(process::Pipe& pipe) {
    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::blocked_write) {
            continue;
        }
        if (proc.blocked_io_fd >= process::kMaxFileHandles) {
            continue;
        }

        object::IoObject* file = object::as_io(proc.handles[proc.blocked_io_fd].object);
        if (file == nullptr || file->kind != process::HandleKind::pipe || file->pipe != &pipe) {
            continue;
        }

        const int step = process::detail::pipe_write_chunk(
            proc,
            pipe,
            proc.blocked_write_buffer,
            static_cast<size_t>(proc.blocked_write_length),
            static_cast<size_t>(proc.blocked_write_progress));

        if (step == process::kBlockedResult) {
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

    // Hacer lugar en el pipe es lo que un poll por POLLOUT esta esperando.
    recheck_parked_processes(0, false);
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
 * de trabajo real. Ver docs/SYSTEM_MONITORING.md.
 *
 * Ahora se prueba una vez y, si no hay nada listo, el proceso se estaciona con
 * su subsistema y lo despierta wake_parked_waiters() desde el tick. La
 * granularidad de la espera no cambia: el bucle viejo tampoco miraba mas
 * seguido que eso, porque entre chequeo y chequeo estaba halteado.
 */
void recheck_parked_processes(uint64_t current_tick, bool honour_deadline) {
    for (process::Process& proc : g_processes) {
        // El kernel solo sabe que el proceso esta estacionado con vencimiento:
        // QUE esta esperando es de la capa que lo estaciono (rebanada C). Antes
        // decia wait_reason == poll y reevaluaba los descriptores aqui mismo.
        if (proc.state != process::State::blocked_wait) {
            continue;
        }
        subsystem::recheck_parked_process(proc, current_tick, honour_deadline);
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

    for (const process::Process& proc : g_processes) {
        if (proc.state == process::State::blocked_wait) {
            any_waiting = true;
            break;
        }
    }
    if (!any_waiting) {
        return;
    }

    net::poll();
    recheck_parked_processes(current_tick, true);
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

    for (const process::Process& proc : g_processes) {
        if (proc.state != process::State::blocked_read || proc.blocked_io_fd >= process::kMaxFileHandles) {
            continue;
        }
        const object::IoObject* file = object::as_io(proc.handles[proc.blocked_io_fd].object);
        if (file != nullptr && file->kind == process::HandleKind::socket) {
            any_waiting = true;
            break;
        }
    }
    if (!any_waiting) {
        return;
    }

    net::poll();

    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::blocked_read || proc.blocked_io_fd >= process::kMaxFileHandles) {
            continue;
        }
        object::IoObject* file = object::as_io(proc.handles[proc.blocked_io_fd].object);
        if (file == nullptr || file->kind != process::HandleKind::socket || file->socket == nullptr) {
            continue;
        }

        uint8_t scratch[kSocketReadChunk] = {};
        const size_t want = proc.blocked_read_capacity < sizeof(scratch)
            ? static_cast<size_t>(proc.blocked_read_capacity)
            : sizeof(scratch);
        int result = net::read_socket_bytes(file->socket, scratch, want, true);
        if (result > 0 &&
            !write_to_process_memory(proc, proc.blocked_read_buffer, scratch, static_cast<size_t>(result))) {
            result = negative_error(SX_EINVAL);
        }
        if (result == negative_error(SX_EAGAIN)) {
            if (proc.wake_deadline_ms != 0 && proc.wake_deadline_ms <= current_tick) {
                proc.wake_deadline_ms = 0;
                complete_blocked_read(proc, negative_error(SX_ETIMEDOUT));
            }
            continue;
        }
        proc.wake_deadline_ms = 0;
        complete_blocked_read(proc, result);
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
void reset_time_slice(Process& proc);
uint32_t init_pid();
Process* allocate_process_slot(bool idle);
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
int begin_object_wait(Process& proc, object::Header* const* handles, size_t count, bool wait_all, int64_t timeout_ms);
// Los usa tambien la traduccion POSIX: el fin de un poll y la evaluacion de
// un descriptor. El kernel no los llama para si (rebanada C).
int complete_blocked_wait(Process& proc, int result);
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


void reset_time_slice(process::Process& proc) {
    proc.time_slice = proc.idle ? 1 : kDefaultTimeSlice;
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
        if (slot.state == process::State::unused) {
            memset(&slot, 0, sizeof(slot));
            slot.pid = g_next_pid++;
            slot.state = process::State::ready;
            slot.idle = idle;
            process::detail::reset_time_slice(slot);
            arch::x86_64::fpu_init_area(slot.fpu_state);
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

void release_address_space(process::Process& proc) {
    vm::destroy_address_space(proc.address_space);
    proc.context = nullptr;
}

void release_process_resources(process::Process& proc) {
    if (proc.sleep_timer != nullptr) {
        object::Header* header = &proc.sleep_timer->header;
        object::release(header);
        proc.sleep_timer = nullptr;
    }
    free_subsystem_state(proc);
    process::detail::release_address_space(proc);
    release_kernel_stack(proc);
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

    for (process::Process& proc : g_processes) {
        if (proc.state != process::State::blocked_wait ||
            proc.wait_reason != process::WaitReason::object) {
            continue;
        }
        bool references_object = false;
        for (size_t index = 0; index < proc.wait_handle_count; ++index) {
            if (proc.waited_objects[index] == handle_object) {
                references_object = true;
                break;
            }
        }
        if (!references_object || !sx::can_complete_object_wait(proc)) {
            continue;
        }

        const int result = sx::consume_object_wait(proc);
        if (result < 0) {
            continue;
        }
        process::detail::complete_blocked_wait(proc, result);
    }

    // Un objeto senalizado tambien puede ser lo que espera un poll. El evento de
    // submit de un cliente grafico entra por aca, que es el camino por el que el
    // compositor se entera de que hay un frame.
    recheck_parked_processes(0, false);
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
    if (proc.idle || proc.state == process::State::unused || proc.state == process::State::zombie) {
        return;
    }

    // Corriendo en otro core: su pila de kernel y sus tablas de paginas estan
    // en uso ahi mismo, y liberarlas ahora es pisarselas. Lo termina su propio
    // core en la proxima vuelta a ring 3 (tick, IPI o fin de syscall).
    if (proc.state == process::State::running && &proc != this_cpu().current) {
        if (!proc.kill_pending) {
            proc.kill_pending = true;
            proc.kill_exit_code = exit_code;
        }
        smp::kick(proc.cpu);
        return;
    }
    proc.kill_pending = false;

    const uint32_t exiting_pid = proc.pid;
    process::detail::proc_log("proc: terminate pid=%u status=%d\n", proc.pid, exit_code);
    proc.exit_code = exit_code;
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
    proc.state = process::State::zombie;
    wake_waiting_parent(proc);
    reparent_orphaned_children(exiting_pid);
}

int begin_object_wait(process::Process& proc, object::Header* const* handles, size_t count, bool wait_all, int64_t timeout_ms) {
    const int consumed = sx::try_consume_objects(proc, handles, count, wait_all);
    if (consumed != sx::negative_error(SX_EAGAIN)) {
        return consumed;
    }
    if (timeout_ms == 0) {
        clear_wait_state(proc);
        return negative_error(SX_ETIMEDOUT);
    }

    proc.wake_deadline_ms = timeout_ms > 0 ? (process::detail::now_ms() + static_cast<uint64_t>(timeout_ms)) : 0;
    proc.state = process::State::blocked_wait;
    return process::kBlockedResult;
}

int complete_blocked_wait(process::Process& proc, int result) {
    clear_wait_state(proc);
    proc.context->rax = static_cast<uint64_t>(result);
    proc.state = process::State::ready;
    process::detail::reset_time_slice(proc);
    note_woken(proc);
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

void clear_wait_state(process::Process& proc) {
    proc.wait_reason = process::WaitReason::none;
    proc.wait_handle_count = 0;
    proc.wait_all = 0;
    proc.wait_reserved = 0;
    proc.waiting_for_pid = 0;
    proc.wait_status_address = 0;
    for (size_t index = 0; index < process::kMaxWaitHandles; ++index) {
        proc.waited_objects[index] = nullptr;
    }
    proc.wake_deadline_ms = 0;
}

void switch_to_process(process::Process* target) {
    if (target == nullptr) {
        return;
    }

    // Cambio real de proceso: guardar el estado FPU/SSE del saliente y restaurar
    // el del entrante. Si target ya es this_cpu().current, el proceso sigue
    // corriendo y su FPU ya esta viva en registros -- un FXRSTOR aqui la pisaria
    // con una copia vieja, asi que se omite. El kernel es -mno-sse, no toca la
    // FPU entremedio.
    if (this_cpu().current != target) {
        if (this_cpu().current != nullptr) {
            arch::x86_64::fpu_save(this_cpu().current->fpu_state);
        }
        arch::x86_64::fpu_restore(target->fpu_state);
    }

    this_cpu().current = target;
    target->state = process::State::running;
    target->cpu = this_cpu().index;
    arch::x86_64::set_kernel_stack(target->kernel_stack_base + target->kernel_stack_size);
    write_cr3(target->address_space.pml4_physical);
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
    }
    return SX_EIO;
}

bool prepare_exec_image(
    process::Process& proc,
    const char* path,
    int argc,
    const char* const* argv,
    process::ImageFailure& failure
) {
    failure = process::ImageFailure::none;

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
    vm::randomize_section_base(new_space, arch::x86_64::random_u64() ^ proc.pid);

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
    proc.blocked_io_fd = 0;
    proc.blocked_read_buffer = 0;
    proc.blocked_read_capacity = 0;
    proc.blocked_write_buffer = 0;
    proc.blocked_write_length = 0;
    proc.blocked_write_progress = 0;
    clear_wait_state(proc);
    process::detail::reset_time_slice(proc);
    fabricate_initial_context(proc, load_result.entry_point, load_result.stack_pointer, load_result.accepted_argc, load_result.stack_pointer, load_result.interpreter_address, load_result.image_base);
    return true;
}

process::Process* create_process_internal(
    const char* path,
    int argc,
    const char* const* argv,
    process::Process* parent,
    int stdin_fd,
    int stdout_fd,
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
        arch::x86_64::random_u64() ^ static_cast<uint64_t>(proc->pid));

    memory::PageAllocation kernel_stack = {};
    if (!memory::allocate_contiguous_pages(process::detail::kKernelStackPages, kernel_stack)) {
        process::detail::release_address_space(*proc);
        process::detail::reset_process_slot(*proc);
        failure = process::ImageFailure::no_address_space;
        report_image_failure("spawn", path, failure, reported_size);
        return nullptr;
    }

    proc->kernel_stack_base = reinterpret_cast<uint64_t>(kernel_stack.virtual_address);
    proc->kernel_stack_size = process::detail::kKernelStackPages * memory::kPageSize;

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
            !inherit_handle(*proc, 2, *parent, 2)) {
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
    if (proc->subsystem_id == subsystem::Id::native) {
        console::printf("process: pid=%u marcado nativo\n", static_cast<unsigned>(proc->pid));
    }

    fabricate_initial_context(*proc, load_result.entry_point, load_result.stack_pointer, load_result.accepted_argc, load_result.stack_pointer, load_result.interpreter_address, load_result.image_base);
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
int waitpid_child(process::Process& proc, int64_t waited, uint64_t status_address) {
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

    clear_wait_state(proc);
    proc.wait_reason = process::WaitReason::child;
    proc.waiting_for_pid = waited_pid;
    proc.wait_status_address = status_address;
    proc.state = process::State::blocked_wait;
    return process::kBlockedResult;
}

// Native spawn clones the caller's whole handle table into the child (the
// POSIX spawn only takes stdio): without a native fork this is the only
// channel that carries handles across, and per-handle opt-in stays future
// work next to FD_CLOEXEC (see docs/KERNEL_SECURITY.md). On failure the
// fully created child is reaped here, so the caller only sees ENOMEM.
bool clone_handles_for_spawn(process::Process& proc, const process::Process& parent) {
    if (!process::detail::inherit_all_handles(proc, parent)) {
        reap_process(proc);
        return false;
    }
    return true;
}

bool has_runnable_non_idle() {
    for (const process::Process& proc : g_processes) {
        if (runnable_here(proc)) {
            return true;
        }
    }
    return false;
}

process::SavedContext* choose_next_context(process::SavedContext* current_context) {
    if (this_cpu().current != nullptr && current_context != nullptr) {
        this_cpu().current->context = current_context;
    }

    // Pasar por aca atiende cualquier IPI de replanificar que ande en vuelo:
    // el que llegue despues encuentra la decision ya tomada y no hace nada.
    this_cpu().kicked = false;

    process::Process* next = pick_next_runnable();
    if (next == nullptr) {
        panic("scheduler: no runnable process");
    }

    process::detail::reset_time_slice(*next);
    switch_to_process(next);
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

int wait_on_handle(process::Process& proc, uint64_t fd, int64_t timeout_ms) {
    object::Header* handle_object = process::detail::lookup_handle(proc, fd, object::access_synchronize);
    if (handle_object == nullptr) {
        return negative_error(SX_EBADF);
    }
    if (!sx::is_waitable(handle_object)) {
        return negative_error(SX_EINVAL);
    }

    object::Header* handles[1] = {handle_object};
    return process::detail::begin_object_wait(proc, handles, 1, false, timeout_ms);
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
    return this_cpu().current;
}

uint32_t current_pid() {
    return this_cpu().current != nullptr ? this_cpu().current->pid : 0;
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
        info.cpu_ticks = proc.cpu_ticks;
        // Un zombie ya solto su espacio de direcciones: preguntarle la memoria
        // devolveria 0 igual, pero recorrer tablas que ya no son suyas no es
        // algo que convenga ni intentar.
        info.memory_bytes = proc.state == State::zombie
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

    info.input_ready = ps2::ready() ? 1u : 0u;
    info.framebuffer_ready = ui::framebuffer_available() ? 1u : 0u;
    info.net_present = net::present() ? 1u : 0u;
    info.speaker_ready = pcspeaker::ready() ? 1u : 0u;
    info.block_ready = block::ready() ? 1u : 0u;
    info.sxfs_mounted = sxfs::mounted(sxfs::root()) ? 1u : 0u;
    info.timer_backend = exported_timer_backend(timer::backend());
    info.timer_frequency_hz = timer::frequency_hz();
    info.pci_device_count = static_cast<uint32_t>(pci::device_count());
    info.sxfs_file_count = static_cast<uint32_t>(sxfs::file_count(sxfs::root()));
    object::section_live_counts(info.sections_live, info.file_sections_live);
    info.last_view_failure = vm::last_view_failure();
    info.memory_total_pages = memory::total_page_count();
    info.sxfs_total_bytes = sxfs::total_bytes(sxfs::root());
    info.sxfs_used_bytes = sxfs::used_bytes(sxfs::root());
    info.sxfs_free_bytes = sxfs::free_bytes(sxfs::root());
    info.uptime_ms = current_uptime_ms();

    info.memory_free_bytes = memory::free_page_count() * memory::kPageSize;
    info.cpu_ticks_total = g_cpu_ticks_total;
    info.process_count = 0;
    for (const Process& proc : g_processes) {
        if (proc.state != State::unused) {
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

    arch::x86_64::CpuIdentity identity = {};
    arch::x86_64::query_cpu_identity(identity);
    memcpy(info.cpu_vendor, identity.vendor, sizeof(info.cpu_vendor));
    memcpy(info.cpu_brand, identity.brand, sizeof(info.cpu_brand));
    info.cpu_vendor[sizeof(info.cpu_vendor) - 1] = '\0';
    info.cpu_brand[sizeof(info.cpu_brand) - 1] = '\0';
    info.cpu_features = 0;
    if ((identity.features & arch::x86_64::kCpuFeatureSse2) != 0) {
        info.cpu_features |= SAVANXP_CPU_FEATURE_SSE2;
    }
    if ((identity.features & arch::x86_64::kCpuFeaturePae) != 0) {
        info.cpu_features |= SAVANXP_CPU_FEATURE_PAE;
    }
    if ((identity.features & arch::x86_64::kCpuFeatureNx) != 0) {
        info.cpu_features |= SAVANXP_CPU_FEATURE_NX;
    }
    if ((identity.features & arch::x86_64::kCpuFeatureLongMode) != 0) {
        info.cpu_features |= SAVANXP_CPU_FEATURE_LONG_MODE;
    }
    if ((identity.features & arch::x86_64::kCpuFeatureX2Apic) != 0) {
        info.cpu_features |= SAVANXP_CPU_FEATURE_X2APIC;
    }
    if ((identity.features & arch::x86_64::kCpuFeatureHypervisor) != 0) {
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
    if (this_cpu().current == nullptr || handle_object == nullptr) {
        return negative_error(SX_EBADF);
    }
    return process::detail::allocate_fd(*this_cpu().current, handle_object, access, flags);
}

void notify_object_signal(object::Header* handle_object) {
    process::detail::wake_waiters_for_object(handle_object);
}

Process* create_user_process(const char* path, int argc, const char* const* argv, uint32_t parent_pid) {
    process::ImageFailure failure = process::ImageFailure::none;
    return create_process_internal(path, argc, argv, find(parent_pid), -1, -1, failure);
}

[[noreturn]] void start_init(const char* path) {
    // Desde aca el kernel deja de ser solo del BSP. El arranque corrio sin el
    // lock porque no habia con quien competir; lo que sigue ya no. IF=0 antes
    // de tomarlo: un tick con el lock tomado por este mismo core no entra.
    arch::x86_64::disable_interrupts();
    smp::lock_kernel();

    // Un idle por core que pueda planificar. El del BSP es imprescindible; el
    // de un AP que no se pudo crear deja a ese AP estacionado, nada mas.
    for (uint32_t index = 0; index < smp::kMaxCpus; ++index) {
        if (!smp::may_schedule(index)) {
            continue;
        }
        Process* idle = create_idle_process();
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
    Process* init = create_process_internal(path, 1, argv, nullptr, -1, -1, init_failure);
    if (init == nullptr) {
        panic("process: unable to start init");
    }
    g_init_pid = init->pid;

    // Los APs salen a buscar trabajo en cuanto el lock se libere, que es al
    // retomar init.
    smp::start_scheduling();

    g_schedule_cursor = process_index(init);
    switch_to_process(init);
    arch::x86_64::resume_context(init->context, init->address_space.pml4_physical);
}

[[noreturn]] void run_secondary_core() {
    SavedContext* next = choose_next_context(nullptr);
    arch::x86_64::resume_context(next, this_cpu().current->address_space.pml4_physical);
}

void terminate_current(int exit_code) {
    if (this_cpu().current == nullptr || this_cpu().current->idle) {
        panic("process: invalid current task on exit");
    }

    process::detail::terminate_process(*this_cpu().current, exit_code);

    SavedContext* next = choose_next_context(nullptr);
    arch::x86_64::resume_context(next, this_cpu().current->address_space.pml4_physical);
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
    process::Process* proc = this_cpu().current;
    uint64_t page_address = 0;

    if (proc == nullptr || proc->state == process::State::unused) {
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
        this_cpu().current != nullptr ? this_cpu().current->pid : 0
    );
    terminate_current(128 + vector);
}

SavedContext* handle_syscall(SavedContext* context) {
    Process* const caller = this_cpu().current;
    const subsystem::Id id = caller != nullptr ? caller->subsystem_id : subsystem::Id::posix;
    SavedContext* result = subsystem::dispatch(id, context);

    // Lo mataron desde otro core mientras estaba en esta syscall. Si todavia es
    // el que corre aca, termina ahora, antes de volver a ring 3. Si la syscall
    // lo bloqueo o le cedio la CPU a otro, ya no corre en ningun lado y se lo
    // puede desarmar como a cualquier proceso parado.
    if (caller != nullptr && caller->kill_pending) {
        if (caller == this_cpu().current && caller->state == State::running) {
            terminate_current(caller->kill_exit_code);
        }
        process::detail::terminate_process(*caller, caller->kill_exit_code);
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
    // Este tick lo consumio el que estaba corriendo cuando llego, antes de que
    // nada de abajo pueda reemplazarlo.
    this_cpu().current->cpu_ticks += 1;
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
    }
    // The tick makes its own reschedule decision below, so any wakeups above
    // must not leak a preemptive-resched request into the next syscall return.
    this_cpu().resched_pending = false;

    if (this_cpu().current->kill_pending && (context->cs & 0x3) == 0x3) {
        terminate_current(this_cpu().current->kill_exit_code);
    }

    if (this_cpu().current->state != State::running) {
        return choose_next_context(context);
    }

    if (this_cpu().current->idle) {
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
    // Solo desde ring 3: en ring 0 esta dormido adentro de una syscall, y el
    // final de esa syscall es quien lo termina.
    if (this_cpu().current->kill_pending && (context->cs & 0x3) == 0x3) {
        terminate_current(this_cpu().current->kill_exit_code);
    }
    if (this_cpu().current->idle && has_runnable_non_idle()) {
        this_cpu().current->state = State::ready;
        return choose_next_context(context);
    }
    return context;
}

void notify_tty_line_ready() {
    for (Process& proc : g_processes) {
        if (proc.state != State::blocked_read) {
            continue;
        }
        if (proc.blocked_io_fd >= kMaxFileHandles) {
            continue;
        }

        object::IoObject* file = object::as_io(proc.handles[proc.blocked_io_fd].object);
        if (file == nullptr || file->kind != HandleKind::tty) {
            continue;
        }

        const size_t to_copy = tty::main().pending_length < proc.blocked_read_capacity
            ? tty::main().pending_length
            : static_cast<size_t>(proc.blocked_read_capacity);
        if (!write_to_process_memory(proc, proc.blocked_read_buffer, tty::main().pending_line, to_copy)) {
            complete_blocked_read(proc, negative_error(SX_EINVAL));
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
