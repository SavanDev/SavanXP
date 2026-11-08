#pragma once
#include "azalia_port.hpp"

namespace azalia_port {
struct RecoveryTarget {
    codec_t* codec;
    Path* output;
    Path* input;
    int output_pin, input_pin;
    bool playback, capture, available = false;
};
// Stops all DMA through a verified controller reset before reusing command
// rings. Reprobes the same codec identities and restores desired analog routes.
// Streams stay stopped; callers must invalidate old capture subscriptions.
bool recover_controller(azalia_t*, RecoveryTarget*, unsigned count);
}
