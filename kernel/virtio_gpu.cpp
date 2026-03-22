#include "kernel/virtio_gpu.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/device.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "kernel/ui.hpp"
#include "kernel/virtio_pci.hpp"

namespace {

constexpr uint16_t kVirtioGpuModernDevice = 0x1050u;
constexpr uint16_t kVirtioGpuSubsystemDevice = 16u;
constexpr uint16_t kVirtioGpuControlQueue = 0;
constexpr uint16_t kVirtioGpuQueueDescriptorLimit = 16;
constexpr uint32_t kVirtioGpuFormatB8G8R8X8Unorm = 2u;
constexpr uint32_t kVirtioGpuFlagFence = 1u << 0;
constexpr uint32_t kVirtioGpuCmdGetDisplayInfo = 0x0100u;
constexpr uint32_t kVirtioGpuCmdResourceCreate2d = 0x0101u;
constexpr uint32_t kVirtioGpuCmdResourceUnref = 0x0102u;
constexpr uint32_t kVirtioGpuCmdSetScanout = 0x0103u;
constexpr uint32_t kVirtioGpuCmdResourceFlush = 0x0104u;
constexpr uint32_t kVirtioGpuCmdTransferToHost2d = 0x0105u;
constexpr uint32_t kVirtioGpuCmdResourceAttachBacking = 0x0106u;
constexpr uint32_t kVirtioGpuRespOkNoData = 0x1100u;
constexpr uint32_t kVirtioGpuRespOkDisplayInfo = 0x1101u;
constexpr uint32_t kGpuSurfaceCount = 3u;
constexpr uint32_t kFirstGpuResourceId = 1u;
constexpr size_t kRequestBufferBytes = 512;
constexpr size_t kResponseBufferBytes = 512;
constexpr size_t kCommandSlotBytes = kRequestBufferBytes + kResponseBufferBytes;
constexpr uint16_t kCommandSlotCount = kVirtioGpuQueueDescriptorLimit / 2u;
constexpr size_t kQueueExtraBytes = kCommandSlotBytes * kCommandSlotCount;
constexpr size_t kResponseBufferOffset = kRequestBufferBytes;
constexpr uint32_t kCommandTimeoutSpins = 5000000u;

struct [[gnu::packed]] VirtioGpuCtrlHdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};

struct [[gnu::packed]] VirtioGpuRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct [[gnu::packed]] VirtioGpuDisplayOne {
    VirtioGpuRect rect;
    uint32_t enabled;
    uint32_t flags;
};

struct [[gnu::packed]] VirtioGpuRespDisplayInfo {
    VirtioGpuCtrlHdr header;
    VirtioGpuDisplayOne scanouts[16];
};

struct [[gnu::packed]] VirtioGpuConfig {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
};

