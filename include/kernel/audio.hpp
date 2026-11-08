#pragma once

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_abi.h"

namespace audio {

// Hardware operations. Ownership, validation and period splitting live in
// audio_device.cpp; optional endpoint callbacks describe real connectors.
struct Backend {
    bool (*ready)(void*);
    bool (*get_info)(void*, savanxp_audio_info& info);
    // Prepare playback idempotently before submitting a period.
    bool (*configure)(void*);
    // Submit user PCM; return zero or negative errno.
    int (*submit_period)(void*, uint64_t user_buffer, uint32_t byte_count);
    // Stop and release the playback session.
    void (*stop)(void*);

    // Optional independent capture direction.
    bool (*capture_ready)(void*);
    bool (*capture_configure)(void*);
    // Fill kernel-owned PCM; zero or negative errno.
    int (*capture_read_period)(void*, void* buffer, uint32_t byte_count);
    void (*capture_stop)(void*);
    bool (*endpoint)(void*, uint32_t index, savanxp_audio_endpoint&) = nullptr;
    int (*select_endpoint)(void*, uint64_t id, uint32_t direction) = nullptr;
    int (*capture_available)(void*) = nullptr; // bytes ready without waiting, or negative errno
    bool (*capture_info)(void*, savanxp_audio_info&) = nullptr;
    void* context = nullptr;
};

// Registered hardware driver, probed by descending priority.
struct Driver {
    const char* name;
    // Larger priority chooses the initial default, independently of registration order.
    int priority;
    // Initialize hardware and report whether the backend is usable.
    bool (*probe)();
    // Valid after a successful probe.
    const Backend& (*backend)();
    // Optional additional instances, indexed from one after backend().
    const Backend* (*backend_at)(uint32_t index) = nullptr;
};

constexpr size_t kMaxDrivers = 8;

bool register_driver(const Driver& driver);
const Driver* bind_best();
const Driver* bound_driver();
void set_backend(const Backend& backend);
// Indexed enumeration ends with false. Selection reports a negative errno;
// a failed transition must retain the previous effective endpoint if possible.
// Inventory includes every bound backend; each direction has its own default.
bool endpoint(uint32_t index, savanxp_audio_endpoint& info);
int select_endpoint(uint64_t id, uint32_t direction);

bool ready();
bool get_info(savanxp_audio_info& info);
bool configure();
int submit_period(uint64_t user_buffer, uint32_t byte_count);
void stop();

// Shared software master gain (0..100), with independent mute. Backends
// apply it after copying PCM so controls do not need the writer slot.
uint32_t master_volume();
bool set_master_volume(uint32_t volume);
bool master_muted();
void set_master_muted(bool muted);
// Scale S16LE samples in place. Mute clears them; 100 is identity.
// An odd trailing byte is left untouched.
void apply_playback_gain(void* frames, uint32_t byte_count);

uint32_t capture_volume();
bool set_capture_volume(uint32_t volume);
bool capture_muted();
void set_capture_muted(bool muted);
void apply_capture_gain(void* frames, uint32_t byte_count);
bool get_capture_info(savanxp_audio_info& info);
int capture_available();
bool capture_ready();
bool capture_configure();
int capture_read_period(void* buffer, uint32_t byte_count);
void capture_stop();

} // namespace audio
