#pragma once
#include "azalia_port.hpp"

namespace azalia_port {
// Polling capture cursor, independent of OS allocation and user-copy services.
// Readers consume complete periods one period behind DMA, never its active
// block. Lost laps or an overwritten unread block fail instead of returning
// stale data as a successful recording.
struct CaptureCursor {
    uint64_t produced = 0, consumed = 0, last_poll_ns = 0;
    uint32_t position = 0;
    bool failed = false;
};
int capture_poll(stream_t*, CaptureCursor&, uint64_t now_ns);
uint32_t capture_available(const stream_t*, const CaptureCursor&);
int capture_copy(stream_t*, CaptureCursor&, uint64_t now_ns, void* output, uint32_t bytes);
} // namespace azalia_port