struct [[gnu::packed]] VirtioGpuResourceCreate2d {
    VirtioGpuCtrlHdr header;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct [[gnu::packed]] VirtioGpuResourceUnref {
    VirtioGpuCtrlHdr header;
    uint32_t resource_id;
    uint32_t padding;
};

struct [[gnu::packed]] VirtioGpuMemEntry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

struct [[gnu::packed]] VirtioGpuResourceAttachBacking {
    VirtioGpuCtrlHdr header;
    uint32_t resource_id;
    uint32_t nr_entries;
    VirtioGpuMemEntry entry;
};

struct [[gnu::packed]] VirtioGpuSetScanout {
    VirtioGpuCtrlHdr header;
    VirtioGpuRect rect;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct [[gnu::packed]] VirtioGpuTransferToHost2d {
    VirtioGpuCtrlHdr header;
    VirtioGpuRect rect;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct [[gnu::packed]] VirtioGpuResourceFlush {
    VirtioGpuCtrlHdr header;
    VirtioGpuRect rect;
    uint32_t resource_id;
    uint32_t padding;
};

enum CommandStage : uint8_t {
    kCommandStageNone = 0,
    kCommandStageSync = 1,
    kCommandStagePresentTransfer = 2,
    kCommandStagePresentFlush = 3,
    kCommandStagePresentScanout = 4,
};

struct CommandSlot {
    bool in_use;
    bool completed;
    uint16_t head_descriptor;
    uint16_t response_bytes;
    uint32_t response_type;
    CommandStage stage;
    uint32_t pending_present_index;
};

struct PendingPresent {
    bool in_use;
    bool completed;
    bool failed;
    uint32_t surface_index;
    uint32_t pending_commands;
};

device::Device g_gpu_device = {
    .name = "gpu0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
};

virtio_pci::Device g_device = {};
virtio_pci::Queue g_control_queue = {};
struct Surface {
    memory::PageAllocation allocation;
    uint32_t resource_id;
};
Surface* surface_at(uint32_t surface_index);
Surface* front_surface();

Surface g_surfaces[kGpuSurfaceCount] = {};
savanxp_fb_info g_framebuffer_info = {};
savanxp_gpu_info g_gpu_info = {};
uint32_t g_scanout_id = 0;
uint32_t g_scanout_width = 0;
uint32_t g_scanout_height = 0;
uint32_t g_front_surface_index = 0;
uint64_t g_next_fence_id = 1;
bool g_present = false;
bool g_ready = false;
uint32_t g_last_response_type = 0;
uint16_t g_command_slot_capacity = 0;
CommandSlot g_command_slots[kCommandSlotCount] = {};
PendingPresent g_pending_presents[kGpuSurfaceCount] = {};
uint32_t g_pending_present_order[kGpuSurfaceCount] = {};
uint32_t g_pending_present_count = 0;

int negative_error(savanxp_error_code code) {
    return -static_cast<int>(code);
}

volatile VirtioGpuConfig* device_cfg() {
    return reinterpret_cast<volatile VirtioGpuConfig*>(virtio_pci::device_cfg_base(g_device));
}

void initialize_header(VirtioGpuCtrlHdr& header, uint32_t type) {
    memset(&header, 0, sizeof(header));
    header.type = type;
    header.flags = kVirtioGpuFlagFence;
    header.fence_id = g_next_fence_id++;
}

uint8_t* slot_request_buffer(uint16_t slot_index) {
    return virtio_pci::queue_extra(g_control_queue, static_cast<size_t>(slot_index) * kCommandSlotBytes);
}

uint8_t* slot_response_buffer(uint16_t slot_index) {
    return slot_request_buffer(slot_index) + kResponseBufferOffset;
}

uint64_t slot_request_physical(uint16_t slot_index) {
    return virtio_pci::queue_extra_physical(g_control_queue, static_cast<size_t>(slot_index) * kCommandSlotBytes);
}

uint64_t slot_response_physical(uint16_t slot_index) {
    return slot_request_physical(slot_index) + kResponseBufferOffset;
}

void reset_present_tracking() {
    memset(g_command_slots, 0, sizeof(g_command_slots));
    memset(g_pending_presents, 0, sizeof(g_pending_presents));
    memset(g_pending_present_order, 0, sizeof(g_pending_present_order));
    g_pending_present_count = 0;
}

bool reserve_command_slot(uint16_t& slot_index) {
    for (uint16_t index = 0; index < g_command_slot_capacity; ++index) {
        if (g_command_slots[index].in_use) {
            continue;
        }
        g_command_slots[index] = {};
        g_command_slots[index].in_use = true;
        g_command_slots[index].head_descriptor = static_cast<uint16_t>(index * 2u);
        slot_index = index;
        return true;
    }
    return false;
}

void release_command_slot(uint16_t slot_index) {
    if (slot_index >= g_command_slot_capacity) {
        return;
    }
    g_command_slots[slot_index] = {};
}

bool reserve_pending_present(uint32_t surface_index, uint32_t& pending_index) {
    for (uint32_t index = 0; index < kGpuSurfaceCount; ++index) {
        if (g_pending_presents[index].in_use) {
            continue;
        }
        g_pending_presents[index] = {
            .in_use = true,
            .completed = false,
            .failed = false,
            .surface_index = surface_index,
            .pending_commands = 0,
        };
        g_pending_present_order[g_pending_present_count++] = index;
        pending_index = index;
        return true;
    }
    return false;
}

void release_pending_head() {
    if (g_pending_present_count == 0) {
        return;
    }
    for (uint32_t index = 1; index < g_pending_present_count; ++index) {
        g_pending_present_order[index - 1] = g_pending_present_order[index];
    }
    g_pending_present_count -= 1;
}

bool submit_command_slot(uint16_t slot_index, const void* request, size_t request_bytes, size_t response_bytes) {
    if (!g_ready || !g_control_queue.enabled || slot_index >= g_command_slot_capacity || request == nullptr ||
        request_bytes == 0 || response_bytes == 0 ||
        request_bytes > kRequestBufferBytes || response_bytes > kResponseBufferBytes) {
        return false;
    }

    CommandSlot& slot = g_command_slots[slot_index];
    if (!slot.in_use) {
        return false;
    }

    void* request_buffer = slot_request_buffer(slot_index);
    void* response_buffer = slot_response_buffer(slot_index);
    memcpy(request_buffer, request, request_bytes);
    memset(response_buffer, 0, response_bytes);
    slot.completed = false;
    slot.response_bytes = static_cast<uint16_t>(response_bytes);
    slot.response_type = 0;

    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_control_queue);
    const uint16_t head = slot.head_descriptor;
    descriptors[head] = {
        .addr = slot_request_physical(slot_index),
        .len = static_cast<uint32_t>(request_bytes),
        .flags = virtio_pci::kDescriptorFlagNext,
        .next = static_cast<uint16_t>(head + 1u),
    };
    descriptors[head + 1u] = {
        .addr = slot_response_physical(slot_index),
        .len = static_cast<uint32_t>(response_bytes),
        .flags = virtio_pci::kDescriptorFlagWrite,
        .next = 0,
    };

    if (!virtio_pci::submit_descriptor_head(g_control_queue, head)) {
        return false;
    }
    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_control_queue);
    return true;
}

void finalize_completed_presents() {
    while (g_pending_present_count != 0) {
        PendingPresent& present = g_pending_presents[g_pending_present_order[0]];
        if (!present.in_use || !present.completed) {
            break;
        }

        if (!present.failed) {
            g_front_surface_index = present.surface_index;
            Surface* surface = surface_at(g_front_surface_index);
            if (surface != nullptr) {
                console::set_external_framebuffer(surface->allocation.virtual_address, g_framebuffer_info);
            }
        }

        present = {};
        release_pending_head();
    }
}

void poll_command_completions() {
    if (!g_ready || !g_control_queue.enabled) {
        return;
    }

    const volatile virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_control_queue);
    const virtio_pci::UsedElement* ring = virtio_pci::queue_used_ring(g_control_queue);

