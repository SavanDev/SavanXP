#include "kernel/virtio_gpu.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/device.hpp"
#include "kernel/heap.hpp"
#include "kernel/object.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/ui.hpp"
#include "kernel/virtio_input.hpp"
#include "kernel/virtio_pci.hpp"
#include "kernel/vmm.hpp"

namespace {

constexpr uint16_t kVirtioGpuModernDevice = 0x1050u;
constexpr uint16_t kVirtioGpuSubsystemDevice = 16u;
constexpr uint16_t kVirtioGpuControlQueue = 0;
constexpr uint16_t kVirtioGpuCursorQueue = 1;
constexpr uint16_t kVirtioGpuQueueDescriptorLimit = 16;
constexpr uint16_t kVirtioGpuCursorQueueDescriptorLimit = 2;
constexpr uint32_t kVirtioGpuFormatB8G8R8A8Unorm = 1u;
constexpr uint32_t kVirtioGpuFormatB8G8R8X8Unorm = 2u;
constexpr uint32_t kVirtioGpuFlagFence = 1u << 0;
constexpr uint32_t kVirtioGpuCmdGetDisplayInfo = 0x0100u;
constexpr uint32_t kVirtioGpuCmdResourceCreate2d = 0x0101u;
constexpr uint32_t kVirtioGpuCmdResourceUnref = 0x0102u;
constexpr uint32_t kVirtioGpuCmdSetScanout = 0x0103u;
constexpr uint32_t kVirtioGpuCmdResourceFlush = 0x0104u;
constexpr uint32_t kVirtioGpuCmdTransferToHost2d = 0x0105u;
constexpr uint32_t kVirtioGpuCmdResourceAttachBacking = 0x0106u;
constexpr uint32_t kVirtioGpuCmdUpdateCursor = 0x0300u;
constexpr uint32_t kVirtioGpuCmdMoveCursor = 0x0301u;
constexpr uint32_t kVirtioGpuRespOkNoData = 0x1100u;
constexpr uint32_t kVirtioGpuRespOkDisplayInfo = 0x1101u;
constexpr uint32_t kVirtioGpuEventDisplay = 1u << 0;
constexpr uint32_t kGpuSurfaceCount = 3u;
constexpr uint32_t kImportedSurfaceCount = 8u;
constexpr uint32_t kPendingPresentCapacity = kGpuSurfaceCount + kImportedSurfaceCount;
constexpr uint32_t kRetiredPresentHistoryCapacity = 64u;
constexpr uint32_t kFirstGpuResourceId = 1u;
constexpr size_t kRequestBufferBytes = 32768;
constexpr size_t kResponseBufferBytes = 512;
constexpr size_t kCommandSlotBytes = kRequestBufferBytes + kResponseBufferBytes;
constexpr uint16_t kCommandSlotCount = kVirtioGpuQueueDescriptorLimit / 2u;
constexpr size_t kQueueExtraBytes = kCommandSlotBytes * kCommandSlotCount;
constexpr size_t kResponseBufferOffset = kRequestBufferBytes;
constexpr size_t kCursorRequestBufferBytes = 256;
constexpr size_t kCursorResponseBufferBytes = 64;
constexpr size_t kCursorCommandSlotBytes = kCursorRequestBufferBytes + kCursorResponseBufferBytes;
constexpr size_t kCursorQueueExtraBytes = kCursorCommandSlotBytes;
constexpr size_t kCursorResponseBufferOffset = kCursorRequestBufferBytes;
constexpr uint64_t kCommandWaitTicks = 20u;
constexpr uint64_t kSurfaceWaitTicks = 20u;
constexpr uint64_t kIdleWaitTicks = 40u;
constexpr uint32_t kCommandActivePollIterations = 50000u;
constexpr uint32_t kSurfaceActivePollIterations = 50000u;
constexpr uint32_t kIdleActivePollIterations = 50000u;
constexpr uint32_t kCursorMaxDimension = 64u;
constexpr uint32_t kCursorResourceId = kFirstGpuResourceId + kGpuSurfaceCount + kImportedSurfaceCount;

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

struct [[gnu::packed]] VirtioGpuResourceAttachBackingHeader {
    VirtioGpuCtrlHdr header;
    uint32_t resource_id;
    uint32_t nr_entries;
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

struct [[gnu::packed]] VirtioGpuCursorPos {
    uint32_t scanout_id;
    uint32_t x;
    uint32_t y;
    uint32_t padding;
};

struct [[gnu::packed]] VirtioGpuUpdateCursor {
    VirtioGpuCtrlHdr header;
    VirtioGpuCursorPos pos;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
};

enum CommandStage : uint8_t {
    kCommandStageNone = 0,
    kCommandStageSync = 1,
    kCommandStagePresentTransfer = 2,
    kCommandStagePresentFlush = 3,
    kCommandStagePresentScanout = 4,
};

enum PendingPresentTarget : uint8_t {
    kPendingPresentTargetPrimary = 0,
    kPendingPresentTargetImported = 1,
};

enum SubmitPresentResult : uint8_t {
    kSubmitPresentFailed = 0,
    kSubmitPresentDeferred = 1,
    kSubmitPresentSubmitted = 2,
};

struct CommandSlot {
    bool in_use;
    bool completed;
    uint16_t head_descriptor;
    uint16_t response_bytes;
    uint32_t response_type;
    uint32_t request_type;
    CommandStage stage;
    uint32_t pending_present_index;
    uint64_t submit_tick;
};

struct PendingPresent {
    bool in_use;
    bool completed;
    bool failed;
    bool started;
    bool stage_in_flight;
    PendingPresentTarget target;
    CommandStage next_stage;
    uint32_t surface_index;
    uint32_t surface_id;
    uint32_t resource_id;
    savanxp_fb_info info;
    void* virtual_address;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint64_t sequence;
    uint64_t enqueue_tick;
};

struct RetiredPresentStatus {
    bool valid;
    bool failed;
    uint16_t reserved0;
    uint32_t reserved1;
    uint64_t sequence;
};

struct CursorCommandSlot {
    bool in_use;
    bool completed;
    uint16_t head_descriptor;
    uint32_t request_type;
    uint32_t response_type;
    uint64_t submit_tick;
};

struct GpuScanoutState {
    bool valid;
    bool enabled;
    uint16_t reserved0;
    uint32_t scanout_id;
    uint32_t native_width;
    uint32_t native_height;
    uint32_t preferred_width;
    uint32_t preferred_height;
    uint32_t active_width;
    uint32_t active_height;
};

struct CursorPlane {
    bool image_loaded;
    bool visible;
    uint16_t reserved0;
    uint32_t x;
    uint32_t y;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t resource_id;
    uint64_t page_count;
    uint64_t* physical_pages;
    void* virtual_address;
    savanxp_fb_info info;
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
virtio_pci::Queue g_cursor_queue = {};
struct Surface {
    uint32_t resource_id;
    uint32_t reserved0;
    uint64_t page_count;
    uint64_t* physical_pages;
    void* virtual_address;
};

struct ImportedSurface {
    bool in_use;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t surface_id;
    uint32_t resource_id;
    uint32_t flags;
    uint64_t page_count;
    savanxp_fb_info info;
    object::SectionObject* section;
    void* virtual_address;
};
Surface* surface_at(uint32_t surface_index);
Surface* front_surface();
ImportedSurface* imported_surface_at(uint32_t surface_id);
void release_imported_surface(ImportedSurface& surface);
void release_all_imported_surfaces();
bool any_imported_surface_in_use();
void release_surface_allocation(uint32_t surface_index);
bool set_scanout_surface(uint32_t surface_index);
void poll_command_completions();
void poll_cursor_completions();
SubmitPresentResult submit_next_present_stage(uint32_t pending_index);
void log_command_failure(const char* stage);
void submit_ready_deferred_presents();
void service_queue_progress();
void service_background_work();
bool refresh_scanouts(bool explicit_refresh);
bool ensure_primary_scanout_restored();
void refresh_gpu_info_flags();
bool configure_primary_surface(uint32_t width, uint32_t height);
bool submit_cursor_plane_command(uint32_t type);
bool recover_device(const char* reason);
void enter_degraded_mode(const char* reason);
void fail_device(const char* reason);
void gpu_irq();

Surface g_surfaces[kGpuSurfaceCount] = {};
ImportedSurface g_imported_surfaces[kImportedSurfaceCount] = {};
GpuScanoutState g_scanouts[16] = {};
CursorPlane g_cursor_plane = {};
savanxp_fb_info g_framebuffer_info = {};
savanxp_gpu_info g_gpu_info = {};
savanxp_gpu_scanout_state g_scanout_state = {};
savanxp_gpu_stats g_gpu_stats = {};
uint32_t g_scanout_id = 0;
uint32_t g_scanout_resource_id = 0;
uint32_t g_scanout_width = 0;
uint32_t g_scanout_height = 0;
uint32_t g_preferred_scanout_width = 0;
uint32_t g_preferred_scanout_height = 0;
uint32_t g_front_surface_index = 0;
uint64_t g_next_fence_id = 1;
bool g_present = false;
bool g_ready = false;
bool g_degraded = false;
uint32_t g_last_response_type = 0;
uint16_t g_command_slot_capacity = 0;
CommandSlot g_command_slots[kCommandSlotCount] = {};
CursorCommandSlot g_cursor_command = {};
PendingPresent g_pending_presents[kPendingPresentCapacity] = {};
uint32_t g_pending_present_order[kPendingPresentCapacity] = {};
uint32_t g_pending_present_count = 0;
RetiredPresentStatus g_retired_present_history[kRetiredPresentHistoryCapacity] = {};
uint32_t g_retired_present_history_next = 0;
uint64_t g_next_present_sequence = 1;
uint64_t g_last_submitted_present_sequence = 0;
uint64_t g_last_retired_present_sequence = 0;
uint8_t g_irq_line = 0xffu;
bool g_irq_registered = false;
bool g_irq_work_pending = false;
bool g_scanout_refresh_pending = false;
uint32_t g_driver_busy_depth = 0;
bool g_background_service_active = false;

int negative_error(savanxp_error_code code) {
    return -static_cast<int>(code);
}

uint64_t page_count_for_bytes(uint32_t byte_count) {
    return (static_cast<uint64_t>(byte_count) + memory::kPageSize - 1u) / memory::kPageSize;
}

struct DriverBusyGuard {
    DriverBusyGuard() {
        g_driver_busy_depth += 1u;
    }

