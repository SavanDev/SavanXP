#include "syscall.hpp"

#include <stdint.h>

#include "boot_state.hpp"
#include "cpu.hpp"
#include "kernel/sxfs.hpp"
#include "kernel/string.hpp"
#include "savanxp/syscall.h"
#include "serial.hpp"

namespace i386 {

namespace syscall {

namespace {

/* Ventana de usuario: los mismos cuatro PDEs de 4 MiB que paging.cpp marca
 * con el bit de usuario. Todo puntero que venga de un proceso tiene que caer
 * aqui antes de tocarlo. */
constexpr uint32_t kUserWindowBase = 0x08000000u;
constexpr uint32_t kUserWindowEnd = 0x09000000u;

bool is_user_range(uint32_t address, uint32_t size) {
    if (address < kUserWindowBase || address >= kUserWindowEnd) {
        return false;
    }
    return static_cast<uint64_t>(address) + size <= kUserWindowEnd;
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

void handle(SyscallFrame& frame) {
    /* Los argumentos se leen de edi/esi/edx y no de ebx/ecx/edx: las
     * restricciones del SDK ("D", "S", "d") significan lo mismo en los dos
     * ABI, y el usuario pone el numero en eax. */
    switch (frame.eax) {
        case kSysWrite: {
            if (frame.edi != kStdoutFd && frame.edi != kStderrFd) {
                frame.eax = failure(SAVANXP_EBADF);
                return;
            }
            if (!is_user_range(frame.esi, frame.edx)) {
                frame.eax = failure(SAVANXP_EFAULT);
                return;
            }
            const char* text = reinterpret_cast<const char*>(static_cast<uintptr_t>(frame.esi));
            for (uint32_t index = 0; index < frame.edx; ++index) {
                serial::write_char(text[index]);
            }
            frame.eax = frame.edx;
            return;
        }
        case kSysGetPid: {
            frame.eax = kInitPid;
            return;
        }
        case kSysSystemInfo: {
            /* Todo lo que el arranque annoto mas el estado real del volumen; lo
             * que aun no existe (reloj, PCI, red) se queda en cero y el
             * programa lo ve como tal. */
            struct savanxp_system_info info = {};
            const boot_state::State& boot = boot_state::state();
            const sxfs::VolumeId volume = sxfs::root();

            copy_text(info.bootloader_name, sizeof(info.bootloader_name), boot.bootloader_name);
            copy_text(info.bootloader_version, sizeof(info.bootloader_version), boot.bootloader_version);
            copy_text(info.firmware, sizeof(info.firmware), boot.firmware);

            info.framebuffer_ready = boot.framebuffer_ready;
            info.framebuffer_width = boot.framebuffer_width;
            info.framebuffer_height = boot.framebuffer_height;
            info.framebuffer_bpp = boot.framebuffer_bpp;
            info.block_ready = boot.block_ready;
            info.sxfs_mounted = boot.sxfs_mounted;
            info.memory_usable_bytes = boot.memory_usable_bytes;
            info.memory_reclaimable_bytes = boot.memory_reclaimable_bytes;
            info.memory_total_pages = boot.memory_usable_bytes / 4096ull;
            info.timer_backend = SAVANXP_TIMER_NONE;
            info.sxfs_file_count = static_cast<uint32_t>(sxfs::file_count(volume));
            info.sxfs_total_bytes = sxfs::total_bytes(volume);
            info.sxfs_used_bytes = sxfs::used_bytes(volume);
            info.sxfs_free_bytes = sxfs::free_bytes(volume);
            info.process_count = 1;
            info.cpu_count = 1;
            info.cpu_online = 1;

            if (!is_user_range(frame.edi, static_cast<uint32_t>(sizeof(info)))) {
                frame.eax = failure(SAVANXP_EFAULT);
                return;
            }
            memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(frame.edi)), &info, sizeof(info));
            frame.eax = 0;
            return;
        }
        case kSysExit: {
            serial::write("i386: proceso salio con codigo ");
            serial::write_dec(frame.edi);
            serial::write("\n");
            cpu::halt_forever();
        }
        default: {
            serial::write("i386: syscall ");
            serial::write_dec(frame.eax);
            serial::write(" sin implementar\n");
            frame.eax = failure(SAVANXP_ENOSYS);
            return;
        }
            serial::write("i386: syscall ");
            serial::write_dec(frame.eax);
            serial::write(" sin implementar\n");
            frame.eax = failure(SAVANXP_ENOSYS);
            return;
        }
    }
}

/* El stub de interrupts32.S apila, por debajo de esp0 y en este orden: el
 * puntero al marco (4), %es (4), %ds (4), los ocho registros de pusha (32) y
 * el marco del processor (20). Son 64 bytes fijos, asi que el marco se lee
 * desde esp0 en vez de fiarse del puntero que empuja el stub: ese puntero
 * llega desplazado una palabra y hacia que el manejader lea el eip del
 * proceso donde deberia estar eax. */
constexpr uint32_t kSyscallFrameOffset = 64;

SyscallFrame* current_frame() {
    return reinterpret_cast<SyscallFrame*>(
        static_cast<uintptr_t>(cpu::ring0_stack_top()) - kSyscallFrameOffset);
}

/* El stub de interrupts32.S llama a este simbolo plano: el ensamblador no
 * puede escribir un nombre con espacio de nombres. */
extern "C" void i386_handle_syscall() {
    syscall::handle(*current_frame());
}

} // namespace i386