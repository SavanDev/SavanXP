#include "kernel/object.hpp"

#include "kernel/heap.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/vmm.hpp"

namespace object {

namespace {

EventObject g_event_objects[kMaxEventObjects] = {};
TimerObject g_timer_objects[kMaxTimerObjects] = {};
SectionObject g_section_objects[kMaxSectionObjects] = {};

uint64_t align_up(uint64_t value, uint64_t alignment) {
    const uint64_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

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

void destroy_section(Header* header) {
    SectionObject* section_object = as_section(header);
    if (section_object == nullptr) {
        return;
    }

    if (section_object->physical_pages != nullptr) {
        for (uint64_t index = 0; index < section_object->page_count; ++index) {
            if (section_object->physical_pages[index] != 0) {
                (void)memory::free_pages(section_object->physical_pages[index], 1);
            }
        }
        heap::free(section_object->physical_pages);
    }

    memset(section_object, 0, sizeof(*section_object));
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

SectionObject* as_section(Header* object) {
    if (object == nullptr || object->type != Type::section) {
        return nullptr;
    }
    return reinterpret_cast<SectionObject*>(object);
}

const SectionObject* as_section(const Header* object) {
    if (object == nullptr || object->type != Type::section) {
        return nullptr;
    }
    return reinterpret_cast<const SectionObject*>(object);
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

SectionObject* create_section(uint64_t size_bytes, uint32_t access_mask) {
    if (size_bytes == 0) {
        return nullptr;
    }

    const uint64_t aligned_size = align_up(size_bytes, memory::kPageSize);
    const uint64_t page_count = aligned_size / memory::kPageSize;
    if (page_count == 0) {
        return nullptr;
    }

    for (SectionObject& section_object : g_section_objects) {
        if (section_object.in_use) {
            continue;
        }

        memset(&section_object, 0, sizeof(section_object));
        section_object.physical_pages = static_cast<uint64_t*>(
            heap::allocate(static_cast<size_t>(page_count * sizeof(uint64_t)), alignof(uint64_t)));
        if (section_object.physical_pages == nullptr) {
            return nullptr;
        }
        memset(section_object.physical_pages, 0, static_cast<size_t>(page_count * sizeof(uint64_t)));

        bool okay = true;
        for (uint64_t page_index = 0; page_index < page_count; ++page_index) {
            memory::PageAllocation allocation = {};
            if (!memory::allocate_page(allocation)) {
                okay = false;
                break;
            }

            memset(allocation.virtual_address, 0, memory::kPageSize);
            section_object.physical_pages[page_index] = allocation.physical_address;
        }

        if (!okay) {
            for (uint64_t page_index = 0; page_index < page_count; ++page_index) {
                if (section_object.physical_pages[page_index] != 0) {
                    (void)memory::free_pages(section_object.physical_pages[page_index], 1);
                }
            }
            heap::free(section_object.physical_pages);
            memset(&section_object, 0, sizeof(section_object));
            return nullptr;
        }

        section_object.header.type = Type::section;
        section_object.header.destroy = destroy_section;
        section_object.in_use = true;
        section_object.access_mask = access_mask;
        section_object.size_bytes = aligned_size;
        section_object.page_count = page_count;
        return &section_object;
    }

    return nullptr;
}

SectionObject* clone_section(const SectionObject& source) {
    if (!source.in_use) {
        return nullptr;
    }

    SectionObject* section_object = create_section(source.size_bytes, source.access_mask);
    if (section_object == nullptr) {
        return nullptr;
    }

    for (uint64_t page_index = 0; page_index < source.page_count; ++page_index) {
        memcpy(
            vm::physical_to_virtual(section_object->physical_pages[page_index]),
            vm::physical_to_virtual(source.physical_pages[page_index]),
            memory::kPageSize);
    }
    return section_object;
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