    while (true) {
        virtio_pci::memory_barrier();
        if (g_control_queue.last_used_index == used->idx) {
            break;
        }

        const virtio_pci::UsedElement element = ring[g_control_queue.last_used_index % g_control_queue.size];
        g_control_queue.last_used_index = static_cast<uint16_t>(g_control_queue.last_used_index + 1u);
        if (element.id >= g_control_queue.size || (element.id % 2u) != 0) {
            continue;
        }

        const uint16_t slot_index = static_cast<uint16_t>(element.id / 2u);
        if (slot_index >= g_command_slot_capacity) {
            continue;
        }

        CommandSlot& slot = g_command_slots[slot_index];
        if (!slot.in_use || slot.head_descriptor != element.id) {
            continue;
        }

        const VirtioGpuCtrlHdr* response = reinterpret_cast<const VirtioGpuCtrlHdr*>(slot_response_buffer(slot_index));
        slot.completed = true;
        slot.response_type = response->type;
        g_last_response_type = slot.response_type;

        if (slot.stage != kCommandStageSync && slot.pending_present_index < kGpuSurfaceCount) {
            PendingPresent& present = g_pending_presents[slot.pending_present_index];
            if (present.in_use) {
                if (slot.response_type != kVirtioGpuRespOkNoData) {
                    present.failed = true;
                }
                if (present.pending_commands != 0) {
                    present.pending_commands -= 1u;
                }
                if (present.pending_commands == 0) {
                    present.completed = true;
                }
            }
            release_command_slot(slot_index);
        }
    }

    finalize_completed_presents();
}

bool wait_for_command_slot(uint16_t slot_index) {
    if (slot_index >= g_command_slot_capacity || !g_command_slots[slot_index].in_use) {
        return false;
    }

    for (uint32_t spin = 0; spin < kCommandTimeoutSpins; ++spin) {
        poll_command_completions();
        if (g_command_slots[slot_index].completed) {
            return true;
        }
    }
    return false;
}

bool submit_command(const void* request, size_t request_bytes, void* response, size_t response_bytes) {
    if (response == nullptr) {
        return false;
    }

    uint16_t slot_index = 0;
    if (!reserve_command_slot(slot_index)) {
        return false;
    }

    g_command_slots[slot_index].stage = kCommandStageSync;
    g_command_slots[slot_index].pending_present_index = 0;
    if (!submit_command_slot(slot_index, request, request_bytes, response_bytes) || !wait_for_command_slot(slot_index)) {
        release_command_slot(slot_index);
        return false;
    }

    memcpy(response, slot_response_buffer(slot_index), response_bytes);
    release_command_slot(slot_index);
    return true;
}

bool send_ok_nodata_command(const void* request, size_t request_bytes) {
    VirtioGpuCtrlHdr response = {};
    if (!submit_command(request, request_bytes, &response, sizeof(response))) {
        g_last_response_type = 0;
        return false;
    }
    g_last_response_type = response.type;
    return response.type == kVirtioGpuRespOkNoData;
}

bool get_display_info(VirtioGpuRespDisplayInfo& display_info) {
    VirtioGpuCtrlHdr request = {};
    initialize_header(request, kVirtioGpuCmdGetDisplayInfo);
    if (!submit_command(&request, sizeof(request), &display_info, sizeof(display_info))) {
        g_last_response_type = 0;
        return false;
    }
    g_last_response_type = display_info.header.type;
    return display_info.header.type == kVirtioGpuRespOkDisplayInfo;
}

Surface* surface_at(uint32_t surface_index) {
    if (surface_index >= kGpuSurfaceCount) {
        return nullptr;
    }
    return &g_surfaces[surface_index];
}

Surface* front_surface() {
    return surface_at(g_front_surface_index);
}

bool surface_pending(uint32_t surface_index) {
    for (uint32_t index = 0; index < kGpuSurfaceCount; ++index) {
        if (g_pending_presents[index].in_use && g_pending_presents[index].surface_index == surface_index) {
            return true;
        }
    }
    return false;
}

bool surface_available(uint32_t surface_index) {
    return surface_index < kGpuSurfaceCount &&
        surface_index != g_front_surface_index &&
        !surface_pending(surface_index);
}

