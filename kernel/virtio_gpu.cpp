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
constexpr uint16_t kVirtioGpuMaxDescriptors = 2;
constexpr uint32_t kVirtioGpuFormatB8G8R8X8Unorm = 2u;
constexpr uint32_t kVirtioGpuFlagFence = 1u << 0;
constexpr uint32_t kVirtioGpuCmdGetDisplayInfo = 0x0100u;
constexpr uint32_t kVirtioGpuCmdResourceCreate2d = 0x0101u;
constexpr uint32_t kVirtioGpuCmdSetScanout = 0x0103u;
constexpr uint32_t kVirtioGpuCmdResourceFlush = 0x0104u;
constexpr uint32_t kVirtioGpuCmdTransferToHost2d = 0x0105u;
constexpr uint32_t kVirtioGpuCmdResourceAttachBacking = 0x0106u;
constexpr uint32_t kVirtioGpuRespOkNoData = 0x1100u;
constexpr uint32_t kVirtioGpuRespOkDisplayInfo = 0x1101u;
constexpr uint32_t kGpuResourceId = 1u;
constexpr size_t kRequestBufferBytes = 512;
constexpr size_t kResponseBufferBytes = 512;
constexpr size_t kQueueExtraBytes = kRequestBufferBytes + kResponseBufferBytes;
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

device::Device g_gpu_device = {
    .name = "gpu0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
};

virtio_pci::Device g_device = {};
virtio_pci::Queue g_control_queue = {};
memory::PageAllocation g_surface = {};
savanxp_fb_info g_framebuffer_info = {};
savanxp_gpu_info g_gpu_info = {};
uint32_t g_scanout_id = 0;
uint32_t g_scanout_width = 0;
uint32_t g_scanout_height = 0;
uint64_t g_next_fence_id = 1;
bool g_present = false;
bool g_ready = false;

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

bool wait_for_used_element(virtio_pci::UsedElement& element) {
    const volatile virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_control_queue);
    const virtio_pci::UsedElement* ring = virtio_pci::queue_used_ring(g_control_queue);

    for (uint32_t spin = 0; spin < kCommandTimeoutSpins; ++spin) {
        virtio_pci::memory_barrier();
        if (g_control_queue.last_used_index == used->idx) {
            continue;
        }

        element = ring[g_control_queue.last_used_index % g_control_queue.size];
        g_control_queue.last_used_index = static_cast<uint16_t>(g_control_queue.last_used_index + 1);
        return true;
    }
    return false;
}

bool submit_command(const void* request, size_t request_bytes, void* response, size_t response_bytes) {
    if (!g_ready || !g_control_queue.enabled || request == nullptr || response == nullptr ||
        request_bytes == 0 || response_bytes == 0 ||
        request_bytes > kRequestBufferBytes || response_bytes > kResponseBufferBytes) {
        return false;
    }

    void* request_buffer = virtio_pci::queue_extra(g_control_queue);
    void* response_buffer = virtio_pci::queue_extra(g_control_queue, kResponseBufferOffset);
    memcpy(request_buffer, request, request_bytes);
    memset(response_buffer, 0, response_bytes);

    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_control_queue);
    descriptors[0] = {
        .addr = virtio_pci::queue_extra_physical(g_control_queue),
        .len = static_cast<uint32_t>(request_bytes),
        .flags = virtio_pci::kDescriptorFlagNext,
        .next = 1,
    };
    descriptors[1] = {
        .addr = virtio_pci::queue_extra_physical(g_control_queue, kResponseBufferOffset),
        .len = static_cast<uint32_t>(response_bytes),
        .flags = virtio_pci::kDescriptorFlagWrite,
        .next = 0,
    };

    if (!virtio_pci::submit_descriptor_head(g_control_queue, 0)) {
        return false;
    }
    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_control_queue);

    virtio_pci::UsedElement element = {};
    if (!wait_for_used_element(element) || element.id != 0) {
        return false;
    }

    memcpy(response, response_buffer, response_bytes);
    return true;
}

bool send_ok_nodata_command(const void* request, size_t request_bytes) {
    VirtioGpuCtrlHdr response = {};
    if (!submit_command(request, request_bytes, &response, sizeof(response))) {
        return false;
    }
    return response.type == kVirtioGpuRespOkNoData;
}

bool get_display_info(VirtioGpuRespDisplayInfo& display_info) {
    VirtioGpuCtrlHdr request = {};
    initialize_header(request, kVirtioGpuCmdGetDisplayInfo);
    if (!submit_command(&request, sizeof(request), &display_info, sizeof(display_info))) {
        return false;
    }
    return display_info.header.type == kVirtioGpuRespOkDisplayInfo;
}

bool create_primary_resource() {
    VirtioGpuResourceCreate2d request = {};
    initialize_header(request.header, kVirtioGpuCmdResourceCreate2d);
    request.resource_id = kGpuResourceId;
    request.format = kVirtioGpuFormatB8G8R8X8Unorm;
    request.width = g_framebuffer_info.width;
    request.height = g_framebuffer_info.height;
    return send_ok_nodata_command(&request, sizeof(request));
}

bool attach_primary_backing() {
    VirtioGpuResourceAttachBacking request = {};
    initialize_header(request.header, kVirtioGpuCmdResourceAttachBacking);
    request.resource_id = kGpuResourceId;
    request.nr_entries = 1;
    request.entry.addr = g_surface.physical_address;
    request.entry.length = g_framebuffer_info.buffer_size;
    return send_ok_nodata_command(&request, sizeof(request));
}

