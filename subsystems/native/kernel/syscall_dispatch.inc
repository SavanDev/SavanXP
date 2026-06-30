SavedContext* dispatch_native_syscall(SavedContext* context) {
    if (g_current == nullptr) {
        return context;
    }

    // Fase 1: prueba observable de que el camino nativo esta vivo. Lo emitimos
    // una sola vez (la primera syscall nativa de todo el sistema) para no ensuciar
    // el log; confirma que un proceso marcado nativo entra por ESTE dispatcher y
    // no por el de posix.
    static bool announced = false;
    if (!announced) {
        announced = true;
        console::printf(
            "native: dispatcher activo (pid=%u, syscall=%u)\n",
            static_cast<unsigned>(g_current->pid),
            static_cast<unsigned>(context->rax)
        );
    }

    // El ABI nativo todavia comparte la convencion de posix (ver
    // subsystems/native/README.md). Este dispatcher es el punto de divergencia:
    // por ahora delega el baseline en posix, y aqui iran apareciendo las syscalls
    // propias del mundo nativo (Fase 2).
    return dispatch_posix_syscall(context);
}
