#pragma once
#include <stdint.h>

namespace azalia_port {
// LPIB measures link consumption; FIFOS bounds controller prefetch. Reserve
// that FIFO plus one software period before touching output memory. A reduced
// but still safe queue must not be discarded merely for falling below its
// initial lead, which would insert silence into otherwise continuous PCM.
inline uint64_t playback_write_position(uint64_t played, uint64_t written,
    uint32_t fifo_bytes, uint32_t period) {
    const uint64_t safe = played+fifo_bytes+period;
    return written >= safe ? written : ((safe+period-1)/period)*period;
}
}
