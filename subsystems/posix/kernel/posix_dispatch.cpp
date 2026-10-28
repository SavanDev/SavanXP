#include "kernel/process.hpp"
#include "kernel/block.hpp"

// The POSIX layer speaks the POSIX surface. This is the ONLY place in the
// kernel image allowed to include it: kernel/ and include/kernel/ use the
// neutral contract instead, which is what keeps the two userland lanes
// independent of each other.
#include "savanxp/syscall.h"
#include "posix_process_state.hpp"

// POSIX per-process state lives here; register its size so the kernel
// reserves and frees it without knowing what is inside (isolation slice B).
// The wait hook is the other half (slice C): the kernel hands over any
// process parked with a deadline and POSIX decides whether it was a poll and
// whether it is ready now.
static void register_posix_process_state(void) {
    subsystem::register_process_state_size(subsystem::Id::posix, sizeof(posix::ProcessState));
    subsystem::register_thread_state_size(subsystem::Id::posix, sizeof(posix::ThreadState));
    subsystem::register_wait_recheck(subsystem::Id::posix, &posix::recheck_parked_poll);
}

#include <stddef.h>
#include <stdint.h>

#include "kernel/heap.hpp"
#include "kernel/rtc.hpp"
#include "kernel/smp.hpp"
#include "kernel/sx.hpp"
#include "kernel/fs.hpp"
#include "kernel/sxfs.hpp"
#include "kernel/timer.hpp"
#include "kernel/tty.hpp"
#include "kernel/vmm.hpp"

namespace process {

// POSIX translation dispatcher: runs in its own unit against the published
// process:: boundary (frontera de traduccion POSIX). Same numbers, structs,
// errors and blocking protocol as when it shared process.cpp's translation
// unit; only the call path moved (see docs/kernel/KERNEL_NATIVE_API.md).
//
// The registration is deliberately NOT in a static initializer: it has to
// happen before the first process is created, and static-init order across
// translation units is not something to rely on for a kernel image.
void initialize_posix_subsystem() {
    register_posix_process_state();
}

SavedContext* dispatch_posix_syscall(SavedContext* context) {
    if (smp::this_cpu().current == nullptr) {
        return context;
    }

    // The calling thread blocks, sleeps and resumes; the process it belongs
    // to owns the handles, the address space and the POSIX state.
    Thread& self = *smp::this_cpu().current;
    Process& caller = *self.process;
    self.context = context;
    self.state = State::running;

    switch (context->rax) {
        case SAVANXP_SYS_READ: {
            const int result = read_handle(self, context->rdi, context->rsi, static_cast<size_t>(context->rdx));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_WRITE: {
            const int result = write_handle(self, context->rdi, context->rsi, static_cast<size_t>(context->rdx));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_OPEN: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(caller, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(caller, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(open_node(caller, resolved, static_cast<uint32_t>(context->rsi)));
            return context;
        }
        case SAVANXP_SYS_CLOSE:
            context->rax = static_cast<uint64_t>(close_fd(caller, context->rdi));
            return context;
        case SAVANXP_SYS_READDIR:
            context->rax = static_cast<uint64_t>(readdir_node(caller, context->rdi, context->rsi, static_cast<size_t>(context->rdx)));
            return context;
        case SAVANXP_SYS_SPAWN:
        case SAVANXP_SYS_SPAWN_FD: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            auto* arguments = static_cast<SpawnArguments*>(heap::allocate(sizeof(SpawnArguments)));
            if (arguments == nullptr) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_ENOMEM));
                return context;
            }
            if (!copy_spawn_arguments(caller, *context, path, sizeof(path), *arguments) ||
                !resolve_process_path(caller, path, resolved, sizeof(resolved))) {
                heap::free(arguments);
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_E2BIG));
                return context;
            }

            int stdin_fd = -1;
            int stdout_fd = -1;
            if (context->rax == SAVANXP_SYS_SPAWN_FD) {
                stdin_fd = static_cast<int>(context->r10);
                stdout_fd = static_cast<int>(context->r8);
            }

