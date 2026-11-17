#pragma once

// One checked description of the framebuffer the bootloader handed over
// (FB1 in docs/roadmaps/open/FRAMEBUFFER_ROADMAP.md). `validate` is pure: it
// reads the descriptor's fields and never the pixels behind `address`, so a
// rejected descriptor is diagnosed without touching its memory.

#include <stdint.h>

#include "boot/boot_info.hpp"

namespace framebuffer {

// The Limine protocol's only memory model: direct RGB with channel masks.
constexpr uint8_t kMemoryModelRgb = 1;

enum class Format : uint8_t {
    unknown = 0,
    // 32 bpp, blue in the low byte: the compositor's canonical surface format.
    xrgb8888,
    // 32 bpp, red in the low byte. Recognized for diagnostics; converting to
    // it is FB5's work, so it is rejected until then.
    xbgr8888,
};

enum class Reject : uint8_t {
    none = 0,
    unavailable,
    null_address,
    zero_dimension,
    unsupported_depth,
    unsupported_memory_model,
    invalid_masks,
    unsupported_format,
    pitch_too_small,
    misaligned_pitch,
    size_overflow,
    address_overflow,
    exceeds_abi,
    mapped_too_small,
};

struct Layout {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    Format format;
    // pitch * height: what the visible mode occupies.
    uint64_t visible_bytes;
    // What is established as mapped from `address`: the framebuffer region of
    // the memory map, or the visible mode alone when no region holds it.
    uint64_t mapped_bytes;
};

struct Result {
    Reject reason;
    // Filled only when `reason` is `none`.
    Layout layout;
};

Result validate(const boot::FramebufferInfo& info);

// A short stable name for a log line, e.g. "pitch_too_small".
const char* reject_name(Reject reason);
const char* format_name(Format format);

} // namespace framebuffer
