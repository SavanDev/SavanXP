#pragma once

#include <stddef.h>
#include <stdint.h>

namespace process {
struct SavedContext;
}

namespace subsystem {

enum class Id : uint8_t {
    posix = 0,   // default; coincide con el zero-init de allocate_process_slot
    native = 1,  // stub de validacion
    count
};

using Dispatcher = process::SavedContext* (*)(process::SavedContext*);

void reset();
bool register_dispatcher(Id id, Dispatcher fn);
Dispatcher dispatcher_for(Id id);
process::SavedContext* dispatch(Id id, process::SavedContext* context);

} // namespace subsystem
