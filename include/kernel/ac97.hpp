#pragma once

#include "kernel/audio.hpp"

namespace ac97 {

// Driver del controlador Intel AC'97 (ICH, clase PCI 0x04 / subclase 0x01), el
// chip de audio que VirtualBox y QEMU emulan por defecto. Es el fallback de
// audio cuando no hay virtio-sound-pci (caso VirtualBox), espejando el rol de
// fb_gpu frente a virtio-gpu. Reproduce por bus-master DMA en modo polling puro
// (sin IRQ), lo que esquiva el problema conocido de INTx legacy en VirtualBox.
void initialize();
bool ready();
const audio::Backend& backend();

} // namespace ac97
