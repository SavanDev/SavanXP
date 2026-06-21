SavedContext* dispatch_native_syscall(SavedContext* context) {
    if (g_current == nullptr) {
        return context;
    }
    context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ENOSYS));
    return context;
}
