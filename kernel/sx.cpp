#include "kernel/sx.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/cpu.hpp"
#include "kernel/object.hpp"
#include "kernel/net.hpp"
#include "abi/savanxp_native_abi.h"
#include "kernel/physical_memory.hpp"
#include "kernel/process.hpp"
#include "kernel/process_detail.hpp"
#include "kernel/smp.hpp"
#include "kernel/string.hpp"
#include "kernel/vmm.hpp"

namespace sx {

process::Thread* current_thread() {
    return smp::this_cpu().current;
}

process::Process* current() {
    process::Thread* const thread = smp::this_cpu().current;
    return thread != nullptr ? thread->process : nullptr;
}

uint32_t current_pid() {
    process::Process* const proc = current();
    return proc != nullptr ? proc->pid : 0;
}

bool read_user_memory(const process::Process& proc, void* destination, uint64_t user_address, size_t count, bool require_write) {
    if (destination == nullptr || count == 0) {
        return false;
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_address, count, require_write)) {
        return false;
    }

    if (&proc == current()) {
        arch::user_access_begin();
        memcpy(destination, reinterpret_cast<const void*>(user_address), count);
        arch::user_access_end();
        return true;
    }

    const uint64_t previous_cr3 = arch::read_page_table_root();
    arch::load_page_table_root(proc.address_space.pml4_physical);
    arch::user_access_begin();
    memcpy(destination, reinterpret_cast<const void*>(user_address), count);
    arch::user_access_end();
    arch::load_page_table_root(previous_cr3);
    return true;
}

bool write_user_memory(process::Process& proc, uint64_t user_address, const void* source, size_t count) {
    if (source == nullptr || count == 0) {
        return false;
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_address, count, true)) {
        return false;
    }

    if (&proc == current()) {
        arch::user_access_begin();
        memcpy(reinterpret_cast<void*>(user_address), source, count);
        arch::user_access_end();
        return true;
    }

    const uint64_t previous_cr3 = arch::read_page_table_root();
    arch::load_page_table_root(proc.address_space.pml4_physical);
    arch::user_access_begin();
    memcpy(reinterpret_cast<void*>(user_address), source, count);
    arch::user_access_end();
    arch::load_page_table_root(previous_cr3);
    return true;
}

sx_status copy_user_path_from(const process::Process& proc, uint64_t user_address, char* buffer, size_t capacity) {
    if (buffer == nullptr || capacity == 0 || user_address == 0) {
        return SX_EINVAL;
    }

    size_t index = 0;
    while (index + 1 < capacity) {
        if (!read_user_memory(proc, &buffer[index], user_address + index, 1, false)) {
            buffer[0] = '\0';
            return SX_EINVAL;
        }
        if (buffer[index] == '\0') {
            return kStatusOk;
        }
        ++index;
    }

    buffer[capacity - 1] = '\0';
    return SX_ENAMETOOLONG;
}

bool copy_user_string_from(const process::Process& proc, uint64_t user_address, char* buffer, size_t capacity) {
    return copy_user_path_from(proc, user_address, buffer, capacity) == kStatusOk;
}

bool copy_from_user(void* destination, uint64_t user_address, size_t count) {
    process::Process* const me = current();
    if (me == nullptr) {
        return false;
    }
    return read_user_memory(*me, destination, user_address, count, false);
}

bool copy_to_user(uint64_t user_address, const void* source, size_t count) {
    process::Process* const me = current();
    if (me == nullptr) {
        return false;
    }
    return write_user_memory(*me, user_address, source, count);
}

bool validate_user_range(uint64_t user_address, size_t count, bool require_write) {
    process::Process* const me = current();
    if (me == nullptr) {
        return false;
    }
    return vm::is_user_range_accessible(me->address_space, user_address, count, require_write);
}

bool copy_user_string(char* buffer, size_t capacity, uint64_t user_address) {
    process::Process* const me = current();
    if (me == nullptr) {
        return false;
    }
    return copy_user_string_from(*me, user_address, buffer, capacity);
}