    ~DriverBusyGuard() {
        if (g_driver_busy_depth != 0) {
            g_driver_busy_depth -= 1u;
        }
    }
};

const char* request_type_name(uint32_t type) {
    switch (type) {
        case kVirtioGpuCmdResourceCreate2d:
            return "RESOURCE_CREATE_2D";
        case kVirtioGpuCmdResourceAttachBacking:
            return "RESOURCE_ATTACH_BACKING";
        case kVirtioGpuCmdSetScanout:
            return "SET_SCANOUT";
        case kVirtioGpuCmdTransferToHost2d:
            return "TRANSFER_TO_HOST_2D";
        case kVirtioGpuCmdResourceFlush:
            return "RESOURCE_FLUSH";
        case kVirtioGpuCmdUpdateCursor:
            return "UPDATE_CURSOR";
        case kVirtioGpuCmdMoveCursor:
            return "MOVE_CURSOR";
        default:
            return nullptr;
    }
}

volatile VirtioGpuConfig* device_cfg() {
    return reinterpret_cast<volatile VirtioGpuConfig*>(virtio_pci::device_cfg_base(g_device));
}

uint32_t current_gpu_info_flags() {
    uint32_t flags = SAVANXP_GPU_INFO_FLAG_SCANOUT_ENUMERATION | SAVANXP_GPU_INFO_FLAG_HOTPLUG_REFRESH;
    if (g_irq_registered) {
        flags |= SAVANXP_GPU_INFO_FLAG_IRQ_DRIVEN;
    }
    if (g_cursor_queue.enabled) {
        flags |= SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE;
    }
    return flags;
}

void refresh_gpu_info_flags() {
    g_gpu_info.flags = current_gpu_info_flags();
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

uint8_t* cursor_request_buffer() {
    return virtio_pci::queue_extra(g_cursor_queue, 0);
}

uint8_t* cursor_response_buffer() {
    return cursor_request_buffer() + kCursorResponseBufferOffset;
}

uint64_t cursor_request_physical() {
    return virtio_pci::queue_extra_physical(g_cursor_queue, 0);
}

uint64_t cursor_response_physical() {
    return cursor_request_physical() + kCursorResponseBufferOffset;
}

uint64_t wait_deadline(uint64_t wait_ticks) {
    const uint64_t now = timer::ticks();
    return now + (wait_ticks != 0 ? wait_ticks : 1u);
}

bool wait_for_gpu_tick(uint64_t deadline_tick) {
    if (timer::ticks() >= deadline_tick) {
        return false;
    }
    timer::wait_ticks(1);
    return timer::ticks() < deadline_tick;
}

void pause_briefly() {
    asm volatile("pause" : : : "memory");
}

uint32_t scanout_flags(const GpuScanoutState& scanout) {
    uint32_t flags = 0;
    if (scanout.enabled) {
        flags |= SAVANXP_GPU_SCANOUT_FLAG_ENABLED;
    }
    if (scanout.scanout_id == g_scanout_id) {
        flags |= SAVANXP_GPU_SCANOUT_FLAG_PRIMARY;
        if (scanout.active_width != 0 && scanout.active_height != 0) {
            flags |= SAVANXP_GPU_SCANOUT_FLAG_ACTIVE;
        }
        if (scanout.preferred_width != 0 && scanout.preferred_height != 0) {
            flags |= SAVANXP_GPU_SCANOUT_FLAG_PREFERRED;
        }
    }
    return flags;
}

void refresh_cached_scanout_state() {
    memset(&g_scanout_state, 0, sizeof(g_scanout_state));
    g_scanout_state.active_scanout_id = g_scanout_id;
    for (uint32_t index = 0; index < 16; ++index) {
        const GpuScanoutState& scanout = g_scanouts[index];
        if (!scanout.valid) {
            continue;
        }
        savanxp_gpu_scanout_info& out = g_scanout_state.scanouts[g_scanout_state.count++];
        out.scanout_id = scanout.scanout_id;
        out.flags = scanout_flags(scanout);
        out.native_width = scanout.native_width;
        out.native_height = scanout.native_height;
        out.preferred_width = scanout.preferred_width;
        out.preferred_height = scanout.preferred_height;
        out.active_width = scanout.active_width;
        out.active_height = scanout.active_height;
    }
}

void update_active_scanout_mode(uint32_t width, uint32_t height) {
    for (uint32_t index = 0; index < 16; ++index) {
        GpuScanoutState& scanout = g_scanouts[index];
        if (!scanout.valid) {
            continue;
        }
        if (scanout.scanout_id == g_scanout_id) {
            scanout.active_width = width;
            scanout.active_height = height;
        } else {
            scanout.active_width = 0;
            scanout.active_height = 0;
        }
    }
    refresh_cached_scanout_state();
}

uint32_t current_present_timeline_flags() {
    return g_degraded ? SAVANXP_GPU_PRESENT_TIMELINE_FLAG_DEGRADED : 0u;
}

void fill_present_timeline(savanxp_gpu_present_timeline& timeline) {
    timeline = {
        .submitted_sequence = g_last_submitted_present_sequence,
        .retired_sequence = g_last_retired_present_sequence,
        .pending_count = g_pending_present_count,
        .flags = current_present_timeline_flags(),
    };
}

void remember_retired_present(uint64_t sequence, bool failed) {
    if (sequence == 0) {
        return;
    }

    RetiredPresentStatus& entry = g_retired_present_history[g_retired_present_history_next];
    entry = {
        .valid = true,
        .failed = failed,
        .reserved0 = 0,
        .reserved1 = 0,
        .sequence = sequence,
    };
    g_retired_present_history_next =
        static_cast<uint32_t>((g_retired_present_history_next + 1u) % kRetiredPresentHistoryCapacity);
    if (sequence > g_last_retired_present_sequence) {
        g_last_retired_present_sequence = sequence;
    }
}

bool query_retired_present(uint64_t sequence, bool& failed) {
    if (sequence == 0) {
        failed = false;
        return true;
    }

    for (uint32_t index = 0; index < kRetiredPresentHistoryCapacity; ++index) {
        const RetiredPresentStatus& entry = g_retired_present_history[index];
        if (entry.valid && entry.sequence == sequence) {
            failed = entry.failed;
            return true;
        }
    }

    if (sequence <= g_last_retired_present_sequence) {
        failed = false;
        return true;
    }
    return false;
}

void retire_pending_presents_as_failed() {
    for (uint32_t index = 0; index < kPendingPresentCapacity; ++index) {
        const PendingPresent& present = g_pending_presents[index];
        if (!present.in_use || present.sequence == 0) {
            continue;
        }
        remember_retired_present(present.sequence, true);
    }
}

void reset_present_tracking() {
    retire_pending_presents_as_failed();
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

bool wait_for_command_slot_reservation(uint16_t& slot_index) {
    if (reserve_command_slot(slot_index)) {
        return true;
    }

    for (uint32_t spin = 0; spin < kCommandActivePollIterations; ++spin) {
        service_queue_progress();
        if (reserve_command_slot(slot_index)) {
            return true;
        }
        pause_briefly();
    }

    const uint64_t deadline_tick = wait_deadline(kCommandWaitTicks);
    for (;;) {
        service_queue_progress();
        if (reserve_command_slot(slot_index)) {
            return true;
        }
        if (!wait_for_gpu_tick(deadline_tick)) {
            break;
        }
    }

    g_gpu_stats.command_timeouts += 1u;
    console::write_line("virtio-gpu: wait timeout command slot reservation");
    enter_degraded_mode("wait timeout slot reservation");
    (void)recover_device("wait timeout slot reservation");
    return false;
}

void release_command_slot(uint16_t slot_index) {
    if (slot_index >= g_command_slot_capacity) {
        return;
    }
    g_command_slots[slot_index] = {};
}

bool command_slots_in_flight() {
    for (uint16_t index = 0; index < g_command_slot_capacity; ++index) {
        if (g_command_slots[index].in_use) {
            return true;
        }
    }
    return g_cursor_command.in_use;
}

bool reserve_pending_present_slot(uint32_t& pending_index) {
    for (uint32_t index = 0; index < kPendingPresentCapacity; ++index) {
        if (!g_pending_presents[index].in_use) {
            pending_index = index;
            return true;
        }
    }
    return false;
}

bool wait_for_pending_present_slot(uint32_t& pending_index) {
    if (reserve_pending_present_slot(pending_index)) {
        return true;
    }

    for (uint32_t spin = 0; spin < kSurfaceActivePollIterations; ++spin) {
        g_gpu_stats.wait_pending_slot_polls += 1u;
        service_queue_progress();
        if (reserve_pending_present_slot(pending_index)) {
            return true;
        }
        pause_briefly();
    }

    const uint64_t deadline_tick = wait_deadline(kSurfaceWaitTicks);
    for (;;) {
        service_queue_progress();
        if (reserve_pending_present_slot(pending_index)) {
            return true;
        }
        if (!wait_for_gpu_tick(deadline_tick)) {
            break;
        }
        g_gpu_stats.wait_pending_slot_ticks += 1u;
    }

    g_gpu_stats.pending_slot_timeouts += 1u;
    console::write_line("virtio-gpu: wait timeout pending present slot");
    return false;
}

void merge_pending_present_rect(PendingPresent& present, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    const uint32_t right = present.x + present.width;
    const uint32_t bottom = present.y + present.height;
    const uint32_t new_right = x + width;
    const uint32_t new_bottom = y + height;

    if (x < present.x) {
        present.x = x;
    }
    if (y < present.y) {
        present.y = y;
    }
    present.width = (new_right > right ? new_right : right) - present.x;
    present.height = (new_bottom > bottom ? new_bottom : bottom) - present.y;
}

bool resource_present_in_progress(uint32_t resource_id, uint32_t exclude_index) {
    if (resource_id == 0) {
        return false;
    }

    for (uint32_t index = 0; index < kPendingPresentCapacity; ++index) {
        const PendingPresent& present = g_pending_presents[index];
        if (index == exclude_index ||
            !present.in_use ||
            present.resource_id != resource_id ||
            present.failed ||
            present.completed ||
            !present.started) {
            continue;
        }
        return true;
    }
    return false;
}

PendingPresent* latest_pending_present_for_resource(uint32_t resource_id) {
    if (resource_id == 0) {
        return nullptr;
    }

    for (uint32_t order_index = g_pending_present_count; order_index != 0; --order_index) {
        PendingPresent& present = g_pending_presents[g_pending_present_order[order_index - 1]];
        if (present.in_use && present.resource_id == resource_id) {
            return &present;
        }
    }
    return nullptr;
}

bool enqueue_pending_present(const PendingPresent& pending, uint32_t& pending_index) {
    if (!wait_for_pending_present_slot(pending_index) || g_pending_present_count >= kPendingPresentCapacity) {
        return false;
    }

    g_pending_presents[pending_index] = pending;
    g_pending_presents[pending_index].in_use = true;
    g_pending_presents[pending_index].sequence = g_next_present_sequence++;
    g_pending_presents[pending_index].enqueue_tick = timer::ticks();
    g_pending_present_order[g_pending_present_count++] = pending_index;
    g_last_submitted_present_sequence = g_pending_presents[pending_index].sequence;
    g_gpu_stats.present_enqueued += 1u;
    if (g_pending_present_count > g_gpu_stats.pending_depth_max) {
        g_gpu_stats.pending_depth_max = g_pending_present_count;
    }
    return true;
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

    const VirtioGpuCtrlHdr* header = reinterpret_cast<const VirtioGpuCtrlHdr*>(request);
    const uint32_t request_type = header != nullptr ? header->type : 0;

    void* request_buffer = slot_request_buffer(slot_index);
    void* response_buffer = slot_response_buffer(slot_index);
    memcpy(request_buffer, request, request_bytes);
    memset(response_buffer, 0, response_bytes);
    slot.completed = false;
    slot.response_bytes = static_cast<uint16_t>(response_bytes);
    slot.response_type = 0;
    slot.request_type = request_type;
    slot.submit_tick = timer::ticks();

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
    if (slot.stage == kCommandStageSync) {
        g_gpu_stats.sync_command_submitted += 1u;
    }
    return true;
}

bool submit_cursor_command(const void* request, size_t request_bytes, size_t response_bytes) {
    if (!g_ready || !g_cursor_queue.enabled || request == nullptr || request_bytes == 0 || response_bytes == 0 ||
        request_bytes > kCursorRequestBufferBytes || response_bytes > kCursorResponseBufferBytes ||
        g_cursor_command.in_use) {
        return false;
    }

    VirtioGpuCtrlHdr const* header = reinterpret_cast<VirtioGpuCtrlHdr const*>(request);
    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_cursor_queue);
    void* request_buffer = cursor_request_buffer();
    void* response_buffer = cursor_response_buffer();

    memcpy(request_buffer, request, request_bytes);
    memset(response_buffer, 0, response_bytes);

    g_cursor_command = {};
    g_cursor_command.in_use = true;
    g_cursor_command.head_descriptor = 0;
    g_cursor_command.request_type = header != nullptr ? header->type : 0;
    g_cursor_command.submit_tick = timer::ticks();

    descriptors[0] = {
        .addr = cursor_request_physical(),
        .len = static_cast<uint32_t>(request_bytes),
        .flags = virtio_pci::kDescriptorFlagNext,
        .next = 1,
    };
    descriptors[1] = {
        .addr = cursor_response_physical(),
        .len = static_cast<uint32_t>(response_bytes),
        .flags = virtio_pci::kDescriptorFlagWrite,
        .next = 0,
    };

    if (!virtio_pci::submit_descriptor_head(g_cursor_queue, 0)) {
        g_cursor_command = {};
        return false;
    }

    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_cursor_queue);
    return true;
}

void poll_cursor_completions() {
    if (!g_ready || !g_cursor_queue.enabled || !g_cursor_command.in_use) {
        return;
    }

    const virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_cursor_queue);
    const virtio_pci::UsedElement* ring = virtio_pci::queue_used_ring(g_cursor_queue);

