SavedContext* dispatch_native_syscall(SavedContext* context) {
    if (g_current == nullptr) {
        return context;
    }

    g_current->context = context;
    g_current->state = State::running;

    // Fase 1: prueba observable de que el camino nativo esta vivo. Se emite una
    // sola vez (la primera syscall nativa de todo el sistema); confirma que un
    // proceso marcado nativo entra por ESTE dispatcher y no por el de posix.
    static bool announced = false;
    if (!announced) {
        announced = true;
        console::printf(
            "native: dispatcher activo (pid=%u, syscall=%u)\n",
            static_cast<unsigned>(g_current->pid),
            static_cast<unsigned>(context->rax)
        );
    }

    // Syscalls PROPIAS del subsistema nativo (>= SXN_SYS_BASE). Solo existen
    // aca: un proceso posix que invoque estos numeros recibe ENOSYS. El
    // contrato vive en subsystems/native/sdk/include/savanxp_native_abi.h.
    switch (context->rax) {
        case SXN_SYS_INFO: {
            if (!vm::is_user_range_accessible(
                    g_current->address_space, context->rdi, sizeof(sxn_native_info), true)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            sxn_native_info info = {};
            info.abi_version = SXN_ABI_VERSION;
            info.subsystem_id = static_cast<uint32_t>(g_current->subsystem_id);
            info.pid = g_current->pid;
            if (!write_to_process_memory(*g_current, context->rdi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SXN_SYS_LOG: {
            char message[256] = {};
            if (!copy_user_string(*g_current, context->rdi, message, sizeof(message))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            console::printf("native[%u]: %s\n", static_cast<unsigned>(g_current->pid), message);
            context->rax = 0;
            return context;
        }
        // Graficos: mismos internals (display::*) y misma sesion exclusiva por
        // pid que los ioctls GPU de posix (kernel/gpu_device.cpp). La sesion se
        // libera sola al morir el proceso (display::release_session_for en el
        // terminate).
        case SXN_SYS_GFX_INFO: {
            if (!vm::is_user_range_accessible(
                    g_current->address_space, context->rdi, sizeof(sxn_gfx_info), true)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            savanxp_gpu_info gpu_info = {};
            if (!display::get_info(gpu_info)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ENODEV));
                return context;
            }

            sxn_gfx_info info = {};
            info.width = gpu_info.width;
            info.height = gpu_info.height;
            info.pitch = gpu_info.pitch;
            info.bpp = gpu_info.bpp;
            info.buffer_size = gpu_info.buffer_size;
            if (!write_to_process_memory(*g_current, context->rdi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SXN_SYS_GFX_ACQUIRE: {
            context->rax = static_cast<uint64_t>(
                display::acquire_session(g_current->pid) ? 0 : negative_error(SAVANXP_EBUSY));
            return context;
        }
        case SXN_SYS_GFX_RELEASE: {
            if (!display::owns_session(g_current->pid)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EBUSY));
                return context;
            }
            display::release_session_resources();
            display::release_session(g_current->pid);
            context->rax = 0;
            return context;
        }
        case SXN_SYS_GFX_PRESENT: {
            if (!display::owns_session(g_current->pid)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EBUSY));
                return context;
            }

            sxn_gfx_present region = {};
            if (!vm::is_user_range_accessible(
                    g_current->address_space, context->rdi, sizeof(region), false) ||
                !read_from_process_memory(*g_current, &region, context->rdi, sizeof(region))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            const savanxp_fb_info& fb = display::framebuffer_info();
            if (region.pixels == 0 || region.source_pitch == 0 || region.width == 0 ||
                region.height == 0 || region.x >= fb.width || region.y >= fb.height ||
                region.width > (fb.width - region.x) ||
                region.height > (fb.height - region.y)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            const uint64_t row_bytes = static_cast<uint64_t>(region.width) * sizeof(uint32_t);
            const uint64_t last_row = static_cast<uint64_t>(region.y + region.height - 1);
            const uint64_t touched_bytes = (last_row * region.source_pitch) +
                (static_cast<uint64_t>(region.x) * sizeof(uint32_t)) + row_bytes;
            if (!vm::is_user_range_accessible(
                    g_current->address_space, region.pixels, touched_bytes, false)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
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
            context->rax = static_cast<uint64_t>(presented ? 0 : negative_error(SAVANXP_EIO));
            return context;
        }
        default:
            break;
    }

    // Numeros del rango nativo no implementados: ENOSYS aca mismo, sin pasar
    // por posix (el rango >= SXN_SYS_BASE pertenece a este dispatcher).
    if (context->rax >= SXN_SYS_BASE) {
        context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ENOSYS));
        return context;
    }

    // Baseline transitorio (< SXN_SYS_BASE): READ/WRITE/EXIT/etc. se delegan en
    // la tabla posix mientras el ABI nativo propio crece syscall a syscall.
    return dispatch_posix_syscall(context);
}