int negative_error(sx_status code) {
    return -static_cast<int>(code);
}

void exit_current(int exit_code) {
    process::terminate_current(exit_code);
}

object::EventObject* create_event(bool manual_reset, bool initial_state) {
    return object::create_event(manual_reset, initial_state);
}

void set_event(object::EventObject* event_object) {
    object::set_event(event_object);
}

void reset_event(object::EventObject* event_object) {
    object::reset_event(event_object);
}

object::TimerObject* create_timer(bool manual_reset) {
    return object::create_timer(manual_reset);
}

void set_timer(object::TimerObject* timer_object, uint64_t due_tick, uint64_t period_ticks) {
    object::set_timer(timer_object, due_tick, period_ticks);
}

void cancel_timer(object::TimerObject* timer_object) {
    object::cancel_timer(timer_object);
}

void poll_timers(uint64_t now_ms, void (*on_signal)(object::Header* object)) {
    object::poll_timers(now_ms, on_signal);
}

object::SemaphoreObject* create_semaphore(int32_t initial_count, int32_t max_count) {
    return object::create_semaphore(initial_count, max_count);
}

bool release_semaphore(object::SemaphoreObject* semaphore_object, int32_t release_count, int32_t* previous_count) {
    return object::release_semaphore(semaphore_object, release_count, previous_count);
}

bool is_waitable(const object::Header* handle_object) {
    return handle_object != nullptr && handle_object->waitable;
}

namespace {

// A stream a read would not block on: data, or the other end gone.
bool stream_ready(const object::Header* handle_object) {
    const object::IoObject* file = object::as_io(handle_object);
    if (file == nullptr) {
        return false;
    }
    const short revents = process::detail::poll_open_file(file, SAVANXP_POLLIN);
    return (revents & (SAVANXP_POLLIN | SAVANXP_POLLHUP)) != 0;
}

bool io_ready(const object::Header* handle_object, uint32_t interests) {
    const object::IoObject* file = object::as_io(handle_object);
    if (!file) return false;
    short events = 0;
    if (interests & SXN_IO_READ) events |= SAVANXP_POLLIN;
    if (interests & SXN_IO_WRITE) events |= SAVANXP_POLLOUT;
    const short ready = process::detail::poll_open_file(file, events);
    if (ready & (events | SAVANXP_POLLHUP)) return true;
    return file->kind == process::HandleKind::socket && net::socket_has_error(file->socket);
}

bool target_ready(const object::Header* handle_object, uint32_t interests) {
    if (interests) return io_ready(handle_object, interests);
    return is_waitable(handle_object) ? object::can_satisfy_wait(handle_object) : stream_ready(handle_object);
}

bool acquire_target(object::Header* handle_object, uint32_t interests) {
    if (interests) return io_ready(handle_object, interests);
    return is_waitable(handle_object) ? object::try_acquire_wait(handle_object) : stream_ready(handle_object);
}

} // namespace

bool is_wait_target(const object::Header* handle_object) {
    if (is_waitable(handle_object)) {
        return true;
    }
    const object::IoObject* file = object::as_io(handle_object);
    return file != nullptr && (file->open_flags & process::open_read) != 0;
}

bool can_complete_object_wait(const process::Thread& thread) {
    if (thread.wait_reason != process::WaitReason::object || thread.wait_handle_count == 0) {
        return false;
    }

    if (thread.wait_all != 0) {
        for (size_t index = 0; index < thread.wait_handle_count; ++index) {
            if (!target_ready(thread.waited_objects[index], thread.wait_io_interests[index])) {
                return false;
            }
        }
        return true;
    }

    for (size_t index = 0; index < thread.wait_handle_count; ++index) {
        if (target_ready(thread.waited_objects[index], thread.wait_io_interests[index])) {
            return true;
        }
    }
    return false;
}