    while (g_cursor_queue.last_used_index != used->idx) {
        virtio_pci::memory_barrier();
        const virtio_pci::UsedElement element = ring[g_cursor_queue.last_used_index % g_cursor_queue.size];
        g_cursor_queue.last_used_index = static_cast<uint16_t>(g_cursor_queue.last_used_index + 1u);
        if (element.id != 0 || !g_cursor_command.in_use) {
            continue;
        }

        VirtioGpuCtrlHdr const* response = reinterpret_cast<VirtioGpuCtrlHdr const*>(cursor_response_buffer());
        g_cursor_command.completed = true;
        g_cursor_command.response_type = response->type;
        g_last_response_type = response->type;
        g_gpu_stats.command_completions += 1u;
    }
}

void service_queue_progress() {
    poll_command_completions();
    poll_cursor_completions();
}

bool wait_for_cursor_command() {
    for (uint32_t spin = 0; spin < kCommandActivePollIterations; ++spin) {
        poll_cursor_completions();
        if (!g_cursor_command.in_use || g_cursor_command.completed) {
            break;
        }
        pause_briefly();
    }

    if (g_cursor_command.in_use && !g_cursor_command.completed) {
        const uint64_t deadline_tick = wait_deadline(kCommandWaitTicks);
        for (;;) {
            poll_cursor_completions();
            if (!g_cursor_command.in_use || g_cursor_command.completed) {
                break;
            }
            if (!wait_for_gpu_tick(deadline_tick)) {
                g_gpu_stats.command_timeouts += 1u;
                break;
            }
        }
    }

    if (!g_cursor_command.in_use) {
        return false;
    }

    const bool success = g_cursor_command.completed && g_cursor_command.response_type == kVirtioGpuRespOkNoData;
    if (!success) {
        const char* request_name = request_type_name(g_cursor_command.request_type);
        if (request_name != nullptr) {
            console::printf("virtio-gpu: cursor command failed request=%s\n", request_name);
        }
    }
    g_cursor_command = {};
    return success;
}

void finalize_completed_presents() {
    while (g_pending_present_count != 0) {
        PendingPresent& present = g_pending_presents[g_pending_present_order[0]];
        if (!present.in_use || !present.completed) {
            break;
        }

        remember_retired_present(present.sequence, present.failed);
        if (!present.failed) {
            const uint64_t completion_tick = timer::ticks();
            const uint64_t elapsed_ticks =
                completion_tick >= present.enqueue_tick ? (completion_tick - present.enqueue_tick) : 0u;
            g_gpu_stats.present_completed += 1u;
            g_gpu_stats.present_end_to_end_ticks += elapsed_ticks;
            g_gpu_stats.present_end_to_end_samples += 1u;
            if (elapsed_ticks > g_gpu_stats.present_end_to_end_max_ticks) {
                g_gpu_stats.present_end_to_end_max_ticks = elapsed_ticks;
            }
            if (present.target == kPendingPresentTargetPrimary) {
                g_front_surface_index = present.surface_index;
            }
            g_scanout_resource_id = present.resource_id;
            if (present.virtual_address != nullptr) {
                console::set_external_framebuffer(present.virtual_address, present.info);
            }
        }

        present = {};
        release_pending_head();
    }
}

CommandStage next_present_stage(const PendingPresent& present, CommandStage completed_stage) {
    switch (completed_stage) {
        case kCommandStagePresentTransfer:
            return kCommandStagePresentFlush;
        case kCommandStagePresentFlush:
            return present.resource_id != 0 && present.resource_id != g_scanout_resource_id
                ? kCommandStagePresentScanout
                : kCommandStageNone;
        default:
            return kCommandStageNone;
    }
}

void submit_ready_deferred_presents() {
    for (uint32_t order_index = 0; order_index < g_pending_present_count; ++order_index) {
        const uint32_t pending_index = g_pending_present_order[order_index];
        PendingPresent& present = g_pending_presents[pending_index];
        if (!present.in_use || present.completed || present.failed || present.stage_in_flight) {
            continue;
        }
        if (!present.started && resource_present_in_progress(present.resource_id, pending_index)) {
            continue;
        }

        const SubmitPresentResult submit_result = submit_next_present_stage(pending_index);
        if (submit_result == kSubmitPresentDeferred) {
            break;
        }
        if (submit_result == kSubmitPresentFailed) {
            present.failed = true;
            present.completed = true;
        }
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

        const uint64_t completion_tick = timer::ticks();
        const uint64_t elapsed_ticks = completion_tick >= slot.submit_tick ? (completion_tick - slot.submit_tick) : 0u;
        const VirtioGpuCtrlHdr* response = reinterpret_cast<const VirtioGpuCtrlHdr*>(slot_response_buffer(slot_index));
        slot.completed = true;
        slot.response_type = response->type;
        g_last_response_type = slot.response_type;
        g_gpu_stats.command_completions += 1u;
        switch (slot.stage) {
            case kCommandStageSync:
                g_gpu_stats.sync_command_ticks += elapsed_ticks;
                break;
            case kCommandStagePresentTransfer:
                g_gpu_stats.transfer_stage_ticks += elapsed_ticks;
                break;
            case kCommandStagePresentFlush:
                g_gpu_stats.flush_stage_ticks += elapsed_ticks;
                break;
            case kCommandStagePresentScanout:
                g_gpu_stats.scanout_stage_ticks += elapsed_ticks;
                break;
            default:
                break;
        }

        if (slot.stage != kCommandStageSync && slot.pending_present_index < kPendingPresentCapacity) {
            const uint32_t pending_present_index = slot.pending_present_index;
            const CommandStage completed_stage = slot.stage;
            const uint32_t response_type = slot.response_type;
            PendingPresent& present = g_pending_presents[pending_present_index];
            present.stage_in_flight = false;
            release_command_slot(slot_index);
            if (present.in_use) {
                if (response_type != kVirtioGpuRespOkNoData) {
                    present.failed = true;
                    present.completed = true;
                } else {
                    present.next_stage = next_present_stage(present, completed_stage);
                    if (present.next_stage == kCommandStageNone) {
                        present.completed = true;
                    } else if (submit_next_present_stage(pending_present_index) == kSubmitPresentFailed) {
                        present.failed = true;
                        present.completed = true;
                    }
                }
            }
        }
    }

    finalize_completed_presents();
    submit_ready_deferred_presents();
    finalize_completed_presents();
}

bool wait_for_command_slot(uint16_t slot_index) {
    if (slot_index >= g_command_slot_capacity || !g_command_slots[slot_index].in_use) {
        return false;
    }

    for (uint32_t spin = 0; spin < kCommandActivePollIterations; ++spin) {
        g_gpu_stats.wait_command_polls += 1u;
        service_queue_progress();
        if (g_command_slots[slot_index].completed) {
            return true;
        }
        pause_briefly();
    }

    const uint64_t deadline_tick = wait_deadline(kCommandWaitTicks);
    for (;;) {
        service_queue_progress();
        if (g_command_slots[slot_index].completed) {
            return true;
        }
        if (!wait_for_gpu_tick(deadline_tick)) {
            break;
        }
        g_gpu_stats.wait_command_ticks += 1u;
    }
    g_gpu_stats.command_timeouts += 1u;
    const char* request_name = request_type_name(g_command_slots[slot_index].request_type);
    if (request_name != nullptr) {
        console::printf("virtio-gpu: wait timeout request=%s slot=%u\n", request_name, static_cast<unsigned>(slot_index));
    }
    enter_degraded_mode("wait timeout command");
    (void)recover_device("wait timeout command");
    return false;
}

bool submit_command(const void* request, size_t request_bytes, void* response, size_t response_bytes) {
    if (response == nullptr) {
        return false;
    }

    uint16_t slot_index = 0;
    if (!wait_for_command_slot_reservation(slot_index)) {
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

ImportedSurface* imported_surface_at(uint32_t surface_id) {
    if (surface_id == 0) {
        return nullptr;
    }

    for (ImportedSurface& surface : g_imported_surfaces) {
        if (surface.in_use && surface.surface_id == surface_id) {
            return &surface;
        }
    }
    return nullptr;
}

bool create_resource_with_format(uint32_t resource_id, const savanxp_fb_info& info, uint32_t format) {
    VirtioGpuResourceCreate2d request = {};
    initialize_header(request.header, kVirtioGpuCmdResourceCreate2d);
    request.resource_id = resource_id;
    request.format = format;
    request.width = info.width;
    request.height = info.height;
    if (send_ok_nodata_command(&request, sizeof(request))) {
        return true;
    }
    log_command_failure("RESOURCE_CREATE_2D");
    return false;
}

bool create_resource(uint32_t resource_id, const savanxp_fb_info& info) {
    return create_resource_with_format(resource_id, info, kVirtioGpuFormatB8G8R8X8Unorm);
}

bool destroy_resource(uint32_t resource_id) {
    if (resource_id == 0) {
        return false;
    }

    VirtioGpuResourceUnref request = {};
    initialize_header(request.header, kVirtioGpuCmdResourceUnref);
    request.resource_id = resource_id;
    if (resource_id == g_scanout_resource_id) {
        g_scanout_resource_id = 0;
    }
    return send_ok_nodata_command(&request, sizeof(request));
}

bool attach_resource_backing(uint32_t resource_id, const uint64_t* physical_pages, uint64_t page_count) {
    if (resource_id == 0 || physical_pages == nullptr || page_count == 0) {
        return false;
    }

    const size_t entry_capacity = (kRequestBufferBytes - sizeof(VirtioGpuResourceAttachBackingHeader)) / sizeof(VirtioGpuMemEntry);
    if (entry_capacity == 0) {
        return false;
    }

    VirtioGpuMemEntry* entries = static_cast<VirtioGpuMemEntry*>(
        heap::allocate(entry_capacity * sizeof(VirtioGpuMemEntry), alignof(VirtioGpuMemEntry)));
    if (entries == nullptr) {
        return false;
    }

    size_t entry_count = 0;
    uint64_t run_base = physical_pages[0];
    uint64_t run_length = memory::kPageSize;
    bool success = false;

    for (uint64_t page_index = 1; page_index < page_count; ++page_index) {
        if (physical_pages[page_index] == (run_base + run_length) &&
            run_length + memory::kPageSize <= 0xffffffffu) {
            run_length += memory::kPageSize;
            continue;
        }

        if (entry_count >= entry_capacity) {
            heap::free(entries);
            return false;
        }
        entries[entry_count++] = {
            .addr = run_base,
            .length = static_cast<uint32_t>(run_length),
            .padding = 0,
        };
        run_base = physical_pages[page_index];
        run_length = memory::kPageSize;
    }

    if (entry_count >= entry_capacity) {
        heap::free(entries);
        return false;
    }
    entries[entry_count++] = {
        .addr = run_base,
        .length = static_cast<uint32_t>(run_length),
        .padding = 0,
    };

    const size_t request_bytes = sizeof(VirtioGpuResourceAttachBackingHeader) + (entry_count * sizeof(VirtioGpuMemEntry));
    uint8_t* request_bytes_buffer = static_cast<uint8_t*>(heap::allocate(request_bytes, alignof(VirtioGpuMemEntry)));
    if (request_bytes_buffer == nullptr) {
        heap::free(entries);
        return false;
    }

    memset(request_bytes_buffer, 0, request_bytes);
    VirtioGpuResourceAttachBackingHeader* request = reinterpret_cast<VirtioGpuResourceAttachBackingHeader*>(request_bytes_buffer);
    initialize_header(request->header, kVirtioGpuCmdResourceAttachBacking);
    request->resource_id = resource_id;
    request->nr_entries = static_cast<uint32_t>(entry_count);
    memcpy(request_bytes_buffer + sizeof(*request), entries, entry_count * sizeof(VirtioGpuMemEntry));
    success = send_ok_nodata_command(request_bytes_buffer, request_bytes);

    heap::free(request_bytes_buffer);
    heap::free(entries);
    if (!success) {
        log_command_failure("RESOURCE_ATTACH_BACKING");
    }
    return success;
}

bool build_scanout_request_for_resource(uint32_t resource_id, const savanxp_fb_info& info, VirtioGpuSetScanout& request) {
    if (resource_id == 0) {
        return false;
    }

    request = {};
    initialize_header(request.header, kVirtioGpuCmdSetScanout);
    request.rect = {
        .x = 0,
        .y = 0,
        .width = info.width,
        .height = info.height,
    };
    request.scanout_id = g_scanout_id;
    request.resource_id = resource_id;
    return true;
}

bool set_scanout_resource(uint32_t resource_id, const savanxp_fb_info& info) {
    VirtioGpuSetScanout request = {};
    if (resource_id == g_scanout_resource_id) {
        return true;
    }
    if (!(build_scanout_request_for_resource(resource_id, info, request) &&
        send_ok_nodata_command(&request, sizeof(request)))) {
        return false;
    }
    g_scanout_resource_id = resource_id;
    return true;
}

bool build_transfer_request_for_resource(
    uint32_t resource_id,
    const savanxp_fb_info& info,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    VirtioGpuTransferToHost2d& request) {
    if (resource_id == 0) {
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
    request.offset = (static_cast<uint64_t>(y) * info.pitch) + (static_cast<uint64_t>(x) * sizeof(uint32_t));
    request.resource_id = resource_id;
    return true;
}

bool transfer_rect_resource(
    uint32_t resource_id,
    const savanxp_fb_info& info,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height) {
    VirtioGpuTransferToHost2d request = {};
    return build_transfer_request_for_resource(resource_id, info, x, y, width, height, request) &&
        send_ok_nodata_command(&request, sizeof(request));
}

bool build_flush_request_for_resource(
    uint32_t resource_id,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    VirtioGpuResourceFlush& request) {
    if (resource_id == 0) {
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
    request.resource_id = resource_id;
    return true;
}

bool flush_rect_resource(uint32_t resource_id, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    VirtioGpuResourceFlush request = {};
    return build_flush_request_for_resource(resource_id, x, y, width, height, request) &&
        send_ok_nodata_command(&request, sizeof(request));
}

bool present_resource_sync(
    uint32_t resource_id,
    const savanxp_fb_info& info,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height) {
    if (!transfer_rect_resource(resource_id, info, x, y, width, height)) {
        log_command_failure("TRANSFER_TO_HOST_2D");
        return false;
    }
    if (!flush_rect_resource(resource_id, x, y, width, height)) {
        log_command_failure("RESOURCE_FLUSH");
        return false;
    }
    if (!set_scanout_resource(resource_id, info)) {
        log_command_failure("SET_SCANOUT");
        return false;
    }
    return true;
}

bool surface_pending(uint32_t surface_index) {
    for (uint32_t index = 0; index < kPendingPresentCapacity; ++index) {
        if (g_pending_presents[index].in_use &&
            g_pending_presents[index].target == kPendingPresentTargetPrimary &&
            g_pending_presents[index].surface_index == surface_index) {
            return true;
        }
    }
    return false;
}

bool resource_pending(uint32_t resource_id) {
    if (resource_id == 0) {
        return false;
    }

    for (uint32_t index = 0; index < kPendingPresentCapacity; ++index) {
        if (g_pending_presents[index].in_use && g_pending_presents[index].resource_id == resource_id) {
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
    service_queue_progress();
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

    for (uint32_t spin = 0; spin < kSurfaceActivePollIterations; ++spin) {
        g_gpu_stats.wait_surface_polls += 1u;
        service_queue_progress();
        if (find_available_surface(surface_index)) {
            return true;
        }
        pause_briefly();
    }

    const uint64_t deadline_tick = wait_deadline(kSurfaceWaitTicks);
    for (;;) {
        service_queue_progress();
        if (find_available_surface(surface_index)) {
            return true;
        }
        if (!wait_for_gpu_tick(deadline_tick)) {
            break;
        }
        g_gpu_stats.wait_surface_ticks += 1u;
    }
    g_gpu_stats.surface_timeouts += 1u;
    return false;
}

bool acquire_present_surface(uint32_t& surface_index) {
    return find_available_surface(surface_index) || wait_for_available_surface(surface_index);
}

void wait_for_idle_internal() {
    if (!g_ready) {
        return;
    }
    while (g_pending_present_count != 0) {
        bool progressed = false;
        for (uint32_t spin = 0; spin < kIdleActivePollIterations; ++spin) {
            const uint32_t before = g_pending_present_count;
            g_gpu_stats.wait_idle_polls += 1u;
            service_queue_progress();
            if (g_pending_present_count < before) {
                progressed = true;
                break;
            }
            pause_briefly();
        }
        if (progressed) {
            continue;
        }
        const uint64_t deadline_tick = wait_deadline(kIdleWaitTicks);
        for (;;) {
            const uint32_t before = g_pending_present_count;
            service_queue_progress();
            if (g_pending_present_count < before) {
                progressed = true;
                break;
            }
            if (!wait_for_gpu_tick(deadline_tick)) {
                break;
            }
            g_gpu_stats.wait_idle_ticks += 1u;
        }
        if (!progressed) {
            g_gpu_stats.idle_timeouts += 1u;
            enter_degraded_mode("wait timeout idle");
            (void)recover_device("wait timeout idle");
            break;
        }
    }
}

bool wait_for_resource_idle(uint32_t resource_id) {
    if (!g_ready) {
        return false;
    }
    if (!resource_pending(resource_id)) {
        return true;
    }

    for (uint32_t spin = 0; spin < kIdleActivePollIterations; ++spin) {
        service_queue_progress();
        if (!resource_pending(resource_id)) {
            return true;
        }
        pause_briefly();
    }

    const uint64_t deadline_tick = wait_deadline(kIdleWaitTicks);
    for (;;) {
        service_queue_progress();
        if (!resource_pending(resource_id)) {
            return true;
        }
        if (!wait_for_gpu_tick(deadline_tick)) {
            break;
        }
    }

    g_gpu_stats.resource_timeouts += 1u;
    console::printf("virtio-gpu: wait timeout resource=%u\n", static_cast<unsigned>(resource_id));
    enter_degraded_mode("wait timeout resource");
    (void)recover_device("wait timeout resource");
    return false;
}

bool wait_for_present_sequence_internal(uint64_t target_sequence, bool& target_failed) {
    if (!g_ready) {
        return false;
    }
    if (query_retired_present(target_sequence, target_failed)) {
        return true;
    }

    for (uint32_t spin = 0; spin < kIdleActivePollIterations; ++spin) {
        g_gpu_stats.wait_present_polls += 1u;
        service_queue_progress();
        if (query_retired_present(target_sequence, target_failed)) {
            return true;
        }
        pause_briefly();
    }

    const uint64_t deadline_tick = wait_deadline(kIdleWaitTicks);
    for (;;) {
        service_queue_progress();
        if (query_retired_present(target_sequence, target_failed)) {
            return true;
        }
        if (!wait_for_gpu_tick(deadline_tick)) {
            break;
        }
        g_gpu_stats.wait_present_ticks += 1u;
    }

    g_gpu_stats.present_wait_timeouts += 1u;
    console::printf(
        "virtio-gpu: wait timeout present sequence=%llu\n",
        static_cast<unsigned long long>(target_sequence));
    enter_degraded_mode("wait timeout present");
    (void)recover_device("wait timeout present");
    return false;
}

bool any_imported_surface_in_use() {
    for (const ImportedSurface& surface : g_imported_surfaces) {
        if (surface.in_use) {
            return true;
        }
    }
    return false;
}

void release_page_backing(uint64_t*& physical_pages, uint64_t& page_count, void*& virtual_address) {
    if (virtual_address != nullptr && page_count != 0) {
        (void)vm::unmap_kernel_pages(virtual_address, page_count);
    }
    if (physical_pages != nullptr) {
        for (uint64_t page_index = 0; page_index < page_count; ++page_index) {
            if (physical_pages[page_index] != 0) {
                (void)memory::free_pages(physical_pages[page_index], 1);
            }
        }
        heap::free(physical_pages);
    }
    physical_pages = nullptr;
    page_count = 0;
    virtual_address = nullptr;
}

bool allocate_page_backing(uint64_t page_count, uint64_t*& physical_pages, void*& virtual_address) {
    if (page_count == 0) {
        return false;
    }

    physical_pages = static_cast<uint64_t*>(
        heap::allocate(static_cast<size_t>(page_count * sizeof(uint64_t)), alignof(uint64_t)));
    if (physical_pages == nullptr) {
        return false;
    }
    memset(physical_pages, 0, static_cast<size_t>(page_count * sizeof(uint64_t)));

    for (uint64_t page_index = 0; page_index < page_count; ++page_index) {
        memory::PageAllocation page = {};
        if (!memory::allocate_page(page)) {
            release_page_backing(physical_pages, page_count, virtual_address);
            return false;
        }

        memset(page.virtual_address, 0, memory::kPageSize);
        physical_pages[page_index] = page.physical_address;
    }

    if (!vm::map_kernel_pages(physical_pages, page_count, vm::kPageWrite, &virtual_address)) {
        release_page_backing(physical_pages, page_count, virtual_address);
        return false;
    }
    return true;
}

bool allocate_surface_backing(Surface& surface, uint64_t page_count) {
    surface.page_count = page_count;
    return allocate_page_backing(surface.page_count, surface.physical_pages, surface.virtual_address);
}

void release_imported_surface(ImportedSurface& surface) {
    if (!surface.in_use) {
        return;
    }

    if (surface.resource_id != 0) {
        (void)wait_for_resource_idle(surface.resource_id);
    }

    const bool restore_front_surface = surface.resource_id != 0 && surface.resource_id == g_scanout_resource_id;
    if (restore_front_surface) {
        g_scanout_resource_id = 0;
        (void)set_scanout_surface(g_front_surface_index);
        Surface* primary = front_surface();
        if (primary != nullptr && primary->virtual_address != nullptr) {
            g_scanout_resource_id = primary->resource_id;
            console::set_external_framebuffer(primary->virtual_address, g_framebuffer_info);
        }
    }

    if (surface.resource_id != 0) {
        (void)destroy_resource(surface.resource_id);
    }
    if (surface.virtual_address != nullptr && surface.page_count != 0) {
        (void)vm::unmap_kernel_pages(surface.virtual_address, surface.page_count);
    }
    if (surface.section != nullptr) {
        object::Header* header = &surface.section->header;
        object::release(header);
    }
    memset(&surface, 0, sizeof(surface));
}

void release_all_imported_surfaces() {
    wait_for_idle_internal();
    for (ImportedSurface& surface : g_imported_surfaces) {
        release_imported_surface(surface);
    }
}

void release_surface_allocation(uint32_t surface_index) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return;
    }

    release_page_backing(surface->physical_pages, surface->page_count, surface->virtual_address);
}

void release_surface_allocations() {
    wait_for_idle_internal();
    for (uint32_t surface_index = 0; surface_index < kGpuSurfaceCount; ++surface_index) {
        release_surface_allocation(surface_index);
        g_surfaces[surface_index].resource_id = 0;
    }
    reset_present_tracking();
    g_front_surface_index = 0;
    g_scanout_resource_id = 0;
}

void release_cursor_plane() {
    if (g_cursor_plane.resource_id != 0) {
        (void)destroy_resource(g_cursor_plane.resource_id);
    }
    release_page_backing(g_cursor_plane.physical_pages, g_cursor_plane.page_count, g_cursor_plane.virtual_address);
    memset(&g_cursor_plane, 0, sizeof(g_cursor_plane));
}

bool configure_cursor_plane(const savanxp_gpu_cursor_image& image) {
    uint64_t page_count = page_count_for_bytes(image.pitch * image.height);
    uint64_t* physical_pages = nullptr;
    void* virtual_address = nullptr;
    const uint32_t row_bytes = image.width * sizeof(uint32_t);
    uint8_t* destination = nullptr;

    if (image.width == 0 || image.height == 0 || image.width > kCursorMaxDimension ||
        image.height > kCursorMaxDimension || image.pitch < row_bytes || image.pixels == 0) {
        return false;
    }
    if (!process::validate_user_range(image.pixels, static_cast<uint64_t>(image.pitch) * image.height, false)) {
        return false;
    }

    if (!allocate_page_backing(page_count, physical_pages, virtual_address)) {
        return false;
    }

    destination = static_cast<uint8_t*>(virtual_address);
    memset(destination, 0, static_cast<size_t>(page_count * memory::kPageSize));
    for (uint32_t row = 0; row < image.height; ++row) {
        if (!process::copy_from_user(
                destination + (static_cast<size_t>(row) * image.pitch),
                image.pixels + (static_cast<uint64_t>(row) * image.pitch),
                row_bytes)) {
            release_page_backing(physical_pages, page_count, virtual_address);
            return false;
        }
    }

    release_cursor_plane();
    g_cursor_plane.resource_id = kCursorResourceId;
    g_cursor_plane.page_count = page_count;
    g_cursor_plane.physical_pages = physical_pages;
    g_cursor_plane.virtual_address = virtual_address;
    g_cursor_plane.info = {
        .width = image.width,
        .height = image.height,
        .pitch = image.pitch,
        .bpp = 32,
        .buffer_size = image.pitch * image.height,
    };
    g_cursor_plane.hot_x = image.hotspot_x;
    g_cursor_plane.hot_y = image.hotspot_y;
    g_cursor_plane.visible = true;

    if (!create_resource_with_format(g_cursor_plane.resource_id, g_cursor_plane.info, kVirtioGpuFormatB8G8R8A8Unorm) ||
        !attach_resource_backing(g_cursor_plane.resource_id, g_cursor_plane.physical_pages, g_cursor_plane.page_count) ||
        !transfer_rect_resource(g_cursor_plane.resource_id, g_cursor_plane.info, 0, 0, g_cursor_plane.info.width, g_cursor_plane.info.height) ||
        !flush_rect_resource(g_cursor_plane.resource_id, 0, 0, g_cursor_plane.info.width, g_cursor_plane.info.height) ||
        !submit_cursor_plane_command(kVirtioGpuCmdUpdateCursor)) {
        release_cursor_plane();
        return false;
    }

    g_cursor_plane.image_loaded = true;
    g_gpu_stats.cursor_updates += 1u;
    return true;
}

bool move_cursor_plane(const savanxp_gpu_cursor_position& position) {
    if (!g_cursor_plane.image_loaded) {
        return false;
    }

    g_cursor_plane.x = position.x;
    g_cursor_plane.y = position.y;
    g_cursor_plane.visible = position.visible != 0;
    if (!submit_cursor_plane_command(kVirtioGpuCmdMoveCursor)) {
        return false;
    }
    g_gpu_stats.cursor_moves += 1u;
    return true;
}

bool create_surface_resource(uint32_t surface_index) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return false;
    }
    return create_resource(surface->resource_id, g_framebuffer_info);
}

bool destroy_surface_resource(uint32_t surface_index) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr || surface->resource_id == 0) {
        return false;
    }
    return destroy_resource(surface->resource_id);
}

bool attach_surface_backing(uint32_t surface_index) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return false;
    }
    return attach_resource_backing(surface->resource_id, surface->physical_pages, surface->page_count);
}

