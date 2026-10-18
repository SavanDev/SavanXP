#pragma once

#include <stdint.h>

namespace i386 {

/* Lo que el bootstrap sabe del arranque y que un proceso de usuario puede
 * preguntar por la llamada system_info(). Lo rellena main32.cpp al recorrer
 * las etiquetas Multiboot2 y al montar SxFS; lo lee la syscall 35. */
namespace boot_state {

struct State {
    char bootloader_name[32];
    char bootloader_version[32];
    char firmware[16];
    uint64_t memory_usable_bytes;
    uint64_t memory_reclaimable_bytes;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_bpp;
    uint8_t framebuffer_ready;
    uint8_t block_ready;
    uint8_t sxfs_mounted;
};

extern State g_state;

inline State& state() {
    return g_state;
}

} // namespace boot_state

} // namespace i386