bool set_primary_scanout() {
    VirtioGpuSetScanout request = {};
    initialize_header(request.header, kVirtioGpuCmdSetScanout);
    request.rect = {
        .x = 0,
        .y = 0,
        .width = g_framebuffer_info.width,
        .height = g_framebuffer_info.height,
    };
    request.scanout_id = g_scanout_id;
    request.resource_id = kGpuResourceId;
    return send_ok_nodata_command(&request, sizeof(request));
}

bool transfer_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    VirtioGpuTransferToHost2d request = {};
    initialize_header(request.header, kVirtioGpuCmdTransferToHost2d);
    request.rect = {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
    };
    request.offset = (static_cast<uint64_t>(y) * g_framebuffer_info.pitch) + (static_cast<uint64_t>(x) * sizeof(uint32_t));
    request.resource_id = kGpuResourceId;
    return send_ok_nodata_command(&request, sizeof(request));
}

bool flush_rect_internal(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    VirtioGpuResourceFlush request = {};
    initialize_header(request.header, kVirtioGpuCmdResourceFlush);
    request.rect = {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
    };
    request.resource_id = kGpuResourceId;
    return send_ok_nodata_command(&request, sizeof(request));
}

bool copy_region(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    const uint8_t* source = static_cast<const uint8_t*>(pixels);
    uint8_t* destination = static_cast<uint8_t*>(g_surface.virtual_address);
    const size_t row_bytes = static_cast<size_t>(width) * sizeof(uint32_t);

    for (uint32_t row = 0; row < height; ++row) {
        const size_t source_offset = static_cast<size_t>(y + row) * source_pitch + (static_cast<size_t>(x) * sizeof(uint32_t));
        const size_t destination_offset = static_cast<size_t>(y + row) * g_framebuffer_info.pitch + (static_cast<size_t>(x) * sizeof(uint32_t));
        memcpy(destination + destination_offset, source + source_offset, row_bytes);
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

    uint32_t width = g_scanout_width;
    uint32_t height = g_scanout_height;
    if (boot_framebuffer.available &&
        boot_framebuffer.width != 0 &&
        boot_framebuffer.height != 0 &&
        boot_framebuffer.bpp == 32 &&
        boot_framebuffer.width <= g_scanout_width &&
        boot_framebuffer.height <= g_scanout_height) {
        width = static_cast<uint32_t>(boot_framebuffer.width);
        height = static_cast<uint32_t>(boot_framebuffer.height);
    }

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

} // namespace

namespace virtio_gpu {

void initialize(const boot::FramebufferInfo& framebuffer) {
    memset(&g_device, 0, sizeof(g_device));
    memset(&g_control_queue, 0, sizeof(g_control_queue));
    memset(&g_surface, 0, sizeof(g_surface));
    memset(&g_framebuffer_info, 0, sizeof(g_framebuffer_info));
    memset(&g_gpu_info, 0, sizeof(g_gpu_info));
    g_present = false;
    g_ready = false;
    g_next_fence_id = 1;

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
    if (!virtio_pci::setup_queue(g_device, kVirtioGpuControlQueue, kVirtioGpuMaxDescriptors, kQueueExtraBytes, 16, g_control_queue)) {
        fail_device("failed to setup control queue");
        return;
    }

    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::device_status(g_device) | virtio_pci::kStatusDriverOk));
    g_ready = true;

    VirtioGpuRespDisplayInfo display_info = {};
    if (!get_display_info(display_info) || !choose_scanout(display_info, framebuffer)) {
        fail_device("failed to query display info");
        return;
    }

    const uint64_t page_count = static_cast<uint64_t>((g_framebuffer_info.buffer_size + memory::kPageSize - 1) / memory::kPageSize);
    if (!memory::allocate_contiguous_pages(page_count, g_surface)) {
        fail_device("failed to allocate primary surface");
        return;
    }
    memset(g_surface.virtual_address, 0, g_surface.page_count * memory::kPageSize);

    if (!create_primary_resource() || !attach_primary_backing() || !set_primary_scanout() || !flush()) {
        fail_device("failed to create primary scanout");
        return;
    }

    g_gpu_device.ioctl = gpu_ioctl;
    g_gpu_device.close = gpu_close;
    if (!device::register_node("/dev/gpu0", &g_gpu_device, true)) {
        fail_device("failed to register /dev/gpu0");
        return;
    }

    console::set_external_framebuffer(g_surface.virtual_address, g_framebuffer_info);
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
    return g_surface.virtual_address;
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
    return transfer_rect(x, y, width, height) && flush_rect_internal(x, y, width, height);
}

bool present_from_kernel(const void* pixels, size_t byte_count) {
    if (!g_ready || pixels == nullptr || byte_count != g_framebuffer_info.buffer_size) {
        return false;
    }

    memcpy(g_surface.virtual_address, pixels, byte_count);
    return flush();
}

bool present_region_from_kernel(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!g_ready || pixels == nullptr || source_pitch == 0 || width == 0 || height == 0 ||
        x >= g_framebuffer_info.width || y >= g_framebuffer_info.height ||
        width > (g_framebuffer_info.width - x) || height > (g_framebuffer_info.height - y) ||
        source_pitch < (width * sizeof(uint32_t))) {
        return false;
    }

    return copy_region(pixels, source_pitch, x, y, width, height) && flush_rect(x, y, width, height);
}

} // namespace virtio_gpu
