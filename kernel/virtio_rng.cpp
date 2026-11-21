#include "kernel/virtio_rng.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/random.hpp"
#include "kernel/random_crypto.hpp"
#include "kernel/string.hpp"
#include "kernel/virtio_pci.hpp"

namespace {

constexpr uint16_t kVirtioRngModernDevice = 0x1044u;
constexpr uint16_t kVirtioRngSubsystemDevice = 4u;
constexpr uint16_t kRequestQueue = 0;
constexpr uint16_t kQueueLimit = 4;
// One request at a time: 64 bytes is 512 bits, two seeds' worth.
constexpr uint32_t kRequestBytes = 64;

virtio_pci::Device g_device = {};
virtio_pci::Queue g_queue = {};
bool g_present = false;
bool g_pending = false;

bool post_request() {
    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_queue);
    descriptors[0] = {
        .addr = virtio_pci::queue_extra_physical(g_queue, 0),
        .len = kRequestBytes,
        .flags = virtio_pci::kDescriptorFlagWrite,
        .next = 0,
    };
    if (!virtio_pci::submit_descriptor_head(g_queue, 0)) {
        return false;
    }
    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_queue);
    g_pending = true;
    return true;
}

} // namespace

namespace virtio_rng {

bool initialize() {
    pci::DeviceInfo pci_device = {};

    memset(&g_device, 0, sizeof(g_device));
    memset(&g_queue, 0, sizeof(g_queue));
    g_present = false;
    g_pending = false;
    if (!pci::ready() ||
        !virtio_pci::find_modern_device(kVirtioRngModernDevice, kVirtioRngSubsystemDevice, pci_device)) {
        return false;
    }
    // virtio-rng has no device configuration.
    if (!virtio_pci::initialize_device(pci_device, false, g_device)) {
        console::write_line("virtio-rng: missing required MMIO capabilities");
        return false;
    }
    virtio_pci::set_device_status(g_device, 0);
    virtio_pci::memory_barrier();
    virtio_pci::set_device_status(g_device,
        static_cast<uint8_t>(virtio_pci::kStatusAcknowledge | virtio_pci::kStatusDriver));
    if (!virtio_pci::negotiate_features(g_device, 0, virtio_pci::kFeatureVersion1Bit) ||
        !virtio_pci::setup_queue(g_device, kRequestQueue, kQueueLimit, kRequestBytes, 16, g_queue)) {
        console::write_line("virtio-rng: could not set the device up");
        virtio_pci::fail_device(g_device);
        return false;
    }
    virtio_pci::set_device_status(g_device,
        static_cast<uint8_t>(virtio_pci::device_status(g_device) | virtio_pci::kStatusDriverOk));
    g_present = post_request();
    if (g_present) {
        console::printf("virtio-rng: up pci=%x:%x.%u\n", static_cast<unsigned>(g_device.pci_device.bus),
            static_cast<unsigned>(g_device.pci_device.slot),
            static_cast<unsigned>(g_device.pci_device.function));
    }
    return g_present;
}

bool present() { return g_present; }

bool poll() {
    if (!g_present || !g_pending) {
        return false;
    }
    virtio_pci::memory_barrier();
    const volatile virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_queue);
    if (g_queue.last_used_index == used->idx) {
        return false;
    }
    const virtio_pci::UsedElement element =
        virtio_pci::queue_used_ring(g_queue)[g_queue.last_used_index % g_queue.size];
    g_queue.last_used_index = static_cast<uint16_t>(g_queue.last_used_index + 1);
    g_pending = false;

    uint8_t* buffer = virtio_pci::queue_extra(g_queue, 0);
    // The device says how much it wrote; never more than was offered.
    const uint32_t length = element.len < kRequestBytes ? element.len : kRequestBytes;
    if (length > 0) {
        random::add_entropy(buffer, length, length * 8u, random::kSourceVirtioRng);
    }
    random_crypto::wipe(buffer, kRequestBytes);
    (void)post_request();
    return length > 0;
}

} // namespace virtio_rng
