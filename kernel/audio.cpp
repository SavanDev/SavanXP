#include "kernel/audio.hpp"

#include "savanxp/syscall.h"

namespace audio {

namespace {
const Backend* g_backend = nullptr;
} // namespace

void set_backend(const Backend& backend) { g_backend = &backend; }

bool ready() { return g_backend != nullptr && g_backend->ready(); }
bool get_info(savanxp_audio_info& info) { return g_backend != nullptr && g_backend->get_info(info); }
bool configure() { return g_backend != nullptr && g_backend->configure(); }

int submit_period(uint64_t user_buffer, uint32_t byte_count) {
    return g_backend != nullptr ? g_backend->submit_period(user_buffer, byte_count)
                                : -static_cast<int>(SAVANXP_ENODEV);
}

void stop() { if (g_backend != nullptr) { g_backend->stop(); } }

} // namespace audio
