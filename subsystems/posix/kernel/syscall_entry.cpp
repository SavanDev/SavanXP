#include "kernel/process.hpp"

extern "C" process::SavedContext* savanxp_handle_syscall(process::SavedContext* context) {
    return process::handle_syscall(context);
}