bool build_scanout_request(uint32_t surface_index, VirtioGpuSetScanout& request) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return false;
    }
    return build_scanout_request_for_resource(surface->resource_id, g_framebuffer_info, request);
}

bool build_scanout_request_for_present(const PendingPresent& present, VirtioGpuSetScanout& request) {
    return build_scanout_request_for_resource(present.resource_id, present.info, request);
}

bool set_scanout_surface(uint32_t surface_index) {
    VirtioGpuSetScanout request = {};
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return false;
    }
    if (!(build_scanout_request(surface_index, request) &&
        send_ok_nodata_command(&request, sizeof(request)))) {
        return false;
    }
    g_scanout_resource_id = surface->resource_id;
    return true;
}

bool build_cursor_command(uint32_t type, VirtioGpuUpdateCursor& request) {
    if (!g_cursor_plane.image_loaded || g_cursor_plane.resource_id == 0) {
        return false;
    }

    request = {};
    initialize_header(request.header, type);
    request.pos.scanout_id = g_scanout_id;
    request.pos.x = g_cursor_plane.x;
    request.pos.y = g_cursor_plane.y;
    request.resource_id = g_cursor_plane.visible ? g_cursor_plane.resource_id : 0;
    request.hot_x = g_cursor_plane.hot_x;
    request.hot_y = g_cursor_plane.hot_y;
    return true;
}

