#pragma once

#include "kernel/audio.hpp"

namespace virtio_sound {

void initialize();
bool ready();
// Backend de audio sobre virtio-sound-pci. Solo valido si ready() es true.
const audio::Backend& backend();

} // namespace virtio_sound
