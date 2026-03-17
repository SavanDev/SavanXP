#include "kernel/virtio_input.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/input.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/vmm.hpp"
#include "shared/syscall.h"

namespace {

constexpr uint16_t kPciVendorVirtio = 0x1af4u;
constexpr uint16_t kVirtioInputModernDevice = 0x1052u;
constexpr uint16_t kVirtioInputSubsystemDevice = 18u;
constexpr uint16_t kPciCommandOffset = 0x04;
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;

constexpr uint8_t kVirtioPciCapCommonCfg = 1;
constexpr uint8_t kVirtioPciCapNotifyCfg = 2;
constexpr uint8_t kVirtioPciCapIsrCfg = 3;
constexpr uint8_t kVirtioPciCapDeviceCfg = 4;

constexpr uint8_t kVirtioStatusAcknowledge = 1u << 0;
constexpr uint8_t kVirtioStatusDriver = 1u << 1;
constexpr uint8_t kVirtioStatusDriverOk = 1u << 2;
constexpr uint8_t kVirtioStatusFeaturesOk = 1u << 3;
constexpr uint8_t kVirtioStatusFailed = 1u << 7;

constexpr uint32_t kVirtioFeatureVersion1Word = 1u;
constexpr uint32_t kVirtioFeatureVersion1Bit = 1u << 0;

constexpr uint16_t kVirtqueueDescriptorWrite = 1u << 1;
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

struct [[gnu::packed]] VirtioPciCommonCfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
};

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

