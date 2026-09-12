SavedContext* dispatch_posix_syscall(SavedContext* context) {
    if (this_cpu().current == nullptr) {
        return context;
    }

    this_cpu().current->context = context;
    this_cpu().current->state = State::running;

    switch (context->rax) {
        case SAVANXP_SYS_READ: {
            const int result = read_handle(*this_cpu().current, context->rdi, context->rsi, static_cast<size_t>(context->rdx));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_WRITE: {
            const int result = write_handle(*this_cpu().current, context->rdi, context->rsi, static_cast<size_t>(context->rdx));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_OPEN: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*this_cpu().current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*this_cpu().current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(open_node(*this_cpu().current, resolved, static_cast<uint32_t>(context->rsi)));
            return context;
        }
        case SAVANXP_SYS_CLOSE:
            context->rax = static_cast<uint64_t>(close_fd(*this_cpu().current, context->rdi));
            return context;
        case SAVANXP_SYS_READDIR:
            context->rax = static_cast<uint64_t>(readdir_node(*this_cpu().current, context->rdi, context->rsi, static_cast<size_t>(context->rdx)));
            return context;
        case SAVANXP_SYS_SPAWN:
        case SAVANXP_SYS_SPAWN_FD: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            auto* arguments = static_cast<SpawnArguments*>(heap::allocate(sizeof(SpawnArguments)));
            if (arguments == nullptr) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ENOMEM));
                return context;
            }
            if (!copy_spawn_arguments(*this_cpu().current, *context, path, sizeof(path), *arguments) ||
                !resolve_process_path(*this_cpu().current, path, resolved, sizeof(resolved))) {
                heap::free(arguments);
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_E2BIG));
                return context;
            }

            int stdin_fd = -1;
            int stdout_fd = -1;
            if (context->rax == SAVANXP_SYS_SPAWN_FD) {
                stdin_fd = static_cast<int>(context->r10);
                stdout_fd = static_cast<int>(context->r8);
            }

            ImageFailure failure = ImageFailure::none;
            Process* child = create_process_internal(resolved, arguments->argc, arguments->argv, this_cpu().current, stdin_fd, stdout_fd, failure);
            heap::free(arguments);
            context->rax = static_cast<uint64_t>(child != nullptr
                ? static_cast<int>(child->pid)
                : negative_error(image_failure_errno(failure)));
            return context;
        }
        case SAVANXP_SYS_EXEC: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            auto* arguments = static_cast<SpawnArguments*>(heap::allocate(sizeof(SpawnArguments)));
            if (arguments == nullptr) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ENOMEM));
                return context;
            }
            if (!copy_spawn_arguments(*this_cpu().current, *context, path, sizeof(path), *arguments) ||
                !resolve_process_path(*this_cpu().current, path, resolved, sizeof(resolved))) {
                heap::free(arguments);
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_E2BIG));
                return context;
            }

            ImageFailure failure = ImageFailure::none;
            const bool prepared = prepare_exec_image(*this_cpu().current, resolved, arguments->argc, arguments->argv, failure);
            heap::free(arguments);
            if (!prepared) {
                context->rax = static_cast<uint64_t>(negative_error(image_failure_errno(failure)));
                return context;
            }

            switch_to_process(this_cpu().current);
            return this_cpu().current->context;
        }
        case SAVANXP_SYS_WAITPID: {
            const uint32_t waited_pid = static_cast<int64_t>(context->rdi) == -1 ? kWaitAnyPid : static_cast<uint32_t>(context->rdi);
            Process* child = find_zombie_child(this_cpu().current->pid, waited_pid);
            if (child != nullptr) {
                write_int_to_process_memory(*this_cpu().current, context->rsi, child->exit_code);
                const int pid = static_cast<int>(child->pid);
                reap_process(*child);
                context->rax = static_cast<uint64_t>(pid);
                return context;
            }

            if (waited_pid != kWaitAnyPid && child_of(this_cpu().current->pid, waited_pid) == nullptr) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ECHILD));
                return context;
            }
            if (waited_pid == kWaitAnyPid && !has_child(this_cpu().current->pid)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ECHILD));
                return context;
            }

            clear_wait_state(*this_cpu().current);
            this_cpu().current->wait_reason = WaitReason::child;
            this_cpu().current->waiting_for_pid = waited_pid;
            this_cpu().current->wait_status_address = context->rsi;
            this_cpu().current->state = State::blocked_wait;
            return choose_next_context(context);
        }
        case SAVANXP_SYS_EXIT:
            terminate_current(static_cast<int>(context->rdi));
            return nullptr;
        case SAVANXP_SYS_YIELD:
            // El proceso ocioso es un bucle de yield en userland, que no puede
            // haltear porque hlt es privilegiada. Si el que cede es EL y no hay
            // nadie mas listo, la maquina no tiene nada que hacer: se haltea
            // aca, en su nombre, hasta la proxima interrupcion. Sin esto la
            // ociosidad se gasta girando por el syscall a toda velocidad --
            // invisible mientras poll() se quedaba con la CPU, y el costo real
            // apenas poll paso a bloquear.
            //
            // sti;hlt es el par atomico de siempre: sti recien surte efecto
            // despues de la instruccion que sigue, asi que una interrupcion que
            // llegue entre el chequeo y el hlt lo despierta igual.
            if (this_cpu().current->idle && !has_runnable_non_idle()) {
                arch::x86_64::enable_interrupts();
                arch::x86_64::halt_once();
                arch::x86_64::disable_interrupts();
            }
            this_cpu().current->state = State::ready;
            context->rax = 0;
            return choose_next_context(context);
        case SAVANXP_SYS_UPTIME_MS:
            context->rax = current_uptime_ms();
            return context;
        case SAVANXP_SYS_MONOTONIC_NS:
            context->rax = timer::monotonic_ns();
            return context;
        case SAVANXP_SYS_CLEAR:
            tty::clear();
            context->rax = 0;
            return context;
        case SAVANXP_SYS_SLEEP_MS: {
            const int result = sleep_via_timer(*this_cpu().current, context->rdi);
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_PIPE:
            context->rax = static_cast<uint64_t>(create_pipe(*this_cpu().current, context->rdi));
            return context;
        case SAVANXP_SYS_DUP:
            context->rax = static_cast<uint64_t>(duplicate_fd(*this_cpu().current, context->rdi));
            return context;
        case SAVANXP_SYS_DUP2:
            context->rax = static_cast<uint64_t>(duplicate_fd_to(*this_cpu().current, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_PROC_INFO: {
            if (!vm::is_user_range_accessible(this_cpu().current->address_space, context->rsi, sizeof(savanxp_process_info), true)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            savanxp_process_info info = {};
            if (!snapshot_process(static_cast<size_t>(context->rdi), info)) {
                context->rax = 0;
                return context;
            }
            if (!write_to_process_memory(*this_cpu().current, context->rsi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = 1;
            return context;
        }
        case SAVANXP_SYS_SEEK:
            context->rax = static_cast<uint64_t>(seek_fd(*this_cpu().current, context->rdi, static_cast<int64_t>(context->rsi), context->rdx));
            return context;
        case SAVANXP_SYS_UNLINK: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*this_cpu().current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*this_cpu().current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(unlink_path(resolved));
            return context;
        }
        case SAVANXP_SYS_MKDIR: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*this_cpu().current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*this_cpu().current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(mkdir_path(resolved));
            return context;
        }
        case SAVANXP_SYS_RMDIR: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*this_cpu().current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*this_cpu().current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(rmdir_path(resolved));
            return context;
        }
        case SAVANXP_SYS_TRUNCATE: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*this_cpu().current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*this_cpu().current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(truncate_path(resolved, context->rsi));
            return context;
        }
        case SAVANXP_SYS_RENAME: {
            char old_path[process::kProcessPathLength] = {};
            char new_path[process::kProcessPathLength] = {};
            char resolved_old[process::kProcessPathLength] = {};
            char resolved_new[process::kProcessPathLength] = {};
            if (!copy_user_string(*this_cpu().current, context->rdi, old_path, sizeof(old_path)) ||
                !copy_user_string(*this_cpu().current, context->rsi, new_path, sizeof(new_path)) ||
                !resolve_process_path(*this_cpu().current, old_path, resolved_old, sizeof(resolved_old)) ||
                !resolve_process_path(*this_cpu().current, new_path, resolved_new, sizeof(resolved_new))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(rename_path(resolved_old, resolved_new));
            return context;
        }
        case SAVANXP_SYS_IOCTL:
            context->rax = static_cast<uint64_t>(ioctl_handle(*this_cpu().current, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_SOCKET:
            context->rax = static_cast<uint64_t>(create_socket_fd(*this_cpu().current, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_BIND:
            context->rax = static_cast<uint64_t>(bind_fd(*this_cpu().current, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_SENDTO:
            context->rax = static_cast<uint64_t>(sendto_fd(*this_cpu().current, context->rdi, context->rsi, static_cast<size_t>(context->rdx), context->r10));
            return context;
        case SAVANXP_SYS_RECVFROM:
            context->rax = static_cast<uint64_t>(recvfrom_fd(
                *this_cpu().current,
                context->rdi,
                context->rsi,
                static_cast<size_t>(context->rdx),
                context->r10,
                static_cast<uint32_t>(context->r8)));
            return context;
        case SAVANXP_SYS_SETSOCKOPT:
            context->rax = static_cast<uint64_t>(setsockopt_fd(*this_cpu().current, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_CONNECT:
            context->rax = static_cast<uint64_t>(connect_fd(*this_cpu().current, context->rdi, context->rsi, static_cast<uint32_t>(context->rdx)));
            return context;
        case SAVANXP_SYS_GETPID:
            context->rax = current_pid();
            return context;
        case SAVANXP_SYS_STAT: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*this_cpu().current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*this_cpu().current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(stat_path(*this_cpu().current, resolved, context->rsi));
            return context;
        }
        case SAVANXP_SYS_FSTAT:
            context->rax = static_cast<uint64_t>(fstat_fd(*this_cpu().current, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_CHDIR: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*this_cpu().current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*this_cpu().current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(chdir_path(*this_cpu().current, resolved));
            return context;
        }
        case SAVANXP_SYS_GETCWD:
            context->rax = static_cast<uint64_t>(getcwd_path(*this_cpu().current, context->rdi, static_cast<size_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_SYSTEM_INFO: {
            if (!vm::is_user_range_accessible(this_cpu().current->address_space, context->rdi, sizeof(savanxp_system_info), true)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            savanxp_system_info info = {};
            if (!snapshot_system_info(info) ||
                !write_to_process_memory(*this_cpu().current, context->rdi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SAVANXP_SYS_SYNC:
            context->rax = static_cast<uint64_t>(sxfs::sync(sxfs::root()) ? 0 : negative_error(SAVANXP_EIO));
            return context;
        case SAVANXP_SYS_REALTIME: {
            if (!vm::is_user_range_accessible(this_cpu().current->address_space, context->rdi, sizeof(savanxp_realtime), true)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            savanxp_realtime value = {};
            if (!rtc::read_time(&value) ||
                !write_to_process_memory(*this_cpu().current, context->rdi, &value, sizeof(value))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EIO));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SAVANXP_SYS_FCNTL:
            context->rax = static_cast<uint64_t>(fcntl_fd(*this_cpu().current, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_POLL: {
            const int result = poll_fds(
                *this_cpu().current,
                context->rdi,
                static_cast<size_t>(context->rsi),
                static_cast<int32_t>(context->rdx));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_FORK: {
            Process* child = fork_process(*this_cpu().current, *context);
            context->rax = static_cast<uint64_t>(child != nullptr ? static_cast<int>(child->pid) : negative_error(SAVANXP_ENOMEM));
            return context;
        }
        case SAVANXP_SYS_KILL:
            context->rax = static_cast<uint64_t>(kill_process(*this_cpu().current, static_cast<int>(context->rdi), static_cast<int>(context->rsi)));
            return context;
        case SAVANXP_SYS_EVENT_CREATE:
            context->rax = static_cast<uint64_t>(create_event_handle(*this_cpu().current, static_cast<uint32_t>(context->rdi)));
            return context;
        case SAVANXP_SYS_EVENT_SET:
            context->rax = static_cast<uint64_t>(set_event_handle(*this_cpu().current, context->rdi));
            return context;
        case SAVANXP_SYS_EVENT_RESET:
            context->rax = static_cast<uint64_t>(reset_event_handle(*this_cpu().current, context->rdi));
            return context;
        case SAVANXP_SYS_WAIT_ONE: {
            const int result = wait_on_handle(*this_cpu().current, context->rdi, static_cast<int64_t>(context->rsi));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_WAIT_MANY: {
            const int result = wait_on_many_handles(
                *this_cpu().current,
                context->rdi,
                static_cast<size_t>(context->rsi),
                (context->rdx & SAVANXP_WAIT_FLAG_ALL) != 0,
                static_cast<int64_t>(context->r10));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_TIMER_CREATE:
            context->rax = static_cast<uint64_t>(create_timer_handle(*this_cpu().current, static_cast<uint32_t>(context->rdi)));
            return context;
        case SAVANXP_SYS_TIMER_SET:
            context->rax = static_cast<uint64_t>(set_timer_handle(*this_cpu().current, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_TIMER_CANCEL:
            context->rax = static_cast<uint64_t>(cancel_timer_handle(*this_cpu().current, context->rdi));
            return context;
        case SAVANXP_SYS_SECTION_CREATE:
            context->rax = static_cast<uint64_t>(create_section_handle(*this_cpu().current, context->rdi, static_cast<uint32_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_MAP_VIEW:
            context->rax = static_cast<uint64_t>(map_view_handle(*this_cpu().current, context->rdi, static_cast<uint32_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_UNMAP_VIEW:
            context->rax = static_cast<uint64_t>(unmap_view_address(*this_cpu().current, context->rdi));
            return context;
        case SAVANXP_SYS_SEMAPHORE_CREATE:
            context->rax = static_cast<uint64_t>(create_semaphore_handle(
                *this_cpu().current,
                static_cast<int32_t>(context->rdi),
                static_cast<int32_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_SEMAPHORE_RELEASE:
            context->rax = static_cast<uint64_t>(release_semaphore_handle(
                *this_cpu().current,
                context->rdi,
                static_cast<uint32_t>(context->rsi)));
            return context;
        default:
            context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ENOSYS));
            return context;
    }
}
