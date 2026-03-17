#include "kernel/virtio_pci.hpp"

#include "kernel/string.hpp"
#include "kernel/vmm.hpp"

namespace virtio_pci {

void memory_barrier() {
    asm volatile("" : : : "memory");
}

size_t align_up(size_t value, size_t alignment) {
    const size_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

bool looks_like_modern_device(const pci::DeviceInfo& info, uint16_t modern_device_id, uint16_t subsystem_device_id) {
    if (!info.present || info.vendor_id != kPciVendorVirtio) {
        return false;
    }
    return info.device_id == modern_device_id || info.subsystem_device_id == subsystem_device_id;
}

bool find_modern_device(uint16_t modern_device_id, uint16_t subsystem_device_id, pci::DeviceInfo& info) {
    for (size_t index = 0; index < pci::device_count(); ++index) {
        pci::DeviceInfo candidate = {};
        if (!pci::device_info(index, candidate)) {
            continue;
        }
        if (looks_like_modern_device(candidate, modern_device_id, subsystem_device_id)) {
            info = candidate;
            return true;
        }
    }
    return false;
}

static bool map_bar(Device& device, uint8_t bar_index, MappedBar*& mapped) {
    mapped = nullptr;
    if (bar_index >= 6) {
        return false;
    }

    MappedBar& bar = device.bars[bar_index];
    if (bar.mapped) {
        mapped = &bar;
        return true;
    }

    if (!pci::bar_info(device.pci_device, bar_index, bar.info) || bar.info.io_space) {
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

static bool resolve_capability(Device& device, uint8_t cfg_type, bool required, CapabilityView& view) {
    view = {};

    pci::VendorCapabilityInfo capability = {};
    if (!pci::find_vendor_capability(device.pci_device, cfg_type, capability)) {
        return !required;
    }

    MappedBar* mapped_bar = nullptr;
    if (!map_bar(device, capability.bar_index, mapped_bar) || mapped_bar == nullptr) {
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

bool initialize_device(const pci::DeviceInfo& pci_device, bool require_device_cfg, Device& device) {
    memset(&device, 0, sizeof(device));
    device.pci_device = pci_device;

    if (!resolve_capability(device, kCapabilityCommonCfg, true, device.common_view) ||
        !resolve_capability(device, kCapabilityNotifyCfg, true, device.notify_view) ||
        !resolve_capability(device, kCapabilityDeviceCfg, require_device_cfg, device.device_view)) {
        memset(&device, 0, sizeof(device));
        return false;
    }
    (void)resolve_capability(device, kCapabilityIsrCfg, false, device.isr_view);

    uint16_t command = pci::read_config_u16(device.pci_device.bus, device.pci_device.slot, device.pci_device.function, kPciCommandOffset);
    command = static_cast<uint16_t>(command | kPciCommandMemory | kPciCommandBusMaster);
    pci::write_config_u16(device.pci_device.bus, device.pci_device.slot, device.pci_device.function, kPciCommandOffset, command);
    device.ready = true;
    return true;
}

volatile CommonCfg* common_cfg(Device& device) {
    return reinterpret_cast<volatile CommonCfg*>(device.common_view.base);
}

const volatile CommonCfg* common_cfg(const Device& device) {
    return reinterpret_cast<const volatile CommonCfg*>(device.common_view.base);
}

uint8_t* device_cfg_base(Device& device) {
    return device.device_view.base;
}

const uint8_t* device_cfg_base(const Device& device) {
    return device.device_view.base;
}

void set_device_status(Device& device, uint8_t status) {
    common_cfg(device)->device_status = status;
}

uint8_t device_status(const Device& device) {
    return common_cfg(device)->device_status;
}

bool negotiate_features(Device& device, uint32_t driver_features_word0, uint32_t driver_features_word1) {
    volatile CommonCfg* cfg = common_cfg(device);
    cfg->device_feature_select = 0;
    memory_barrier();
    const uint32_t device_features_word0 = cfg->device_feature;
    cfg->device_feature_select = kFeatureVersion1Word;
    memory_barrier();
    const uint32_t device_features_word1 = cfg->device_feature;

    if ((device_features_word1 & kFeatureVersion1Bit) == 0) {
        return false;
    }
    if ((driver_features_word0 & ~device_features_word0) != 0 ||
        (driver_features_word1 & ~device_features_word1) != 0) {
        return false;
    }

    cfg->driver_feature_select = 0;
    cfg->driver_feature = driver_features_word0;
    cfg->driver_feature_select = kFeatureVersion1Word;
    cfg->driver_feature = driver_features_word1;
    memory_barrier();

    set_device_status(device, static_cast<uint8_t>(device_status(device) | kStatusFeaturesOk));
    memory_barrier();
    return (device_status(device) & kStatusFeaturesOk) != 0;
}

void fail_device(Device& device) {
    if (device.ready) {
        set_device_status(device, static_cast<uint8_t>(device_status(device) | kStatusFailed));
    }
    device.ready = false;
}

uint16_t choose_queue_size(uint16_t maximum, uint16_t limit) {
    if (maximum == 0 || limit == 0) {
        return 0;
    }

    uint16_t chosen = maximum > limit ? limit : maximum;
    uint16_t power_of_two = 1;
    while (static_cast<uint16_t>(power_of_two << 1) != 0 &&
           static_cast<uint16_t>(power_of_two << 1) <= chosen) {
        power_of_two = static_cast<uint16_t>(power_of_two << 1);
    }
    return power_of_two;
}

QueueLayout build_queue_layout(uint16_t size, size_t extra_bytes, size_t extra_alignment) {
    QueueLayout layout = {};
    layout.size = size;
    layout.desc_offset = 0;
    layout.avail_offset = sizeof(Descriptor) * size;
    layout.used_offset = align_up(layout.avail_offset + sizeof(AvailHeader) + (sizeof(uint16_t) * size), 4);
    layout.ring_bytes = layout.used_offset + sizeof(UsedHeader) + (sizeof(UsedElement) * size);
    layout.extra_offset = align_up(layout.ring_bytes, extra_alignment != 0 ? extra_alignment : 1);
    layout.total_bytes = layout.extra_offset + extra_bytes;
    return layout;
}

bool setup_queue(Device& device, uint16_t queue_index, uint16_t queue_limit, size_t extra_bytes, size_t extra_alignment, Queue& queue) {
    memset(&queue, 0, sizeof(queue));

    volatile CommonCfg* cfg = common_cfg(device);
    cfg->queue_select = queue_index;
    memory_barrier();

    const uint16_t chosen_size = choose_queue_size(cfg->queue_size, queue_limit);
    if (chosen_size == 0) {
        return false;
    }

    queue.layout = build_queue_layout(chosen_size, extra_bytes, extra_alignment);
    const uint64_t page_count = static_cast<uint64_t>((queue.layout.total_bytes + memory::kPageSize - 1) / memory::kPageSize);
    if (!memory::allocate_contiguous_pages(page_count, queue.allocation)) {
        return false;
    }
    memset(queue.allocation.virtual_address, 0, queue.allocation.page_count * memory::kPageSize);

    queue.index = queue_index;
    queue.size = chosen_size;
    queue.notify_off = cfg->queue_notify_off;
    queue.last_used_index = 0;
    queue.next_avail_index = 0;

    cfg->queue_size = chosen_size;
    memory_barrier();
    cfg->queue_desc = queue.allocation.physical_address + queue.layout.desc_offset;
    cfg->queue_driver = queue.allocation.physical_address + queue.layout.avail_offset;
    cfg->queue_device = queue.allocation.physical_address + queue.layout.used_offset;
    cfg->queue_enable = 1;
    memory_barrier();

    queue.enabled = cfg->queue_enable != 0;
    return queue.enabled;
}

Descriptor* queue_descriptors(Queue& queue) {
    return reinterpret_cast<Descriptor*>(static_cast<uint8_t*>(queue.allocation.virtual_address) + queue.layout.desc_offset);
}

const Descriptor* queue_descriptors(const Queue& queue) {
    return reinterpret_cast<const Descriptor*>(static_cast<const uint8_t*>(queue.allocation.virtual_address) + queue.layout.desc_offset);
}

AvailHeader* queue_avail_header(Queue& queue) {
    return reinterpret_cast<AvailHeader*>(static_cast<uint8_t*>(queue.allocation.virtual_address) + queue.layout.avail_offset);
}

uint16_t* queue_avail_ring(Queue& queue) {
    return reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(queue.allocation.virtual_address) + queue.layout.avail_offset + sizeof(AvailHeader));
}

UsedHeader* queue_used_header(Queue& queue) {
    return reinterpret_cast<UsedHeader*>(static_cast<uint8_t*>(queue.allocation.virtual_address) + queue.layout.used_offset);
}

const UsedHeader* queue_used_header(const Queue& queue) {
    return reinterpret_cast<const UsedHeader*>(static_cast<const uint8_t*>(queue.allocation.virtual_address) + queue.layout.used_offset);
}

UsedElement* queue_used_ring(Queue& queue) {
    return reinterpret_cast<UsedElement*>(static_cast<uint8_t*>(queue.allocation.virtual_address) + queue.layout.used_offset + sizeof(UsedHeader));
}

const UsedElement* queue_used_ring(const Queue& queue) {
    return reinterpret_cast<const UsedElement*>(static_cast<const uint8_t*>(queue.allocation.virtual_address) + queue.layout.used_offset + sizeof(UsedHeader));
}

uint8_t* queue_extra(Queue& queue, size_t offset) {
    return static_cast<uint8_t*>(queue.allocation.virtual_address) + queue.layout.extra_offset + offset;
}

const uint8_t* queue_extra(const Queue& queue, size_t offset) {
    return static_cast<const uint8_t*>(queue.allocation.virtual_address) + queue.layout.extra_offset + offset;
}

uint64_t queue_extra_physical(const Queue& queue, size_t offset) {
    return queue.allocation.physical_address + queue.layout.extra_offset + offset;
}

void notify_queue(Device& device, const Queue& queue) {
    if (!device.notify_view.valid || device.notify_view.bar == nullptr || device.notify_view.extra == 0) {
        return;
    }

    volatile uint16_t* notify = reinterpret_cast<volatile uint16_t*>(
        device.notify_view.bar->base + device.notify_view.offset_within_bar + (queue.notify_off * device.notify_view.extra)
    );
    *notify = queue.index;
}

bool submit_descriptor_head(Queue& queue, uint16_t head) {
    if (!queue.enabled || queue.size == 0 || head >= queue.size) {
        return false;
    }

    uint16_t* ring = queue_avail_ring(queue);
    AvailHeader* avail = queue_avail_header(queue);
    ring[queue.next_avail_index % queue.size] = head;
    queue.next_avail_index = static_cast<uint16_t>(queue.next_avail_index + 1);
    memory_barrier();
    avail->idx = queue.next_avail_index;
    return true;
}

} // namespace virtio_pci