struct [[gnu::packed]] VirtqDescriptor {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct [[gnu::packed]] VirtqAvailHeader {
    uint16_t flags;
    uint16_t idx;
};

struct [[gnu::packed]] VirtqUsedElement {
    uint32_t id;
    uint32_t len;
};

struct [[gnu::packed]] VirtqUsedHeader {
    uint16_t flags;
    uint16_t idx;
};

struct [[gnu::packed]] VirtioInputEvent {
    uint16_t type;
    uint16_t code;
    int32_t value;
};

struct QueueLayout {
    uint16_t size;
    size_t desc_offset;
    size_t avail_offset;
    size_t used_offset;
    size_t ring_bytes;
    size_t buffers_offset;
    size_t total_bytes;
};

struct QueueState {
    memory::PageAllocation allocation;
    uint16_t size;
    uint16_t notify_off;
    uint16_t last_used_index;
    uint16_t next_avail_index;
    bool enabled;
};

struct MappedBar {
    bool mapped;
    pci::BarInfo info;
    uint8_t* base;
};

struct CapabilityView {
    bool valid;
    MappedBar* bar;
    uint8_t* base;
    uint32_t offset_within_bar;
    uint32_t length;
    uint32_t extra;
};

pci::DeviceInfo g_device = {};
MappedBar g_bars[6] = {};
CapabilityView g_common_view = {};
CapabilityView g_notify_view = {};
CapabilityView g_isr_view = {};
CapabilityView g_device_view = {};
QueueState g_event_queue = {};
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

inline void memory_barrier() {
    asm volatile("" : : : "memory");
}

size_t align_up(size_t value, size_t alignment) {
    const size_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

volatile VirtioPciCommonCfg* common_cfg() {
    return reinterpret_cast<volatile VirtioPciCommonCfg*>(g_common_view.base);
}

volatile VirtioInputConfig* device_cfg() {
    return reinterpret_cast<volatile VirtioInputConfig*>(g_device_view.base);
}

bool looks_like_virtio_input(const pci::DeviceInfo& info) {
    if (!info.present || info.vendor_id != kPciVendorVirtio) {
        return false;
    }
    return info.device_id == kVirtioInputModernDevice || info.subsystem_device_id == kVirtioInputSubsystemDevice;
}

bool map_bar(uint8_t bar_index, MappedBar*& mapped) {
    mapped = nullptr;
    if (bar_index >= 6) {
        return false;
    }

    MappedBar& bar = g_bars[bar_index];
    if (bar.mapped) {
        mapped = &bar;
        return true;
    }

    if (!pci::bar_info(g_device, bar_index, bar.info) || bar.info.io_space) {
        return false;
    }

    void* virtual_base = nullptr;
    if (!vm::map_kernel_mmio(
            bar.info.base,
            static_cast<size_t>(bar.info.size),
            vm::kPageWriteThrough | vm::kPageCacheDisable,
            &virtual_base)) {
        return false;
    }

    bar.base = reinterpret_cast<uint8_t*>(virtual_base);
    bar.mapped = true;
    mapped = &bar;
    return true;
}

bool resolve_capability(uint8_t cfg_type, CapabilityView& view) {
    view = {};

    pci::VendorCapabilityInfo capability = {};
    if (!pci::find_vendor_capability(g_device, cfg_type, capability)) {
        return false;
    }

    MappedBar* mapped_bar = nullptr;
    if (!map_bar(capability.bar_index, mapped_bar) || mapped_bar == nullptr) {
        return false;
    }
    if (capability.capability_offset >= mapped_bar->info.size ||
        capability.capability_offset + capability.capability_length > mapped_bar->info.size) {
        return false;
    }

    view.valid = true;
    view.bar = mapped_bar;
    view.base = mapped_bar->base + capability.capability_offset;
    view.offset_within_bar = capability.capability_offset;
    view.length = capability.capability_length;
    view.extra = capability.extra;
    return true;
}

bool find_device(pci::DeviceInfo& info) {
    for (size_t index = 0; index < pci::device_count(); ++index) {
        pci::DeviceInfo candidate = {};
        if (!pci::device_info(index, candidate)) {
            continue;
        }
        if (looks_like_virtio_input(candidate)) {
            info = candidate;
            return true;
        }
    }
    return false;
}

void set_device_status(uint8_t status) {
    common_cfg()->device_status = status;
}

uint8_t device_status() {
    return common_cfg()->device_status;
}

void fail_device(const char* reason) {
    if (g_common_view.valid) {
        set_device_status(static_cast<uint8_t>(device_status() | kVirtioStatusFailed));
    }
    if (reason != nullptr) {
        console::printf("virtio-input: %s\n", reason);
    }
    g_ready = false;
}

bool negotiate_features() {
    volatile VirtioPciCommonCfg* cfg = common_cfg();
    cfg->device_feature_select = kVirtioFeatureVersion1Word;
    memory_barrier();
    const uint32_t device_features = cfg->device_feature;
    if ((device_features & kVirtioFeatureVersion1Bit) == 0) {
        return false;
    }

    cfg->driver_feature_select = 0;
    cfg->driver_feature = 0;
    cfg->driver_feature_select = kVirtioFeatureVersion1Word;
    cfg->driver_feature = kVirtioFeatureVersion1Bit;
    memory_barrier();

    set_device_status(static_cast<uint8_t>(device_status() | kVirtioStatusFeaturesOk));
    memory_barrier();
    return (device_status() & kVirtioStatusFeaturesOk) != 0;
}

bool read_abs_info(uint8_t axis, int32_t& minimum, int32_t& maximum) {
    volatile VirtioInputConfig* cfg = device_cfg();
    cfg->select = kVirtioInputCfgAbsInfo;
    cfg->subsel = axis;
    memory_barrier();

    if (cfg->size < sizeof(VirtioInputAbsInfo)) {
        return false;
    }

    minimum = static_cast<int32_t>(cfg->abs.min);
    maximum = static_cast<int32_t>(cfg->abs.max);
    return maximum > minimum;
}

QueueLayout build_queue_layout(uint16_t size) {
    QueueLayout layout = {};
    layout.size = size;
    layout.desc_offset = 0;
    layout.avail_offset = sizeof(VirtqDescriptor) * size;
    layout.used_offset = align_up(layout.avail_offset + 6 + (sizeof(uint16_t) * size), 4);
    layout.ring_bytes = layout.used_offset + 6 + (sizeof(VirtqUsedElement) * size);
    layout.buffers_offset = align_up(layout.ring_bytes, 16);
    layout.total_bytes = layout.buffers_offset + (sizeof(VirtioInputEvent) * size);
    return layout;
}

VirtqDescriptor* queue_descriptors(const QueueLayout& layout) {
    return reinterpret_cast<VirtqDescriptor*>(
        static_cast<uint8_t*>(g_event_queue.allocation.virtual_address) + layout.desc_offset
    );
}

VirtqAvailHeader* queue_avail_header(const QueueLayout& layout) {
    return reinterpret_cast<VirtqAvailHeader*>(
        static_cast<uint8_t*>(g_event_queue.allocation.virtual_address) + layout.avail_offset
    );
}

uint16_t* queue_avail_ring(const QueueLayout& layout) {
    return reinterpret_cast<uint16_t*>(
        static_cast<uint8_t*>(g_event_queue.allocation.virtual_address) + layout.avail_offset + sizeof(VirtqAvailHeader)
    );
}

VirtqUsedHeader* queue_used_header(const QueueLayout& layout) {
    return reinterpret_cast<VirtqUsedHeader*>(
        static_cast<uint8_t*>(g_event_queue.allocation.virtual_address) + layout.used_offset
    );
}

VirtqUsedElement* queue_used_ring(const QueueLayout& layout) {
    return reinterpret_cast<VirtqUsedElement*>(
        static_cast<uint8_t*>(g_event_queue.allocation.virtual_address) + layout.used_offset + sizeof(VirtqUsedHeader)
    );
}

VirtioInputEvent* queue_event_buffer(const QueueLayout& layout, uint16_t index) {
    return reinterpret_cast<VirtioInputEvent*>(
        static_cast<uint8_t*>(g_event_queue.allocation.virtual_address) + layout.buffers_offset + (sizeof(VirtioInputEvent) * index)
    );
}

uint64_t queue_event_buffer_physical(const QueueLayout& layout, uint16_t index) {
    return g_event_queue.allocation.physical_address + layout.buffers_offset + (sizeof(VirtioInputEvent) * index);
}

uint16_t choose_queue_size(uint16_t maximum) {
    if (maximum == 0) {
        return 0;
    }

    uint16_t chosen = maximum > kMaxEventQueueSize ? kMaxEventQueueSize : maximum;
    uint16_t power_of_two = 1;
    while (static_cast<uint16_t>(power_of_two << 1) != 0 &&
           static_cast<uint16_t>(power_of_two << 1) <= chosen) {
        power_of_two = static_cast<uint16_t>(power_of_two << 1);
    }
    return power_of_two;
}

void notify_queue(uint16_t queue_notify_off) {
    if (!g_notify_view.valid || g_notify_view.bar == nullptr || g_notify_view.extra == 0) {
        return;
    }

    volatile uint16_t* notify = reinterpret_cast<volatile uint16_t*>(
        g_notify_view.bar->base + g_notify_view.offset_within_bar + (queue_notify_off * g_notify_view.extra)
    );
    *notify = kVirtioInputEventQueue;
}

bool setup_event_queue() {
    volatile VirtioPciCommonCfg* cfg = common_cfg();
    cfg->queue_select = kVirtioInputEventQueue;
    memory_barrier();

    const uint16_t chosen_size = choose_queue_size(cfg->queue_size);
    if (chosen_size == 0) {
        return false;
    }

    const QueueLayout layout = build_queue_layout(chosen_size);
    const uint64_t page_count = static_cast<uint64_t>((layout.total_bytes + memory::kPageSize - 1) / memory::kPageSize);
    if (!memory::allocate_contiguous_pages(page_count, g_event_queue.allocation)) {
        return false;
    }
    memset(g_event_queue.allocation.virtual_address, 0, g_event_queue.allocation.page_count * memory::kPageSize);

    g_event_queue.size = chosen_size;
    g_event_queue.notify_off = cfg->queue_notify_off;
    g_event_queue.last_used_index = 0;
    g_event_queue.next_avail_index = 0;
    g_event_queue.enabled = false;

    VirtqDescriptor* descriptors = queue_descriptors(layout);
    VirtqUsedHeader* used = queue_used_header(layout);
    used->flags = 0;
    used->idx = 0;

    for (uint16_t index = 0; index < chosen_size; ++index) {
        descriptors[index] = {
            .addr = queue_event_buffer_physical(layout, index),
            .len = sizeof(VirtioInputEvent),
            .flags = kVirtqueueDescriptorWrite,
            .next = 0,
        };
    }

    cfg->queue_desc = g_event_queue.allocation.physical_address + layout.desc_offset;
    cfg->queue_driver = g_event_queue.allocation.physical_address + layout.avail_offset;
    cfg->queue_device = g_event_queue.allocation.physical_address + layout.used_offset;
    cfg->queue_enable = 1;
    memory_barrier();
    g_event_queue.enabled = cfg->queue_enable != 0;
    if (!g_event_queue.enabled) {
        return false;
    }

    VirtqAvailHeader* avail = queue_avail_header(layout);
    uint16_t* ring = queue_avail_ring(layout);
    for (uint16_t index = 0; index < chosen_size; ++index) {
        ring[g_event_queue.next_avail_index % chosen_size] = index;
        g_event_queue.next_avail_index = static_cast<uint16_t>(g_event_queue.next_avail_index + 1);
    }
    memory_barrier();
    avail->idx = g_event_queue.next_avail_index;
    memory_barrier();
    notify_queue(g_event_queue.notify_off);
    return true;
}

void recycle_event_buffer(uint16_t descriptor_id) {
    const QueueLayout layout = build_queue_layout(g_event_queue.size);
    VirtqAvailHeader* avail = queue_avail_header(layout);
    uint16_t* ring = queue_avail_ring(layout);

    ring[g_event_queue.next_avail_index % g_event_queue.size] = descriptor_id;
    g_event_queue.next_avail_index = static_cast<uint16_t>(g_event_queue.next_avail_index + 1);
    memory_barrier();
    avail->idx = g_event_queue.next_avail_index;
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

} // namespace

namespace virtio_input {

void initialize(const boot::FramebufferInfo& framebuffer) {
    memset(g_bars, 0, sizeof(g_bars));
    g_common_view = {};
    g_notify_view = {};
    g_isr_view = {};
    g_device_view = {};
    g_event_queue = {};
    g_ready = false;
    g_buttons = 0;
    g_have_abs_x = false;
    g_have_abs_y = false;
    g_have_screen_position = false;
    g_framebuffer_width = framebuffer.available ? static_cast<uint32_t>(framebuffer.width) : 0u;
    g_framebuffer_height = framebuffer.available ? static_cast<uint32_t>(framebuffer.height) : 0u;

    if (!pci::ready() || !find_device(g_device)) {
        return;
    }

    if (!resolve_capability(kVirtioPciCapCommonCfg, g_common_view) ||
        !resolve_capability(kVirtioPciCapNotifyCfg, g_notify_view) ||
        !resolve_capability(kVirtioPciCapDeviceCfg, g_device_view)) {
        console::write_line("virtio-input: missing required MMIO capabilities");
        return;
    }
    (void)resolve_capability(kVirtioPciCapIsrCfg, g_isr_view);

    uint16_t command = pci::read_config_u16(g_device.bus, g_device.slot, g_device.function, kPciCommandOffset);
    command = static_cast<uint16_t>(command | kPciCommandMemory | kPciCommandBusMaster);
    pci::write_config_u16(g_device.bus, g_device.slot, g_device.function, kPciCommandOffset, command);

    set_device_status(0);
    memory_barrier();
    set_device_status(static_cast<uint8_t>(kVirtioStatusAcknowledge | kVirtioStatusDriver));

    if (!negotiate_features()) {
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

    set_device_status(static_cast<uint8_t>(device_status() | kVirtioStatusDriverOk));
    g_ready = true;
    console::printf(
        "virtio-input: tablet ready pci=%x:%x.%u abs_x=%d..%d abs_y=%d..%d\n",
        static_cast<unsigned>(g_device.bus),
        static_cast<unsigned>(g_device.slot),
        static_cast<unsigned>(g_device.function),
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

    const QueueLayout layout = build_queue_layout(g_event_queue.size);
    VirtqUsedHeader* used = queue_used_header(layout);
    VirtqUsedElement* ring = queue_used_ring(layout);
    bool notified = false;

    while (g_event_queue.last_used_index != used->idx) {
        memory_barrier();
        const VirtqUsedElement element = ring[g_event_queue.last_used_index % g_event_queue.size];
        if (element.id < g_event_queue.size) {
            const VirtioInputEvent* event = queue_event_buffer(layout, static_cast<uint16_t>(element.id));
            process_event(*event);
            recycle_event_buffer(static_cast<uint16_t>(element.id));
            notified = true;
        }
        g_event_queue.last_used_index = static_cast<uint16_t>(g_event_queue.last_used_index + 1);
    }

    if (notified) {
        memory_barrier();
        notify_queue(g_event_queue.notify_off);
    }
}

bool mouse_ready() {
    return g_ready;
}

void begin_graphics_session() {
    g_have_screen_position = false;
}

void end_graphics_session() {
    g_have_screen_position = false;
}

} // namespace virtio_input
