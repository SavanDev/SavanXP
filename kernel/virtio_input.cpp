#include "kernel/virtio_input.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/input.hpp"
#include "kernel/string.hpp"
#include "kernel/virtio_pci.hpp"
#include "shared/syscall.h"

namespace {

constexpr uint16_t kVirtioInputModernDevice = 0x1052u;
constexpr uint16_t kVirtioInputSubsystemDevice = 18u;
constexpr uint16_t kVirtioInputEventQueue = 0;
constexpr uint16_t kMaxEventQueueSize = 64;

constexpr uint8_t kVirtioInputCfgAbsInfo = 0x12;

constexpr uint16_t kEvSyn = 0x00;
constexpr uint16_t kEvKey = 0x01;
constexpr uint16_t kEvAbs = 0x03;
constexpr uint16_t kSynReport = 0;
constexpr uint16_t kAbsX = 0x00;
constexpr uint16_t kAbsY = 0x01;
constexpr uint16_t kBtnLeft = 0x110;
constexpr uint16_t kBtnRight = 0x111;
constexpr uint16_t kBtnMiddle = 0x112;

struct [[gnu::packed]] VirtioInputAbsInfo {
    uint32_t min;
    uint32_t max;
    uint32_t fuzz;
    uint32_t flat;
    uint32_t resolution;
};

struct [[gnu::packed]] VirtioInputConfig {
    uint8_t select;
    uint8_t subsel;
    uint8_t size;
    uint8_t reserved[5];
    VirtioInputAbsInfo abs;
};

struct [[gnu::packed]] VirtioInputEvent {
    uint16_t type;
    uint16_t code;
    int32_t value;
};

virtio_pci::Device g_device = {};
virtio_pci::Queue g_event_queue = {};
uint32_t g_framebuffer_width = 0;
uint32_t g_framebuffer_height = 0;
int32_t g_abs_min_x = 0;
int32_t g_abs_max_x = 0;
int32_t g_abs_min_y = 0;
int32_t g_abs_max_y = 0;
int32_t g_current_abs_x = 0;
int32_t g_current_abs_y = 0;
int32_t g_last_screen_x = 0;
int32_t g_last_screen_y = 0;
uint32_t g_buttons = 0;
bool g_have_abs_x = false;
bool g_have_abs_y = false;
bool g_have_screen_position = false;
bool g_ready = false;

volatile VirtioInputConfig* device_cfg() {
    return reinterpret_cast<volatile VirtioInputConfig*>(virtio_pci::device_cfg_base(g_device));
}

bool read_abs_info(uint8_t axis, int32_t& minimum, int32_t& maximum) {
    volatile VirtioInputConfig* cfg = device_cfg();
    cfg->select = kVirtioInputCfgAbsInfo;
    cfg->subsel = axis;
    virtio_pci::memory_barrier();

    if (cfg->size < sizeof(VirtioInputAbsInfo)) {
        return false;
    }

    minimum = static_cast<int32_t>(cfg->abs.min);
    maximum = static_cast<int32_t>(cfg->abs.max);
    return maximum > minimum;
}

VirtioInputEvent* queue_event_buffer(uint16_t index) {
    return reinterpret_cast<VirtioInputEvent*>(
        virtio_pci::queue_extra(g_event_queue, sizeof(VirtioInputEvent) * index)
    );
}

uint64_t queue_event_buffer_physical(uint16_t index) {
    return virtio_pci::queue_extra_physical(g_event_queue, sizeof(VirtioInputEvent) * index);
}

bool setup_event_queue() {
    if (!virtio_pci::setup_queue(
            g_device,
            kVirtioInputEventQueue,
            kMaxEventQueueSize,
            sizeof(VirtioInputEvent) * kMaxEventQueueSize,
            16,
            g_event_queue)) {
        return false;
    }

    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_event_queue);
    virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_event_queue);
    used->flags = 0;
    used->idx = 0;

    for (uint16_t index = 0; index < g_event_queue.size; ++index) {
        descriptors[index] = {
            .addr = queue_event_buffer_physical(index),
            .len = sizeof(VirtioInputEvent),
            .flags = virtio_pci::kDescriptorFlagWrite,
            .next = 0,
        };
        if (!virtio_pci::submit_descriptor_head(g_event_queue, index)) {
            return false;
        }
    }

    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_event_queue);
    return true;
}

int32_t normalize_axis(int32_t value, int32_t minimum, int32_t maximum, uint32_t extent) {
    if (extent <= 1 || maximum <= minimum) {
        return 0;
    }

    if (value < minimum) {
        value = minimum;
    } else if (value > maximum) {
        value = maximum;
    }

    const int64_t range = static_cast<int64_t>(maximum) - static_cast<int64_t>(minimum);
    const int64_t scaled = (static_cast<int64_t>(value - minimum) * static_cast<int64_t>(extent - 1)) + (range / 2);
    return static_cast<int32_t>(scaled / range);
}

void submit_screen_position(int32_t screen_x, int32_t screen_y) {
    if (!g_have_screen_position) {
        g_last_screen_x = screen_x;
        g_last_screen_y = screen_y;
        g_have_screen_position = true;
        input::submit_mouse_event({
            .delta_x = screen_x,
            .delta_y = screen_y,
            .buttons = g_buttons,
            .source = input::MouseSource::virtio_tablet,
        });
        return;
    }

    input::submit_mouse_event({
        .delta_x = screen_x - g_last_screen_x,
        .delta_y = screen_y - g_last_screen_y,
        .buttons = g_buttons,
        .source = input::MouseSource::virtio_tablet,
    });

    g_last_screen_x = screen_x;
    g_last_screen_y = screen_y;
}

