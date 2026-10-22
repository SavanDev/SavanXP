#include "kernel/subsystem.hpp"

#include "kernel/process.hpp"

namespace subsystem {
namespace {

Dispatcher g_table[static_cast<size_t>(Id::count)] = {};
size_t g_state_size[static_cast<size_t>(Id::count)] = {};
WaitRecheck g_wait_recheck[static_cast<size_t>(Id::count)] = {};

} // namespace

void reset() {
    for (auto& entry : g_table) {
        entry = nullptr;
    }
    for (auto& bytes : g_state_size) {
        bytes = 0;
    }
    for (auto& hook : g_wait_recheck) {
        hook = nullptr;
    }
}

// Per-process private state sizing, registered by the subsystem itself.
size_t registered_state_size(Id id) {
    const auto index = static_cast<size_t>(id);
    if (index >= static_cast<size_t>(Id::count)) {
        return 0;
    }
    return g_state_size[index];
}

bool register_dispatcher(Id id, Dispatcher fn) {
    const auto index = static_cast<size_t>(id);
    if (index >= static_cast<size_t>(Id::count)) {
        return false;
    }
    g_table[index] = fn;
    return true;
}

Dispatcher dispatcher_for(Id id) {
    const auto index = static_cast<size_t>(id);
    if (index >= static_cast<size_t>(Id::count)) {
        return nullptr;
    }
    return g_table[index];
}

void register_process_state_size(Id id, size_t bytes) {
    const auto index = static_cast<size_t>(id);
    if (index >= static_cast<size_t>(Id::count)) {
        return;
    }
    g_state_size[index] = bytes;
}

size_t process_state_size(Id id) {
    const auto index = static_cast<size_t>(id);
    if (index >= static_cast<size_t>(Id::count)) {
        return 0;
    }
    return g_state_size[index];
}

void register_wait_recheck(Id id, WaitRecheck fn) {
    const auto index = static_cast<size_t>(id);
    if (index >= static_cast<size_t>(Id::count)) {
        return;
    }
    g_wait_recheck[index] = fn;
}

void recheck_parked_thread(process::Thread& thread, uint64_t now_ms, bool honour_deadline) {
    const auto index = static_cast<size_t>(thread.process->subsystem_id);
    if (index >= static_cast<size_t>(Id::count)) {
        return;
    }
    const WaitRecheck hook = g_wait_recheck[index];
    if (hook != nullptr) {
        hook(thread, now_ms, honour_deadline);
    }
}

process::SavedContext* dispatch(Id id, process::SavedContext* context) {
    Dispatcher fn = dispatcher_for(id);
    return fn != nullptr ? fn(context) : context;
}

} // namespace subsystem