bool find_available_surface(uint32_t& surface_index) {
    poll_command_completions();
    for (uint32_t index = 0; index < kGpuSurfaceCount; ++index) {
        if (surface_available(index)) {
            surface_index = index;
            return true;
        }
    }
    return false;
}

bool wait_for_available_surface(uint32_t& surface_index) {
    if (find_available_surface(surface_index)) {
        return true;
    }

    for (uint32_t spin = 0; spin < kCommandTimeoutSpins; ++spin) {
        poll_command_completions();
        if (find_available_surface(surface_index)) {
            return true;
        }
    }
    return false;
}

bool acquire_present_surface(uint32_t& surface_index) {
    return find_available_surface(surface_index) || wait_for_available_surface(surface_index);
}

void wait_for_idle_internal() {
    while (g_pending_present_count != 0) {
        bool progressed = false;
        for (uint32_t spin = 0; spin < kCommandTimeoutSpins; ++spin) {
            const uint32_t before = g_pending_present_count;
            poll_command_completions();
            if (g_pending_present_count < before) {
                progressed = true;
                break;
            }
        }
        if (!progressed) {
            break;
        }
    }
}

void release_surface_allocation(uint32_t surface_index) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return;
    }

    if (surface->allocation.physical_address != 0 && surface->allocation.page_count != 0) {
        (void)memory::free_allocation(surface->allocation);
    }
    memset(&surface->allocation, 0, sizeof(surface->allocation));
}

void release_surface_allocations() {
    wait_for_idle_internal();
    for (uint32_t surface_index = 0; surface_index < kGpuSurfaceCount; ++surface_index) {
        release_surface_allocation(surface_index);
        g_surfaces[surface_index].resource_id = 0;
    }
    reset_present_tracking();
    g_front_surface_index = 0;
}

bool create_surface_resource(uint32_t surface_index) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return false;
    }

    VirtioGpuResourceCreate2d request = {};
    initialize_header(request.header, kVirtioGpuCmdResourceCreate2d);
    request.resource_id = surface->resource_id;
    request.format = kVirtioGpuFormatB8G8R8X8Unorm;
    request.width = g_framebuffer_info.width;
    request.height = g_framebuffer_info.height;
    return send_ok_nodata_command(&request, sizeof(request));
}

bool destroy_surface_resource(uint32_t surface_index) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr || surface->resource_id == 0) {
        return false;
    }

    VirtioGpuResourceUnref request = {};
    initialize_header(request.header, kVirtioGpuCmdResourceUnref);
    request.resource_id = surface->resource_id;
    return send_ok_nodata_command(&request, sizeof(request));
}

bool attach_surface_backing(uint32_t surface_index) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return false;
    }

    VirtioGpuResourceAttachBacking request = {};
    initialize_header(request.header, kVirtioGpuCmdResourceAttachBacking);
    request.resource_id = surface->resource_id;
    request.nr_entries = 1;
    request.entry.addr = surface->allocation.physical_address;
    request.entry.length = g_framebuffer_info.buffer_size;
    return send_ok_nodata_command(&request, sizeof(request));
}

bool build_scanout_request(uint32_t surface_index, VirtioGpuSetScanout& request) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return false;
    }

    request = {};
    initialize_header(request.header, kVirtioGpuCmdSetScanout);
    request.rect = {
        .x = 0,
        .y = 0,
        .width = g_framebuffer_info.width,
        .height = g_framebuffer_info.height,
    };
    request.scanout_id = g_scanout_id;
    request.resource_id = surface->resource_id;
    return true;
}

bool set_scanout_surface(uint32_t surface_index) {
    VirtioGpuSetScanout request = {};
    return build_scanout_request(surface_index, request) &&
        send_ok_nodata_command(&request, sizeof(request));
}

bool build_transfer_request(uint32_t surface_index, uint32_t x, uint32_t y, uint32_t width, uint32_t height, VirtioGpuTransferToHost2d& request) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return false;
    }

    request = {};
    initialize_header(request.header, kVirtioGpuCmdTransferToHost2d);
    request.rect = {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
    };
    request.offset = (static_cast<uint64_t>(y) * g_framebuffer_info.pitch) + (static_cast<uint64_t>(x) * sizeof(uint32_t));
    request.resource_id = surface->resource_id;
    return true;
}

bool transfer_rect(uint32_t surface_index, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    VirtioGpuTransferToHost2d request = {};
    return build_transfer_request(surface_index, x, y, width, height, request) &&
        send_ok_nodata_command(&request, sizeof(request));
}

bool build_flush_request(uint32_t surface_index, uint32_t x, uint32_t y, uint32_t width, uint32_t height, VirtioGpuResourceFlush& request) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return false;
    }

    request = {};
    initialize_header(request.header, kVirtioGpuCmdResourceFlush);
    request.rect = {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
    };
    request.resource_id = surface->resource_id;
    return true;
}

