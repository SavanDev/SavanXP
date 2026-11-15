#include "kernel/object.hpp"

#include "kernel/string.hpp"

namespace object {

namespace {

EventObject g_event_objects[kMaxEventObjects] = {};
TimerObject g_timer_objects[kMaxTimerObjects] = {};
SemaphoreObject g_semaphore_objects[kMaxSemaphoreObjects] = {};

void destroy_event(Header* header) {
    EventObject* event_object = as_event(header);
    if (event_object == nullptr) {
        return;
    }
    memset(event_object, 0, sizeof(*event_object));
}

void destroy_timer(Header* header) {
    TimerObject* timer_object = as_timer(header);
    if (timer_object == nullptr) {
        return;
    }
    memset(timer_object, 0, sizeof(*timer_object));
}

void destroy_semaphore(Header* header) {
    SemaphoreObject* semaphore_object = as_semaphore(header);
    if (semaphore_object == nullptr) {
        return;
    }
    memset(semaphore_object, 0, sizeof(*semaphore_object));
}

} // namespace

EventObject* as_event(Header* object) {
    if (object == nullptr || object->type != Type::event) {
        return nullptr;
    }
    return reinterpret_cast<EventObject*>(object);
}

const EventObject* as_event(const Header* object) {
    if (object == nullptr || object->type != Type::event) {
        return nullptr;
    }
    return reinterpret_cast<const EventObject*>(object);
}

TimerObject* as_timer(Header* object) {
    if (object == nullptr || object->type != Type::timer) {
        return nullptr;
    }
    return reinterpret_cast<TimerObject*>(object);
}

const TimerObject* as_timer(const Header* object) {
    if (object == nullptr || object->type != Type::timer) {
        return nullptr;
    }
    return reinterpret_cast<const TimerObject*>(object);
}

SemaphoreObject* as_semaphore(Header* object) {
    if (object == nullptr || object->type != Type::semaphore) {
        return nullptr;
    }
    return reinterpret_cast<SemaphoreObject*>(object);
}

const SemaphoreObject* as_semaphore(const Header* object) {
    if (object == nullptr || object->type != Type::semaphore) {
        return nullptr;
    }
    return reinterpret_cast<const SemaphoreObject*>(object);
}

EventObject* create_event(bool manual_reset, bool initial_state) {
    for (EventObject& event_object : g_event_objects) {
        if (event_object.in_use) {
            continue;
        }
        memset(&event_object, 0, sizeof(event_object));
        event_object.header.type = Type::event;
        event_object.header.destroy = destroy_event;
        event_object.header.waitable = true;
        event_object.header.manual_reset = manual_reset;
        event_object.header.signal_count = initial_state ? 1 : 0;
        event_object.in_use = true;
        return &event_object;
    }
    return nullptr;
}

TimerObject* create_timer(bool manual_reset) {
    for (TimerObject& timer_object : g_timer_objects) {
        if (timer_object.in_use) {
            continue;
        }
        memset(&timer_object, 0, sizeof(timer_object));
        timer_object.header.type = Type::timer;
        timer_object.header.destroy = destroy_timer;
        timer_object.header.waitable = true;
        timer_object.header.manual_reset = manual_reset;
        timer_object.in_use = true;
        return &timer_object;
    }
    return nullptr;
}

SemaphoreObject* create_semaphore(int32_t initial_count, int32_t max_count) {
    if (max_count <= 0 || initial_count < 0 || initial_count > max_count) {
        return nullptr;
    }

    for (SemaphoreObject& semaphore_object : g_semaphore_objects) {
        if (semaphore_object.in_use) {
            continue;
        }
        memset(&semaphore_object, 0, sizeof(semaphore_object));
        semaphore_object.header.type = Type::semaphore;
        semaphore_object.header.destroy = destroy_semaphore;
        semaphore_object.header.waitable = true;
        semaphore_object.header.manual_reset = false;
        semaphore_object.header.signal_count = initial_count;
        semaphore_object.in_use = true;
        semaphore_object.max_count = max_count;
        return &semaphore_object;
    }
    return nullptr;
}

bool release_semaphore(SemaphoreObject* semaphore_object, int32_t release_count, int32_t* previous_count) {
    if (semaphore_object == nullptr || !semaphore_object->in_use || release_count <= 0) {
        return false;
    }

    const int32_t previous = semaphore_object->header.signal_count;
    if (release_count > semaphore_object->max_count - previous) {
        return false;
    }

    if (previous_count != nullptr) {
        *previous_count = previous;
    }
    semaphore_object->header.signal_count = previous + release_count;
    return true;
}

void set_event(EventObject* event_object) {
    if (event_object != nullptr && event_object->in_use) {
        event_object->header.signal_count = 1;
    }
}

void reset_event(EventObject* event_object) {
    if (event_object != nullptr && event_object->in_use) {
        event_object->header.signal_count = 0;
    }
}

void set_timer(TimerObject* timer_object, uint64_t due_ms, uint64_t period_ms) {
    if (timer_object == nullptr || !timer_object->in_use) {
        return;
    }
    timer_object->armed = true;
    timer_object->header.signal_count = 0;
    timer_object->due_ms = due_ms;
    timer_object->period_ms = period_ms;
}

void cancel_timer(TimerObject* timer_object) {
    if (timer_object == nullptr || !timer_object->in_use) {
        return;
    }
    timer_object->armed = false;
    timer_object->header.signal_count = 0;
    timer_object->due_ms = 0;
    timer_object->period_ms = 0;
}

void poll_timers(uint64_t now_ms, void (*on_signal)(Header* object)) {
    for (TimerObject& timer_object : g_timer_objects) {
        if (!timer_object.in_use || !timer_object.armed || timer_object.due_ms > now_ms) {
            continue;
        }

        timer_object.header.signal_count = 1;
        if (timer_object.period_ms != 0) {
            do {
                timer_object.due_ms += timer_object.period_ms;
            } while (timer_object.due_ms <= now_ms);
        } else {
            timer_object.armed = false;
            timer_object.due_ms = 0;
        }

        if (on_signal != nullptr) {
            on_signal(&timer_object.header);
        }
    }
}

bool can_satisfy_wait(const Header* object) {
    return object != nullptr && object->waitable && object->signal_count > 0;
}

bool try_acquire_wait(Header* object) {
    if (!can_satisfy_wait(object)) {
        return false;
    }
    if (!object->manual_reset) {
        object->signal_count -= 1;
    }
    return true;
}

SectionObject* acquire_file_section(vfs::Vnode& node, uint32_t access_mask) {
    return acquire_file_range_section(node, 0, 0, access_mask);
}

} // namespace object