bool submit_cursor_plane_command(uint32_t type) {
    VirtioGpuUpdateCursor request = {};
    if (!build_cursor_command(type, request) ||
        !submit_cursor_command(&request, sizeof(request), sizeof(VirtioGpuCtrlHdr))) {
        return false;
    }
    return wait_for_cursor_command();
}

bool build_transfer_request(uint32_t surface_index, uint32_t x, uint32_t y, uint32_t width, uint32_t height, VirtioGpuTransferToHost2d& request) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return false;
    }
    return build_transfer_request_for_resource(surface->resource_id, g_framebuffer_info, x, y, width, height, request);
}

bool build_transfer_request_for_present(const PendingPresent& present, VirtioGpuTransferToHost2d& request) {
    return build_transfer_request_for_resource(present.resource_id, present.info, present.x, present.y, present.width, present.height, request);
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
    return build_flush_request_for_resource(surface->resource_id, x, y, width, height, request);
}

bool build_flush_request_for_present(const PendingPresent& present, VirtioGpuResourceFlush& request) {
    return build_flush_request_for_resource(present.resource_id, present.x, present.y, present.width, present.height, request);
}

bool flush_rect_internal(uint32_t surface_index, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    VirtioGpuResourceFlush request = {};
    return build_flush_request(surface_index, x, y, width, height, request) &&
        send_ok_nodata_command(&request, sizeof(request));
}

bool copy_region_to_surface(uint32_t surface_index, const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr || surface->virtual_address == nullptr) {
        return false;
    }

    const uint8_t* source = static_cast<const uint8_t*>(pixels);
    uint8_t* destination = static_cast<uint8_t*>(surface->virtual_address);
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
        destination_surface->virtual_address == nullptr ||
        source_surface->virtual_address == nullptr) {
        return false;
    }

    memcpy(destination_surface->virtual_address, source_surface->virtual_address, g_framebuffer_info.buffer_size);
    return true;
}

bool present_surface_sync(uint32_t surface_index, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr ||
        !present_resource_sync(surface->resource_id, g_framebuffer_info, x, y, width, height)) {
        return false;
    }

    g_front_surface_index = surface_index;
    if (surface->virtual_address != nullptr) {
        g_scanout_resource_id = surface->resource_id;
        console::set_external_framebuffer(surface->virtual_address, g_framebuffer_info);
    }
    return true;
}

SubmitPresentResult submit_next_present_stage(uint32_t pending_index) {
    if (pending_index >= kPendingPresentCapacity) {
        return kSubmitPresentFailed;
    }

    PendingPresent& present = g_pending_presents[pending_index];
    if (!present.in_use || present.completed || present.failed) {
        return kSubmitPresentFailed;
    }

    uint16_t slot_index = 0;
    if (!reserve_command_slot(slot_index)) {
        return kSubmitPresentDeferred;
    }

    bool submitted = false;
    g_command_slots[slot_index].pending_present_index = pending_index;
    switch (present.next_stage) {
        case kCommandStagePresentTransfer: {
            VirtioGpuTransferToHost2d request = {};
            submitted = build_transfer_request_for_present(present, request);
            if (submitted) {
                g_command_slots[slot_index].stage = kCommandStagePresentTransfer;
                submitted = submit_command_slot(slot_index, &request, sizeof(request), sizeof(VirtioGpuCtrlHdr));
            }
            break;
        }
        case kCommandStagePresentFlush: {
            VirtioGpuResourceFlush request = {};
            submitted = build_flush_request_for_present(present, request);
            if (submitted) {
                g_command_slots[slot_index].stage = kCommandStagePresentFlush;
                submitted = submit_command_slot(slot_index, &request, sizeof(request), sizeof(VirtioGpuCtrlHdr));
            }
            break;
        }
        case kCommandStagePresentScanout: {
            VirtioGpuSetScanout request = {};
            submitted = build_scanout_request_for_present(present, request);
            if (submitted) {
                g_command_slots[slot_index].stage = kCommandStagePresentScanout;
                submitted = submit_command_slot(slot_index, &request, sizeof(request), sizeof(VirtioGpuCtrlHdr));
            }
            break;
        }
        default:
            break;
    }

    if (!submitted) {
        release_command_slot(slot_index);
        return kSubmitPresentFailed;
    }
    present.stage_in_flight = true;
    if (!present.started && present.next_stage == kCommandStagePresentTransfer) {
        present.started = true;
    }
    switch (g_command_slots[slot_index].stage) {
        case kCommandStagePresentTransfer:
            g_gpu_stats.transfer_stage_submitted += 1u;
            break;
        case kCommandStagePresentFlush:
            g_gpu_stats.flush_stage_submitted += 1u;
            break;
        case kCommandStagePresentScanout:
            g_gpu_stats.scanout_stage_submitted += 1u;
            break;
        default:
            break;
    }
    return kSubmitPresentSubmitted;
}