bool flush_rect_internal(uint32_t surface_index, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    VirtioGpuResourceFlush request = {};
    return build_flush_request(surface_index, x, y, width, height, request) &&
        send_ok_nodata_command(&request, sizeof(request));
}

bool copy_region_to_surface(uint32_t surface_index, const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr || surface->allocation.virtual_address == nullptr) {
        return false;
    }

    const uint8_t* source = static_cast<const uint8_t*>(pixels);
    uint8_t* destination = static_cast<uint8_t*>(surface->allocation.virtual_address);
    const size_t row_bytes = static_cast<size_t>(width) * sizeof(uint32_t);

    for (uint32_t row = 0; row < height; ++row) {
        const size_t source_offset = static_cast<size_t>(y + row) * source_pitch + (static_cast<size_t>(x) * sizeof(uint32_t));
        const size_t destination_offset = static_cast<size_t>(y + row) * g_framebuffer_info.pitch + (static_cast<size_t>(x) * sizeof(uint32_t));
        memcpy(destination + destination_offset, source + source_offset, row_bytes);
    }
    return true;
}

bool clone_surface(uint32_t destination_surface_index, uint32_t source_surface_index) {
    Surface* destination_surface = surface_at(destination_surface_index);
    Surface* source_surface = surface_at(source_surface_index);
    if (destination_surface == nullptr || source_surface == nullptr ||
        destination_surface->allocation.virtual_address == nullptr ||
        source_surface->allocation.virtual_address == nullptr) {
        return false;
    }

    memcpy(destination_surface->allocation.virtual_address, source_surface->allocation.virtual_address, g_framebuffer_info.buffer_size);
    return true;
}

bool present_surface_sync(uint32_t surface_index, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!set_scanout_surface(surface_index) ||
        !transfer_rect(surface_index, x, y, width, height) ||
        !flush_rect_internal(surface_index, x, y, width, height)) {
        return false;
    }

    g_front_surface_index = surface_index;
    Surface* surface = front_surface();
    if (surface != nullptr) {
        console::set_external_framebuffer(surface->allocation.virtual_address, g_framebuffer_info);
    }
    return true;
}

bool submit_present_surface(uint32_t surface_index, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    uint32_t pending_index = 0;
    uint16_t slot_indices[3] = {};
    VirtioGpuTransferToHost2d transfer_request = {};
    VirtioGpuResourceFlush flush_request = {};
    VirtioGpuSetScanout scanout_request = {};

    if (!reserve_pending_present(surface_index, pending_index) ||
        !reserve_command_slot(slot_indices[0]) ||
        !reserve_command_slot(slot_indices[1]) ||
        !reserve_command_slot(slot_indices[2]) ||
        !build_transfer_request(surface_index, x, y, width, height, transfer_request) ||
        !build_flush_request(surface_index, x, y, width, height, flush_request) ||
        !build_scanout_request(surface_index, scanout_request)) {
        for (uint32_t slot = 0; slot < 3; ++slot) {
            release_command_slot(slot_indices[slot]);
        }
        if (pending_index < kGpuSurfaceCount && g_pending_presents[pending_index].in_use) {
            g_pending_presents[pending_index] = {};
            if (g_pending_present_count != 0 && g_pending_present_order[g_pending_present_count - 1] == pending_index) {
                g_pending_present_count -= 1u;
            }
        }
        return false;
    }

    g_pending_presents[pending_index].pending_commands = 3;

    g_command_slots[slot_indices[0]].stage = kCommandStagePresentTransfer;
    g_command_slots[slot_indices[0]].pending_present_index = pending_index;
    g_command_slots[slot_indices[1]].stage = kCommandStagePresentFlush;
    g_command_slots[slot_indices[1]].pending_present_index = pending_index;
    g_command_slots[slot_indices[2]].stage = kCommandStagePresentScanout;
    g_command_slots[slot_indices[2]].pending_present_index = pending_index;

    if (!submit_command_slot(slot_indices[0], &transfer_request, sizeof(transfer_request), sizeof(VirtioGpuCtrlHdr)) ||
        !submit_command_slot(slot_indices[1], &flush_request, sizeof(flush_request), sizeof(VirtioGpuCtrlHdr)) ||
        !submit_command_slot(slot_indices[2], &scanout_request, sizeof(scanout_request), sizeof(VirtioGpuCtrlHdr))) {
        g_pending_presents[pending_index].failed = true;
        g_pending_presents[pending_index].completed = true;
        g_pending_presents[pending_index].pending_commands = 0;
        finalize_completed_presents();
        return false;
    }

    return true;
}

