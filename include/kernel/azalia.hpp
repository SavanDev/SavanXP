#pragma once
#include "kernel/audio.hpp"
namespace azalia {
// Analog playback/capture OpenBSD azalia port; see docs/kernel/AZALIA.md.
const audio::Driver& driver();
// Called on the BSP timer under the kernel lock. Reclaims cyclic DMA bytes and
// replaces completed samples with silence so an idle producer cannot replay them.
void poll();
}