bool submit_pending_present(const PendingPresent& pending) {
    PendingPresent* existing = latest_pending_present_for_resource(pending.resource_id);
    if (existing != nullptr &&
        !existing->started &&
        !existing->stage_in_flight &&
        !existing->completed &&
        !existing->failed) {
        merge_pending_present_rect(*existing, pending.x, pending.y, pending.width, pending.height);
        g_gpu_stats.present_coalesced += 1u;
        submit_ready_deferred_presents();
        finalize_completed_presents();
        return true;
    }

    uint32_t pending_index = 0;
    if (!enqueue_pending_present(pending, pending_index)) {
        return false;
    }

    submit_ready_deferred_presents();
    const bool failed = g_pending_presents[pending_index].failed;
    finalize_completed_presents();
    return !failed;
}

bool submit_present_surface(uint32_t surface_index, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr) {
        return false;
    }

    return submit_pending_present({
        .in_use = false,
        .completed = false,
        .failed = false,
        .started = false,
        .stage_in_flight = false,
        .target = kPendingPresentTargetPrimary,
        .next_stage = kCommandStagePresentTransfer,
        .surface_index = surface_index,
        .surface_id = 0,
        .resource_id = surface->resource_id,
        .info = g_framebuffer_info,
        .virtual_address = surface->virtual_address,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .sequence = 0,
        .enqueue_tick = 0,
    });
}

bool submit_imported_present(const ImportedSurface& surface, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    return submit_pending_present({
        .in_use = false,
        .completed = false,
        .failed = false,
        .started = false,
        .stage_in_flight = false,
        .target = kPendingPresentTargetImported,
        .next_stage = kCommandStagePresentTransfer,
        .surface_index = 0,
        .surface_id = surface.surface_id,
        .resource_id = surface.resource_id,
        .info = surface.info,
        .virtual_address = surface.virtual_address,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .sequence = 0,
        .enqueue_tick = 0,
    });
}

bool choose_scanout(const VirtioGpuRespDisplayInfo& display_info, const boot::FramebufferInfo& boot_framebuffer) {
    uint32_t candidate_scanout_id = 0;
    bool have_candidate = false;

    memset(g_scanouts, 0, sizeof(g_scanouts));
    memset(&g_scanout_state, 0, sizeof(g_scanout_state));
    g_scanout_id = 0;
    g_scanout_width = 0;
    g_scanout_height = 0;

    const uint32_t scanout_count = device_cfg()->num_scanouts < 16 ? device_cfg()->num_scanouts : 16;
    for (uint32_t index = 0; index < scanout_count; ++index) {
        const VirtioGpuDisplayOne& scanout = display_info.scanouts[index];
        GpuScanoutState& state = g_scanouts[index];
        state.scanout_id = index;
        if (scanout.rect.width == 0 || scanout.rect.height == 0) {
            continue;
        }
        state.valid = true;
        state.enabled = scanout.enabled != 0;
        state.native_width = scanout.rect.width;
        state.native_height = scanout.rect.height;
        state.preferred_width = scanout.rect.width;
        state.preferred_height = scanout.rect.height;
        if (!have_candidate || state.enabled) {
            candidate_scanout_id = index;
            have_candidate = true;
        }
        if (state.enabled) {
            g_scanout_id = index;
            g_scanout_width = state.native_width;
            g_scanout_height = state.native_height;
        }
    }
    if ((g_scanout_width == 0 || g_scanout_height == 0) && have_candidate) {
        g_scanout_id = candidate_scanout_id;
        g_scanout_width = g_scanouts[candidate_scanout_id].native_width;
        g_scanout_height = g_scanouts[candidate_scanout_id].native_height;
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
    update_active_scanout_mode(width, height);
    refresh_gpu_info_flags();
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
        .flags = current_gpu_info_flags(),
    };
    update_active_scanout_mode(width, height);
}

bool ensure_primary_scanout_restored() {
    Surface* primary = front_surface();
    if (primary == nullptr || primary->virtual_address == nullptr || primary->resource_id == 0) {
        return false;
    }
    if (!set_scanout_surface(g_front_surface_index)) {
        return false;
    }
    console::set_external_framebuffer(primary->virtual_address, g_framebuffer_info);
    g_scanout_resource_id = primary->resource_id;
    return true;
}

bool refresh_scanouts(bool explicit_refresh) {
    VirtioGpuRespDisplayInfo display_info = {};
    uint32_t candidate_scanout_id = g_scanout_id;
    bool have_candidate = false;

    if (!get_display_info(display_info)) {
        if (explicit_refresh) {
            log_command_failure("GET_DISPLAY_INFO refresh");
        }
        return false;
    }

    memset(g_scanouts, 0, sizeof(g_scanouts));
    const uint32_t scanout_count = device_cfg()->num_scanouts < 16 ? device_cfg()->num_scanouts : 16;
    for (uint32_t index = 0; index < scanout_count; ++index) {
        const VirtioGpuDisplayOne& scanout = display_info.scanouts[index];
        GpuScanoutState& state = g_scanouts[index];
        if (scanout.rect.width == 0 || scanout.rect.height == 0) {
            continue;
        }

        state.valid = true;
        state.enabled = scanout.enabled != 0;
        state.scanout_id = index;
        state.native_width = scanout.rect.width;
        state.native_height = scanout.rect.height;
        state.preferred_width = index == g_scanout_id && g_preferred_scanout_width != 0 ? g_preferred_scanout_width : scanout.rect.width;
        state.preferred_height = index == g_scanout_id && g_preferred_scanout_height != 0 ? g_preferred_scanout_height : scanout.rect.height;
        if (!have_candidate || state.enabled) {
            candidate_scanout_id = index;
            have_candidate = true;
        }
    }

    if (!have_candidate) {
        return false;
    }
    if (!g_scanouts[g_scanout_id].valid) {
        g_scanout_id = candidate_scanout_id;
    }

    g_scanout_width = g_scanouts[g_scanout_id].native_width;
    g_scanout_height = g_scanouts[g_scanout_id].native_height;
    update_active_scanout_mode(g_framebuffer_info.width, g_framebuffer_info.height);
    if (!ensure_primary_scanout_restored()) {
        return false;
    }

    g_gpu_stats.scanout_refreshes += 1u;
    refresh_cached_scanout_state();
    refresh_gpu_info_flags();
    return true;
}

