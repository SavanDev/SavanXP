#include "syscall.hpp"
#include "waits.hpp"
#include "services.hpp"
#include "kernel/rtc.hpp"
#include "kernel/cpu.hpp"
#include "kernel/device.hpp"
#include "kernel/net.hpp"
#include "kernel/pci.hpp"
#include "kernel/pcspeaker.hpp"
#include "kernel/timer.hpp"

#include <stdint.h>

#include "boot_state.hpp"
#include "cpu.hpp"
#include "fb32.hpp"
#include "kernel/sxfs.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "pit.hpp"
#include "ps2_32.hpp"
#include "process.hpp"
#include "smp.hpp"
#include "savanxp/syscall.h"
#include "serial.hpp"
#include "user32.hpp"
#include "io.hpp"
#include "sections.hpp"
#include "paging.hpp"
#include "kernel/vfs.hpp"

namespace i386 {

namespace syscall {

namespace {

bool is_user_range(uint32_t address, uint32_t size, bool writable = false) {
    return process::user_range(address, size, writable);
}

int copy_user_string(uint32_t address, char* output, uint32_t capacity) {
    for (uint32_t index = 0; index < capacity; ++index) {
        if (!is_user_range(address + index, 1)) {
            return SAVANXP_EFAULT;
        }
        output[index] = *reinterpret_cast<const char*>(address + index);
        if (output[index] == '\0') {
            return 0;
        }
    }
    return SAVANXP_E2BIG;
}

uint32_t failure(int error) {
    return static_cast<uint32_t>(-static_cast<int32_t>(error));
}

void copy_text(char* destination, size_t capacity, const char* source) {
    size_t index = 0;
    while (index + 1 < capacity && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

}

void handle(ContextFrame& frame) {
    // SDK arguments occupy EDI, ESI, EDX, ECX and EBX on i386.
    if (native_syscall(frame)) return;
    if (waits::syscall(frame)) return;
    switch (frame.eax) {
        case SAVANXP_SYS_SOCKET:
            frame.eax = io::socket(*process::current(), frame.edi, frame.esi, frame.edx); return;
        case SAVANXP_SYS_BIND:
        case SAVANXP_SYS_CONNECT:
        case SAVANXP_SYS_SETSOCKOPT:
        case SAVANXP_SYS_SENDTO:
        case SAVANXP_SYS_RECVFROM:
            frame.eax = io::socket_call(*process::current(), frame.eax, frame.edi,
                frame.esi, frame.edx, frame.ecx, frame.ebx); return;
        case SAVANXP_SYS_FORK:
            frame.eax = process::fork(frame); return;
        case SAVANXP_SYS_PIPE_SEND_HANDLE:
            frame.eax = io::send_handle(*process::current(), frame.edi, frame.esi); return;
        case SAVANXP_SYS_PIPE_RECEIVE_HANDLE:
            frame.eax = io::receive_handle(*process::current(), frame.edi); return;
        case SAVANXP_SYS_SECTION_CREATE:
            frame.eax = sections::create(*process::current(), frame.edi, frame.esi); return;
        case SAVANXP_SYS_SECTION_OPEN:
            frame.eax = sections::open(*process::current(), frame.edi, 0, 0, frame.esi); return;
        case SAVANXP_SYS_SECTION_OPEN_RANGE:
            frame.eax = sections::open(*process::current(), frame.edi, frame.esi, frame.edx, frame.ecx); return;
        case SAVANXP_SYS_MAP_VIEW:
            frame.eax = sections::map(*process::current(), frame.edi, 0, frame.esi); return;
        case SAVANXP_SYS_MAP_VIEW_AT:
            frame.eax = sections::map(*process::current(), frame.edi, frame.esi, frame.edx); return;
        case SAVANXP_SYS_UNMAP_VIEW:
            frame.eax = sections::unmap(*process::current(), frame.edi); return;
        case kSysRead:
            frame.eax = io::read(*process::current(), frame.edi, frame.esi, frame.edx);
            return;
        case kSysWrite:
            frame.eax = io::write(*process::current(), frame.edi, frame.esi, frame.edx);
            return;
        case kSysSpawn:
        case SAVANXP_SYS_SPAWN_FD:
        case SAVANXP_SYS_EXEC: {
            char path[256];
            int error = copy_user_string(frame.edi, path, sizeof(path));
            if (error != 0) {
                frame.eax = failure(error);
                return;
            }
            if (frame.edx > SAVANXP_MAX_ARGC) {
                frame.eax = failure(SAVANXP_E2BIG);
                return;
            }
            if (frame.edx != 0 && !is_user_range(frame.esi, frame.edx * sizeof(uint32_t))) {
                frame.eax = failure(SAVANXP_EFAULT);
                return;
            }
            char strings[SAVANXP_MAX_ARGC][SAVANXP_MAX_ARG_LENGTH + 1];
            const char* arguments[SAVANXP_MAX_ARGC];
            for (uint32_t index = 0; index < frame.edx; ++index) {
                uint32_t address;
                memcpy(&address, reinterpret_cast<const void*>(frame.esi + index * 4), 4);
                error = copy_user_string(address, strings[index], sizeof(strings[index]));
                if (error != 0) {
                    frame.eax = failure(error);
                    return;
                }
                arguments[index] = strings[index];
            }
            char resolved[256];
            if (!vfs::normalize_path(process::current()->cwd, path, resolved, sizeof(resolved))) {
                frame.eax = failure(SAVANXP_EINVAL); return;
            }
            if (frame.eax == SAVANXP_SYS_EXEC) {
                const int result = process::replace_image(resolved, arguments, frame.edx, frame);
                if (result) frame.eax = failure(result);
            } else if (frame.eax == SAVANXP_SYS_SPAWN_FD) {
                // The five-register ABI maps arguments 4/5 to ECX/EBX.
                frame.eax = process::spawn(resolved, arguments, frame.edx,
                                          static_cast<int>(frame.ecx), static_cast<int>(frame.ebx));
            } else {
                frame.eax = static_cast<uint32_t>(process::spawn(resolved, arguments, frame.edx));
            }
            return;
        }
        case kSysWaitPid: {
            if (frame.esi != 0 && !is_user_range(frame.esi, sizeof(int), true)) {
                frame.eax = failure(SAVANXP_EFAULT);
                return;
            }
            frame.eax = static_cast<uint32_t>(process::wait(static_cast<int>(frame.edi), frame.esi));
            return;
        }
        case kSysYield: {
            process::yield();
            frame.eax = 0;
            return;
        }
        case kSysOpen: {
            char path[256], resolved[256];
            const int error = copy_user_string(frame.edi, path, sizeof(path));
            if (error) { frame.eax = failure(error); return; }
            if (!vfs::normalize_path(process::current()->cwd, path, resolved, sizeof(resolved))) {
                frame.eax = failure(SAVANXP_EINVAL); return;
            }
            frame.eax = io::open(*process::current(), resolved, frame.esi);
            return;
        }
        case kSysClose:
            frame.eax = io::close(*process::current(), frame.edi); return;
        case SAVANXP_SYS_KILL:
            frame.eax = process::kill(frame.edi, static_cast<int>(frame.esi)); return;
        case SAVANXP_SYS_DUP:
            frame.eax = io::duplicate(*process::current(), frame.edi); return;
        case SAVANXP_SYS_DUP2:
            if (frame.esi >= io::kMaxHandles) { frame.eax = failure(SAVANXP_EBADF); return; }
            frame.eax = io::duplicate(*process::current(), frame.edi, static_cast<int>(frame.esi)); return;
        case SAVANXP_SYS_PIPE:
            frame.eax = io::pipe(*process::current(), frame.edi); return;
        case SAVANXP_SYS_READDIR:
            frame.eax = io::readdir(*process::current(), frame.edi, frame.esi, frame.edx); return;
        case SAVANXP_SYS_SEEK:
            frame.eax = io::seek(*process::current(), frame.edi, static_cast<int32_t>(frame.esi), frame.edx); return;
        case SAVANXP_SYS_FCNTL:
            frame.eax = io::fcntl(*process::current(), frame.edi, frame.esi, frame.edx); return;
        case SAVANXP_SYS_POLL:
            frame.eax = io::poll(*process::current(), frame.edi, frame.esi, static_cast<int32_t>(frame.edx)); return;
        case SAVANXP_SYS_FSTAT:
            frame.eax = io::fstat(*process::current(), frame.edi, frame.esi); return;
        case SAVANXP_SYS_CLEAR:
            io::clear_console(); frame.eax = 0; return;
        case SAVANXP_SYS_SYNC:
            frame.eax = sxfs::sync(sxfs::root()) ? 0 : failure(SAVANXP_EIO); return;
        case SAVANXP_SYS_GETCWD: {
            const auto* task = process::current();
            const uint32_t size = strlen(task->cwd) + 1;
            if (frame.esi < size) { frame.eax = failure(SAVANXP_EINVAL); return; }
            if (!is_user_range(frame.edi, frame.esi, true)) { frame.eax = failure(SAVANXP_EFAULT); return; }
            memcpy(reinterpret_cast<void*>(frame.edi), task->cwd, size);
            frame.eax = size - 1; return;
        }
        case SAVANXP_SYS_STAT:
        case SAVANXP_SYS_CHDIR:
        case SAVANXP_SYS_UNLINK:
        case SAVANXP_SYS_MKDIR:
        case SAVANXP_SYS_RMDIR:
        case SAVANXP_SYS_TRUNCATE:
        case SAVANXP_SYS_RENAME: {
            char path[256], resolved[256];
            const int error = copy_user_string(frame.edi, path, sizeof(path));
            if (error) { frame.eax = failure(error); return; }
            if (!vfs::normalize_path(process::current()->cwd, path, resolved, sizeof(resolved))) {
                frame.eax = failure(SAVANXP_EINVAL); return;
            }
            if (frame.eax == SAVANXP_SYS_STAT) frame.eax = io::stat(*process::current(), resolved, frame.esi);
            else if (frame.eax == SAVANXP_SYS_CHDIR) {
                const auto* node = vfs::resolve(resolved);
                if (!node) frame.eax = failure(SAVANXP_ENOENT);
                else if (node->type != vfs::NodeType::directory) frame.eax = failure(SAVANXP_ENOTDIR);
                else { strcpy(process::current()->cwd, resolved); frame.eax = 0; }
            } else if (frame.eax == SAVANXP_SYS_RENAME) {
                char new_path[256], new_resolved[256];
                const int result = copy_user_string(frame.esi, new_path, sizeof(new_path));
                if (result) { frame.eax = failure(result); return; }
                if (!vfs::normalize_path(process::current()->cwd, new_path, new_resolved, sizeof(new_resolved))) {
                    frame.eax = failure(SAVANXP_EINVAL); return;
                }
                frame.eax = io::mutate(resolved, frame.eax, 0, new_resolved);
            } else frame.eax = io::mutate(resolved, frame.eax, frame.esi);
            return;
        }
        case SAVANXP_SYS_REALTIME: {
            if (!is_user_range(frame.edi, sizeof(savanxp_realtime), true)) { frame.eax = failure(SAVANXP_EFAULT); return; }
            savanxp_realtime value = {};
            const bool valid = rtc::read_time(&value);
            memcpy(reinterpret_cast<void*>(frame.edi), &value, sizeof(value));
            frame.eax = valid ? 0 : failure(SAVANXP_EIO); return;
        }
        case SAVANXP_SYS_FSCHECK: {
            if (!is_user_range(frame.edi, sizeof(savanxp_fscheck_report), true)) {
                frame.eax = failure(SAVANXP_EFAULT); return;
            }
            sxfs::CheckReport report = {};
            if (!sxfs::check(sxfs::root(), report)) { frame.eax = failure(SAVANXP_EIO); return; }
            savanxp_fscheck_report out = {};
            out.total_sectors = report.total_sectors;
            out.data_lba = report.data_lba;
            out.sequence = report.sequence;
            out.journal_pending = report.journal_pending;
            out.clean_shutdown = report.clean_shutdown;
            out.journal_valid = report.journal_valid;
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
            memcpy(reinterpret_cast<void*>(frame.edi), &out, sizeof(out));
            frame.eax = 0; return;
        }
        case SAVANXP_SYS_PROC_INFO: {
            if (!is_user_range(frame.esi, sizeof(savanxp_process_info), true)) { frame.eax = failure(SAVANXP_EFAULT); return; }
            uint32_t index = 0;
            for (uint32_t slot = 0; slot < process::kCapacity; ++slot) {
                const auto& task = *process::at(slot);
                if (task.state == process::State::unused) continue;
                if (index++ != frame.edi) continue;
                savanxp_process_info info = {};
                info.cpu_ticks = task.cpu_ticks;
                info.pid = task.pid; info.parent_pid = task.parent; info.exit_code = task.exit_status;
                const uint32_t states[] = {SAVANXP_PROC_UNUSED, SAVANXP_PROC_READY, SAVANXP_PROC_RUNNING,
                    SAVANXP_PROC_SLEEPING, SAVANXP_PROC_BLOCKED_READ, SAVANXP_PROC_BLOCKED_WRITE,
                    SAVANXP_PROC_BLOCKED_WAIT, SAVANXP_PROC_BLOCKED_WAIT, SAVANXP_PROC_ZOMBIE,
                    SAVANXP_PROC_BLOCKED_WAIT};
                info.state = states[static_cast<uint32_t>(task.state)];
                copy_text(info.name, sizeof(info.name), task.name);
                info.memory_bytes = paging::resident_bytes(task.directory);
                info.handle_count = io::handle_count(task);
                memcpy(reinterpret_cast<void*>(frame.esi), &info, sizeof(info));
                frame.eax = 1; return;
            }
            frame.eax = 0; return;
        }
        case kSysIoctl: {
            if (io::input_descriptor(*process::current(), frame.edi)) {
                if (frame.esi == INPUT_IOC_GET_LAYOUT) frame.eax = ps2_32::get_layout();
                else if (frame.esi == INPUT_IOC_SET_LAYOUT) {
                    if (!io::input_descriptor(*process::current(), frame.edi, true)) frame.eax = failure(SAVANXP_EACCES);
                    else frame.eax = ps2_32::set_layout(static_cast<int>(frame.edx)) ? 0 : failure(SAVANXP_EINVAL);
                }
                else frame.eax = failure(SAVANXP_ENOTTY);
                return;
            }
            if (frame.edi >= io::kMaxHandles || !object::as_io(process::current()->handles[frame.edi].object)) {
                frame.eax = failure(SAVANXP_EBADF);
                return;
            }
            auto& entry = process::current()->handles[frame.edi];
            auto* file = object::as_io(entry.object);
            frame.eax = device::ioctl(file->device, frame.esi, frame.edx, entry.granted_access);
            return;
        }
        case SAVANXP_SYS_MONOTONIC_NS: {
            const uint64_t ns = pit::uptime_ms() * 1000000ull;
            frame.eax = static_cast<uint32_t>(ns);
            frame.edx = static_cast<uint32_t>(ns >> 32);
            return;
        }
        case kSysUptimeMs: {
            /* El SDK devuelve unsigned long: en i386 caben los primeros 49
             * dias; el wrap completo se documenta antes que esconderse. */
            frame.eax = static_cast<uint32_t>(pit::uptime_ms());
            return;
        }
        case kSysSleepMs: {
            // Record a deadline; the timer wakes this process without keeping
            // a suspended kernel continuation on its stack.
            process::sleep(frame.edi);
            frame.eax = 0;
            return;
        }
        case kSysGetPid: {
            frame.eax = process::current_pid();
            return;
        }
        case kSysSystemInfo: {
            // Combine the recorded boot handoff with live service state.
            struct savanxp_system_info info = {};
            const boot_state::State& boot = boot_state::state();
            const sxfs::VolumeId volume = sxfs::root();

            copy_text(info.bootloader_name, sizeof(info.bootloader_name), boot.bootloader_name);
            copy_text(info.bootloader_version, sizeof(info.bootloader_version), boot.bootloader_version);
            copy_text(info.firmware, sizeof(info.firmware), boot.firmware);

            info.input_ready = 1;
            info.net_present = net::present();
            info.speaker_ready = pcspeaker::ready();
            info.pci_device_count = pci::device_count();
            info.cpu_ticks_total = pit::ticks();
            info.cpu_khz = timer::tsc_khz();
            arch::x86_64::CpuIdentity identity = {};
            arch::x86_64::query_cpu_identity(identity);
            static_assert(arch::x86_64::kCpuFeatureSse2 == SAVANXP_CPU_FEATURE_SSE2 &&
                arch::x86_64::kCpuFeaturePae == SAVANXP_CPU_FEATURE_PAE &&
                arch::x86_64::kCpuFeatureNx == SAVANXP_CPU_FEATURE_NX &&
                arch::x86_64::kCpuFeatureLongMode == SAVANXP_CPU_FEATURE_LONG_MODE &&
                arch::x86_64::kCpuFeatureX2Apic == SAVANXP_CPU_FEATURE_X2APIC &&
                arch::x86_64::kCpuFeatureHypervisor == SAVANXP_CPU_FEATURE_HYPERVISOR);
            info.cpu_features = identity.features;
            memcpy(info.cpu_vendor, identity.vendor, sizeof(info.cpu_vendor));
            memcpy(info.cpu_brand, identity.brand, sizeof(info.cpu_brand));
            for (size_t index = 0; index < pci::device_count(); ++index) {
                pci::DeviceInfo device = {};
                if (pci::device_info(index, device) && device.class_code == 2) {
                    info.net_hardware = 1;
                    info.net_hardware_vendor = device.vendor_id;
                    info.net_hardware_device = device.device_id;
                    break;
                }
            }
            info.framebuffer_ready = boot.framebuffer_ready;
            info.framebuffer_width = boot.framebuffer_width;
            info.framebuffer_height = boot.framebuffer_height;
            info.framebuffer_bpp = boot.framebuffer_bpp;
            info.block_ready = boot.block_ready;
            info.sxfs_mounted = boot.sxfs_mounted;
            info.initramfs_size = boot.initramfs_size;
            info.memory_usable_bytes = boot.memory_usable_bytes;
            info.memory_reclaimable_bytes = boot.memory_reclaimable_bytes;
            info.memory_total_pages = memory::total_page_count();
            info.memory_free_bytes = memory::free_page_count() * memory::kPageSize;
            info.timer_backend = SAVANXP_TIMER_PIT;
            info.timer_frequency_hz = pit::kFrequencyHz;
            info.uptime_ms = pit::uptime_ms();
            info.sxfs_file_count = static_cast<uint32_t>(sxfs::file_count(volume));
            info.sxfs_total_bytes = sxfs::total_bytes(volume);
            info.sxfs_used_bytes = sxfs::used_bytes(volume);
            info.sxfs_free_bytes = sxfs::free_bytes(volume);
            info.process_count = process::count();
            object::section_live_counts(info.sections_live, info.file_sections_live);
            info.last_view_failure = sections::last_failure();
            info.cpu_count = smp::cpu_count();
            info.cpu_online = smp::online_count();

            if (!is_user_range(frame.edi, static_cast<uint32_t>(sizeof(info)), true)) {
                frame.eax = failure(SAVANXP_EFAULT);
                return;
            }
            memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(frame.edi)), &info, sizeof(info));
            frame.eax = 0;
            return;
        }
        case kSysExit: {
            serial::write("i386: process exited with status ");
            serial::write_dec(frame.edi);
            /* El uptime en la misma linea: es la prueba mas directa de que el
             * PIT corre (sysinfo sale con IF=1 desde ring 3, asi que ya hay
             * ticks; el token del test no lo exige, solo lo deja a la vista). */
            serial::write(", uptime ");
            serial::write_dec(static_cast<uint32_t>(pit::uptime_ms()));
            serial::write(" ms\n");
            process::exit(static_cast<int>(frame.edi));
            return;
        }

        default: {
            serial::write("i386: syscall ");
            serial::write_dec(frame.eax);
            serial::write(" sin implementar\n");
            frame.eax = failure(SAVANXP_ENOSYS);
            return;
        }
    }
}

}  // namespace syscall

// The assembly passes the actual saved frame and restores the returned one.
extern "C" ContextFrame* i386_handle_syscall(ContextFrame* frame) {
    syscall::handle(*frame);
    return process::finish_syscall(frame);
}

} // namespace i386
