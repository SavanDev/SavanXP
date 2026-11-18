// Host tests of framebuffer::validate (include/kernel/framebuffer_layout.hpp):
// FB1a's acceptance cases in docs/roadmaps/open/FRAMEBUFFER_ROADMAP.md --
// zero dimensions, undersized pitch and span, overflow, narrowing, padding
// and invalid masks -- plus the proof that validation never reads the pixels.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

#include "kernel/framebuffer_layout.hpp"

namespace {

using framebuffer::Format;
using framebuffer::Reject;

int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "framebuffer-layout-test: FAIL %s\n", what);
        std::exit(1);
    }
}

// A plausible address that is never dereferenced by these cases.
void* const kAddress = reinterpret_cast<void*>(uintptr_t{0xffff800080000000ull});

// What QEMU's standard VGA hands over: 1280x800, XRGB8888, tight pitch.
boot::FramebufferInfo good() {
    boot::FramebufferInfo info = {};
    info.available = true;
    info.address = kAddress;
    info.width = 1280;
    info.height = 800;
    info.pitch = 1280 * 4;
    info.bpp = 32;
    info.memory_model = framebuffer::kMemoryModelRgb;
    info.red_mask_size = 8;
    info.red_mask_shift = 16;
    info.green_mask_size = 8;
    info.green_mask_shift = 8;
    info.blue_mask_size = 8;
    info.blue_mask_shift = 0;
    info.region_bytes = 16u * 1024u * 1024u;
    return info;
}

Reject reason_of(const boot::FramebufferInfo& info) {
    return framebuffer::validate(info).reason;
}

void accepts_the_common_layout() {
    const framebuffer::Result result = framebuffer::validate(good());
    check(result.reason == Reject::none, "good descriptor accepted");
    check(result.layout.width == 1280 && result.layout.height == 800, "geometry");
    check(result.layout.pitch == 5120 && result.layout.bpp == 32, "pitch and depth");
    check(result.layout.format == Format::xrgb8888, "format xrgb8888");
    check(result.layout.visible_bytes == 5120ull * 800ull, "visible bytes");
    check(result.layout.mapped_bytes == 16ull * 1024ull * 1024ull, "mapped bytes from region");
}

void rejects_missing_or_empty() {
    boot::FramebufferInfo info = good();
    info.available = false;
    check(reason_of(info) == Reject::unavailable, "unavailable");

    info = good();
    info.address = nullptr;
    check(reason_of(info) == Reject::null_address, "null address");

    info = good();
    info.width = 0;
    check(reason_of(info) == Reject::zero_dimension, "zero width");
    info = good();
    info.height = 0;
    check(reason_of(info) == Reject::zero_dimension, "zero height");
    info = good();
    info.pitch = 0;
    check(reason_of(info) == Reject::zero_dimension, "zero pitch");
}

void handles_pitch() {
    boot::FramebufferInfo info = good();
    info.pitch = 1280 * 4 - 4;
    check(reason_of(info) == Reject::pitch_too_small, "pitch one pixel short");

    info = good();
    info.pitch = 1280 * 4 + 2;
    check(reason_of(info) == Reject::misaligned_pitch, "pitch not a pixel multiple");

    // Padded rows are normal (a 1366-wide mode on a 1408-pixel stride).
    info = good();
    info.width = 1366;
    info.height = 768;
    info.pitch = 1408 * 4;
    const framebuffer::Result padded = framebuffer::validate(info);
    check(padded.reason == Reject::none, "padded pitch accepted");
    check(padded.layout.pitch == 5632 && padded.layout.width == 1366, "padded geometry kept");
    check(padded.layout.visible_bytes == 5632ull * 768ull, "padded visible bytes use pitch");
}

void handles_depth_and_masks() {
    boot::FramebufferInfo info = good();
    info.bpp = 24;
    check(reason_of(info) == Reject::unsupported_depth, "24 bpp not yet supported");
    info.bpp = 16;
    check(reason_of(info) == Reject::unsupported_depth, "16 bpp not yet supported");

    info = good();
    info.memory_model = 0;
    check(reason_of(info) == Reject::unsupported_memory_model, "memory model 0");

    info = good();
    info.green_mask_size = 0;
    check(reason_of(info) == Reject::invalid_masks, "empty channel");

    info = good();
    info.red_mask_shift = 28;
    check(reason_of(info) == Reject::invalid_masks, "channel past the pixel");

    info = good();
    info.green_mask_shift = 12;
    check(reason_of(info) == Reject::invalid_masks, "overlapping channels");

    info = good();
    info.red_mask_size = 255;
    check(reason_of(info) == Reject::invalid_masks, "absurd channel size");

    // BGR is accepted and named, so the caller can warn; FB5 converts to it.
    info = good();
    info.red_mask_shift = 0;
    info.blue_mask_shift = 16;
    const framebuffer::Result bgr = framebuffer::validate(info);
    check(bgr.reason == Reject::none, "xbgr8888 accepted");
    check(bgr.layout.format == Format::xbgr8888, "xbgr8888 reported as such");

    // Valid, non-overlapping, but no layout the writers know (10-bit channels).
    info = good();
    info.red_mask_size = 10;
    info.red_mask_shift = 20;
    info.green_mask_size = 10;
    info.green_mask_shift = 10;
    info.blue_mask_size = 10;
    info.blue_mask_shift = 0;
    check(reason_of(info) == Reject::unsupported_format, "x2rgb10 rejected");
}