bool configure_primary_surface(uint32_t width, uint32_t height) {
    bool resource_created[kGpuSurfaceCount] = {};

    release_surface_allocations();
    g_preferred_scanout_width = width;
    g_preferred_scanout_height = height;
    set_framebuffer_mode(width, height);
    console::printf("virtio-gpu: primary mode %ux%u\n", static_cast<unsigned>(width), static_cast<unsigned>(height));

    const uint64_t page_count = page_count_for_bytes(g_framebuffer_info.buffer_size);
    console::printf("virtio-gpu: primary alloc %u pages/surface\n", static_cast<unsigned>(page_count));
    for (uint32_t surface_index = 0; surface_index < kGpuSurfaceCount; ++surface_index) {
        Surface* surface = surface_at(surface_index);
        if (surface == nullptr) {
            continue;
        }
        surface->resource_id = kFirstGpuResourceId + surface_index;
        if (!allocate_surface_backing(*surface, page_count)) {
            release_surface_allocations();
            return false;
        }
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
    console::write_line("virtio-gpu: primary resources created");

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
    console::write_line("virtio-gpu: primary backing attached");

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
    console::write_line("virtio-gpu: primary scanout armed");

    return true;
}

bool fill_mode_info(savanxp_gpu_mode& mode) {
    mode = {
        .width = g_framebuffer_info.width,
        .height = g_framebuffer_info.height,
        .pitch = g_framebuffer_info.pitch,
        .bpp = g_framebuffer_info.bpp,
        .buffer_size = g_framebuffer_info.buffer_size,
    };
    return true;
}

bool normalize_import_info(const savanxp_gpu_surface_import& request, savanxp_fb_info& info) {
    const uint32_t width = request.width != 0 ? request.width : g_framebuffer_info.width;
    const uint32_t height = request.height != 0 ? request.height : g_framebuffer_info.height;
    const uint32_t bpp = request.bpp != 0 ? request.bpp : 32u;
    const uint32_t pitch = request.pitch != 0 ? request.pitch : static_cast<uint32_t>(width * sizeof(uint32_t));
    const uint32_t buffer_size = request.buffer_size != 0 ? request.buffer_size : static_cast<uint32_t>(pitch * height);

    if (width == 0 || height == 0 || bpp != 32u || pitch < (width * sizeof(uint32_t))) {
        return false;
    }
    if (buffer_size < (pitch * height)) {
        return false;
    }
    if (width != g_framebuffer_info.width || height != g_framebuffer_info.height) {
        return false;
    }

    info = {
        .width = width,
        .height = height,
        .pitch = pitch,
        .bpp = static_cast<uint32_t>(bpp),
        .buffer_size = buffer_size,
    };
    return true;
}

ImportedSurface* allocate_imported_surface_slot() {
    for (uint32_t index = 0; index < kImportedSurfaceCount; ++index) {
        ImportedSurface& surface = g_imported_surfaces[index];
        if (!surface.in_use) {
            memset(&surface, 0, sizeof(surface));
            surface.in_use = true;
            surface.surface_id = index + 1u;
            surface.resource_id = kFirstGpuResourceId + kGpuSurfaceCount + index;
            return &surface;
        }
    }
    return nullptr;
}

int set_mode_ioctl(uint64_t argument) {
    if (!process::validate_user_range(argument, sizeof(savanxp_gpu_mode), true)) {
        return negative_error(SAVANXP_EINVAL);
    }

    savanxp_gpu_mode mode = {};
    if (!process::copy_from_user(&mode, argument, sizeof(mode))) {
        return negative_error(SAVANXP_EINVAL);
    }

    const uint32_t requested_width = mode.width != 0 ? mode.width : g_framebuffer_info.width;
    const uint32_t requested_height = mode.height != 0 ? mode.height : g_framebuffer_info.height;
    if (requested_width == 0 || requested_height == 0) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (mode.bpp != 0 && mode.bpp != 32u) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (any_imported_surface_in_use()) {
        return negative_error(SAVANXP_EBUSY);
    }

    if ((requested_width != g_framebuffer_info.width || requested_height != g_framebuffer_info.height) &&
        !configure_primary_surface(requested_width, requested_height)) {
        return negative_error(SAVANXP_EIO);
    }

    virtio_input::set_framebuffer_extent(g_framebuffer_info.width, g_framebuffer_info.height);
    fill_mode_info(mode);
    return process::copy_to_user(argument, &mode, sizeof(mode)) ? 0 : negative_error(SAVANXP_EINVAL);
}

int import_surface_ioctl(uint64_t argument) {
    if (!process::validate_user_range(argument, sizeof(savanxp_gpu_surface_import), true)) {
        return negative_error(SAVANXP_EINVAL);
    }

    savanxp_gpu_surface_import request = {};
    if (!process::copy_from_user(&request, argument, sizeof(request))) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (request.section_handle < 0) {
        return negative_error(SAVANXP_EBADF);
    }
    process::Process* current = process::current();
    if (current == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }

    if (static_cast<uint64_t>(request.section_handle) >= process::kMaxFileHandles) {
        return negative_error(SAVANXP_EBADF);
    }
    process::HandleEntry& entry = current->handles[request.section_handle];
    if (entry.object == nullptr || (entry.granted_access & object::access_query) == 0) {
        return negative_error(SAVANXP_EBADF);
    }
    object::SectionObject* section_object = object::as_section(entry.object);
    if (section_object == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }

    savanxp_fb_info info = {};
    if (!normalize_import_info(request, info)) {
        return negative_error(SAVANXP_EINVAL);
    }
    const uint64_t page_count = page_count_for_bytes(info.buffer_size);
    if (section_object->size_bytes < info.buffer_size ||
        section_object->physical_pages == nullptr ||
        section_object->page_count < page_count) {
        return negative_error(SAVANXP_EINVAL);
    }

    ImportedSurface* imported = allocate_imported_surface_slot();
    if (imported == nullptr) {
        return negative_error(SAVANXP_ENOMEM);
    }

    imported->flags = request.flags;
    imported->page_count = page_count;
    imported->info = info;
    imported->section = section_object;
    object::retain(&section_object->header);

    if (!vm::map_kernel_pages(section_object->physical_pages, page_count, vm::kPageWrite, &imported->virtual_address)) {
        release_imported_surface(*imported);
        return negative_error(SAVANXP_EIO);
    }

    if (!create_resource(imported->resource_id, imported->info) ||
        !attach_resource_backing(imported->resource_id, section_object->physical_pages, page_count)) {
        release_imported_surface(*imported);
        return negative_error(SAVANXP_EIO);
    }

    request.surface_id = static_cast<int32_t>(imported->surface_id);
    request.width = imported->info.width;
    request.height = imported->info.height;
    request.pitch = imported->info.pitch;
    request.bpp = imported->info.bpp;
    request.buffer_size = imported->info.buffer_size;
    return process::copy_to_user(argument, &request, sizeof(request)) ? 0 : negative_error(SAVANXP_EINVAL);
}

int release_surface_ioctl(uint64_t argument) {
    const uint32_t surface_id = static_cast<uint32_t>(argument);
    ImportedSurface* surface = imported_surface_at(surface_id);
    if (surface == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }
    release_imported_surface(*surface);
    return 0;
}

int present_surface_region_ioctl(uint64_t argument) {
    if (!process::validate_user_range(argument, sizeof(savanxp_gpu_surface_present), true)) {
        return negative_error(SAVANXP_EINVAL);
    }

    savanxp_gpu_surface_present present = {};
    if (!process::copy_from_user(&present, argument, sizeof(present))) {
        return negative_error(SAVANXP_EINVAL);
    }

    ImportedSurface* surface = imported_surface_at(present.surface_id);
    if (surface == nullptr) {
        return negative_error(SAVANXP_EBADF);
    }
    if (present.width == 0 || present.height == 0 ||
        present.x >= surface->info.width || present.y >= surface->info.height ||
        present.width > (surface->info.width - present.x) ||
        present.height > (surface->info.height - present.y)) {
        return negative_error(SAVANXP_EINVAL);
    }

    if (!submit_imported_present(*surface, present.x, present.y, present.width, present.height)) {
        return negative_error(SAVANXP_EIO);
    }
    if (surface->resource_id != g_scanout_resource_id) {
        if (!wait_for_resource_idle(surface->resource_id)) {
            return negative_error(SAVANXP_EIO);
        }
    } else {
        service_queue_progress();
    }
    return 0;
}

int get_stats_ioctl(uint64_t argument) {
    if (!process::validate_user_range(argument, sizeof(g_gpu_stats), true)) {
        return negative_error(SAVANXP_EINVAL);
    }
    return process::copy_to_user(argument, &g_gpu_stats, sizeof(g_gpu_stats))
        ? 0
        : negative_error(SAVANXP_EINVAL);
}

int get_present_timeline_ioctl(uint64_t argument) {
    savanxp_gpu_present_timeline timeline = {};
    if (!process::validate_user_range(argument, sizeof(timeline), true)) {
        return negative_error(SAVANXP_EINVAL);
    }

    fill_present_timeline(timeline);
    return process::copy_to_user(argument, &timeline, sizeof(timeline))
        ? 0
        : negative_error(SAVANXP_EINVAL);
}

int wait_present_ioctl(uint64_t argument) {
    savanxp_gpu_present_wait request = {};
    bool target_failed = false;

    if (!process::validate_user_range(argument, sizeof(request), true) ||
        !process::copy_from_user(&request, argument, sizeof(request))) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (request.target_sequence != 0 &&
        request.target_sequence > g_last_submitted_present_sequence) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (request.target_sequence != 0 &&
        !wait_for_present_sequence_internal(request.target_sequence, target_failed)) {
        return negative_error(SAVANXP_ETIMEDOUT);
    }

    request.retired_sequence = g_last_retired_present_sequence;
    request.pending_count = g_pending_present_count;
    request.flags = current_present_timeline_flags();
    if (target_failed) {
        request.flags |= SAVANXP_GPU_PRESENT_TIMELINE_FLAG_TARGET_FAILED;
    }
    return process::copy_to_user(argument, &request, sizeof(request))
        ? 0
        : negative_error(SAVANXP_EINVAL);
}

int get_scanouts_ioctl(uint64_t argument) {
    if (!process::validate_user_range(argument, sizeof(g_scanout_state), true)) {
        return negative_error(SAVANXP_EINVAL);
    }
    refresh_cached_scanout_state();
    return process::copy_to_user(argument, &g_scanout_state, sizeof(g_scanout_state))
        ? 0
        : negative_error(SAVANXP_EINVAL);
}

int refresh_scanouts_ioctl() {
    return refresh_scanouts(true) ? 0 : negative_error(SAVANXP_EIO);
}

int set_cursor_ioctl(uint64_t argument) {
    savanxp_gpu_cursor_image image = {};

    if (!g_cursor_queue.enabled) {
        return negative_error(SAVANXP_ENODEV);
    }
    if (!process::validate_user_range(argument, sizeof(image), true) ||
        !process::copy_from_user(&image, argument, sizeof(image))) {
        return negative_error(SAVANXP_EINVAL);
    }
    return configure_cursor_plane(image) ? 0 : negative_error(SAVANXP_EIO);
}

int move_cursor_ioctl(uint64_t argument) {
    savanxp_gpu_cursor_position position = {};

    if (!g_cursor_queue.enabled) {
        return negative_error(SAVANXP_ENODEV);
    }
    if (!process::validate_user_range(argument, sizeof(position), true) ||
        !process::copy_from_user(&position, argument, sizeof(position))) {
        return negative_error(SAVANXP_EINVAL);
    }

    if (position.x >= g_framebuffer_info.width) {
        position.x = g_framebuffer_info.width != 0 ? g_framebuffer_info.width - 1u : 0u;
    }
    if (position.y >= g_framebuffer_info.height) {
        position.y = g_framebuffer_info.height != 0 ? g_framebuffer_info.height - 1u : 0u;
    }
    return move_cursor_plane(position) ? 0 : negative_error(SAVANXP_EIO);
}

int gpu_ioctl(uint64_t request, uint64_t argument) {
    DriverBusyGuard guard;
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
            release_all_imported_surfaces();
            release_cursor_plane();
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
        case GPU_IOC_SET_MODE:
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            return set_mode_ioctl(argument);
        case GPU_IOC_IMPORT_SECTION:
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            return import_surface_ioctl(argument);
        case GPU_IOC_RELEASE_SURFACE:
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            return release_surface_ioctl(argument);
        case GPU_IOC_PRESENT_SURFACE_REGION:
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            return present_surface_region_ioctl(argument);
        case GPU_IOC_WAIT_IDLE:
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            virtio_gpu::wait_for_idle();
            return 0;
        case GPU_IOC_GET_STATS:
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            return get_stats_ioctl(argument);
        case GPU_IOC_GET_PRESENT_TIMELINE:
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            return get_present_timeline_ioctl(argument);
        case GPU_IOC_WAIT_PRESENT:
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            return wait_present_ioctl(argument);
        case GPU_IOC_GET_SCANOUTS:
            return get_scanouts_ioctl(argument);
        case GPU_IOC_REFRESH_SCANOUTS:
            return refresh_scanouts_ioctl();
        case GPU_IOC_SET_CURSOR:
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            return set_cursor_ioctl(argument);
        case GPU_IOC_MOVE_CURSOR:
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            return move_cursor_ioctl(argument);
        default:
            return negative_error(SAVANXP_ENOSYS);
    }
}

void gpu_close() {
    DriverBusyGuard guard;
    if (ui::owns_graphics_session(process::current_pid())) {
        release_all_imported_surfaces();
        release_cursor_plane();
        ui::release_graphics_session(process::current_pid());
    }
}

void release_queue(virtio_pci::Queue& queue) {
    if (queue.allocation.page_count != 0) {
        (void)memory::free_allocation(queue.allocation);
    }
    memset(&queue, 0, sizeof(queue));
}

bool setup_control_queue() {
    release_queue(g_control_queue);
    if (!virtio_pci::setup_queue(g_device, kVirtioGpuControlQueue, kVirtioGpuQueueDescriptorLimit, kQueueExtraBytes, 16, g_control_queue)) {
        return false;
    }
    g_command_slot_capacity = g_control_queue.size / 2u;
    if (g_command_slot_capacity > kCommandSlotCount) {
        g_command_slot_capacity = kCommandSlotCount;
    }
    reset_present_tracking();
    return g_command_slot_capacity >= 3u;
}

bool setup_cursor_queue() {
    release_queue(g_cursor_queue);
    if (!virtio_pci::setup_queue(g_device, kVirtioGpuCursorQueue, kVirtioGpuCursorQueueDescriptorLimit, kCursorQueueExtraBytes, 16, g_cursor_queue)) {
        return false;
    }
    g_cursor_command = {};
    return true;
}

bool rearm_primary_resources() {
    for (uint32_t surface_index = 0; surface_index < kGpuSurfaceCount; ++surface_index) {
        Surface* surface = surface_at(surface_index);
        if (surface == nullptr || surface->resource_id == 0 || surface->page_count == 0 || surface->physical_pages == nullptr) {
            continue;
        }
        if (!create_resource(surface->resource_id, g_framebuffer_info) ||
            !attach_resource_backing(surface->resource_id, surface->physical_pages, surface->page_count)) {
            return false;
        }
    }
    g_scanout_resource_id = 0;
    return ensure_primary_scanout_restored();
}

