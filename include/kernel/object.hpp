#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/physical_memory.hpp"

namespace device {
struct Device;
}

namespace net {
struct Socket;
}

namespace vfs {
struct Vnode;
}

namespace process {
enum class HandleKind : uint8_t;
struct Pipe;
}

namespace object {

constexpr size_t kMaxIoObjects = 256;
constexpr size_t kMaxEventObjects = 128;
constexpr size_t kMaxTimerObjects = 128;
/* Secciones del sistema, tabla GLOBAL (no por proceso).
 *
 * El presupuesto que la fija son las arenas de heap, no las imagenes de
 * librerias: cada proceso puede retener SX_ARENA_CAPACITY (8), asi que el techo
 * teorico es kMaxProcesses (64) x 8 = 512. Una sesion real corre 8-12 procesos,
 * y 12 x 8 + unas 10 librerias + superficies anda por 115; 256 deja margen para
 * duplicarlo. Agotarla degrada con ENOMEM limpio, no es una corrupcion.
 * sizeof(SectionObject) son 80 bytes, asi que el reparto son 20 KiB de BSS.
 *
 * Quien escriba el cargador de librerias deberia leer antes
 * docs/kernel/SECTIONS.md: por que esta tabla -- y no una sola para imagenes -- es el
 * lugar donde va a vivir la identidad de un archivo respaldado. */
constexpr size_t kMaxSectionObjects = 256;
constexpr size_t kMaxSemaphoreObjects = 64;
/* Ports: named rendezvous points (docs/desktop/DESKTOP_ARCHITECTURE.md, phase
 * 3). A handful of system services, not one per program. */
constexpr size_t kMaxPortObjects = 16;
/* One per thread slot (process::kMaxThreads): every thread has its object
 * from the moment it exists, and a handle can keep it after the thread ends. */
constexpr size_t kMaxThreadObjects = 256;
constexpr size_t kPortNameCapacity = 32;
constexpr size_t kPortPendingCapacity = 8;

enum class Type : uint8_t {
    none = 0,
    io = 1,
    process = 2,
    thread = 3,
    event = 4,
    semaphore = 5,
    timer = 6,
    section = 7,
    port = 8,
};

enum Access : uint32_t {
    access_none = 0,
    access_read = 1u << 0,
    access_write = 1u << 1,
    access_execute = 1u << 2,
    access_query = 1u << 3,
    access_modify = 1u << 4,
    access_synchronize = 1u << 5,
};

enum HandleFlags : uint32_t {
    handle_none = 0,
    handle_inherit = 1u << 0,
    handle_protect_close = 1u << 1,
};

struct Header {
    Type type;
    uint16_t attributes;
    uint32_t refcount;
    void (*destroy)(Header* self);