void process_event(const VirtioInputEvent& event) {
    if (event.type == kEvAbs) {
        if (event.code == kAbsX) {
            g_current_abs_x = event.value;
            g_have_abs_x = true;
        } else if (event.code == kAbsY) {
            g_current_abs_y = event.value;
            g_have_abs_y = true;
        }
        return;
    }

    if (event.type == kEvKey) {
        const uint32_t mask =
            event.code == kBtnLeft ? SAVANXP_MOUSE_BUTTON_LEFT :
            event.code == kBtnRight ? SAVANXP_MOUSE_BUTTON_RIGHT :
            event.code == kBtnMiddle ? SAVANXP_MOUSE_BUTTON_MIDDLE : 0u;
        if (mask != 0) {
            if (event.value != 0) {
                g_buttons |= mask;
            } else {
                g_buttons &= ~mask;
            }
        }
        return;
    }

    if (event.type != kEvSyn || event.code != kSynReport || !g_have_abs_x || !g_have_abs_y) {
        return;
    }

    submit_screen_position(
        normalize_axis(g_current_abs_x, g_abs_min_x, g_abs_max_x, g_framebuffer_width),
        normalize_axis(g_current_abs_y, g_abs_min_y, g_abs_max_y, g_framebuffer_height)
    );
}

void fail_device(const char* reason) {
    virtio_pci::fail_device(g_device);
    if (reason != nullptr) {
        console::printf("virtio-input: %s\n", reason);
    }
    g_ready = false;
}

} // namespace

namespace virtio_input {

void initialize(const boot::FramebufferInfo& framebuffer) {
    memset(&g_device, 0, sizeof(g_device));
    memset(&g_event_queue, 0, sizeof(g_event_queue));
    g_ready = false;
    g_buttons = 0;
    g_have_abs_x = false;
    g_have_abs_y = false;
    g_have_screen_position = false;
    g_framebuffer_width = framebuffer.available ? static_cast<uint32_t>(framebuffer.width) : 0u;
    g_framebuffer_height = framebuffer.available ? static_cast<uint32_t>(framebuffer.height) : 0u;

    pci::DeviceInfo pci_device = {};
    if (!pci::ready() || !virtio_pci::find_modern_device(kVirtioInputModernDevice, kVirtioInputSubsystemDevice, pci_device)) {
        return;
    }

    if (!virtio_pci::initialize_device(pci_device, true, g_device)) {
        console::write_line("virtio-input: missing required MMIO capabilities");
        return;
    }

    virtio_pci::set_device_status(g_device, 0);
    virtio_pci::memory_barrier();
    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::kStatusAcknowledge | virtio_pci::kStatusDriver));

    if (!virtio_pci::negotiate_features(g_device, 0, virtio_pci::kFeatureVersion1Bit)) {
        fail_device("feature negotiation failed");
        return;
    }
    if (!read_abs_info(kAbsX, g_abs_min_x, g_abs_max_x) || !read_abs_info(kAbsY, g_abs_min_y, g_abs_max_y)) {
        fail_device("failed to read ABS ranges");
        return;
    }
    if (!setup_event_queue()) {
        fail_device("failed to initialize event queue");
        return;
    }

    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::device_status(g_device) | virtio_pci::kStatusDriverOk));
    g_ready = true;
    console::printf(
        "virtio-input: tablet ready pci=%x:%x.%u abs_x=%d..%d abs_y=%d..%d\n",
        static_cast<unsigned>(g_device.pci_device.bus),
        static_cast<unsigned>(g_device.pci_device.slot),
        static_cast<unsigned>(g_device.pci_device.function),
        static_cast<int>(g_abs_min_x),
        static_cast<int>(g_abs_max_x),
        static_cast<int>(g_abs_min_y),
        static_cast<int>(g_abs_max_y)
    );
}

void poll() {
    if (!g_ready || !g_event_queue.enabled || g_event_queue.size == 0) {
        return;
    }

    const virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_event_queue);
    const virtio_pci::UsedElement* ring = virtio_pci::queue_used_ring(g_event_queue);
    bool notified = false;

    while (g_event_queue.last_used_index != used->idx) {
        virtio_pci::memory_barrier();
        const virtio_pci::UsedElement element = ring[g_event_queue.last_used_index % g_event_queue.size];
        if (element.id < g_event_queue.size) {
            const VirtioInputEvent* event = queue_event_buffer(static_cast<uint16_t>(element.id));
            process_event(*event);
            if (virtio_pci::submit_descriptor_head(g_event_queue, static_cast<uint16_t>(element.id))) {
                notified = true;
            }
        }
        g_event_queue.last_used_index = static_cast<uint16_t>(g_event_queue.last_used_index + 1);
    }

    if (notified) {
        virtio_pci::memory_barrier();
        virtio_pci::notify_queue(g_device, g_event_queue);
    }
}

bool mouse_ready() {
    return g_ready;
}

void set_framebuffer_extent(uint32_t width, uint32_t height) {
    g_framebuffer_width = width;
    g_framebuffer_height = height;
}

void begin_graphics_session() {
    g_have_screen_position = false;
}

void end_graphics_session() {
    g_have_screen_position = false;
}

} // namespace virtio_input
