#pragma once

#include <stdarg.h>
#include <stddef.h>

#include "boot/boot_info.hpp"
#include "abi/savanxp_abi.h"

namespace console {

void early_init();
void init(const boot::BootInfo& boot_info);
const boot::FramebufferInfo& framebuffer_info();
bool framebuffer_ready();
void set_framebuffer_console_enabled(bool enabled);
void set_external_framebuffer(void* address, const savanxp_fb_info& info);
// Stops drawing into the framebuffer for good: its geometry can no longer be
// trusted. Serial output goes on. For the emergency path (FB4).
void detach_framebuffer();
void redraw();
bool present_pixels(const void* pixels, size_t byte_count);
bool present_region(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void clear();
void write_char(char character);
/* Solo al puerto serie, sin pasar por la consola de texto del framebuffer.
 * Es lo que hace falta para emitir diagnostico mientras hay algo compuesto en
 * pantalla; ver kernel/serial_dev.cpp. */
void serial_write(const char* text, size_t length);
// Called by the BSP's tick, under the kernel lock: feeds the UART what its
// transmitter takes without waiting. The first call starts queuing serial
// output instead of waiting for the port at every character.
void serial_tick();
// Sends everything queued now, and every later character as it is written.
// For the paths that end the machine: panic, a fatal exception, power-off.
void serial_flush_synchronous();
void write(const char* text);
void write_line(const char* text);
void vprintf(const char* format, va_list args);
void printf(const char* format, ...);

} // namespace console
