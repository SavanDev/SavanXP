#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/object.hpp"
#include "kernel/subsystem.hpp"
#include "kernel/vfs.hpp"
#include "kernel/vmm.hpp"
#include "savanxp/syscall.h"

namespace device {
struct Device;
}

namespace net {
struct Socket;
}

namespace process {

constexpr size_t kMaxProcesses = 64;
constexpr size_t kMaxFileHandles = 64;
constexpr size_t kMaxWaitHandles = 16;
constexpr size_t kProcessNameLength = 32;
constexpr size_t kProcessPathLength = 256;
// Descriptores que admite un poll. Es el tamano de la tabla de descriptores del
// proceso porque no puede haber mas: pedir mas de kMaxFileHandles solo se logra
// repitiendo un fd, que nadie hace. El tope importa porque la copia en kernel
// del pedido vive en el Process (poll_entries): es lo que deja re-evaluar a un
// proceso bloqueado sin tocar su memoria de usuario en cada tick.
constexpr size_t kMaxPollDescriptors = kMaxFileHandles;

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
    device = 4,
    socket = 5,
};

enum OpenFlags : uint32_t {
    open_read = 1u << 0,
    open_write = 1u << 1,
    open_nonblock = 1u << 2,
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

enum SignalFlags : uint32_t {
    signal_none = 0,
    signal_sigint = 1u << 0,
    signal_sigkill = 1u << 1,
    signal_sigpipe = 1u << 2,
    signal_sigterm = 1u << 3,
    signal_sigchld = 1u << 4,
};

enum class WaitReason : uint8_t {
    none = 0,
    child = 1,
    object = 2,
    // Esperando en poll(). Comparte State::blocked_wait con la espera por
    // objetos porque para el resto del kernel son lo mismo -- un proceso
    // parado con vencimiento --; lo que cambia es quien lo despierta y con
    // que resultado.
    poll = 3,
};

struct HandleEntry {
    object::Header* object;
    uint32_t granted_access;
    uint32_t flags;
};

struct Process {
    uint32_t pid;
    uint32_t parent_pid;
    State state;
    bool idle;
    subsystem::Id subsystem_id;  // posix(0) por defecto via memset en allocate_process_slot
    int exit_code;
    WaitReason wait_reason;
    uint8_t wait_handle_count;
    uint8_t wait_all;
    uint16_t wait_reserved;
    uint32_t waiting_for_pid;
    uint64_t wait_status_address;
    object::Header* waited_objects[kMaxWaitHandles];
    uint64_t blocked_io_fd;
    uint64_t blocked_read_buffer;
    uint64_t blocked_read_capacity;
    uint64_t blocked_write_buffer;
    uint64_t blocked_write_length;
    uint64_t blocked_write_progress;
    // Pedido de poll en curso (WaitReason::poll). `poll_entries` es la COPIA EN
    // KERNEL de lo que pidio el proceso, y existe para que la re-evaluacion por
    // tick no tenga que leer su memoria de usuario: eso obligaria a cambiar de
    // CR3 desde el handler del timer, mil veces por segundo y por proceso
    // parado. La memoria de usuario se toca una sola vez, al completar, para
    // devolver los revents en `poll_user_fds`.
    uint64_t poll_user_fds;
    uint32_t poll_count;
    uint64_t wake_tick;
    // Ticks del timer del sistema que encontraron a este proceso corriendo. Es
    // la contabilidad de CPU mas barata posible -- un incremento por tick, en el
    // camino que ya existe -- y alcanza para un porcentaje: el administrador de
    // tareas compara este contador contra timer::ticks() entre dos muestras. El
    // idle suma como cualquier otro, que es justamente lo que hace calculable el
    // uso total del sistema.
    uint64_t cpu_ticks;
    uint32_t time_slice;
    uint32_t pending_signals;
    uint32_t last_signal;
    savanxp_pollfd poll_entries[kMaxPollDescriptors];
    char name[kProcessNameLength];
    char cwd[kProcessPathLength];
    object::TimerObject* sleep_timer;
    vm::VmSpace address_space;
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_size;
    SavedContext* context;
    HandleEntry handles[kMaxFileHandles];
    // Estado FPU/SSE (formato FXSAVE, 512 bytes alineados a 16). El scheduler lo
    // guarda/restaura al cambiar de proceso; se siembra con un estado limpio en
    // allocate_process_slot. alignas(16) fuerza la alineacion del struct entero.
    alignas(16) uint8_t fpu_state[512];
};

void initialize();
bool ready();
Process* current();
uint32_t current_pid();
Process* find(uint32_t pid);
bool copy_from_user(void* destination, uint64_t user_address, size_t count);
bool copy_to_user(uint64_t user_address, const void* source, size_t count);
bool validate_user_range(uint64_t user_address, size_t count, bool require_write);
Process* create_user_process(const char* path, int argc, const char* const* argv, uint32_t parent_pid);
[[noreturn]] void start_init(const char* path);
void terminate_current(int exit_code);
void terminate_current_from_exception(uint8_t vector);
// Atiende un #PF del proceso actual que caiga adentro de la region reservada
// del stack: mapea la pagina que falta y devuelve true para que se reintente la
// instruccion. Fuera de la region -- por ejemplo en la pagina de guarda --
// devuelve false y el fault sigue su camino, que termina matando al proceso.
bool grow_user_stack(uint64_t fault_address);
SavedContext* handle_syscall(SavedContext* context);
SavedContext* handle_timer_tick(SavedContext* context);
void notify_tty_line_ready();
bool snapshot_process(size_t index, savanxp_process_info& info);
void set_boot_system_info(const savanxp_system_info& info);
bool snapshot_system_info(savanxp_system_info& info);
int export_handle(object::Header* handle_object, uint32_t access, uint32_t flags);
void notify_object_signal(object::Header* handle_object);

} // namespace process
