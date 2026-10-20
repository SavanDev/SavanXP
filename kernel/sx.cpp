#include "kernel/sx.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/object.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/process.hpp"
#include "kernel/process_detail.hpp"
#include "kernel/smp.hpp"
#include "kernel/string.hpp"
#include "kernel/vmm.hpp"

namespace sx {
namespace {

uint64_t read_cr3() {
    uint64_t value = 0;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

void write_cr3(uint64_t value) {
    asm volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

} // namespace

process::Process* current() {
    return smp::this_cpu().current;
}

uint32_t current_pid() {
    return smp::this_cpu().current != nullptr ? smp::this_cpu().current->pid : 0;
}

bool read_user_memory(const process::Process& proc, void* destination, uint64_t user_address, size_t count, bool require_write) {
    if (destination == nullptr || count == 0) {
        return false;
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_address, count, require_write)) {
        return false;
    }

    if (&proc == smp::this_cpu().current) {
        memcpy(destination, reinterpret_cast<const void*>(user_address), count);
        return true;
    }

    const uint64_t previous_cr3 = read_cr3();
    write_cr3(proc.address_space.pml4_physical);
    memcpy(destination, reinterpret_cast<const void*>(user_address), count);
    write_cr3(previous_cr3);
    return true;
}

bool write_user_memory(process::Process& proc, uint64_t user_address, const void* source, size_t count) {
    if (source == nullptr || count == 0) {
        return false;
    }
    if (!vm::is_user_range_accessible(proc.address_space, user_address, count, true)) {
        return false;
    }

    if (&proc == smp::this_cpu().current) {
        memcpy(reinterpret_cast<void*>(user_address), source, count);
        return true;
    }

    const uint64_t previous_cr3 = read_cr3();
    write_cr3(proc.address_space.pml4_physical);
    memcpy(reinterpret_cast<void*>(user_address), source, count);
    write_cr3(previous_cr3);
    return true;
}

bool copy_user_string_from(const process::Process& proc, uint64_t user_address, char* buffer, size_t capacity) {
    if (buffer == nullptr || capacity == 0 || user_address == 0) {
        return false;
    }

    size_t index = 0;
    while (index + 1 < capacity) {
        if (!read_user_memory(proc, &buffer[index], user_address + index, 1, false)) {
            buffer[0] = '\0';
            return false;
        }
        if (buffer[index] == '\0') {
            return true;
        }
        ++index;
    }

    buffer[capacity - 1] = '\0';
    return false;
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

bool target_ready(const object::Header* handle_object) {
    return is_waitable(handle_object) ? object::can_satisfy_wait(handle_object) : stream_ready(handle_object);
}

bool acquire_target(object::Header* handle_object) {
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

bool can_complete_object_wait(const process::Process& proc) {
    if (proc.wait_reason != process::WaitReason::object || proc.wait_handle_count == 0) {
        return false;
    }

    if (proc.wait_all != 0) {
        for (size_t index = 0; index < proc.wait_handle_count; ++index) {
            if (!target_ready(proc.waited_objects[index])) {
                return false;
            }
        }
        return true;
    }

    for (size_t index = 0; index < proc.wait_handle_count; ++index) {
        if (target_ready(proc.waited_objects[index])) {
            return true;
        }
    }
    return false;
}

int consume_object_wait(process::Process& proc) {
    if (proc.wait_reason != process::WaitReason::object || proc.wait_handle_count == 0) {
        return negative_error(SX_EINVAL);
    }

    if (proc.wait_all != 0) {
        for (size_t index = 0; index < proc.wait_handle_count; ++index) {
            if (!acquire_target(proc.waited_objects[index])) {
                return negative_error(SX_EINVAL);
            }
        }
        return 0;
    }

    for (size_t index = 0; index < proc.wait_handle_count; ++index) {
        if (acquire_target(proc.waited_objects[index])) {
            return static_cast<int>(index);
        }
    }
    return negative_error(SX_EINVAL);
}

int try_consume_objects(process::Process& proc, object::Header* const* handles, size_t count, bool wait_all) {
    if (count == 0 || count > process::kMaxWaitHandles) {
        return negative_error(SX_EINVAL);
    }

    process::clear_wait_state(proc);
    proc.wait_reason = process::WaitReason::object;
    proc.wait_handle_count = static_cast<uint8_t>(count);
    proc.wait_all = wait_all ? 1u : 0u;
    for (size_t index = 0; index < count; ++index) {
        proc.waited_objects[index] = handles[index];
    }

    if (can_complete_object_wait(proc)) {
        return consume_object_wait(proc);
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

    /* base == 0 deja que el kernel elija, como map_view. Con base fija, el
     * cargador la elige para respetar el p_vaddr que pide el ELF. La pagina se
     * alinea igual hacia abajo: el kernel mapea por paginas completas y no puede
     * mapear un parcial. */
    if (base != 0 && (base & (memory::kPageSize - 1)) != 0) {
        return negative_error(SX_EINVAL);
    }
    uint64_t base_address = base;
    if (!vm::map_section_view(proc.address_space, section, requested_access, base_address, share_on_fork)) {
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