    // Estado de espera genérico: cualquier objeto con waitable=true puede
    // pasarse a can_satisfy_wait()/try_acquire_wait() sin que estas funciones
    // conozcan el tipo concreto. signal_count > 0 == satisfecho; manual_reset
    // controla si try_acquire_wait lo consume (auto-reset) o lo deja intacto.
    bool waitable;
    bool manual_reset;
    int32_t signal_count;
};

struct IoObject {
    Header header;
    bool in_use;
    process::HandleKind kind;
    uint32_t open_flags;
    vfs::Vnode* node;
    device::Device* device;
    net::Socket* socket;
    process::Pipe* pipe;
    size_t offset;
    size_t iterator_index;
};

struct EventObject {
    Header header;
    bool in_use;
};

struct TimerObject {
    Header header;
    bool in_use;
    bool armed;
    uint64_t due_ms;
    uint64_t period_ms;
};

struct SectionObject {
    Header header;
    bool in_use;
    // La seccion nace de un archivo (section_open) y no de section_create. Las
    // respaldadas se comparten por inode_id entre procesos; las anonimas nunca.
    uint8_t file_backed;
    uint16_t reserved1;
    uint32_t access_mask;
    // Solo valido con file_backed. Es la clave del cache: dos peticiones del
    // mismo archivo encuentran la misma seccion y por lo tanto las mismas
    // paginas fisicas. Ver docs/kernel/SECTIONS.md para que NO alcanza como clave.
    uint32_t source_inode_id;
    /* Offset y largo de la parte del archivo que respaldo la seccion. Forman
     * parte de la clave del cache junto con inode_id: pedir el archivo entero y
     * pedir un tramo son secciones distintas, y confundirlas devuelve una
     * seccion mas grande de la que se pidio. */
    uint64_t source_offset;
    uint64_t source_length;
    uint64_t size_bytes;
    uint64_t page_count;
    memory::PageAllocation allocation;
    uint64_t* physical_pages;
};

struct SemaphoreObject {
    Header header;
    bool in_use;
    int32_t max_count;
};

/* What a handle to a thread names (docs/kernel/THREADS_ROADMAP.md). The
 * thread holds one reference while it exists and each handle another, so the
 * object outlives the thread: it is how a waiter learns the thread ended and
 * with which code. Waitable and manual-reset: signaled once, at the end, and
 * waiting consumes nothing -- pthread_join and WaitForSingleObject(hThread). */
struct ThreadObject {
    Header header;
    bool in_use;
    bool ended;
    uint32_t tid;
    uint32_t pid;
    int32_t exit_code;
};

/*
 * A port is how a process finds a service without inheriting a channel from it:
 * the service creates the port under a name, a client connects by that name.
 * Connecting creates two pipes; the client keeps one end of each and the other
 * two wait in the port, with the client's pid, until the service accepts. The
 * port is waitable: signaled while connections are pending. Closing the last
 * handle to it frees the name and drops what was never accepted, which the
 * client sees as end of stream.
 */
struct PortConnection {
    uint32_t client_pid;
    Header* server_read;  /* client -> server pipe, read end */
    Header* server_write; /* server -> client pipe, write end */
};

struct PortObject {
    Header header;
    bool in_use;
    char name[kPortNameCapacity];
    uint32_t pending_head;
    uint32_t pending_count;
    PortConnection pending[kPortPendingCapacity];
};

void retain(Header* object);
void release(Header*& object);
IoObject* as_io(Header* object);
const IoObject* as_io(const Header* object);
EventObject* as_event(Header* object);
const EventObject* as_event(const Header* object);
TimerObject* as_timer(Header* object);
const TimerObject* as_timer(const Header* object);
SectionObject* as_section(Header* object);
const SectionObject* as_section(const Header* object);
SemaphoreObject* as_semaphore(Header* object);
const SemaphoreObject* as_semaphore(const Header* object);
EventObject* create_event(bool manual_reset, bool initial_state);
TimerObject* create_timer(bool manual_reset);
SectionObject* create_section(uint64_t size_bytes, uint32_t access_mask);
// Devuelve una seccion respaldada por el archivo, REUSANDO la existente si ya
// hay una para ese inode. Devuelve nullptr solo si no se pudo crear ninguna.
// No deja referencia puesta, igual que create_section: la que instala el handle
// la toma. Retener aqui seria una referencia de mas y la seccion no muere nunca.
SectionObject* acquire_file_section(vfs::Vnode& node, uint32_t access_mask);
/* Idem pero solo un rango del archivo: [offset, offset+length). Un PT_LOAD es
 * un tramo del archivo, no el archivo entero, y mapear la seccion completa por
 * segmento pondria los bytes en la direccion que no les corresponde. offset==0
 * con length==0 significa "el archivo entero", igual que acquire_file_section. */
SectionObject* acquire_file_range_section(
    vfs::Vnode& node,
    uint64_t offset,
    uint64_t length,
    uint32_t access_mask);
SectionObject* clone_section(const SectionObject& source);
SemaphoreObject* create_semaphore(int32_t initial_count, int32_t max_count);
PortObject* as_port(Header* object);
ThreadObject* as_thread(Header* object);
/* nullptr if the table is full. No reference is left on the object, as with
 * create_event: whoever keeps it takes one. */
ThreadObject* create_thread_object(uint32_t tid, uint32_t pid);
/* Marks the thread ended with `exit_code` and signals the object. The caller
 * wakes the waiters (process::notify_object_signal). Once only. */
void end_thread_object(ThreadObject* thread_object, int32_t exit_code);
/* nullptr if the name is taken or the table is full. No reference is left on
 * the object, as with create_section: the handle that installs it takes one. */
PortObject* create_port(const char* name);
PortObject* find_port(const char* name);
/* Takes over the references in `connection`. false if the queue is full. */
bool port_enqueue(PortObject* port, const PortConnection& connection);
/* Hands over the oldest pending connection's references. false if none. */
bool port_dequeue(PortObject* port, PortConnection& connection);
void set_event(EventObject* event_object);
void reset_event(EventObject* event_object);
void set_timer(TimerObject* timer_object, uint64_t due_tick, uint64_t period_ticks);
void cancel_timer(TimerObject* timer_object);
void poll_timers(uint64_t now_ms, void (*on_signal)(Header* object));
// Suma release_count (>0) al contador del semáforo, sin superar max_count.
// Devuelve false (sin modificar nada) si el resultado excedería max_count.
bool release_semaphore(SemaphoreObject* semaphore_object, int32_t release_count, int32_t* previous_count);
bool can_satisfy_wait(const Header* object);
// Secciones vivas en la tabla, y cuantas respaldan un archivo. Es lo que hace
// observable el cache por inode: dos procesos con la misma libreria tienen que
// sumar 1 entre los dos, no 2.
void section_live_counts(uint32_t& live, uint32_t& file_backed);bool try_acquire_wait(Header* object);

} // namespace object
