#include "kernel/framebuffer_layout.hpp"

namespace framebuffer {
namespace {

struct Channel {
    uint8_t size;
    uint8_t shift;
};

bool channel_fits(const Channel& channel, uint32_t bpp) {
    return channel.size != 0 && channel.size <= 16 &&
        static_cast<uint32_t>(channel.size) + channel.shift <= bpp;
}

uint64_t channel_mask(const Channel& channel) {
    return ((uint64_t{1} << channel.size) - 1) << channel.shift;
}

bool is_channel(const Channel& channel, uint8_t size, uint8_t shift) {
    return channel.size == size && channel.shift == shift;
}

bool multiply(uint64_t a, uint64_t b, uint64_t& out) {
    if (a != 0 && b > UINT64_MAX / a) {
        return false;
    }
    out = a * b;
    return true;
}

} // namespace

Result validate(const boot::FramebufferInfo& info) {
    Result result = {};
    auto reject = [&result](Reject reason) {
        result.reason = reason;
        result.layout = {};
        return result;
    };

    if (!info.available) {
        return reject(Reject::unavailable);
    }
    if (info.address == nullptr) {
        return reject(Reject::null_address);
    }
    if (info.width == 0 || info.height == 0 || info.pitch == 0) {
        return reject(Reject::zero_dimension);
    }
    if (info.bpp != 32) {
        return reject(Reject::unsupported_depth);
    }
    if (info.memory_model != kMemoryModelRgb) {
        return reject(Reject::unsupported_memory_model);
    }

    const Channel red = {info.red_mask_size, info.red_mask_shift};
    const Channel green = {info.green_mask_size, info.green_mask_shift};
    const Channel blue = {info.blue_mask_size, info.blue_mask_shift};
    if (!channel_fits(red, info.bpp) || !channel_fits(green, info.bpp) ||
        !channel_fits(blue, info.bpp)) {
        return reject(Reject::invalid_masks);
    }
    const uint64_t red_mask = channel_mask(red);
    const uint64_t green_mask = channel_mask(green);
    const uint64_t blue_mask = channel_mask(blue);
    if ((red_mask & green_mask) != 0 || (red_mask & blue_mask) != 0 ||
        (green_mask & blue_mask) != 0) {
        return reject(Reject::invalid_masks);
    }

    Format format = Format::unknown;
    if (is_channel(red, 8, 16) && is_channel(green, 8, 8) && is_channel(blue, 8, 0)) {
        format = Format::xrgb8888;
    } else if (is_channel(red, 8, 0) && is_channel(green, 8, 8) && is_channel(blue, 8, 16)) {
        format = Format::xbgr8888;
    }
    if (format != Format::xrgb8888) {
        return reject(Reject::unsupported_format);
    }

    const uint64_t bytes_per_pixel = info.bpp / 8u;
    uint64_t row_bytes = 0;
    if (!multiply(info.width, bytes_per_pixel, row_bytes)) {
        return reject(Reject::size_overflow);
    }
    if (info.pitch < row_bytes) {
        return reject(Reject::pitch_too_small);
    }
    if (info.pitch % bytes_per_pixel != 0) {
        return reject(Reject::misaligned_pitch);
    }
    uint64_t visible_bytes = 0;
    if (!multiply(info.pitch, info.height, visible_bytes)) {
        return reject(Reject::size_overflow);
    }

    // The display ABI (savanxp_fb_info) carries every one of these in 32 bits.
    if (info.width > UINT32_MAX || info.height > UINT32_MAX || info.pitch > UINT32_MAX ||
        visible_bytes > UINT32_MAX) {
        return reject(Reject::exceeds_abi);
    }

    // A region that holds the address but ends before the visible mode does is
    // an inconsistent descriptor; it is not made valid by claiming more.
    uint64_t mapped_bytes = visible_bytes;
    if (info.region_bytes != 0) {
        if (info.region_bytes < visible_bytes) {
            return reject(Reject::mapped_too_small);
        }
        mapped_bytes = info.region_bytes;
    }

    const uint64_t address = reinterpret_cast<uintptr_t>(info.address);
    if (mapped_bytes - 1 > UINT64_MAX - address) {
        return reject(Reject::address_overflow);
    }

    result.reason = Reject::none;
    result.layout = {
        .width = static_cast<uint32_t>(info.width),
        .height = static_cast<uint32_t>(info.height),
        .pitch = static_cast<uint32_t>(info.pitch),
        .bpp = info.bpp,
        .format = format,
        .visible_bytes = visible_bytes,
        .mapped_bytes = mapped_bytes,
    };
    return result;
}

const char* reject_name(Reject reason) {
    switch (reason) {
    case Reject::none: return "none";
    case Reject::unavailable: return "unavailable";
    case Reject::null_address: return "null_address";
    case Reject::zero_dimension: return "zero_dimension";
    case Reject::unsupported_depth: return "unsupported_depth";
    case Reject::unsupported_memory_model: return "unsupported_memory_model";
    case Reject::invalid_masks: return "invalid_masks";
    case Reject::unsupported_format: return "unsupported_format";
    case Reject::pitch_too_small: return "pitch_too_small";
    case Reject::misaligned_pitch: return "misaligned_pitch";
    case Reject::size_overflow: return "size_overflow";
    case Reject::address_overflow: return "address_overflow";
    case Reject::exceeds_abi: return "exceeds_abi";
    case Reject::mapped_too_small: return "mapped_too_small";
    }
    return "unknown";
}

const char* format_name(Format format) {
    switch (format) {
    case Format::unknown: return "unknown";
    case Format::xrgb8888: return "xrgb8888";
    case Format::xbgr8888: return "xbgr8888";
    }
    return "unknown";
}

} // namespace framebuffer
