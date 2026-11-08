#include "recovery.hpp"

namespace azalia_port {
bool recover_controller(azalia_t* az, RecoveryTarget* targets, unsigned count) {
    if (!az || !targets || !count || count > 4) return false;
    for (unsigned i = 0; i < count; ++i) targets[i].available = false;
    az->transport_failed = true;
    // No DMA buffers or BDLs are modified until CRST has proved the hardware
    // stopped. A failed reset quarantines all existing controller allocations.
    if (azalia_reset(az) || azalia_init_rings(az)) { az->transport_failed = true; return false; }
    const uint32_t present = az->io.read(az->io.context,HDA_STATESTS,2);
    for (unsigned i = 0; i < count; ++i) {
        auto& target = targets[i];
        if (!(present&(1u<<target.codec->address))) continue;
        // The graph is too large for a kernel stack; controller operations are
        // serialized by the kernel lock, so a single bounded scratch suffices.
        static codec_t candidate;
        candidate = {}; candidate.az = az; candidate.address = target.codec->address;
        if (probe_codec(&candidate)) {
            if (az->transport_failed) return false;
            continue;
        }
        if (candidate.vid != target.codec->vid) continue;
        Path output, input, empty;
        if ((target.playback && !output_path(&candidate,target.output_pin,output)) ||
            (target.capture && !input_path(&candidate,target.input_pin,input))) continue;
        if (target.playback && switch_output(&candidate,empty,output)) {
            if (az->transport_failed) return false;
            continue;
        }
        *target.codec = candidate; *target.output = output; *target.input = input;
        target.available = true;
    }
    return true;
}
}
