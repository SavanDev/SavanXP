#pragma once

#include "kernel/audio.hpp"

namespace lifecycle {
struct Subject;
}

namespace virtio_sound {

void initialize();
bool ready();
// Backend de audio sobre virtio-sound-pci. Solo valido si ready() es true.
const audio::Backend& backend();
// Descriptor para el registro de audio::. Su probe hace el initialize.
const audio::Driver& driver();
/* Its start and stop, for `lifecycle-selftest=virtio-sound`
 * (docs/kernel/DRIVER_LIFECYCLE.md). */
const lifecycle::Subject& lifecycle_subject();

} // namespace virtio_sound
