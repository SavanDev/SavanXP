#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/object.hpp"
#include "kernel/subsystem.hpp"
#include "kernel/vfs.hpp"
#include "kernel/vmm.hpp"
#include "abi/savanxp_abi.h"

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


// Handles in transit through the pipe (SAVANXP_SYS_PIPE_SEND_HANDLE). A queue
// apart from the byte stream: the pipe only holds them, and sender and receiver
// agree on the order through their own records. Each entry holds a reference to
// its object, so it does not die in transit. Eight: the WM welcome queues up to
// five (savanxp/wm_protocol.h).
constexpr size_t kPipeHandleQueueCapacity = 8;

struct Pipe {
    bool in_use;
    uint32_t reader_refs;
    uint32_t writer_refs;
    size_t read_pos;
    size_t write_pos;
    size_t size;
    uint8_t* buffer;
    object::Header* handles[kPipeHandleQueueCapacity];
    uint32_t handle_access[kPipeHandleQueueCapacity];
    uint32_t handle_head;
    uint32_t handle_count;
};


enum class WaitReason : uint8_t {
    none = 0,
    child = 1,
    object = 2,
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
    bool orphaned;  // reparentado a init; al morir se auto-reapea
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
    // Vencimiento en milisegundos del reloj monotono (process::now_ms()), 0 =
    // sin vencimiento. NO son ticks: ver el comentario de now_ms().
    uint64_t wake_deadline_ms;
    // Ticks del timer del sistema que encontraron a este proceso corriendo. Es
    // la contabilidad de CPU mas barata posible -- un incremento por tick, en el
    // camino que ya existe -- y alcanza para un porcentaje: el administrador de
    // tareas compara este contador contra timer::ticks() entre dos muestras. El
    // idle suma como cualquier otro, que es justamente lo que hace calculable el
    // uso total del sistema.
    uint64_t cpu_ticks;
    uint32_t time_slice;
    char name[kProcessNameLength];
    // Estado privado del subsistema al que pertenece este proceso (isolation
    // slice B). El kernel reserva y libera estos bytes segun lo que el
    // subsistema declara en subsystem::register_process_state_size, y NO
    // interpreta su contenido: el directorio actual de POSIX vive ahi, no
    // aqui. Sin ningun subsistema con estado, esto es nullptr y no ocupa
    // memoria.
    uint8_t* subsystem_state;
    size_t subsystem_state_bytes;
    object::TimerObject* sleep_timer;
    vm::VmSpace address_space;
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_size;
    SavedContext* context;
    // El core que lo corrio por ultima vez (indice de cpu_index()). Solo decide
    // algo cuando `context` quedo en ring 0 -- desalojado mientras dormia
    // adentro de una syscall --: ese contexto se retoma en este core y en
    // ningun otro.
    uint32_t cpu;
    // Lo mataron mientras corria en otro core. No se lo puede desarmar ahi
    // mismo (su core esta usando su pila y sus tablas de paginas): lo termina
    // su propio core, en la proxima vuelta a ring 3, con este codigo de salida.
    bool kill_pending;
    int kill_exit_code;
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
SavedContext* dispatch_posix_syscall(SavedContext* context);
SavedContext* dispatch_native_syscall(SavedContext* context);
/* True for the native calls other subsystems may forward to the native
 * dispatcher (the object floor, not the lifecycle calls). */
bool native_call_open_to_all_subsystems(uint64_t number);
// Cada subsistema declara su estado privado por proceso. El kernel los reserva
// y los libera sin interpretarlos (isolation slice B).
void initialize_posix_subsystem();
void initialize_native_subsystem();
// Frontera de traduccion POSIX: el dispatcher compila en su propia unidad
// (subsystems/posix/kernel/posix_dispatch.cpp) contra estas declaraciones.
// Los cuerpos POSIX-only viven en subsystems/posix/kernel/
// posix_translation.cpp; kernel/process.cpp conserva el ciclo de vida, el
// scheduler y los nucleos compartidos con el dispatcher nativo (spawn,
// waitpid, eventos, secciones, espera simple). El detalle que la traduccion
// necesita del kernel vive en kernel/process_detail.hpp, no aca.
inline constexpr int kBlockedResult = -0x70000000;
inline constexpr uint32_t kWaitAnyPid = 0xffffffffu;
inline constexpr int kMaxSpawnArguments = 128;
inline constexpr size_t kSpawnArgumentBytes = 8192;
enum class ImageFailure : uint8_t {
    none = 0,
    not_found,
    not_a_file,
    read_failed,
    bad_image,
    no_staging_memory,
    no_address_space,
    no_load_memory,
    no_process_slot,
    no_handles,
};
struct SpawnArguments {
    const char* argv[kMaxSpawnArguments + 1];
    char storage[kSpawnArgumentBytes];
    int argc;
};
uint64_t current_uptime_ms();
void clear_wait_state(Process& proc);
void switch_to_process(Process* target);
// Resolucion NEUTRAL de ruta: absoluta, o relativa a la raiz. No hay cwd en el
// kernel; la resolucion contra el directorio actual es de POSIX y vive en
// subsystems/posix/kernel/posix_translation.cpp.
bool resolve_path_from_root(const char* input, char* output, size_t capacity);
// Estado privado del subsistema (isolation slice B). ensure es idempotente y
// conserva los bytes si el tamano no cambio, para que un exec de POSIX a POSIX
// no tire el cwd.
void ensure_subsystem_state(Process& proc);
void free_subsystem_state(Process& proc);
void copy_subsystem_state(Process& child, const Process& parent);
bool read_from_process_memory(const Process& proc, void* destination, uint64_t user_address, size_t count, bool require_write = false);
bool write_to_process_memory(Process& proc, uint64_t user_address, const void* source, size_t count);
bool copy_user_string(const Process& proc, uint64_t user_address, char* buffer, size_t capacity);
void write_int_to_process_memory(Process& proc, uint64_t user_address, int value);
sx_status image_failure_errno(ImageFailure failure);
bool prepare_exec_image(Process& proc, const char* path, int argc, const char* const* argv, ImageFailure& failure);
Process* create_process_internal(const char* path, int argc, const char* const* argv, Process* parent, int stdin_fd, int stdout_fd, ImageFailure& failure);
// Clones the caller's handle table into a spawned child (native spawn only;
// the POSIX spawn takes stdio). Reaps the child and reports false when the
// clone fails, so the caller just sees the error.
bool clone_handles_for_spawn(Process& child, const Process& parent);
Process* child_of(uint32_t parent_pid, uint32_t pid);
bool has_child(uint32_t parent_pid);
Process* find_zombie_child(uint32_t parent_pid, uint32_t waited_pid);
void reap_process(Process& proc);
bool has_runnable_non_idle();
SavedContext* choose_next_context(SavedContext* current_context);
int fcntl_fd(Process& proc, uint64_t fd, uint64_t command, uint64_t value);
int poll_fds(Process& proc, uint64_t user_fds, size_t count, int timeout_ms);
Process* fork_process(Process& parent, const SavedContext& parent_context);
int kill_process(Process& sender, int pid, int signal_number);
int duplicate_fd(Process& proc, uint64_t oldfd);
int duplicate_fd_to(Process& proc, uint64_t oldfd, uint64_t newfd);
int close_fd(Process& proc, uint64_t fd);
int open_node(Process& proc, const char* path, uint32_t flags);
int create_socket_fd(Process& proc, uint64_t domain, uint64_t type, uint64_t protocol);
int bind_fd(Process& proc, uint64_t fd, uint64_t user_address);
int setsockopt_fd(Process& proc, uint64_t fd, uint64_t option, uint64_t value);
int connect_fd(Process& proc, uint64_t fd, uint64_t user_address, uint32_t timeout_ms);
int sendto_fd(Process& proc, uint64_t fd, uint64_t user_buffer, size_t count, uint64_t user_address);
int recvfrom_fd(Process& proc, uint64_t fd, uint64_t user_buffer, size_t count, uint64_t user_address, uint32_t timeout_ms);
int seek_fd(Process& proc, uint64_t fd, int64_t offset, uint64_t whence);
int unlink_path(const char* path);
int mkdir_path(const char* path);
int rmdir_path(const char* path);
int truncate_path(const char* path, uint64_t size);
int rename_path(const char* old_path, const char* new_path);
int stat_path(Process& proc, const char* path, uint64_t user_stat_address);
int fstat_fd(Process& proc, uint64_t fd, uint64_t user_stat_address);
int chdir_path(Process& proc, const char* path);
int getcwd_path(Process& proc, uint64_t user_buffer, size_t capacity);
// POSIX-only path resolution: relative to the process's current directory,
// which lives in the POSIX subsystem's private per-process state.
bool resolve_process_path(const Process& proc, const char* input, char* output, size_t capacity);
int readdir_node(Process& proc, uint64_t fd, uint64_t user_buffer, size_t capacity);
int ioctl_handle(Process& proc, uint64_t fd, uint64_t request, uint64_t argument);
int read_handle(Process& proc, uint64_t fd, uint64_t user_buffer, size_t count);
int write_handle(Process& proc, uint64_t fd, uint64_t user_buffer, size_t count);
int create_pipe(Process& proc, uint64_t user_fd_array);
int pipe_send_handle(Process& proc, uint64_t pipe_fd, uint64_t handle_fd);
int pipe_receive_handle(Process& proc, uint64_t pipe_fd);
int create_event_handle(Process& proc, uint32_t flags);
int set_event_handle(Process& proc, uint64_t fd);
int reset_event_handle(Process& proc, uint64_t fd);
int create_timer_handle(Process& proc, uint32_t flags);
int set_timer_handle(Process& proc, uint64_t fd, uint64_t due_ms, uint64_t period_ms);
int cancel_timer_handle(Process& proc, uint64_t fd);
int create_semaphore_handle(Process& proc, int32_t initial_count, int32_t max_count);
int release_semaphore_handle(Process& proc, uint64_t fd, uint32_t release_count);
int create_section_handle(Process& proc, uint64_t size, uint32_t flags);
int open_range_section_handle(Process& proc, uint64_t fd, uint64_t offset, uint64_t length, uint32_t flags);
int open_section_handle(Process& proc, uint64_t fd, uint32_t flags);
int64_t map_view_base_handle(Process& proc, uint64_t fd, uint64_t base, uint32_t flags);
int64_t map_view_handle(Process& proc, uint64_t fd, uint32_t flags);
int unmap_view_address(Process& proc, uint64_t base_address);
int wait_on_handle(Process& proc, uint64_t fd, int64_t timeout_ms);
int wait_on_many_handles(Process& proc, uint64_t user_handles, size_t count, bool wait_all, int64_t timeout_ms);
// Shared child-wait core behind both WAITPID paths (see kernel/process.cpp).
int waitpid_child(Process& proc, int64_t waited_pid, uint64_t status_address);
int sleep_via_timer(Process& proc, uint64_t milliseconds);
bool copy_spawn_arguments(const Process& proc, const SavedContext& context, char* path, size_t path_capacity, SpawnArguments& arguments);
Process* create_user_process(const char* path, int argc, const char* const* argv, uint32_t parent_pid);
[[noreturn]] void start_init(const char* path);
// Entrada al scheduler de un AP, con el lock del kernel tomado y su timer ya
// corriendo. Arranca por su idle o por lo que haya listo.
[[noreturn]] void run_secondary_core();
void terminate_current(int exit_code);
void terminate_current_from_exception(uint8_t vector);
// Atiende un #PF del proceso actual que caiga adentro de la region reservada
// del stack: mapea la pagina que falta y devuelve true para que se reintente la
// instruccion. Fuera de la region -- por ejemplo en la pagina de guarda --
// devuelve false y el fault sigue su camino, que termina matando al proceso.
bool grow_user_stack(uint64_t fault_address);
SavedContext* handle_syscall(SavedContext* context);
SavedContext* handle_timer_tick(SavedContext* context);
// El IPI de replanificar (smp::kick): tomar un proceso recien despertado si
// este core estaba ocioso, o terminar al actual si lo mataron desde otro core.
SavedContext* handle_reschedule_ipi(SavedContext* context);
void notify_tty_line_ready();
bool snapshot_process(size_t index, savanxp_process_info& info);
void set_boot_system_info(const savanxp_system_info& info);
bool snapshot_system_info(savanxp_system_info& info);
int export_handle(object::Header* handle_object, uint32_t access, uint32_t flags);
void notify_object_signal(object::Header* handle_object);

} // namespace process
