#include "kernel/virtio_rng.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/inventory.hpp"
#include "kernel/lifecycle.hpp"
#include "kernel/lifecycle_selftest.hpp"
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

/* ---- start and stop (docs/kernel/DRIVER_LIFECYCLE.md) --------------------- */

constexpr char kDriverName[] = "virtio-rng";
// transport, features, queue, driver ok.
constexpr unsigned kStartStages = 4;

lifecycle::Acquired g_acquired;
bool g_started = false;
// Set while a failed start unwinds: the function is then given back as a
// failed start, not as a clean stop.
bool g_start_failing = false;

void release_transport(void*) {
    virtio_pci::release_device(g_device, kDriverName, g_start_failing);
}

void release_queue(void*) {
    virtio_pci::release_queue(g_queue);
}

bool stage_failed(unsigned stage, const char* name) {
    if (!lifecycle::injected(kDriverName, stage)) {
        return false;
    }
    console::printf("virtio-rng: start stage %u (%s) failed by injection\n", stage, name);
    return true;
}

// The first virtio-rng no other driver holds.
bool find_device(pci::DeviceInfo& found) {
    size_t from = 0;
    size_t index = 0;
    pci::DeviceInfo candidate = {};
    while (pci::ready() && virtio_pci::find_modern_device_from(from, kVirtioRngModernDevice,
               kVirtioRngSubsystemDevice, candidate, index)) {
        if (inventory::pci_available(candidate, kDriverName)) {
            found = candidate;
            return true;
        }
        from = index + 1;
    }
    return false;
}

// The transport, the features, the request queue, then DRIVER_OK and the
// first request posted. False with everything given back.
bool start(const pci::DeviceInfo& pci_device) {
    memset(&g_device, 0, sizeof(g_device));
    memset(&g_queue, 0, sizeof(g_queue));
    g_present = false;
    g_pending = false;

    unsigned stage = 1;
    if (stage_failed(stage, "transport")) {
        return false;
    }
    // virtio-rng has no device configuration.
    if (!virtio_pci::initialize_device(pci_device, false, g_device)) {
        console::write_line("virtio-rng: missing required MMIO capabilities");
        // Given back if it was taken here; left alone if another driver has it.
        (void)inventory::pci_release_failed(pci_device, kDriverName);
        return false;
    }
    bool ok = g_acquired.push("transport", &release_transport, nullptr);
    if (ok) {
        ++stage;
        ok = !stage_failed(stage, "features");
        if (ok) {
            virtio_pci::set_device_status(g_device, 0);
            virtio_pci::memory_barrier();
            virtio_pci::set_device_status(g_device,
                static_cast<uint8_t>(virtio_pci::kStatusAcknowledge | virtio_pci::kStatusDriver));
            ok = virtio_pci::negotiate_features(g_device, 0, virtio_pci::kFeatureVersion1Bit);
        }
    }
    if (ok) {
        ++stage;
        ok = !stage_failed(stage, "queue") && g_acquired.push("queue", &release_queue, nullptr) &&
            virtio_pci::setup_queue(g_device, kRequestQueue, kQueueLimit, kRequestBytes, 16, g_queue);
    }
    if (ok) {
        ++stage;
        ok = !stage_failed(stage, "driver ok");
        if (ok) {
            virtio_pci::set_device_status(g_device,
                static_cast<uint8_t>(virtio_pci::device_status(g_device) | virtio_pci::kStatusDriverOk));
            ok = post_request();
        }
    }
    if (!ok) {
        console::printf("virtio-rng: could not start at %x:%x.%u (stage %u); everything it took is given back\n",
            static_cast<unsigned>(pci_device.bus), static_cast<unsigned>(pci_device.slot),
            static_cast<unsigned>(pci_device.function), stage);
        // Reset first: a queue the device may still use is never freed.
        (void)virtio_pci::quiesce_device(g_device);
        g_start_failing = true;
        g_acquired.unwind();
        g_start_failing = false;
        g_pending = false;
        return false;
    }
    g_started = true;
    g_present = true;
    console::printf("virtio-rng: up pci=%x:%x.%u\n", static_cast<unsigned>(pci_device.bus),
        static_cast<unsigned>(pci_device.slot), static_cast<unsigned>(pci_device.function));
    return true;
}

// Reset and read back, then the queue and the function given back. A device
// that does not reset keeps its queue and is listed as failed.
bool stop() {
    if (!g_started) {
        return true;
    }
    g_present = false;
    if (!virtio_pci::quiesce_device(g_device)) {
        console::write_line("virtio-rng: the device did not reset; it keeps its queue");
        inventory::pci_failed(g_device.pci_device);
        return false;
    }
    g_acquired.unwind();
    g_started = false;
    g_pending = false;
    return true;
}

bool idle() {
    return !g_started && g_acquired.count() == 0 && g_queue.allocation.physical_address == 0;
}

// One request filled and taken, as random:: would.
bool bring_up() {
    for (uint32_t spin = 0; spin < 10000000u; ++spin) {
        if (virtio_rng::poll()) {
            return true;
        }
    }
    return false;
}

// One device each: the self-test's calls on that device.
bool subject_find(size_t nth, pci::DeviceInfo& device) {
    return nth == 0 && find_device(device);
}

bool subject_bring_up(const pci::DeviceInfo&) {
    return bring_up();
}

bool subject_stop(const pci::DeviceInfo&) {
    return stop();
}

bool subject_idle(const pci::DeviceInfo&) {
    return idle();
}

const lifecycle::Subject kSubject = {
    kDriverName,
    kStartStages,
    &subject_find,
    &start,
    &subject_bring_up,
    &subject_stop,
    &subject_idle,
    nullptr,
};

} // namespace

namespace virtio_rng {

bool initialize() {
    pci::DeviceInfo pci_device = {};
    return find_device(pci_device) && start(pci_device);
}

const lifecycle::Subject& lifecycle_subject() { return kSubject; }

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
