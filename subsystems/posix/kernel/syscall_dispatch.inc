SavedContext* dispatch_posix_syscall(SavedContext* context) {
    if (g_current == nullptr) {
        return context;
    }

    g_current->context = context;
    g_current->state = State::running;

    switch (context->rax) {
        case SAVANXP_SYS_READ: {
            const int result = read_handle(*g_current, context->rdi, context->rsi, static_cast<size_t>(context->rdx));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_WRITE: {
            const int result = write_handle(*g_current, context->rdi, context->rsi, static_cast<size_t>(context->rdx));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_OPEN: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(open_node(*g_current, resolved, static_cast<uint32_t>(context->rsi)));
            return context;
        }
        case SAVANXP_SYS_CLOSE:
            context->rax = static_cast<uint64_t>(close_fd(*g_current, context->rdi));
            return context;
        case SAVANXP_SYS_READDIR:
            context->rax = static_cast<uint64_t>(readdir_node(*g_current, context->rdi, context->rsi, static_cast<size_t>(context->rdx)));
            return context;
        case SAVANXP_SYS_SPAWN:
        case SAVANXP_SYS_SPAWN_FD: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            char argv_storage[16][64] = {};
            const char* argv_local[16] = {};
            int argc = 0;
            if (!copy_spawn_arguments(*g_current, *context, path, sizeof(path), argv_storage, argv_local, argc) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            int stdin_fd = -1;
            int stdout_fd = -1;
            if (context->rax == SAVANXP_SYS_SPAWN_FD) {
                stdin_fd = static_cast<int>(context->r10);
                stdout_fd = static_cast<int>(context->r8);
            }

            Process* child = create_process_internal(resolved, argc, argv_local, g_current, stdin_fd, stdout_fd);
            context->rax = static_cast<uint64_t>(child != nullptr ? static_cast<int>(child->pid) : negative_error(SAVANXP_ENOENT));
            return context;
        }
        case SAVANXP_SYS_EXEC: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            char argv_storage[16][64] = {};
            const char* argv_local[16] = {};
            int argc = 0;
            if (!copy_spawn_arguments(*g_current, *context, path, sizeof(path), argv_storage, argv_local, argc) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            if (!prepare_exec_image(*g_current, resolved, argc, argv_local)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ENOENT));
                return context;
            }

            switch_to_process(g_current);
            return g_current->context;
        }
        case SAVANXP_SYS_WAITPID: {
            const uint32_t waited_pid = static_cast<int64_t>(context->rdi) == -1 ? kWaitAnyPid : static_cast<uint32_t>(context->rdi);
            Process* child = find_zombie_child(g_current->pid, waited_pid);
            if (child != nullptr) {
                write_int_to_process_memory(*g_current, context->rsi, child->exit_code);
                const int pid = static_cast<int>(child->pid);
                reap_process(*child);
                context->rax = static_cast<uint64_t>(pid);
                return context;
            }

            if (waited_pid != kWaitAnyPid && child_of(g_current->pid, waited_pid) == nullptr) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ECHILD));
                return context;
            }
            if (waited_pid == kWaitAnyPid && !has_child(g_current->pid)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ECHILD));
                return context;
            }

            clear_wait_state(*g_current);
            g_current->wait_reason = WaitReason::child;
            g_current->waiting_for_pid = waited_pid;
            g_current->wait_status_address = context->rsi;
            g_current->state = State::blocked_wait;
            return choose_next_context(context);
        }
        case SAVANXP_SYS_EXIT:
            terminate_current(static_cast<int>(context->rdi));
            return nullptr;
        case SAVANXP_SYS_YIELD:
            g_current->state = State::ready;
            context->rax = 0;
            return choose_next_context(context);
        case SAVANXP_SYS_UPTIME_MS:
            context->rax = current_uptime_ms();
            return context;
        case SAVANXP_SYS_CLEAR:
            tty::clear();
            context->rax = 0;
            return context;
        case SAVANXP_SYS_SLEEP_MS: {
            const int result = sleep_via_timer(*g_current, context->rdi);
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_PIPE:
            context->rax = static_cast<uint64_t>(create_pipe(*g_current, context->rdi));
            return context;
        case SAVANXP_SYS_DUP:
            context->rax = static_cast<uint64_t>(duplicate_fd(*g_current, context->rdi));
            return context;
        case SAVANXP_SYS_DUP2:
            context->rax = static_cast<uint64_t>(duplicate_fd_to(*g_current, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_PROC_INFO: {
            if (!vm::is_user_range_accessible(g_current->address_space, context->rsi, sizeof(savanxp_process_info), true)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            savanxp_process_info info = {};
            if (!snapshot_process(static_cast<size_t>(context->rdi), info)) {
                context->rax = 0;
                return context;
            }
            if (!write_to_process_memory(*g_current, context->rsi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = 1;
            return context;
        }
        case SAVANXP_SYS_SEEK:
            context->rax = static_cast<uint64_t>(seek_fd(*g_current, context->rdi, static_cast<int64_t>(context->rsi), context->rdx));
            return context;
        case SAVANXP_SYS_UNLINK: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(unlink_path(resolved));
            return context;
        }
        case SAVANXP_SYS_MKDIR: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(mkdir_path(resolved));
            return context;
        }
        case SAVANXP_SYS_RMDIR: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(rmdir_path(resolved));
            return context;
        }
        case SAVANXP_SYS_TRUNCATE: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
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
            if (!copy_user_string(*g_current, context->rdi, old_path, sizeof(old_path)) ||
                !copy_user_string(*g_current, context->rsi, new_path, sizeof(new_path)) ||
                !resolve_process_path(*g_current, old_path, resolved_old, sizeof(resolved_old)) ||
                !resolve_process_path(*g_current, new_path, resolved_new, sizeof(resolved_new))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(rename_path(resolved_old, resolved_new));
            return context;
        }
        case SAVANXP_SYS_IOCTL:
            context->rax = static_cast<uint64_t>(ioctl_handle(*g_current, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_SOCKET:
            context->rax = static_cast<uint64_t>(create_socket_fd(*g_current, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_BIND:
            context->rax = static_cast<uint64_t>(bind_fd(*g_current, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_SENDTO:
            context->rax = static_cast<uint64_t>(sendto_fd(*g_current, context->rdi, context->rsi, static_cast<size_t>(context->rdx), context->r10));
            return context;
        case SAVANXP_SYS_RECVFROM:
            context->rax = static_cast<uint64_t>(recvfrom_fd(
                *g_current,
                context->rdi,
                context->rsi,
                static_cast<size_t>(context->rdx),
                context->r10,
                static_cast<uint32_t>(context->r8)));
            return context;
        case SAVANXP_SYS_CONNECT:
            context->rax = static_cast<uint64_t>(connect_fd(*g_current, context->rdi, context->rsi, static_cast<uint32_t>(context->rdx)));
            return context;
        case SAVANXP_SYS_GETPID:
            context->rax = current_pid();
            return context;
        case SAVANXP_SYS_STAT: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(stat_path(*g_current, resolved, context->rsi));
            return context;
        }
        case SAVANXP_SYS_FSTAT:
            context->rax = static_cast<uint64_t>(fstat_fd(*g_current, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_CHDIR: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(*g_current, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(*g_current, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(chdir_path(*g_current, resolved));
            return context;
        }
        case SAVANXP_SYS_GETCWD:
            context->rax = static_cast<uint64_t>(getcwd_path(*g_current, context->rdi, static_cast<size_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_SYSTEM_INFO: {
            if (!vm::is_user_range_accessible(g_current->address_space, context->rdi, sizeof(savanxp_system_info), true)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            savanxp_system_info info = {};
            if (!snapshot_system_info(info) ||
                !write_to_process_memory(*g_current, context->rdi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SAVANXP_SYS_SYNC:
            context->rax = static_cast<uint64_t>(svfs::sync() ? 0 : negative_error(SAVANXP_EIO));
            return context;
        case SAVANXP_SYS_REALTIME: {
            if (!vm::is_user_range_accessible(g_current->address_space, context->rdi, sizeof(savanxp_realtime), true)) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EINVAL));
                return context;
            }

            savanxp_realtime value = {};
            if (!rtc::read_time(&value) ||
                !write_to_process_memory(*g_current, context->rdi, &value, sizeof(value))) {
                context->rax = static_cast<uint64_t>(negative_error(SAVANXP_EIO));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SAVANXP_SYS_FCNTL:
            context->rax = static_cast<uint64_t>(fcntl_fd(*g_current, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_POLL:
            context->rax = static_cast<uint64_t>(poll_fds(
                *g_current,
                context->rdi,
                static_cast<size_t>(context->rsi),
                static_cast<int32_t>(context->rdx)));
            return context;
        case SAVANXP_SYS_FORK: {
            Process* child = fork_process(*g_current, *context);
            context->rax = static_cast<uint64_t>(child != nullptr ? static_cast<int>(child->pid) : negative_error(SAVANXP_ENOMEM));
            return context;
        }
        case SAVANXP_SYS_KILL:
            context->rax = static_cast<uint64_t>(kill_process(*g_current, static_cast<int>(context->rdi), static_cast<int>(context->rsi)));
            return context;
        case SAVANXP_SYS_EVENT_CREATE:
            context->rax = static_cast<uint64_t>(create_event_handle(*g_current, static_cast<uint32_t>(context->rdi)));
            return context;
        case SAVANXP_SYS_EVENT_SET:
            context->rax = static_cast<uint64_t>(set_event_handle(*g_current, context->rdi));
            return context;
        case SAVANXP_SYS_EVENT_RESET:
            context->rax = static_cast<uint64_t>(reset_event_handle(*g_current, context->rdi));
            return context;
        case SAVANXP_SYS_WAIT_ONE: {
            const int result = wait_on_handle(*g_current, context->rdi, static_cast<int64_t>(context->rsi));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_WAIT_MANY: {
            const int result = wait_on_many_handles(
                *g_current,
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
            context->rax = static_cast<uint64_t>(create_timer_handle(*g_current, static_cast<uint32_t>(context->rdi)));
            return context;
        case SAVANXP_SYS_TIMER_SET:
            context->rax = static_cast<uint64_t>(set_timer_handle(*g_current, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_TIMER_CANCEL:
            context->rax = static_cast<uint64_t>(cancel_timer_handle(*g_current, context->rdi));
            return context;
        case SAVANXP_SYS_SECTION_CREATE:
            context->rax = static_cast<uint64_t>(create_section_handle(*g_current, context->rdi, static_cast<uint32_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_MAP_VIEW:
            context->rax = static_cast<uint64_t>(map_view_handle(*g_current, context->rdi, static_cast<uint32_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_UNMAP_VIEW:
            context->rax = static_cast<uint64_t>(unmap_view_address(*g_current, context->rdi));
            return context;
        default:
            context->rax = static_cast<uint64_t>(negative_error(SAVANXP_ENOSYS));
            return context;
    }
}