bool choose_scanout(const VirtioGpuRespDisplayInfo& display_info, const boot::FramebufferInfo& boot_framebuffer) {
    g_scanout_id = 0;
    g_scanout_width = 0;
    g_scanout_height = 0;

    const uint32_t scanout_count = device_cfg()->num_scanouts < 16 ? device_cfg()->num_scanouts : 16;
    for (uint32_t index = 0; index < scanout_count; ++index) {
        const VirtioGpuDisplayOne& scanout = display_info.scanouts[index];
        if (scanout.rect.width == 0 || scanout.rect.height == 0) {
            continue;
        }
        g_scanout_id = index;
        g_scanout_width = scanout.rect.width;
        g_scanout_height = scanout.rect.height;
        if (scanout.enabled != 0) {
            break;
        }
    }
    if (g_scanout_width == 0 || g_scanout_height == 0) {
        return false;
    }

    (void)boot_framebuffer;
    const uint32_t width = g_scanout_width;
    const uint32_t height = g_scanout_height;

    g_framebuffer_info = {
        .width = width,
        .height = height,
        .pitch = static_cast<uint32_t>(width * sizeof(uint32_t)),
        .bpp = 32,
        .buffer_size = static_cast<uint32_t>(width * height * sizeof(uint32_t)),
    };
    g_gpu_info = {
        .width = width,
        .height = height,
        .pitch = static_cast<uint32_t>(width * sizeof(uint32_t)),
        .bpp = 32,
        .buffer_size = static_cast<uint32_t>(width * height * sizeof(uint32_t)),
        .backend = SAVANXP_GPU_BACKEND_VIRTIO,
        .flags = 0,
    };
    return true;
}

void set_framebuffer_mode(uint32_t width, uint32_t height) {
    g_framebuffer_info = {
        .width = width,
        .height = height,
        .pitch = static_cast<uint32_t>(width * sizeof(uint32_t)),
        .bpp = 32,
        .buffer_size = static_cast<uint32_t>(width * height * sizeof(uint32_t)),
    };
    g_gpu_info = {
        .width = width,
        .height = height,
        .pitch = static_cast<uint32_t>(width * sizeof(uint32_t)),
        .bpp = 32,
        .buffer_size = static_cast<uint32_t>(width * height * sizeof(uint32_t)),
        .backend = SAVANXP_GPU_BACKEND_VIRTIO,
        .flags = 0,
    };
}

bool configure_primary_surface(uint32_t width, uint32_t height) {
    bool resource_created[kGpuSurfaceCount] = {};

    release_surface_allocations();
    set_framebuffer_mode(width, height);

    const uint64_t page_count = static_cast<uint64_t>((g_framebuffer_info.buffer_size + memory::kPageSize - 1) / memory::kPageSize);
    for (uint32_t surface_index = 0; surface_index < kGpuSurfaceCount; ++surface_index) {
        Surface* surface = surface_at(surface_index);
        if (surface == nullptr) {
            continue;
        }
        surface->resource_id = kFirstGpuResourceId + surface_index;
        if (!memory::allocate_contiguous_pages(page_count, surface->allocation)) {
            release_surface_allocations();
            return false;
        }
        memset(surface->allocation.virtual_address, 0, surface->allocation.page_count * memory::kPageSize);
    }

    for (uint32_t surface_index = 0; surface_index < kGpuSurfaceCount; ++surface_index) {
        if (!create_surface_resource(surface_index)) {
            for (uint32_t destroy_index = 0; destroy_index < surface_index; ++destroy_index) {
                if (resource_created[destroy_index]) {
                    (void)destroy_surface_resource(destroy_index);
                }
            }
            release_surface_allocations();
            return false;
        }
        resource_created[surface_index] = true;
    }

    for (uint32_t surface_index = 0; surface_index < kGpuSurfaceCount; ++surface_index) {
        if (!attach_surface_backing(surface_index)) {
            for (uint32_t destroy_index = 0; destroy_index < kGpuSurfaceCount; ++destroy_index) {
                if (resource_created[destroy_index]) {
                    (void)destroy_surface_resource(destroy_index);
                }
            }
            release_surface_allocations();
            return false;
        }
    }

    g_front_surface_index = 0;
    if (!present_surface_sync(g_front_surface_index, 0, 0, g_framebuffer_info.width, g_framebuffer_info.height)) {
        for (uint32_t surface_index = 0; surface_index < kGpuSurfaceCount; ++surface_index) {
            if (resource_created[surface_index]) {
                (void)destroy_surface_resource(surface_index);
            }
        }
        release_surface_allocations();
        return false;
    }

    return true;
}