int consume_object_wait(process::Thread& thread) {
    if (thread.wait_reason != process::WaitReason::object || thread.wait_handle_count == 0) {
        return negative_error(SX_EINVAL);
    }

    if (thread.wait_all != 0) {
        for (size_t index = 0; index < thread.wait_handle_count; ++index) {
            if (!acquire_target(thread.waited_objects[index], thread.wait_io_interests[index])) {
                return negative_error(SX_EINVAL);
            }
        }
        return 0;
    }

    for (size_t index = 0; index < thread.wait_handle_count; ++index) {
        if (acquire_target(thread.waited_objects[index], thread.wait_io_interests[index])) {
            return static_cast<int>(index);
        }
    }
    return negative_error(SX_EINVAL);
}

int try_consume_objects(process::Thread& thread, object::Header* const* handles, size_t count, bool wait_all, const uint32_t* interests) {
    if (count == 0 || count > process::kMaxWaitHandles) {
        return negative_error(SX_EINVAL);
    }

    process::clear_wait_state(thread);
    thread.wait_reason = process::WaitReason::object;
    thread.wait_handle_count = static_cast<uint8_t>(count);
    thread.wait_all = wait_all ? 1u : 0u;
    for (size_t index = 0; index < count; ++index) {
        thread.waited_objects[index] = handles[index];
        thread.wait_io_interests[index] = interests ? interests[index] : 0;
        if (interests) object::retain(handles[index]);
    }

    thread.wait_reserved = interests ? 1 : 0;
    if (can_complete_object_wait(thread)) {
        return consume_object_wait(thread);
    }
    return negative_error(SX_EAGAIN);
}

object::SectionObject* create_section(uint64_t size_bytes, uint32_t access_mask) {
    return object::create_section(size_bytes, access_mask);
}

object::SectionObject* open_file_section(vfs::Vnode& node, uint64_t offset, uint64_t length, uint32_t access_mask) {
    return object::acquire_file_range_section(node, offset, length, access_mask);
}

int64_t map_view(process::Process& proc, object::SectionObject& section, uint32_t requested_access, uint32_t granted_access, uint64_t base, bool share_on_fork) {
    if ((requested_access & granted_access) != requested_access) {
        return negative_error(SX_EACCES);
    }
    if ((requested_access & section.access_mask) != requested_access) {
        return negative_error(SX_EACCES);
    }
    // W^X tambien se rechaza aca, y no solo en vm::map_section_view: alla el
    // rechazo llega como un ENOMEM que no dice por que fallo la vista. Un
    // cargador que pide texto ejecutable y escribible por error -- el error
    // clasico de un segmento mal declarado -- merece un EINVAL que lo diga.
    if ((requested_access & object::access_execute) != 0 &&
        (requested_access & object::access_write) != 0) {
        return negative_error(SX_EINVAL);
    }

    /* base == 0 lets the kernel choose, as map_view does. A fixed base is the
     * loader's choice, to honour an ELF's p_vaddr or a PE's image base. It
     * must be page-aligned: the kernel maps whole pages, and rounding down
     * would hand back a view that starts before the address asked for. */
    if (base != 0 && (base & (memory::kPageSize - 1)) != 0) {
        return negative_error(SX_EINVAL);
    }
    uint64_t base_address = base;
    if (!vm::map_section_view(proc.address_space, section, requested_access, base_address, share_on_fork)) {
        /* At a chosen base, say why: a range outside the user half is the
         * caller's mistake (EINVAL), and an occupied one is EEXIST, as
         * Linux's MAP_FIXED_NOREPLACE answers. A loader that falls back to
         * another address needs to tell the two from running out of memory. */
        if (base != 0) {
            switch (vm::classify_fixed_range(proc.address_space, base, section.size_bytes)) {
                case vm::FixedRange::outside: return negative_error(SX_EINVAL);
                case vm::FixedRange::occupied: return negative_error(SX_EEXIST);
                case vm::FixedRange::free: break;
            }
        }
        return negative_error(SX_ENOMEM);
    }
    return static_cast<int64_t>(base_address);
}

int unmap_view(process::Process& proc, uint64_t base_address) {
    if (base_address == 0 || !vm::unmap_section_view(proc.address_space, base_address)) {
        return negative_error(SX_EINVAL);
    }
    return 0;
}

} // namespace sx
