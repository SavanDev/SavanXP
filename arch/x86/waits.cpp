#include "waits.hpp"
#include "io.hpp"
#include "paging.hpp"
#include "pit.hpp"

namespace i386::waits {
namespace {
object::Header* lookup(uint32_t fd, uint32_t access) {
    auto& task = *process::current();
    if (fd >= io::kMaxHandles || (task.handles[fd].granted_access & access) != access) return nullptr;
    return task.handles[fd].object;
}
int install(object::Header* header) {
    if (!header) return -SAVANXP_ENOMEM;
    const int fd = io::install_object(*process::current(), header,
        object::access_query | object::access_modify | object::access_synchronize);
    if (fd < 0) { object::retain(header); object::release(header); }
    return fd;
}
int consume(process::Process& task) {
    if (task.wait_all) {
        for (uint32_t n = 0; n < task.object_count; ++n)
            if (!object::can_satisfy_wait(task.wait_objects[n])) return io::kBlocked;
        for (uint32_t n = 0; n < task.object_count; ++n) object::try_acquire_wait(task.wait_objects[n]);
        return 0;
    }
    for (uint32_t n = 0; n < task.object_count; ++n)
        if (object::try_acquire_wait(task.wait_objects[n])) return n;
    return io::kBlocked;
}
int begin(uint32_t address, uint32_t count, uint32_t flags, int32_t timeout, bool single) {
    auto& task = *process::current();
    if (!count || count > 16 || (flags & ~SAVANXP_WAIT_FLAG_ALL)) return -SAVANXP_EINVAL;
    int32_t fds[16];
    if (single) fds[0] = address;
    else if (!paging::copy_from_user(task.directory, fds, address, count * sizeof(int32_t))) return -SAVANXP_EFAULT;
    object::Header* objects[16];
    for (uint32_t n = 0; n < count; ++n) {
        objects[n] = lookup(fds[n], object::access_synchronize);
        if (!objects[n]) return -SAVANXP_EBADF;
        if (!objects[n]->waitable) return -SAVANXP_EINVAL;
        // Repeated auto-reset objects cannot satisfy an atomic wait-all.
        for (uint32_t m = 0; m < n; ++m)
            if (objects[m] == objects[n] && (flags & SAVANXP_WAIT_FLAG_ALL)) return -SAVANXP_EINVAL;
    }
    task.object_count = count;
    task.wait_all = flags & SAVANXP_WAIT_FLAG_ALL;
    for (uint32_t n = 0; n < count; ++n) {
        task.wait_objects[n] = objects[n];
        object::retain(objects[n]);
    }
    int result = consume(task);
    if (result != io::kBlocked || !timeout) {
        clear(task);
        return result == io::kBlocked ? -SAVANXP_ETIMEDOUT : result;
    }
    task.wake_ms = timeout < 0 ? 0 : pit::uptime_ms() + timeout;
    task.state = process::State::object_wait;
    return io::kBlocked;
}
}
void clear(process::Process& task) {
    for (uint32_t n = 0; n < task.object_count; ++n) object::release(task.wait_objects[n]);
    task.object_count = 0;
}
void service() {
    const uint64_t now = pit::uptime_ms();
    object::poll_timers(now, nullptr);
    for (uint32_t slot = 0; slot < process::kCapacity; ++slot) {
        auto& task = *process::at(slot);
        if (task.state != process::State::object_wait) continue;
        int result = consume(task);
        if (result == io::kBlocked) {
            if (!task.wake_ms || task.wake_ms > now) continue;
            result = -SAVANXP_ETIMEDOUT;
        }
        clear(task);
        task.frame->eax = result;
        task.state = process::State::runnable;
    }
}
bool syscall(ContextFrame& frame) {
    int result;
    switch (frame.eax) {
    case SAVANXP_SYS_EVENT_CREATE:
        if (frame.edi & ~(SAVANXP_EVENT_MANUAL_RESET | SAVANXP_EVENT_INITIAL_SET)) result = -SAVANXP_EINVAL;
        else { auto* e = object::create_event(frame.edi & SAVANXP_EVENT_MANUAL_RESET, frame.edi & SAVANXP_EVENT_INITIAL_SET); result = install(e ? &e->header : nullptr); }
        break;
    case SAVANXP_SYS_EVENT_SET:
    case SAVANXP_SYS_EVENT_RESET: {
        auto* e = object::as_event(lookup(frame.edi, object::access_modify));
        if (!e) { result = -SAVANXP_EBADF; break; }
        if (frame.eax == SAVANXP_SYS_EVENT_SET) object::set_event(e); else object::reset_event(e);
        result = 0;
        break;
    }
    case SAVANXP_SYS_TIMER_CREATE:
        if (frame.edi & ~SAVANXP_TIMER_MANUAL_RESET) result = -SAVANXP_EINVAL;
        else { auto* t = object::create_timer(frame.edi & SAVANXP_TIMER_MANUAL_RESET); result = install(t ? &t->header : nullptr); }
        break;
    case SAVANXP_SYS_TIMER_SET:
    case SAVANXP_SYS_TIMER_CANCEL: {
        auto* t = object::as_timer(lookup(frame.edi, object::access_modify));
        if (!t) { result = -SAVANXP_EBADF; break; }
        if (frame.eax == SAVANXP_SYS_TIMER_SET) object::set_timer(t, pit::uptime_ms() + frame.esi, frame.edx);
        else object::cancel_timer(t);
        result = 0;
        break;
    }
    case SAVANXP_SYS_SEMAPHORE_CREATE: {
        if (static_cast<int32_t>(frame.edi) < 0 || static_cast<int32_t>(frame.esi) <= 0 || frame.edi > frame.esi) result = -SAVANXP_EINVAL;
        else { auto* s = object::create_semaphore(frame.edi, frame.esi); result = install(s ? &s->header : nullptr); }
        break;
    }
    case SAVANXP_SYS_SEMAPHORE_RELEASE: {
        auto* s = object::as_semaphore(lookup(frame.edi, object::access_modify));
        int32_t previous = 0;
        result = !s ? -SAVANXP_EBADF : object::release_semaphore(s, frame.esi, &previous) ? previous : -SAVANXP_EINVAL;
        break;
    }
    case SAVANXP_SYS_WAIT_ONE: result = begin(frame.edi, 1, 0, frame.esi, true); break;
    case SAVANXP_SYS_WAIT_MANY: result = begin(frame.edi, frame.esi, frame.edx, frame.ecx, false); break;
    default: return false;
    }
    frame.eax = result;
    service();
    return true;
}
}
