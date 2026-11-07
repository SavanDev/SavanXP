#include "kernel/audio.hpp"

#include "abi/savanxp_abi.h"

namespace audio {

namespace {
const Backend* g_backend = nullptr;

uint32_t g_master_volume = 100u;
bool g_master_muted = false;

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

bool endpoint(uint32_t index, savanxp_audio_endpoint& info) {
    if (!g_backend || index >= SAVANXP_AUDIO_MAX_ENDPOINTS) return false;
    if (g_backend->endpoint) return g_backend->endpoint(index, info);
    savanxp_audio_info format = {};
    if (!g_backend->get_info(format) || index > 1 ||
        (index == 1 && !capture_ready())) return false;
    info = {};
    info.device_id = static_cast<uint64_t>(format.backend) << 56;
    info.id = info.device_id | (index + 1);
    info.backend = format.backend;
    info.flags = (index ? SAVANXP_AUDIO_CAPTURE : SAVANXP_AUDIO_PLAYBACK) |
        SAVANXP_AUDIO_AVAILABLE | SAVANXP_AUDIO_SELECTED;
    const char* name = index ? "Default input" : "Default output";
    const char* device = format.backend == SAVANXP_AUDIO_BACKEND_AC97 ? "AC'97" : "Virtio sound";
    for (unsigned i = 0; name[i] && i + 1 < sizeof(info.name); ++i) info.name[i] = name[i];
    for (unsigned i = 0; device[i] && i + 1 < sizeof(info.device_name); ++i) info.device_name[i] = device[i];
    return true;
}
int select_endpoint(uint64_t id, uint32_t direction) {
    if (direction != SAVANXP_AUDIO_PLAYBACK && direction != SAVANXP_AUDIO_CAPTURE) return -SX_EINVAL;
    savanxp_audio_endpoint info;
    for (uint32_t i = 0; endpoint(i, info); ++i) {
        if (info.id != id || !(info.flags & direction)) continue;
        if (!(info.flags & SAVANXP_AUDIO_AVAILABLE)) return -SX_ENODEV;
        if (info.flags & SAVANXP_AUDIO_SELECTED) return 0;
        return g_backend->select_endpoint ? g_backend->select_endpoint(id, direction) : -SX_ENOSYS;
    }
    return -SX_ENOENT;
}

bool ready() { return g_backend != nullptr && g_backend->ready(); }
bool get_info(savanxp_audio_info& info) { return g_backend != nullptr && g_backend->get_info(info); }
bool configure() { return g_backend != nullptr && g_backend->configure(); }

int submit_period(uint64_t user_buffer, uint32_t byte_count) {
    return g_backend != nullptr ? g_backend->submit_period(user_buffer, byte_count)
                                : -static_cast<int>(SX_ENODEV);
}

void stop() { if (g_backend != nullptr) { g_backend->stop(); } }

uint32_t master_volume() { return g_master_volume; }

bool set_master_volume(uint32_t volume) {
    if (volume > 100u) {
        return false;
    }
    g_master_volume = volume;
    return true;
}

bool master_muted() { return g_master_muted; }

void set_master_muted(bool muted) { g_master_muted = muted; }

void apply_playback_gain(void* frames, uint32_t byte_count) {
    int16_t* samples = static_cast<int16_t*>(frames);
    uint32_t count = byte_count / 2u;
    uint32_t index = 0;
    if (frames == nullptr) {
        return;
    }
    if (g_master_muted) {
        for (index = 0; index < count; ++index) {
            samples[index] = 0;
        }
        return;
    }
    if (g_master_volume >= 100u) {
        return;
    }
    for (index = 0; index < count; ++index) {
        samples[index] = static_cast<int16_t>(
            static_cast<int32_t>(samples[index]) * static_cast<int32_t>(g_master_volume) / 100);
    }
}

bool capture_ready() {
    return g_backend != nullptr && g_backend->capture_ready != nullptr && g_backend->capture_ready();
}

bool capture_configure() {
    return g_backend != nullptr && g_backend->capture_configure != nullptr && g_backend->capture_configure();
}

int capture_read_period(uint64_t user_buffer, uint32_t byte_count) {
    if (g_backend == nullptr || g_backend->capture_read_period == nullptr) {
        return -static_cast<int>(SX_ENODEV);
    }
    return g_backend->capture_read_period(user_buffer, byte_count);
}

void capture_stop() {
    if (g_backend != nullptr && g_backend->capture_stop != nullptr) {
        g_backend->capture_stop();
    }
}

} // namespace audio
