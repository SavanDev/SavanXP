#include "syscall.hpp"

#include <stdint.h>

#include "boot_state.hpp"
#include "cpu.hpp"
#include "fb32.hpp"
#include "kernel/sxfs.hpp"
#include "kernel/string.hpp"
#include "pit.hpp"
#include "ps2_32.hpp"
#include "savanxp/syscall.h"
#include "serial.hpp"
#include "user32.hpp"

namespace i386 {

namespace syscall {

namespace {

/* Ventana de usuario: los mismos cuatro PDEs de 4 MiB que paging.cpp marca
 * con el bit de usuario. Todo puntero que venga de un proceso tiene que caer
 * aqui antes de tocarlo. */
constexpr uint32_t kUserWindowBase = 0x08000000u;
constexpr uint32_t kUserWindowEnd = 0x09000000u;

/* Fixed launch chain: sysinfo -> sh32 -> desktop32. keyecho remains a
 * standalone keyboard diagnostic in the i386 volume. */
constexpr const char* kShellPath = "/disk/bin/sh32";
/* The desktop runs after sh32 exits: there is no scheduler yet. */
constexpr const char* kDesktopPath = "/disk/bin/desktop32";

bool g_gpu_open = false;

bool is_user_range(uint32_t address, uint32_t size) {
    if (address < kUserWindowBase || address >= kUserWindowEnd) {
        return false;
    }
    return static_cast<uint64_t>(address) + size <= kUserWindowEnd;
}

/* Validate every user byte before comparing the device path. */
bool user_path_equals(uint32_t address, const char* text) {
    for (uint32_t index = 0; index <= 64; ++index) {
        if (!is_user_range(address + index, 1)) {
            return false;
        }
        const char value = reinterpret_cast<const char*>(static_cast<uintptr_t>(address))[index];
        if (value != text[index]) {
            return false;
        }
        if (value == '\0') {
            return true;
        }
    }
    return false;
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
        case kSysRead: {
            /* stdin del objetivo i386 es la cola del teclado: un evento
             * savanxp_input_event por lectura, sin flags O_NONBLOCK todavia.
             * Solo existe el fd 0 (ningun archivo esta abierto por proceso). */
            if (frame.edi != kStdinFd) {
                frame.eax = failure(SAVANXP_EBADF);
                return;
            }
            if (!is_user_range(frame.esi, frame.edx)) {
                frame.eax = failure(SAVANXP_EFAULT);
                return;
            }
            if (frame.edx < sizeof(savanxp_input_event)) {
                frame.eax = failure(SAVANXP_EINVAL);
                return;
            }
            /* Espera bloqueante con sti;hlt: el gate de int $0x80 llega con
             * IF=0, asi que el chequeo y la espera son atomicos contra la
             * IRQ1; y un IRQ pendiente en el sti despierta el hlt de
             * inmediato (la sombra de sti lo entrega antes de dormir), asi
             * que una tecla que llegue entre las dos instrucciones no se
             * pierde. pop_event restaura el IF previo (0). */
            savanxp_input_event event = {};
            while (!ps2_32::pop_event(event)) {
                asm volatile("sti\n\thlt");
            }
            memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(frame.esi)), &event,
                   sizeof(event));
            frame.eax = static_cast<uint32_t>(sizeof(event));
            return;
        }
        case kSysOpen: {
            /* A single GPU client gets a fixed descriptor; repeated opens return EBUSY. */
            if (!user_path_equals(frame.edi, kGpuPath)) {
                frame.eax = failure(SAVANXP_ENOENT);
                return;
            }
            if (!fb32::ready()) {
                frame.eax = failure(SAVANXP_ENODEV);
                return;
            }
            if (g_gpu_open) {
                frame.eax = failure(SAVANXP_EBUSY);
                return;
            }
            g_gpu_open = true;
            frame.eax = kGpuFd;
            serial::write("gpu0: opened by userland\n");
            return;
        }
        case kSysClose: {
            if (frame.edi == kGpuFd && g_gpu_open) {
                g_gpu_open = false;
                fb32::console_set_enabled(true);
                frame.eax = 0;
                return;
            }
            /* Standard streams are permanent kernel channels. */
            frame.eax = failure(SAVANXP_EBADF);
            return;
        }
        case kSysIoctl: {
            if (frame.edi != kGpuFd || !g_gpu_open) {
                frame.eax = failure(SAVANXP_EBADF);
                return;
            }
            switch (frame.esi) {
                case GPU_IOC_GET_INFO: {
                    if (!is_user_range(frame.edx, sizeof(savanxp_gpu_info))) {
                        frame.eax = failure(SAVANXP_EFAULT);
                        return;
                    }
                    const savanxp_fb_info& fb = fb32::info();
                    savanxp_gpu_info info = {};
                    info.width = fb.width;
                    info.height = fb.height;
                    info.pitch = fb.pitch;
                    info.bpp = fb.bpp;
                    info.buffer_size = fb.buffer_size;
                    info.backend = SAVANXP_GPU_BACKEND_FRAMEBUFFER;
                    info.flags = 0;
                    memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(frame.edx)), &info,
                           sizeof(info));
                    frame.eax = 0;
                    return;
                }
                case GPU_IOC_ACQUIRE:
                case GPU_IOC_RELEASE: {
                    // Keep serial diagnostics from painting over the desktop.
                    fb32::console_set_enabled(frame.esi == GPU_IOC_RELEASE);
                    frame.eax = 0;
                    return;
                }
                case GPU_IOC_PRESENT: {
                    /* Validate the full client buffer before copying a complete frame. */
                    const uint32_t bytes = fb32::info().buffer_size;
                    if (!is_user_range(frame.edx, bytes)) {
                        frame.eax = failure(SAVANXP_EFAULT);
                        return;
                    }
                    const uint32_t pitch_pixels = fb32::info().pitch / 4u;
                    if (!fb32::present(reinterpret_cast<const uint32_t*>(
                                           static_cast<uintptr_t>(frame.edx)),
                                       pitch_pixels, 0, 0, fb32::info().width,
                                       fb32::info().height)) {
                        frame.eax = failure(SAVANXP_EINVAL);
                        return;
                    }
                    frame.eax = 0;
                    return;
                }
                case GPU_IOC_PRESENT_REGION: {
                    if (!is_user_range(frame.edx, sizeof(savanxp_gpu_present_region))) {
                        frame.eax = failure(SAVANXP_EFAULT);
                        return;
                    }
                    savanxp_gpu_present_region region = {};
                    memcpy(&region, reinterpret_cast<const void*>(static_cast<uintptr_t>(frame.edx)),
                           sizeof(region));
                    if (region.pixels == 0 || region.width == 0 || region.height == 0 ||
                        region.source_pitch < region.width) {
                        frame.eax = failure(SAVANXP_EINVAL);
                        return;
                    }
                    const uint64_t pixel_count = static_cast<uint64_t>(region.source_pitch) *
                                                 region.height;
                    if (pixel_count > 0xFFFFFFFFull / sizeof(uint32_t)) {
                        frame.eax = failure(SAVANXP_EFAULT);
                        return;
                    }
                    const uint32_t bytes = static_cast<uint32_t>(pixel_count * sizeof(uint32_t));
                    if (region.pixels > 0xFFFFFFFFull ||
                        !is_user_range(static_cast<uint32_t>(region.pixels), static_cast<uint32_t>(bytes))) {
                        frame.eax = failure(SAVANXP_EFAULT);
                        return;
                    }
                    if (!fb32::present(reinterpret_cast<const uint32_t*>(
                                           static_cast<uintptr_t>(region.pixels)),
                                       region.source_pitch, region.x, region.y, region.width,
                                       region.height)) {
                        frame.eax = failure(SAVANXP_EINVAL);
                        return;
                    }
                    frame.eax = 0;
                    return;
                }
                default: {
                    frame.eax = failure(SAVANXP_ENOTTY);
                    return;
                }
            }
        }
        case kSysUptimeMs: {
            /* El SDK devuelve unsigned long: en i386 caben los primeros 49
             * dias; el wrap completo se documenta antes que esconderse. */
            frame.eax = static_cast<uint32_t>(pit::uptime_ms());
            return;
        }
        case kSysSleepMs: {
            /* frame.edi trae los ms como unsigned long del SDK. La espera la
             * hace pit::sleep_ms con sti;hlt, que deja el IF previo (0) al
             * volver; la read bloqueante de arriba usa el mismo patron. */
            pit::sleep_ms(frame.edi);
            frame.eax = 0;
            return;
        }
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
            info.timer_backend = SAVANXP_TIMER_PIT;
            info.timer_frequency_hz = pit::kFrequencyHz;
            info.uptime_ms = pit::uptime_ms();
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
            /* El uptime en la misma linea: es la prueba mas directa de que el
             * PIT corre (sysinfo sale con IF=1 desde ring 3, asi que ya hay
             * ticks; el token del test no lo exige, solo lo deja a la vista). */
            serial::write(", uptime ");
            serial::write_dec(static_cast<uint32_t>(pit::uptime_ms()));
            serial::write(" ms\n");
            /* Launch each stage once; keep IRQs live if the desktop exits. */
            g_gpu_open = false;
            fb32::console_set_enabled(true);
            static uint32_t stage = 0;
            if (stage == 0) {
                stage = 1;
                user32::run(kShellPath);
            }
            if (stage == 1) {
                stage = 2;
                user32::run(kDesktopPath);
            }
            cpu::idle_with_interrupts();
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
