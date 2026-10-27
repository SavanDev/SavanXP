#pragma once

#include "boot/boot_info.hpp"

namespace virtio_input {

// Finds the virtio tablet and keyboard and registers them with the input
// layer. Runs before ps2::initialize, so on a virtio machine it wins.
void initialize(const boot::FramebufferInfo& framebuffer);

} // namespace virtio_input