int gpu_ioctl(uint64_t request, uint64_t argument) {
    switch (request) {
        case GPU_IOC_GET_INFO: {
            if (!process::validate_user_range(argument, sizeof(g_gpu_info), true)) {
                return negative_error(SAVANXP_EINVAL);
            }
            return process::copy_to_user(argument, &g_gpu_info, sizeof(g_gpu_info)) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        case GPU_IOC_ACQUIRE: {
            const uint32_t pid = process::current_pid();
            if (pid == 0) {
                return negative_error(SAVANXP_EBADF);
            }
            return ui::acquire_graphics_session(pid) ? 0 : negative_error(SAVANXP_EBUSY);
        }
        case GPU_IOC_RELEASE:
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            ui::release_graphics_session(process::current_pid());
            return 0;
        case GPU_IOC_PRESENT: {
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            if (!process::validate_user_range(argument, g_framebuffer_info.buffer_size, false)) {
                return negative_error(SAVANXP_EINVAL);
            }
            return virtio_gpu::present_from_kernel(reinterpret_cast<const void*>(argument), g_framebuffer_info.buffer_size)
                ? 0
                : negative_error(SAVANXP_EIO);
        }
        case GPU_IOC_PRESENT_REGION: {
            savanxp_gpu_present_region region = {};
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(region), true) ||
                !process::copy_from_user(&region, argument, sizeof(region))) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (region.pixels == 0 || region.source_pitch == 0 || region.width == 0 || region.height == 0 ||
                region.x >= g_framebuffer_info.width || region.y >= g_framebuffer_info.height ||
                region.width > (g_framebuffer_info.width - region.x) ||
                region.height > (g_framebuffer_info.height - region.y)) {
                return negative_error(SAVANXP_EINVAL);
            }

            const uint64_t row_bytes = static_cast<uint64_t>(region.width) * sizeof(uint32_t);
            const uint64_t last_row = static_cast<uint64_t>(region.y + region.height - 1);
            const uint64_t touched_bytes = (last_row * region.source_pitch) +
                (static_cast<uint64_t>(region.x) * sizeof(uint32_t)) + row_bytes;
            if (!process::validate_user_range(region.pixels, touched_bytes, false)) {
                return negative_error(SAVANXP_EINVAL);
            }

            return virtio_gpu::present_region_from_kernel(
                reinterpret_cast<const void*>(region.pixels),
                region.source_pitch,
                region.x,
                region.y,
                region.width,
                region.height
            ) ? 0 : negative_error(SAVANXP_EIO);
        }
        default:
            return negative_error(SAVANXP_ENOSYS);
    }
}

void gpu_close() {
    if (ui::owns_graphics_session(process::current_pid())) {
        ui::release_graphics_session(process::current_pid());
    }
}

void fail_device(const char* reason) {
    virtio_pci::fail_device(g_device);
    if (reason != nullptr) {
        console::printf("virtio-gpu: %s\n", reason);
    }
    g_ready = false;
}

void log_command_failure(const char* stage) {
    if (stage == nullptr) {
        stage = "unknown stage";
    }

    if (g_last_response_type != 0) {
        console::printf(
            "virtio-gpu: %s failed (resp=0x%x)\n",
            stage,
            static_cast<unsigned>(g_last_response_type)
        );
    } else {
        console::printf("virtio-gpu: %s failed (no response)\n", stage);
    }
}

} // namespace

namespace virtio_gpu {

void initialize(const boot::FramebufferInfo& framebuffer) {
    memset(&g_device, 0, sizeof(g_device));
    memset(&g_control_queue, 0, sizeof(g_control_queue));
    memset(g_surfaces, 0, sizeof(g_surfaces));
    memset(&g_framebuffer_info, 0, sizeof(g_framebuffer_info));
    memset(&g_gpu_info, 0, sizeof(g_gpu_info));
    g_front_surface_index = 0;
    g_present = false;
    g_ready = false;
    g_next_fence_id = 1;
    g_last_response_type = 0;
    g_command_slot_capacity = 0;
    reset_present_tracking();

    pci::DeviceInfo pci_device = {};
    if (!pci::ready() || !virtio_pci::find_modern_device(kVirtioGpuModernDevice, kVirtioGpuSubsystemDevice, pci_device)) {
        return;
    }
    g_present = true;

    if (!virtio_pci::initialize_device(pci_device, true, g_device)) {
        console::write_line("virtio-gpu: missing required MMIO capabilities");
        return;
    }

    virtio_pci::set_device_status(g_device, 0);
    virtio_pci::memory_barrier();
    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::kStatusAcknowledge | virtio_pci::kStatusDriver));

    if (!virtio_pci::negotiate_features(g_device, 0, virtio_pci::kFeatureVersion1Bit)) {
        fail_device("feature negotiation failed");
        return;
    }
    if (!virtio_pci::setup_queue(g_device, kVirtioGpuControlQueue, kVirtioGpuQueueDescriptorLimit, kQueueExtraBytes, 16, g_control_queue)) {
        fail_device("failed to setup control queue");
        return;
    }
    g_command_slot_capacity = g_control_queue.size / 2u;
    if (g_command_slot_capacity > kCommandSlotCount) {
        g_command_slot_capacity = kCommandSlotCount;
    }
    reset_present_tracking();
    if (g_command_slot_capacity < 3u) {
        fail_device("control queue too small for buffered presenter");
        return;
    }

    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::device_status(g_device) | virtio_pci::kStatusDriverOk));
    g_ready = true;

    VirtioGpuRespDisplayInfo display_info = {};
    if (!get_display_info(display_info)) {
        log_command_failure("GET_DISPLAY_INFO");
        fail_device("failed to query display info");
        return;
    }
    if (!choose_scanout(display_info, framebuffer)) {
        fail_device("failed to choose scanout");
        return;
    }

    const uint32_t native_width = g_framebuffer_info.width;
    const uint32_t native_height = g_framebuffer_info.height;
    uint32_t preferred_width = native_width;
    uint32_t preferred_height = native_height;

    if (framebuffer.available &&
        framebuffer.width != 0 &&
        framebuffer.height != 0 &&
        framebuffer.bpp == 32 &&
        (framebuffer.width > native_width || framebuffer.height > native_height)) {
        preferred_width = static_cast<uint32_t>(framebuffer.width);
        preferred_height = static_cast<uint32_t>(framebuffer.height);
    }

    if (!configure_primary_surface(preferred_width, preferred_height)) {
        if (preferred_width != native_width || preferred_height != native_height) {
            console::printf(
                "virtio-gpu: preferred mode %ux%u rejected, falling back to %ux%u\n",
                static_cast<unsigned>(preferred_width),
                static_cast<unsigned>(preferred_height),
                static_cast<unsigned>(native_width),
                static_cast<unsigned>(native_height)
            );
        } else {
            log_command_failure("initial RESOURCE_CREATE_2D/SET_SCANOUT");
        }

        if ((preferred_width != native_width || preferred_height != native_height) &&
            configure_primary_surface(native_width, native_height)) {
            console::printf(
                "virtio-gpu: using native scanout mode %ux%u\n",
                static_cast<unsigned>(native_width),
                static_cast<unsigned>(native_height)
            );
        } else {
            log_command_failure("initial RESOURCE_CREATE_2D/SET_SCANOUT");
            fail_device("failed to configure primary scanout");
            return;
        }
    }

    if (front_surface() == nullptr || front_surface()->allocation.virtual_address == nullptr) {
        fail_device("failed to allocate primary surface");
        return;
    }

    g_gpu_device.ioctl = gpu_ioctl;
    g_gpu_device.close = gpu_close;
    if (!device::register_node("/dev/gpu0", &g_gpu_device, true)) {
        fail_device("failed to register /dev/gpu0");
        return;
    }

    console::set_external_framebuffer(front_surface()->allocation.virtual_address, g_framebuffer_info);
    console::printf(
        "virtio-gpu: ready pci=%x:%x.%u scanout=%u mode=%ux%u\n",
        static_cast<unsigned>(g_device.pci_device.bus),
        static_cast<unsigned>(g_device.pci_device.slot),
        static_cast<unsigned>(g_device.pci_device.function),
        static_cast<unsigned>(g_scanout_id),
        static_cast<unsigned>(g_framebuffer_info.width),
        static_cast<unsigned>(g_framebuffer_info.height)
    );
}