void handles_overflow_and_narrowing() {
    boot::FramebufferInfo info = good();
    info.width = UINT64_MAX / 2;
    info.pitch = UINT64_MAX;
    check(reason_of(info) == Reject::size_overflow, "row bytes overflow");

    info = good();
    info.pitch = uint64_t{1} << 40;
    info.height = uint64_t{1} << 30;
    check(reason_of(info) == Reject::size_overflow, "pitch * height overflow");

    // Each field fits 64 bits, but the ABI carries 32.
    info = good();
    info.width = uint64_t{1} << 32;
    info.pitch = (uint64_t{1} << 32) * 4;
    info.height = 1;
    info.region_bytes = 0;
    check(reason_of(info) == Reject::exceeds_abi, "width past 32 bits");

    // 16384 x 16384 x 4 = 1 GiB fits; 32768 x 32768 x 4 = 4 GiB does not.
    info = good();
    info.width = 32768;
    info.height = 32768;
    info.pitch = 32768 * 4;
    info.region_bytes = 0;
    check(reason_of(info) == Reject::exceeds_abi, "visible bytes past 32 bits");
    info.width = 16384;
    info.height = 16384;
    info.pitch = 16384 * 4;
    check(reason_of(info) == Reject::none, "1 GiB mode fits the ABI");

    info = good();
    info.address = reinterpret_cast<void*>(uintptr_t{UINT64_MAX - 4096});
    check(reason_of(info) == Reject::address_overflow, "span wraps the address space");
}

void handles_the_mapped_span() {
    boot::FramebufferInfo info = good();
    info.region_bytes = 0;
    const framebuffer::Result no_region = framebuffer::validate(info);
    check(no_region.reason == Reject::none, "no region accepted");
    check(no_region.layout.mapped_bytes == no_region.layout.visible_bytes,
        "no region maps the visible mode only");

    info = good();
    info.region_bytes = 1280ull * 4ull * 800ull - 1;
    check(reason_of(info) == Reject::mapped_too_small, "region shorter than the mode");

    info = good();
    info.region_bytes = 1280ull * 4ull * 800ull;
    check(reason_of(info) == Reject::none, "region exactly the mode");
}

// The address points at a page with no access: if validate read a pixel the
// test would die with SIGSEGV instead of reporting a reason.
void never_touches_the_pixels() {
    const long page = 4096;
    void* guarded = mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(guarded != MAP_FAILED, "guard page");

    boot::FramebufferInfo info = good();
    info.address = guarded;
    info.width = 16;
    info.height = 16;
    info.pitch = 16 * 4;
    info.region_bytes = 0;
    check(reason_of(info) == Reject::none, "accepted without reading pixels");
    info.pitch = 4;
    check(reason_of(info) == Reject::pitch_too_small, "rejected without reading pixels");
    info.pitch = 64;
    info.memory_model = 7;
    check(reason_of(info) == Reject::unsupported_memory_model, "bad model without reading pixels");

    munmap(guarded, page);
}

// adopt() is what every writer trusts: accepted keeps the descriptor drawable
// with the established span; rejected turns it off and leaves the delivered
// fields for the boot log.
void adopt_sets_the_single_gate() {
    boot::FramebufferInfo info = good();
    info.delivered = true;
    info.mapped_bytes = 1;
    check(framebuffer::adopt(info).reason == Reject::none, "adopt accepts");
    check(info.available, "accepted stays available");
    check(info.mapped_bytes == 16ull * 1024ull * 1024ull, "accepted mapped bytes from region");

    info = good();
    info.delivered = true;
    info.region_bytes = 0;
    check(framebuffer::adopt(info).reason == Reject::none, "adopt accepts without region");
    check(info.mapped_bytes == 1280ull * 4ull * 800ull, "no region maps the visible mode");

    info = good();
    info.delivered = true;
    info.pitch = 16;
    info.mapped_bytes = 123;
    check(framebuffer::adopt(info).reason == Reject::pitch_too_small, "adopt reports the reason");
    check(!info.available, "rejected is not available");
    check(info.mapped_bytes == 0, "rejected maps nothing");
    check(info.delivered && info.width == 1280 && info.pitch == 16, "delivered fields kept for the log");

    // The boot log re-checks the delivered fields and gets the same verdict.
    boot::FramebufferInfo as_delivered = info;
    as_delivered.available = true;
    check(reason_of(as_delivered) == Reject::pitch_too_small, "re-check gives the same reason");

    // A BGR descriptor stays available; the caller warns.
    info = good();
    info.red_mask_shift = 0;
    info.blue_mask_shift = 16;
    const framebuffer::Result bgr = framebuffer::adopt(info);
    check(info.available && bgr.layout.format == Format::xbgr8888, "BGR adopted");
}

void names_every_reason() {
    for (int value = 0; value <= static_cast<int>(Reject::mapped_too_small); ++value) {
        const char* name = framebuffer::reject_name(static_cast<Reject>(value));
        check(name != nullptr && std::strcmp(name, "unknown") != 0, "reject name");
    }
    check(std::strcmp(framebuffer::format_name(Format::xbgr8888), "xbgr8888") == 0, "format name");
}

} // namespace

int main() {
    accepts_the_common_layout();
    rejects_missing_or_empty();
    handles_pitch();
    handles_depth_and_masks();
    handles_overflow_and_narrowing();
    handles_the_mapped_span();
    never_touches_the_pixels();
    adopt_sets_the_single_gate();
    names_every_reason();
    std::printf("framebuffer-layout-test: ok (%d checks)\n", g_checks);
    return 0;
}
