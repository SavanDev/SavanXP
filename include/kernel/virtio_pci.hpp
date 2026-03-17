#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"

namespace virtio_pci {

constexpr uint16_t kPciVendorVirtio = 0x1af4u;
constexpr uint16_t kPciCommandOffset = 0x04;
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;

constexpr uint8_t kCapabilityCommonCfg = 1;
constexpr uint8_t kCapabilityNotifyCfg = 2;
constexpr uint8_t kCapabilityIsrCfg = 3;
constexpr uint8_t kCapabilityDeviceCfg = 4;

constexpr uint8_t kStatusAcknowledge = 1u << 0;
constexpr uint8_t kStatusDriver = 1u << 1;
constexpr uint8_t kStatusDriverOk = 1u << 2;
constexpr uint8_t kStatusFeaturesOk = 1u << 3;
constexpr uint8_t kStatusFailed = 1u << 7;

constexpr uint32_t kFeatureVersion1Word = 1u;
constexpr uint32_t kFeatureVersion1Bit = 1u << 0;

constexpr uint16_t kDescriptorFlagNext = 1u << 0;
constexpr uint16_t kDescriptorFlagWrite = 1u << 1;

struct [[gnu::packed]] CommonCfg {
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

struct [[gnu::packed]] Descriptor {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct [[gnu::packed]] AvailHeader {
    uint16_t flags;
    uint16_t idx;
};

struct [[gnu::packed]] UsedElement {
    uint32_t id;
    uint32_t len;
};

struct [[gnu::packed]] UsedHeader {
    uint16_t flags;
    uint16_t idx;
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

struct Device {
    pci::DeviceInfo pci_device;
    MappedBar bars[6];
    CapabilityView common_view;
    CapabilityView notify_view;
    CapabilityView isr_view;
    CapabilityView device_view;
    bool ready;
};

struct QueueLayout {
    uint16_t size;
    size_t desc_offset;
    size_t avail_offset;
    size_t used_offset;
    size_t ring_bytes;
    size_t extra_offset;
    size_t total_bytes;
};

struct Queue {
    memory::PageAllocation allocation;
    QueueLayout layout;
    uint16_t index;
    uint16_t size;
    uint16_t notify_off;
    uint16_t last_used_index;
    uint16_t next_avail_index;
    bool enabled;
};

void memory_barrier();
size_t align_up(size_t value, size_t alignment);
bool looks_like_modern_device(const pci::DeviceInfo& info, uint16_t modern_device_id, uint16_t subsystem_device_id);
bool find_modern_device(uint16_t modern_device_id, uint16_t subsystem_device_id, pci::DeviceInfo& info);
bool initialize_device(const pci::DeviceInfo& pci_device, bool require_device_cfg, Device& device);
volatile CommonCfg* common_cfg(Device& device);
const volatile CommonCfg* common_cfg(const Device& device);
uint8_t* device_cfg_base(Device& device);
const uint8_t* device_cfg_base(const Device& device);
void set_device_status(Device& device, uint8_t status);
uint8_t device_status(const Device& device);
bool negotiate_features(Device& device, uint32_t driver_features_word0, uint32_t driver_features_word1);
void fail_device(Device& device);
uint16_t choose_queue_size(uint16_t maximum, uint16_t limit);
QueueLayout build_queue_layout(uint16_t size, size_t extra_bytes, size_t extra_alignment);
bool setup_queue(Device& device, uint16_t queue_index, uint16_t queue_limit, size_t extra_bytes, size_t extra_alignment, Queue& queue);
Descriptor* queue_descriptors(Queue& queue);
const Descriptor* queue_descriptors(const Queue& queue);
AvailHeader* queue_avail_header(Queue& queue);
uint16_t* queue_avail_ring(Queue& queue);
UsedHeader* queue_used_header(Queue& queue);
const UsedHeader* queue_used_header(const Queue& queue);
UsedElement* queue_used_ring(Queue& queue);
const UsedElement* queue_used_ring(const Queue& queue);
uint8_t* queue_extra(Queue& queue, size_t offset = 0);
const uint8_t* queue_extra(const Queue& queue, size_t offset = 0);
uint64_t queue_extra_physical(const Queue& queue, size_t offset = 0);
void notify_queue(Device& device, const Queue& queue);
bool submit_descriptor_head(Queue& queue, uint16_t head);

} // namespace virtio_pci
