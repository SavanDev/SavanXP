#include "kernel/subsystem.hpp"

namespace subsystem {
namespace {

Dispatcher g_table[static_cast<size_t>(Id::count)] = {};

} // namespace

void reset() {
    for (auto& entry : g_table) {
        entry = nullptr;
    }
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

process::SavedContext* dispatch(Id id, process::SavedContext* context) {
    Dispatcher fn = dispatcher_for(id);
    return fn != nullptr ? fn(context) : context;
}

} // namespace subsystem
