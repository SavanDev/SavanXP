#pragma once

#include "boot/boot_info.hpp"

namespace lifecycle {
struct Subject;
}

namespace virtio_input {

// Finds the virtio tablet and keyboard and registers them with the input
// layer. Runs before ps2::initialize, so on a virtio machine it wins.
void initialize(const boot::FramebufferInfo& framebuffer);
/* Its start and stop, tablet and keyboard each, for
 * `lifecycle-selftest=virtio-input` (docs/kernel/DRIVER_LIFECYCLE.md). */
const lifecycle::Subject& lifecycle_subject();

} // namespace virtio_input