bool rearm_imported_resources() {
    for (ImportedSurface& surface : g_imported_surfaces) {
        if (!surface.in_use || surface.resource_id == 0 || surface.section == nullptr || surface.page_count == 0) {
            continue;
        }
        if (!create_resource(surface.resource_id, surface.info) ||
            !attach_resource_backing(surface.resource_id, surface.section->physical_pages, surface.page_count)) {
            return false;
        }
    }
    return true;
}

bool rearm_cursor_resource() {
    if (!g_cursor_plane.image_loaded || g_cursor_plane.resource_id == 0 || g_cursor_plane.page_count == 0 || !g_cursor_queue.enabled) {
        return true;
    }
    return create_resource_with_format(g_cursor_plane.resource_id, g_cursor_plane.info, kVirtioGpuFormatB8G8R8A8Unorm) &&
        attach_resource_backing(g_cursor_plane.resource_id, g_cursor_plane.physical_pages, g_cursor_plane.page_count) &&
        transfer_rect_resource(g_cursor_plane.resource_id, g_cursor_plane.info, 0, 0, g_cursor_plane.info.width, g_cursor_plane.info.height) &&
        flush_rect_resource(g_cursor_plane.resource_id, 0, 0, g_cursor_plane.info.width, g_cursor_plane.info.height) &&
        submit_cursor_plane_command(kVirtioGpuCmdUpdateCursor);
}

void enter_degraded_mode(const char* reason) {
    if (g_degraded) {
        return;
    }
    g_degraded = true;
    g_gpu_stats.degraded_entries += 1u;
    if (reason != nullptr) {
        console::printf("virtio-gpu: degraded %s\n", reason);
    }
    (void)ensure_primary_scanout_restored();
}

bool recover_device(const char* reason) {
    if (!g_present) {
        return false;
    }

    g_gpu_stats.recovery_attempts += 1u;
    if (reason != nullptr) {
        console::printf("virtio-gpu: recovery %s\n", reason);
    }

    virtio_pci::set_device_status(g_device, 0);
    virtio_pci::memory_barrier();
    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::kStatusAcknowledge | virtio_pci::kStatusDriver));
    if (!virtio_pci::negotiate_features(g_device, 0, virtio_pci::kFeatureVersion1Bit) ||
        !setup_control_queue()) {
        fail_device("recovery failed to restore queues");
        return false;
    }
    if (!setup_cursor_queue()) {
        release_queue(g_cursor_queue);
    }

    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::device_status(g_device) | virtio_pci::kStatusDriverOk));
    g_ready = true;
    g_irq_work_pending = false;
    g_scanout_refresh_pending = false;

    if (!rearm_primary_resources() || !rearm_imported_resources() || !rearm_cursor_resource()) {
        fail_device("recovery failed to rearm resources");
        return false;
    }

    g_degraded = false;
    refresh_gpu_info_flags();
    g_gpu_stats.recovery_successes += 1u;
    return true;
}

void service_background_work() {
    if (!g_ready || g_driver_busy_depth != 0 || g_background_service_active) {
        return;
    }
    if (!g_irq_work_pending &&
        !command_slots_in_flight() &&
        g_pending_present_count == 0 &&
        !g_scanout_refresh_pending) {
        return;
    }
    g_background_service_active = true;
    service_queue_progress();
    if (g_scanout_refresh_pending) {
        const uint32_t events = device_cfg()->events_read;
        if (events != 0) {
            device_cfg()->events_clear = events;
        }
        g_scanout_refresh_pending = false;
        if ((events & kVirtioGpuEventDisplay) != 0) {
            (void)refresh_scanouts(false);
        }
    }
    g_irq_work_pending = false;
    g_background_service_active = false;
}

void gpu_irq() {
    const uint8_t status = virtio_pci::read_isr_status(g_device);
    if (status == 0) {
        return;
    }

    g_gpu_stats.irq_notifications += 1u;
    if ((status & 0x2u) != 0) {
        g_scanout_refresh_pending = true;
        g_gpu_stats.irq_config_events += 1u;
    }
    g_irq_work_pending = true;
}

void fail_device(const char* reason) {
    g_ready = false;
    release_all_imported_surfaces();
    release_cursor_plane();
    virtio_pci::fail_device(g_device);
    if (reason != nullptr) {
        console::printf("virtio-gpu: %s\n", reason);
    }
    g_degraded = true;
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

void log_init_stage(const char* stage) {
    if (stage != nullptr) {
        console::printf("virtio-gpu: init %s\n", stage);
    }
}

} // namespace

namespace virtio_gpu {

void initialize(const boot::FramebufferInfo& framebuffer) {
    DriverBusyGuard guard;
    log_init_stage("probe");
    memset(&g_device, 0, sizeof(g_device));
    memset(&g_control_queue, 0, sizeof(g_control_queue));
    memset(&g_cursor_queue, 0, sizeof(g_cursor_queue));
    memset(g_surfaces, 0, sizeof(g_surfaces));
    memset(g_imported_surfaces, 0, sizeof(g_imported_surfaces));
    memset(g_scanouts, 0, sizeof(g_scanouts));
    memset(&g_cursor_plane, 0, sizeof(g_cursor_plane));
    memset(&g_framebuffer_info, 0, sizeof(g_framebuffer_info));
    memset(&g_gpu_info, 0, sizeof(g_gpu_info));
    memset(&g_scanout_state, 0, sizeof(g_scanout_state));
    memset(&g_gpu_stats, 0, sizeof(g_gpu_stats));
    memset(g_command_slots, 0, sizeof(g_command_slots));
    memset(g_pending_presents, 0, sizeof(g_pending_presents));
    memset(g_pending_present_order, 0, sizeof(g_pending_present_order));
    memset(g_retired_present_history, 0, sizeof(g_retired_present_history));
    g_front_surface_index = 0;
    g_present = false;
    g_ready = false;
    g_degraded = false;
    g_scanout_resource_id = 0;
    g_preferred_scanout_width = 0;
    g_preferred_scanout_height = 0;
    g_next_fence_id = 1;
    g_next_present_sequence = 1;
    g_last_submitted_present_sequence = 0;
    g_last_retired_present_sequence = 0;
    g_last_response_type = 0;
    g_command_slot_capacity = 0;
    g_cursor_command = {};
    g_retired_present_history_next = 0;
    g_irq_line = 0xffu;
    g_irq_registered = false;
    g_irq_work_pending = false;
    g_scanout_refresh_pending = false;
    g_pending_present_count = 0;
    reset_present_tracking();

    pci::DeviceInfo pci_device = {};
    if (!pci::ready() || !virtio_pci::find_modern_device(kVirtioGpuModernDevice, kVirtioGpuSubsystemDevice, pci_device)) {
        return;
    }
    g_present = true;
    log_init_stage("pci found");

    if (!virtio_pci::initialize_device(pci_device, true, g_device)) {
        console::write_line("virtio-gpu: missing required MMIO capabilities");
        return;
    }
    log_init_stage("mmio ready");

    virtio_pci::set_device_status(g_device, 0);
    virtio_pci::memory_barrier();
    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::kStatusAcknowledge | virtio_pci::kStatusDriver));

    if (!virtio_pci::negotiate_features(g_device, 0, virtio_pci::kFeatureVersion1Bit)) {
        fail_device("feature negotiation failed");
        return;
    }
    log_init_stage("features negotiated");
    if (!setup_control_queue()) {
        fail_device("failed to setup control queue");
        return;
    }
    log_init_stage("control queue ready");
    if (!setup_cursor_queue()) {
        release_queue(g_cursor_queue);
        console::write_line("virtio-gpu: cursor queue unavailable, using software cursor fallback");
    } else {
        log_init_stage("cursor queue ready");
    }

    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::device_status(g_device) | virtio_pci::kStatusDriverOk));
    g_ready = true;
    if (g_device.isr_view.valid &&
        pci_device.irq_line != 0 &&
        pci_device.irq_line != 0xff &&
        pci_device.irq_line < 16 &&
        arch::x86_64::register_irq_handler(pci_device.irq_line, gpu_irq)) {
        arch::x86_64::enable_irq(pci_device.irq_line);
        g_irq_line = pci_device.irq_line;
        g_irq_registered = true;
        console::printf("virtio-gpu: irq line %u armed\n", static_cast<unsigned>(g_irq_line));
    } else {
        console::write_line("virtio-gpu: IRQ unavailable, keeping polling fallback");
    }
    log_init_stage("driver ok");

    VirtioGpuRespDisplayInfo display_info = {};
    log_init_stage("query display info");
    if (!get_display_info(display_info)) {
        log_command_failure("GET_DISPLAY_INFO");
        fail_device("failed to query display info");
        return;
    }
    log_init_stage("display info ready");
    if (!choose_scanout(display_info, framebuffer)) {
        fail_device("failed to choose scanout");
        return;
    }
    console::printf(
        "virtio-gpu: selected scanout=%u native=%ux%u\n",
        static_cast<unsigned>(g_scanout_id),
        static_cast<unsigned>(g_scanout_width),
        static_cast<unsigned>(g_scanout_height));

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

    if (front_surface() == nullptr || front_surface()->virtual_address == nullptr) {
        fail_device("failed to allocate primary surface");
        return;
    }

    g_gpu_device.ioctl = gpu_ioctl;
    g_gpu_device.close = gpu_close;
    if (!device::register_node("/dev/gpu0", &g_gpu_device, true)) {
        fail_device("failed to register /dev/gpu0");
        return;
    }

    console::set_external_framebuffer(front_surface()->virtual_address, g_framebuffer_info);
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

void poll() {
    service_background_work();
}

const savanxp_fb_info& framebuffer_info() {
    return g_framebuffer_info;
}

void* framebuffer_address() {
    Surface* surface = front_surface();
    return surface != nullptr ? surface->virtual_address : nullptr;
}

void wait_for_idle() {
    DriverBusyGuard guard;
    if (!g_ready) {
        return;
    }
    wait_for_idle_internal();
}

bool flush() {
    DriverBusyGuard guard;
    return flush_rect(0, 0, g_framebuffer_info.width, g_framebuffer_info.height);
}

bool flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    DriverBusyGuard guard;
    if (!g_ready || width == 0 || height == 0 ||
        x >= g_framebuffer_info.width || y >= g_framebuffer_info.height ||
        width > (g_framebuffer_info.width - x) || height > (g_framebuffer_info.height - y)) {
        return false;
    }
    return transfer_rect(g_front_surface_index, x, y, width, height) &&
        flush_rect_internal(g_front_surface_index, x, y, width, height);
}

bool present_from_kernel(const void* pixels, size_t byte_count) {
    DriverBusyGuard guard;
    if (!g_ready || pixels == nullptr || byte_count != g_framebuffer_info.buffer_size) {
        return false;
    }

    uint32_t surface_index = 0;
    if (!acquire_present_surface(surface_index)) {
        return false;
    }
    Surface* surface = surface_at(surface_index);
    if (surface == nullptr || surface->virtual_address == nullptr) {
        return false;
    }

    memcpy(surface->virtual_address, pixels, byte_count);
    if (!submit_present_surface(surface_index, 0, 0, g_framebuffer_info.width, g_framebuffer_info.height)) {
        return false;
    }
    uint32_t free_surface = 0;
    return acquire_present_surface(free_surface);
}

bool present_region_from_kernel(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    DriverBusyGuard guard;
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
