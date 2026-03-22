#include "kernel/object.hpp"

#include "kernel/string.hpp"

namespace object {

namespace {

EventObject g_event_objects[kMaxEventObjects] = {};
TimerObject g_timer_objects[kMaxTimerObjects] = {};

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

} // namespace

void retain(Header* object) {
    if (object != nullptr) {
        object->refcount += 1;
    }
}

void release(Header*& object) {
    Header* current = object;
    object = nullptr;
    if (current == nullptr) {
        return;
    }

    if (current->refcount != 0) {
        current->refcount -= 1;
    }
    if (current->refcount == 0 && current->destroy != nullptr) {
        current->destroy(current);
    }
}

IoObject* as_io(Header* object) {
    if (object == nullptr || object->type != Type::io) {
        return nullptr;
    }
    return reinterpret_cast<IoObject*>(object);
}

const IoObject* as_io(const Header* object) {
    if (object == nullptr || object->type != Type::io) {
        return nullptr;
    }
    return reinterpret_cast<const IoObject*>(object);
}

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

EventObject* create_event(bool manual_reset, bool initial_state) {
    for (EventObject& event_object : g_event_objects) {
        if (event_object.in_use) {
            continue;
        }
        memset(&event_object, 0, sizeof(event_object));
        event_object.header.type = Type::event;
        event_object.header.destroy = destroy_event;
        event_object.in_use = true;
        event_object.manual_reset = manual_reset;
        event_object.signaled = initial_state;
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
        timer_object.in_use = true;
        timer_object.manual_reset = manual_reset;
        return &timer_object;
    }
    return nullptr;
}

void set_event(EventObject* event_object) {
    if (event_object != nullptr && event_object->in_use) {
        event_object->signaled = true;
    }
}

void reset_event(EventObject* event_object) {
    if (event_object != nullptr && event_object->in_use) {
        event_object->signaled = false;
    }
}

void set_timer(TimerObject* timer_object, uint64_t due_tick, uint64_t period_ticks) {
    if (timer_object == nullptr || !timer_object->in_use) {
        return;
    }
    timer_object->armed = true;
    timer_object->signaled = false;
    timer_object->due_tick = due_tick;
    timer_object->period_ticks = period_ticks;
}

void cancel_timer(TimerObject* timer_object) {
    if (timer_object == nullptr || !timer_object->in_use) {
        return;
    }
    timer_object->armed = false;
    timer_object->signaled = false;
    timer_object->due_tick = 0;
    timer_object->period_ticks = 0;
}

void poll_timers(uint64_t current_tick, void (*on_signal)(Header* object)) {
    for (TimerObject& timer_object : g_timer_objects) {
        if (!timer_object.in_use || !timer_object.armed || timer_object.due_tick > current_tick) {
            continue;
        }

        timer_object.signaled = true;
        if (timer_object.period_ticks != 0) {
            do {
                timer_object.due_tick += timer_object.period_ticks;
            } while (timer_object.due_tick <= current_tick);
        } else {
            timer_object.armed = false;
            timer_object.due_tick = 0;
        }

        if (on_signal != nullptr) {
            on_signal(&timer_object.header);
        }
    }
}

bool can_satisfy_wait(const Header* object) {
    const EventObject* event_object = as_event(object);
    if (event_object != nullptr) {
        return event_object->in_use && event_object->signaled;
    }

    const TimerObject* timer_object = as_timer(object);
    return timer_object != nullptr && timer_object->in_use && timer_object->signaled;
}

bool try_acquire_wait(Header* object) {
    EventObject* event_object = as_event(object);
    if (event_object != nullptr) {
        if (!can_satisfy_wait(object)) {
            return false;
        }
        if (!event_object->manual_reset) {
            event_object->signaled = false;
        }
        return true;
    }

    TimerObject* timer_object = as_timer(object);
    if (timer_object == nullptr || !can_satisfy_wait(object)) {
        return false;
    }
    if (!timer_object->manual_reset) {
        timer_object->signaled = false;
    }
    return true;
}

} // namespace object