bool ready() {
    return g_ready;
}

bool present() {
    return g_present;
}

const savanxp_fb_info& framebuffer_info() {
    return g_framebuffer_info;
}

void* framebuffer_address() {
    Surface* surface = front_surface();
    return surface != nullptr ? surface->allocation.virtual_address : nullptr;
}

void wait_for_idle() {
    if (!g_ready) {
        return;
    }
    wait_for_idle_internal();
}

bool flush() {
    return flush_rect(0, 0, g_framebuffer_info.width, g_framebuffer_info.height);
}

bool flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!g_ready || width == 0 || height == 0 ||
        x >= g_framebuffer_info.width || y >= g_framebuffer_info.height ||
        width > (g_framebuffer_info.width - x) || height > (g_framebuffer_info.height - y)) {
        return false;
    }
    return transfer_rect(g_front_surface_index, x, y, width, height) &&
        flush_rect_internal(g_front_surface_index, x, y, width, height);
}

bool present_from_kernel(const void* pixels, size_t byte_count) {
    if (!g_ready || pixels == nullptr || byte_count != g_framebuffer_info.buffer_size) {
        return false;
    }

    uint32_t surface_index = 0;
    if (!acquire_present_surface(surface_index)) {
        return false;
    }
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr || surface->allocation.virtual_address == nullptr) {
        return false;
    }

    memcpy(surface->allocation.virtual_address, pixels, byte_count);
    if (!submit_present_surface(surface_index, 0, 0, g_framebuffer_info.width, g_framebuffer_info.height)) {
        return false;
    }
    uint32_t free_surface = 0;
    return acquire_present_surface(free_surface);
}

bool present_region_from_kernel(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!g_ready || pixels == nullptr || source_pitch == 0 || width == 0 || height == 0 ||
        x >= g_framebuffer_info.width || y >= g_framebuffer_info.height ||
        width > (g_framebuffer_info.width - x) || height > (g_framebuffer_info.height - y) ||
        source_pitch < (width * sizeof(uint32_t))) {
        return false;
    }

    uint32_t surface_index = 0;
    if (!acquire_present_surface(surface_index)) {
        return false;
    }
    return clone_surface(surface_index, g_front_surface_index) &&
        copy_region_to_surface(surface_index, pixels, source_pitch, x, y, width, height) &&
        submit_present_surface(surface_index, 0, 0, g_framebuffer_info.width, g_framebuffer_info.height) &&
        acquire_present_surface(surface_index);
}

} // namespace virtio_gpu
