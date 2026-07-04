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
