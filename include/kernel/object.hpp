#pragma once

#include <stddef.h>
#include <stdint.h>

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

constexpr size_t kMaxIoObjects = 128;
constexpr size_t kMaxEventObjects = 64;
constexpr size_t kMaxTimerObjects = 64;

enum class Type : uint8_t {
    none = 0,
    io = 1,
    process = 2,
    thread = 3,
    event = 4,
    semaphore = 5,
    timer = 6,
    section = 7,
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
    bool manual_reset;
    bool signaled;
};

struct TimerObject {
    Header header;
    bool in_use;
    bool manual_reset;
    bool signaled;
    bool armed;
    uint64_t due_tick;
    uint64_t period_ticks;
};

void retain(Header* object);
void release(Header*& object);
IoObject* as_io(Header* object);
const IoObject* as_io(const Header* object);
EventObject* as_event(Header* object);
const EventObject* as_event(const Header* object);
TimerObject* as_timer(Header* object);
const TimerObject* as_timer(const Header* object);
EventObject* create_event(bool manual_reset, bool initial_state);
TimerObject* create_timer(bool manual_reset);
void set_event(EventObject* event_object);
void reset_event(EventObject* event_object);
void set_timer(TimerObject* timer_object, uint64_t due_tick, uint64_t period_ticks);
void cancel_timer(TimerObject* timer_object);
void poll_timers(uint64_t current_tick, void (*on_signal)(Header* object));
bool can_satisfy_wait(const Header* object);
bool try_acquire_wait(Header* object);

} // namespace object
