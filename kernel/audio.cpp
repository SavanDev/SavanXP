#include "kernel/audio.hpp"

namespace audio {
namespace {
constexpr size_t kMaxBackends = 16;
struct Binding { const Backend* backend; const Driver* driver; };
Binding g_bindings[kMaxBackends] = {};
size_t g_binding_count = 0;
const Backend* g_playback = nullptr;
const Backend* g_capture = nullptr;
bool g_playback_active = false, g_capture_active = false;
const Driver* g_drivers[kMaxDrivers] = {};
size_t g_driver_count = 0;
const Driver* g_bound_driver = nullptr;
uint32_t g_master_volume = 100, g_capture_volume = 100;
bool g_master_muted = false, g_capture_muted = false;

bool has_capture(const Backend* b) {
    return b && b->capture_ready && b->capture_ready(b->context);
}
bool append(const Backend& backend, const Driver* driver) {
    for (size_t i = 0; i < g_binding_count; ++i)
        if (g_bindings[i].backend == &backend) return false;
    if (g_binding_count == kMaxBackends) return false;
    g_bindings[g_binding_count++] = {&backend,driver};
    if (!g_playback && backend.ready(backend.context)) {
        g_playback = &backend; g_bound_driver = driver;
    }
    if (!g_capture && has_capture(&backend)) g_capture = &backend;
    return true;
}
bool local_endpoint(const Backend* b, uint32_t index, savanxp_audio_endpoint& info) {
    if (b->endpoint) return b->endpoint(b->context,index,info);
    savanxp_audio_info format = {};
    if (!b->get_info(b->context,format) || index > 1 || (index && !has_capture(b))) return false;
    info = {};
    info.device_id = static_cast<uint64_t>(format.backend) << 56;
    info.id = info.device_id | (index+1); info.backend = format.backend;
    info.flags = (index ? SAVANXP_AUDIO_CAPTURE : SAVANXP_AUDIO_PLAYBACK) |
        SAVANXP_AUDIO_SELECTED;
    if (index ? has_capture(b) : b->ready(b->context)) info.flags |= SAVANXP_AUDIO_AVAILABLE;
    const char* name = index ? "Default input" : "Default output";
    const char* device = format.backend == SAVANXP_AUDIO_BACKEND_AC97 ? "AC'97" : "Virtio sound";
    for (unsigned i = 0; name[i] && i+1 < sizeof(info.name); ++i) info.name[i] = name[i];
    for (unsigned i = 0; device[i] && i+1 < sizeof(info.device_name); ++i) info.device_name[i] = device[i];
    return true;
}
bool format_for(const Backend* b, uint32_t direction, savanxp_audio_info& info) {
    if (!b) return false;
    if (direction == SAVANXP_AUDIO_CAPTURE && b->capture_info) return b->capture_info(b->context,info);
    return b->get_info(b->context,info);
}
bool stop_backend(const Backend* b, uint32_t direction) {
    if (!b) return true;
    if (direction == SAVANXP_AUDIO_PLAYBACK) {
        if (b->stop_checked) return b->stop_checked(b->context);
        b->stop(b->context);
    } else {
        if (b->capture_stop_checked) return b->capture_stop_checked(b->context);
        if (b->capture_stop) b->capture_stop(b->context);
    }
    return true;
}
} // namespace

void set_backend(const Backend& backend) {
    g_binding_count = 0; g_playback = g_capture = nullptr;
    g_playback_active = g_capture_active = false; g_bound_driver = nullptr;
    (void)append(backend,nullptr);
}
bool register_driver(const Driver& driver) {
    if (g_driver_count == kMaxDrivers || !driver.probe || !driver.backend) return false;
    g_drivers[g_driver_count++] = &driver; return true;
}
const Driver* bind_best() {
    if (g_binding_count) return bound_driver();
    bool probed[kMaxDrivers] = {};
    for (size_t attempt = 0; attempt < g_driver_count; ++attempt) {
        size_t best = kMaxDrivers;
        for (size_t i = 0; i < g_driver_count; ++i)
            if (!probed[i] && (best == kMaxDrivers || g_drivers[i]->priority > g_drivers[best]->priority)) best = i;
        if (best == kMaxDrivers) break;
        probed[best] = true;
        const Driver* driver = g_drivers[best];
        if (!driver->probe()) continue;
        (void)append(driver->backend(),driver);
        if (driver->backend_at) {
            for (uint32_t i = 1; i < kMaxBackends; ++i) {
                const Backend* b = driver->backend_at(i);
                if (!b || !append(*b,driver)) break;
            }
        }
    }
    return bound_driver();
}
const Driver* bound_driver() {
    if (g_bound_driver) return g_bound_driver;
    for (size_t i = 0; i < g_binding_count; ++i)
        if (g_bindings[i].backend == g_capture) return g_bindings[i].driver;
    return nullptr;
}

bool endpoint(uint32_t index, savanxp_audio_endpoint& info) {
    if (index >= SAVANXP_AUDIO_MAX_ENDPOINTS) return false;
    for (size_t b = 0; b < g_binding_count; ++b) {
        for (uint32_t i = 0; i < SAVANXP_AUDIO_MAX_ENDPOINTS; ++i) {
            if (!local_endpoint(g_bindings[b].backend,i,info)) break;
            if (index--) continue;
            const Backend* selected = (info.flags & SAVANXP_AUDIO_PLAYBACK) ? g_playback : g_capture;
            if (g_bindings[b].backend != selected) info.flags &= ~SAVANXP_AUDIO_SELECTED;
            return true;
        }
    }
    return false;
}
int select_endpoint(uint64_t id, uint32_t direction) {
    if (direction != SAVANXP_AUDIO_PLAYBACK && direction != SAVANXP_AUDIO_CAPTURE) return -SX_EINVAL;
    const Backend* target = nullptr;
    savanxp_audio_endpoint found = {};
    for (size_t b = 0; b < g_binding_count && !target; ++b) {
        for (uint32_t i = 0; i < SAVANXP_AUDIO_MAX_ENDPOINTS; ++i) {
            if (!local_endpoint(g_bindings[b].backend,i,found)) break;
            if (found.id == id && (found.flags & direction)) { target = g_bindings[b].backend; break; }
        }
    }
    if (!target) return -SX_ENOENT;
    if (!(found.flags & SAVANXP_AUDIO_AVAILABLE)) return -SX_ENODEV;
    const Backend* previous = direction == SAVANXP_AUDIO_PLAYBACK ? g_playback : g_capture;
    const bool active = direction == SAVANXP_AUDIO_PLAYBACK ? g_playback_active : g_capture_active;
    if (target == previous && (found.flags & SAVANXP_AUDIO_SELECTED)) return 0;
    if (active && previous != target) {
        savanxp_audio_info before = {}, after = {};
        if (!format_for(previous,direction,before) || !format_for(target,direction,after) ||
            before.sample_rate_hz != after.sample_rate_hz || before.channels != after.channels ||
            before.bits_per_sample != after.bits_per_sample) return -SX_ENOSYS;
    }
    if (!(found.flags & SAVANXP_AUDIO_SELECTED)) {
        if (!target->select_endpoint) return -SX_ENOSYS;
        const int result = target->select_endpoint(target->context,id,direction);
        if (result) return result;
    }
    if (previous == target) return 0;
    // Prepare the new independent device before stopping the old one. Failed
    // preparation leaves the effective default and its DMA untouched.
    if (active) {
        const bool prepared = direction == SAVANXP_AUDIO_PLAYBACK ? target->configure(target->context) :
            target->capture_configure && target->capture_configure(target->context);
        if (!prepared) return -SX_EIO;
        if (!stop_backend(previous,direction)) {
            (void)stop_backend(target,direction);
            return -SX_EIO;
        }
    }
    if (direction == SAVANXP_AUDIO_PLAYBACK) {
        g_playback = target;
        g_bound_driver = nullptr;
        for (size_t i = 0; i < g_binding_count; ++i)
            if (g_bindings[i].backend == target) g_bound_driver = g_bindings[i].driver;
    } else g_capture = target;
    return 0;
}

bool ready() { return g_playback && g_playback->ready(g_playback->context); }
bool get_info(savanxp_audio_info& info) { return format_for(g_playback,SAVANXP_AUDIO_PLAYBACK,info); }
bool configure() {
    if (!g_playback || !g_playback->configure(g_playback->context)) return false;
    g_playback_active = true; return true;
}
int submit_period(uint64_t user_buffer, uint32_t bytes) {
    return g_playback ? g_playback->submit_period(g_playback->context,user_buffer,bytes) : -SX_ENODEV;
}
bool stop() {
    if (!stop_backend(g_playback,SAVANXP_AUDIO_PLAYBACK)) return false;
    g_playback_active = false;
    return true;
}

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

static void apply_gain(void* frames, uint32_t byte_count, uint32_t volume, bool muted) {
    int16_t* samples = static_cast<int16_t*>(frames);
    uint32_t count = byte_count / 2u;
    uint32_t index = 0;
    if (frames == nullptr) {
        return;
    }
    if (muted) {
        for (index = 0; index < count; ++index) {
            samples[index] = 0;
        }
        return;
    }
    if (volume >= 100u) {
        return;
    }
    for (index = 0; index < count; ++index) {
        samples[index] = static_cast<int16_t>(
            static_cast<int32_t>(samples[index]) * static_cast<int32_t>(volume) / 100);
    }
}

void apply_playback_gain(void* frames, uint32_t bytes) { apply_gain(frames,bytes,g_master_volume,g_master_muted); }
void apply_capture_gain(void* frames, uint32_t bytes) { apply_gain(frames,bytes,g_capture_volume,g_capture_muted); }
uint32_t capture_volume() { return g_capture_volume; }
bool set_capture_volume(uint32_t volume) {
    if (volume > 100) return false;
    g_capture_volume = volume; return true;
}
bool capture_muted() { return g_capture_muted; }
void set_capture_muted(bool muted) { g_capture_muted = muted; }


bool get_capture_info(savanxp_audio_info& info) {
    return has_capture(g_capture) && format_for(g_capture,SAVANXP_AUDIO_CAPTURE,info);
}
int capture_available() {
    return g_capture && g_capture->capture_available ? g_capture->capture_available(g_capture->context) : -SX_ENOSYS;
}
bool capture_ready() { return has_capture(g_capture); }
bool capture_configure() {
    if (!g_capture || !g_capture->capture_configure || !g_capture->capture_configure(g_capture->context)) return false;
    g_capture_active = true; return true;
}
int capture_read_period(void* buffer, uint32_t bytes) {
    return g_capture && g_capture->capture_read_period ?
        g_capture->capture_read_period(g_capture->context,buffer,bytes) : -SX_ENODEV;
}
bool capture_stop() {
    if (!stop_backend(g_capture,SAVANXP_AUDIO_CAPTURE)) return false;
    g_capture_active = false;
    return true;
}
} // namespace audio