            ImageFailure failure = ImageFailure::none;
            Process* child = create_process_internal(resolved, arguments->argc, arguments->argv, &caller, stdin_fd, stdout_fd, -1, failure);
            heap::free(arguments);
            context->rax = static_cast<uint64_t>(child != nullptr
                ? static_cast<int>(child->pid)
                : sx::negative_error(image_failure_errno(failure)));
            return context;
        }
        case SAVANXP_SYS_EXEC: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            auto* arguments = static_cast<SpawnArguments*>(heap::allocate(sizeof(SpawnArguments)));
            if (arguments == nullptr) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_ENOMEM));
                return context;
            }
            if (!copy_spawn_arguments(caller, *context, path, sizeof(path), *arguments) ||
                !resolve_process_path(caller, path, resolved, sizeof(resolved))) {
                heap::free(arguments);
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_E2BIG));
                return context;
            }

            ImageFailure failure = ImageFailure::none;
            const bool prepared = prepare_exec_image(self, resolved, arguments->argc, arguments->argv, failure);
            heap::free(arguments);
            if (!prepared && failure == ImageFailure::threads_running) {
                // Other threads are still stopping on other cores. Run the
                // same int $0x80 again once this core has turned around:
                // nothing was changed yet, and the arguments are still in
                // the registers.
                context->rip -= 2;
                self.state = State::ready;
                return choose_next_context(context);
            }
            if (!prepared) {
                context->rax = static_cast<uint64_t>(sx::negative_error(image_failure_errno(failure)));
                return context;
            }

            switch_to_thread(&self);
            return self.context;
        }
        case SAVANXP_SYS_WAITPID: {
            const int result = waitpid_child(self, static_cast<int64_t>(context->rdi), context->rsi);
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_EXIT:
            terminate_current(static_cast<int>(context->rdi));
            return nullptr;
        case SAVANXP_SYS_YIELD:
            // The idle process is a yield loop in user mode, which cannot halt
            // because hlt is privileged. When IT yields and nothing else is
            // ready for this core, the core has nothing to do: it halts here, on
            // the idle's behalf, until the next interrupt. Without this, idle
            // time is spent spinning through the syscall at full speed.
            //
            // The kernel lock is released while it sleeps, or the other cores
            // would wait for it. Its own tick wakes it, and that tick is what
            // checks whether something showed up in the queue for this core.
            //
            // Before halting, an idle core moves one bounded piece of the disk
            // writeback queue: that is where deferred writes reach the disk,
            // in time nobody else wanted. One piece per yield, so a process
            // that becomes runnable gets the core back within one step.
            if (caller.idle && !has_runnable_non_idle() &&
                !block::writeback_step()) {
                smp::wait_for_interrupt();
            }
            self.state = State::ready;
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
            const int result = sleep_via_timer(self, context->rdi);
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_PIPE:
            context->rax = static_cast<uint64_t>(create_pipe(caller, context->rdi));
            return context;
        case SAVANXP_SYS_DUP:
            context->rax = static_cast<uint64_t>(duplicate_fd(caller, context->rdi));
            return context;
        case SAVANXP_SYS_DUP2:
            context->rax = static_cast<uint64_t>(duplicate_fd_to(caller, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_PROC_INFO: {
            if (!vm::is_user_range_accessible(caller.address_space, context->rsi, sizeof(savanxp_process_info), true)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }

            savanxp_process_info info = {};
            if (!snapshot_process(static_cast<size_t>(context->rdi), info)) {
                context->rax = 0;
                return context;
            }
            if (!write_to_process_memory(caller, context->rsi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = 1;
            return context;
        }
        case SAVANXP_SYS_SEEK:
            context->rax = static_cast<uint64_t>(seek_fd(caller, context->rdi, static_cast<int64_t>(context->rsi), context->rdx));
            return context;
        case SAVANXP_SYS_UNLINK: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(caller, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(caller, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(unlink_path(resolved));
            return context;
        }
        case SAVANXP_SYS_MKDIR: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(caller, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(caller, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(mkdir_path(resolved));
            return context;
        }
        case SAVANXP_SYS_RMDIR: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(caller, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(caller, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(rmdir_path(resolved));
            return context;
        }
        case SAVANXP_SYS_TRUNCATE: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(caller, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(caller, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
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
            if (!copy_user_string(caller, context->rdi, old_path, sizeof(old_path)) ||
                !copy_user_string(caller, context->rsi, new_path, sizeof(new_path)) ||
                !resolve_process_path(caller, old_path, resolved_old, sizeof(resolved_old)) ||
                !resolve_process_path(caller, new_path, resolved_new, sizeof(resolved_new))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(rename_path(resolved_old, resolved_new));
            return context;
        }
        case SAVANXP_SYS_IOCTL:
            context->rax = static_cast<uint64_t>(ioctl_handle(caller, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_SOCKET:
            context->rax = static_cast<uint64_t>(create_socket_fd(caller, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_BIND:
            context->rax = static_cast<uint64_t>(bind_fd(caller, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_SENDTO:
            context->rax = static_cast<uint64_t>(sendto_fd(caller, context->rdi, context->rsi, static_cast<size_t>(context->rdx), context->r10));
            return context;
        case SAVANXP_SYS_RECVFROM:
            context->rax = static_cast<uint64_t>(recvfrom_fd(
                caller,
                context->rdi,
                context->rsi,
                static_cast<size_t>(context->rdx),
                context->r10,
                static_cast<uint32_t>(context->r8)));
            return context;
        case SAVANXP_SYS_SETSOCKOPT:
            context->rax = static_cast<uint64_t>(setsockopt_fd(caller, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_CONNECT:
            context->rax = static_cast<uint64_t>(connect_fd(caller, context->rdi, context->rsi, static_cast<uint32_t>(context->rdx)));
            return context;
        case SAVANXP_SYS_GETPID:
            context->rax = current_pid();
            return context;
        case SAVANXP_SYS_STAT: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(caller, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(caller, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(stat_path(caller, resolved, context->rsi));
            return context;
        }
        case SAVANXP_SYS_FSTAT:
            context->rax = static_cast<uint64_t>(fstat_fd(caller, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_CHDIR: {
            char path[process::kProcessPathLength] = {};
            char resolved[process::kProcessPathLength] = {};
            if (!copy_user_string(caller, context->rdi, path, sizeof(path)) ||
                !resolve_process_path(caller, path, resolved, sizeof(resolved))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }
            context->rax = static_cast<uint64_t>(chdir_path(caller, resolved));
            return context;
        }
        case SAVANXP_SYS_GETCWD:
            context->rax = static_cast<uint64_t>(getcwd_path(caller, context->rdi, static_cast<size_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_SYSTEM_INFO: {
            if (!vm::is_user_range_accessible(caller.address_space, context->rdi, sizeof(savanxp_system_info), true)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }

            savanxp_system_info info = {};
            if (!snapshot_system_info(info) ||
                !write_to_process_memory(caller, context->rdi, &info, sizeof(info))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SAVANXP_SYS_SYNC:
            // fs::sync_all marks every volume clean; block::sync_all then
            // waits for the writeback queue, so everything written is on the
            // medium.
            context->rax = static_cast<uint64_t>(
                fs::sync_all() && block::sync_all() ? 0 : sx::negative_error(SAVANXP_EIO));
            return context;
        case SAVANXP_SYS_FSCHECK: {
            if (!vm::is_user_range_accessible(caller.address_space, context->rdi, sizeof(savanxp_fscheck_report), true)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }

            /* El informe se arma en el struct del kernel y recien ahi se copia al
             * proceso: el driver no debe conocer los tipos del SDK. */
            sxfs::CheckReport report = {};
            if (!sxfs::check(sxfs::root(), report)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EIO));
                return context;
            }

            savanxp_fscheck_report out = {};
            out.total_sectors = report.total_sectors;
            out.data_lba = report.data_lba;
            out.sequence = report.sequence;
            out.journal_pending = report.journal_pending;
            out.clean_shutdown = report.clean_shutdown;
            out.journal_valid = report.journal_valid;
            out.ownership_recorded = report.ownership_recorded;
            out.unowned_inodes = report.unowned_inodes;
            out.inodes_allocated = report.inodes_allocated;
            out.files = report.files;
            out.directories = report.directories;
            out.data_sectors = report.data_sectors;
            out.data_used_sectors = report.data_used_sectors;
            out.data_claimed_sectors = report.data_claimed_sectors;
            out.leaked_blocks = report.leaked_blocks;
            out.lost_blocks = report.lost_blocks;
            out.double_claimed = report.double_claimed_blocks;
            out.metadata_unmarked = report.metadata_unmarked;
            out.orphan_inodes = report.orphan_inodes;
            out.alias_inodes = report.alias_inodes;
            out.duplicate_names = report.duplicate_names;
            out.bad_dir_entries = report.bad_dir_entries;
            out.unreadable_dirs = report.unreadable_dirs;
            out.metadata_commits = report.metadata_commits;
            out.metadata_bytes_written = report.metadata_bytes_written;

            if (!write_to_process_memory(caller, context->rdi, &out, sizeof(out))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SAVANXP_SYS_REALTIME: {
            if (!vm::is_user_range_accessible(caller.address_space, context->rdi, sizeof(savanxp_realtime), true)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }

            savanxp_realtime value = {};
            if (!rtc::read_time(&value) ||
                !write_to_process_memory(caller, context->rdi, &value, sizeof(value))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EIO));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SAVANXP_SYS_SET_REALTIME: {
            savanxp_realtime value = {};

            if (!vm::is_user_range_accessible(caller.address_space, context->rdi, sizeof(value), false)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }
            if (!read_from_process_memory(caller, &value, context->rdi, sizeof(value))) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }
            // Fecha mala y chip atascado son fallos distintos: el llamador
            // tiene que saber si corrija el dato o reintente.
            if (!rtc::valid_time(value)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EINVAL));
                return context;
            }
            if (!rtc::write_time(value)) {
                context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_EIO));
                return context;
            }

            context->rax = 0;
            return context;
        }
        case SAVANXP_SYS_FCNTL:
            context->rax = static_cast<uint64_t>(fcntl_fd(caller, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_POLL: {
            const int result = poll_fds(
                self,
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
            Process* child = fork_process(self, *context);
            context->rax = static_cast<uint64_t>(child != nullptr ? static_cast<int>(child->pid) : sx::negative_error(SAVANXP_ENOMEM));
            return context;
        }
        case SAVANXP_SYS_KILL:
            context->rax = static_cast<uint64_t>(kill_process(caller, static_cast<int>(context->rdi), static_cast<int>(context->rsi)));
            return context;
        case SAVANXP_SYS_EVENT_CREATE:
            context->rax = static_cast<uint64_t>(create_event_handle(caller, static_cast<uint32_t>(context->rdi)));
            return context;
        case SAVANXP_SYS_EVENT_SET:
            context->rax = static_cast<uint64_t>(set_event_handle(caller, context->rdi));
            return context;
        case SAVANXP_SYS_EVENT_RESET:
            context->rax = static_cast<uint64_t>(reset_event_handle(caller, context->rdi));
            return context;
        case SAVANXP_SYS_WAIT_ONE: {
            const int result = wait_on_handle(self, context->rdi, static_cast<int64_t>(context->rsi));
            if (result == kBlockedResult) {
                return choose_next_context(context);
            }
            context->rax = static_cast<uint64_t>(result);
            return context;
        }
        case SAVANXP_SYS_WAIT_MANY: {
            const int result = wait_on_many_handles(
                self,
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
            context->rax = static_cast<uint64_t>(create_timer_handle(caller, static_cast<uint32_t>(context->rdi)));
            return context;
        case SAVANXP_SYS_TIMER_SET:
            context->rax = static_cast<uint64_t>(set_timer_handle(caller, context->rdi, context->rsi, context->rdx));
            return context;
        case SAVANXP_SYS_TIMER_CANCEL:
            context->rax = static_cast<uint64_t>(cancel_timer_handle(caller, context->rdi));
            return context;
        case SAVANXP_SYS_SECTION_CREATE:
            context->rax = static_cast<uint64_t>(create_section_handle(caller, context->rdi, static_cast<uint32_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_SECTION_OPEN:
            context->rax = static_cast<uint64_t>(open_section_handle(caller, context->rdi, static_cast<uint32_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_SECTION_OPEN_RANGE:
            context->rax = static_cast<uint64_t>(open_range_section_handle(caller, context->rdi, context->rsi, context->rdx, static_cast<uint32_t>(context->r10)));
            return context;
        case SAVANXP_SYS_MAP_VIEW:
            context->rax = static_cast<uint64_t>(map_view_handle(caller, context->rdi, static_cast<uint32_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_MAP_VIEW_AT:
            context->rax = static_cast<uint64_t>(map_view_base_handle(caller, context->rdi, context->rsi, static_cast<uint32_t>(context->rdx)));
            return context;
        case SAVANXP_SYS_UNMAP_VIEW:
            context->rax = static_cast<uint64_t>(unmap_view_address(caller, context->rdi));
            return context;
        case SAVANXP_SYS_PIPE_SEND_HANDLE:
            context->rax = static_cast<uint64_t>(pipe_send_handle(caller, context->rdi, context->rsi));
            return context;
        case SAVANXP_SYS_PIPE_RECEIVE_HANDLE:
            context->rax = static_cast<uint64_t>(pipe_receive_handle(caller, context->rdi));
            return context;
        case SAVANXP_SYS_SEMAPHORE_CREATE:
            context->rax = static_cast<uint64_t>(create_semaphore_handle(
                caller,
                static_cast<int32_t>(context->rdi),
                static_cast<int32_t>(context->rsi)));
            return context;
        case SAVANXP_SYS_SEMAPHORE_RELEASE:
            context->rax = static_cast<uint64_t>(release_semaphore_handle(
                caller,
                context->rdi,
                static_cast<uint32_t>(context->rsi)));
            return context;
        default:
            // The native object calls are the floor POSIX stands on: a POSIX
            // process reaches them as it is, e.g. to connect to a service's
            // port (docs/desktop/DESKTOP_ARCHITECTURE.md, phase 3).
            if (native_call_open_to_all_subsystems(context->rax)) {
                return dispatch_native_syscall(context);
            }
            context->rax = static_cast<uint64_t>(sx::negative_error(SAVANXP_ENOSYS));
            return context;
    }
}

} // namespace process
