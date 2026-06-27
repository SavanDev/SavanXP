#pragma once

#include "boot/boot_info.hpp"
#include "kernel/display.hpp"

namespace fb_gpu {

// Backend de display "tonto": sin dispositivo PCI, sin colas ni resource IDs.
// Compone por software escribiendo pixeles directo al framebuffer lineal que
// entrega el firmware/Limine. Es el fallback cuando no hay virtio-gpu (p.ej.
// VirtualBox con VGA estandar). No tiene cursor de hardware ni cambio de modo:
// el compositor usa su cursor por software y la resolucion nativa fija.
void initialize(const boot::FramebufferInfo& framebuffer);
const display::Backend& backend();

} // namespace fb_gpu
