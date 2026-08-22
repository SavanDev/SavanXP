#include "kernel/audio.hpp"

#include "savanxp/syscall.h"

namespace audio {

namespace {
const Backend* g_backend = nullptr;

const Driver* g_drivers[kMaxDrivers] = {};
size_t g_driver_count = 0;
const Driver* g_bound_driver = nullptr;
} // namespace

void set_backend(const Backend& backend) { g_backend = &backend; }

bool register_driver(const Driver& driver) {
    if (g_driver_count >= kMaxDrivers || driver.probe == nullptr || driver.backend == nullptr) {
        return false;
    }
    g_drivers[g_driver_count] = &driver;
    ++g_driver_count;
    return true;
}

const Driver* bind_best() {
    bool probed[kMaxDrivers] = {};
    for (size_t attempt = 0; attempt < g_driver_count; ++attempt) {
        size_t best = kMaxDrivers;
        for (size_t i = 0; i < g_driver_count; ++i) {
            if (probed[i]) {
                continue;
            }
            if (best == kMaxDrivers || g_drivers[i]->priority > g_drivers[best]->priority) {
                best = i;
            }
        }
        if (best == kMaxDrivers) {
            break;
        }

        probed[best] = true;
        const Driver* candidate = g_drivers[best];
        if (candidate->probe()) {
            set_backend(candidate->backend());
            g_bound_driver = candidate;
            return candidate;
        }
    }
    return nullptr;
}

const Driver* bound_driver() { return g_bound_driver; }

bool ready() { return g_backend != nullptr && g_backend->ready(); }
bool get_info(savanxp_audio_info& info) { return g_backend != nullptr && g_backend->get_info(info); }
bool configure() { return g_backend != nullptr && g_backend->configure(); }

int submit_period(uint64_t user_buffer, uint32_t byte_count) {
    return g_backend != nullptr ? g_backend->submit_period(user_buffer, byte_count)
                                : -static_cast<int>(SAVANXP_ENODEV);
}

void stop() { if (g_backend != nullptr) { g_backend->stop(); } }

} // namespace audio
