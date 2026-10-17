#include "kernel/process.hpp"

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_native_abi.h"
#include "kernel/console.hpp"
#include "kernel/display.hpp"
#include "kernel/heap.hpp"
#include "kernel/sx.hpp"

namespace process {

// El subsistema nativo declara su estado privado por proceso como cualquier
// otro (rebanada B). Hoy declara cero: un proceso nativo no tiene estado propio,
// y decirlo es lo que hace medible la afirmacion de que al kernel no le queda
// nada de POSIX.
void initialize_native_subsystem() {
    subsystem::register_process_state_size(subsystem::Id::native, 0);
}

SavedContext* dispatch_native_syscall(SavedContext* context) {
    Process* const caller = sx::current();
    if (caller == nullptr) {
        return context;
    }

    caller->context = context;
    caller->state = State::running;

    // Prueba observable de que el camino nativo esta vivo. Se emite una
    // sola vez (la primera syscall nativa de todo el sistema); confirma que un
    // proceso marcado nativo entra por ESTE dispatcher y no por el de posix.
    static bool announced = false;
    if (!announced) {
        announced = true;
        console::printf(
            "native: dispatcher activo (pid=%u, syscall=%u)\n",
            static_cast<unsigned>(caller->pid),
            static_cast<unsigned>(context->rax)
        );
    }

    // Syscalls PROPIAS del subsistema nativo (>= SXN_SYS_BASE). Solo existen
    // aca: un proceso posix que invoque estos numeros recibe ENOSYS. El
    // contrato vive en include/abi/savanxp_native_abi.h.
    //
    // Esta unidad solo usa la superficie nativa sx:: (current, memoria de
    // usuario, errores) mas display:: y console::. No accede a los internals
    // de process.cpp: es el patron que la rebanada POSIX va a seguir.
    switch (context->rax) {
        case SXN_SYS_INFO: {
            if (!sx::validate_user_range(context->rdi, sizeof(sxn_native_info), true)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            sxn_native_info info = {};
            info.abi_version = SXN_ABI_VERSION;
            info.subsystem_id = static_cast<uint32_t>(caller->subsystem_id);
            info.pid = caller->pid;
            if (!sx::copy_to_user(context->rdi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SXN_SYS_LOG: {
            char message[256] = {};
            if (!sx::copy_user_string(message, sizeof(message), context->rdi)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            console::printf("native[%u]: %s\n", static_cast<unsigned>(caller->pid), message);
            context->rax = 0;
            return context;
        }
        case SXN_SYS_EXIT:
            sx::exit_current(static_cast<int>(context->rdi));
            return nullptr;
        case SXN_SYS_SPAWN: {
            // Misma convencion de registros que el spawn POSIX: el hijo se
            // crea con create_process_internal y su subsistema lo decide el
            // EI_OSABI de su imagen, no el del padre.
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            auto* arguments = static_cast<SpawnArguments*>(heap::allocate(sizeof(SpawnArguments)));
            if (arguments == nullptr) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_ENOMEM));
                return context;
            }
            if (!copy_spawn_arguments(*caller, *context, path, sizeof(path), *arguments) ||
                !resolve_path_from_root(path, resolved, sizeof(resolved))) {
                heap::free(arguments);
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_E2BIG));
                return context;
            }

            ImageFailure failure = ImageFailure::none;
            Process* child = create_process_internal(resolved, arguments->argc, arguments->argv, caller, -1, -1, failure);
            if (child != nullptr && !clone_handles_for_spawn(*child, *caller)) {
                failure = ImageFailure::no_handles;
                child = nullptr;
            }
            heap::free(arguments);
            context->rax = static_cast<uint64_t>(child != nullptr
                ? static_cast<int>(child->pid)
                : sx::negative_error(image_failure_errno(failure)));
            return context;
        }
        case SXN_SYS_WAITPID: {
            const int result = waitpid_child(*caller, static_cast<int64_t>(context->rdi), context->rsi);
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        // Sincronizacion y memoria compartida: los mismos nucleos publicados
        // que usa la traduccion POSIX (las banderas SXN_* coinciden en valor
        // con las del SDK; el contrato nativo es dueno de sus nombres).
        case SXN_SYS_EVENT_CREATE:
            context->rax = static_cast<uint64_t>(create_event_handle(*caller, static_cast<uint32_t>(context->rdi)));
            return context;
        case SXN_SYS_EVENT_SET:
            context->rax = static_cast<uint64_t>(set_event_handle(*caller, context->rdi));
            return context;
        case SXN_SYS_EVENT_RESET:
            context->rax = static_cast<uint64_t>(reset_event_handle(*caller, context->rdi));
            return context;
        case SXN_SYS_WAIT_ONE: {
            const int result = wait_on_handle(*caller, context->rdi, static_cast<int64_t>(context->rsi));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SXN_SYS_SECTION_CREATE:
            context->rax = static_cast<uint64_t>(create_section_handle(*caller, context->rdi, static_cast<uint32_t>(context->rsi)));
            return context;
        case SXN_SYS_MAP_VIEW:
            context->rax = static_cast<uint64_t>(map_view_handle(*caller, context->rdi, static_cast<uint32_t>(context->rsi)));
            return context;
        case SXN_SYS_UNMAP_VIEW:
            context->rax = static_cast<uint64_t>(unmap_view_address(*caller, context->rdi));
            return context;
        // Graficos: mismos internals (display::*) y misma sesion exclusiva por
        // pid que los ioctls GPU de posix (kernel/gpu_device.cpp). La sesion se
        // libera sola al morir el proceso (display::release_session_for en el
        // terminate).
        case SXN_SYS_GFX_INFO: {
            if (!sx::validate_user_range(context->rdi, sizeof(sxn_gfx_info), true)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            savanxp_gpu_info gpu_info = {};
            if (!display::get_info(gpu_info)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_ENODEV));
                return context;
            }

            sxn_gfx_info info = {};
            info.width = gpu_info.width;
            info.height = gpu_info.height;
            info.pitch = gpu_info.pitch;
            info.bpp = gpu_info.bpp;
            info.buffer_size = gpu_info.buffer_size;
            if (!sx::copy_to_user(context->rdi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SXN_SYS_GFX_ACQUIRE: {
            context->rax = static_cast<uint64_t>(
                display::acquire_session(caller->pid) ? 0 : sx::negative_error(SX_EBUSY));
            return context;
        }
        case SXN_SYS_GFX_RELEASE: {
            if (!display::owns_session(caller->pid)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EBUSY));
                return context;
            }
            display::release_session_resources();
            display::release_session(caller->pid);
            context->rax = 0;
            return context;
        }
        case SXN_SYS_GFX_PRESENT: {
            if (!display::owns_session(caller->pid)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EBUSY));
                return context;
            }

            sxn_gfx_present region = {};
            if (!sx::validate_user_range(context->rdi, sizeof(region), false) ||
                !sx::copy_from_user(&region, context->rdi, sizeof(region))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            const savanxp_fb_info& fb = display::framebuffer_info();
            if (region.pixels == 0 || region.source_pitch == 0 || region.width == 0 ||
                region.height == 0 || region.x >= fb.width || region.y >= fb.height ||
                region.width > (fb.width - region.x) ||
                region.height > (fb.height - region.y)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            const uint64_t row_bytes = static_cast<uint64_t>(region.width) * sizeof(uint32_t);
            const uint64_t last_row = static_cast<uint64_t>(region.y + region.height - 1);
            const uint64_t touched_bytes = (last_row * region.source_pitch) +
                (static_cast<uint64_t>(region.x) * sizeof(uint32_t)) + row_bytes;
            if (!sx::validate_user_range(region.pixels, touched_bytes, false)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SX_EINVAL));
                return context;
            }

            const bool presented = display::present_region(
                reinterpret_cast<const void*>(region.pixels),
                region.source_pitch,
                region.x,
                region.y,
                region.width,
                region.height
            );
            context->rax = static_cast<uint64_t>(presented ? 0 : sx::negative_error(SX_EIO));
            return context;
        }
        default:
            break;
    }

    // Standalone native: un proceso nativo solo habla SXN. Cualquier numero
    // fuera del contrato (incluido el rango posix) es ENOSYS, sin delegacion.
    context->rax = static_cast<uint64_t>(sx::negative_error(SX_ENOSYS));
    return context;
}

} // namespace process
