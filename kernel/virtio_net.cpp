#include "kernel/virtio_net.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/inventory.hpp"
#include "kernel/lifecycle.hpp"
#include "kernel/lifecycle_selftest.hpp"
#include "kernel/string.hpp"
#include "kernel/virtio_pci.hpp"
#include "abi/savanxp_abi.h"

namespace {

constexpr uint16_t kVirtioNetModernDevice = 0x1041u;
constexpr uint16_t kVirtioNetSubsystemDevice = 1u;
constexpr uint16_t kNetRxQueue = 0;
constexpr uint16_t kNetTxQueue = 1;

// VIRTIO_NET_F_MAC (bit 5): pedirla es lo unico que hace falta para que el
// device publique una MAC propia en el config space en vez de dejarla en
// cero. Sin CSUM/GSO/MRG_RXBUF: el driver no ofrece offload, igual que
// rtl8139 (que tampoco lo hace), asi que 0 features de esas basta.
constexpr uint32_t kFeatureMacBit = 1u << 5;

// virtio_net_hdr_mrg_rxbuf (12 bytes). El spec obliga este formato -- en vez
// del legacy de 10 bytes sin num_buffers -- apenas se negocia VIRTIO_F_VERSION_1,
// que es requisito de virtio_pci:: para cualquier device moderno; no depende
// de negociar VIRTIO_NET_F_MRG_RXBUF en si. Va siempre en cero: sin GSO/csum
// offload y sin fragmentar en varios buffers (num_buffers == 1 siempre, un
// solo descriptor por paquete tanto en RX como en TX).
constexpr size_t kNetHeaderBytes = 12;

// MTU de Ethernet clasico (sin jumbo frames ni VLAN tag). Frames mas grandes
// los rechaza transmit() y el device nunca los ofrece de vuelta en RX porque
// el buffer posteado ya tiene este tope.
constexpr uint32_t kMaxFrameBytes = 1514;
constexpr size_t kRxBufferBytes = kNetHeaderBytes + kMaxFrameBytes;
constexpr uint16_t kRxSlots = 32;
constexpr uint16_t kRxQueueLimit = 64;

// One DMA descriptor stays owned until completion. The dispatcher queues
// subsequent frames instead of waiting here or overwriting that buffer.
constexpr uint16_t kTxQueueLimit = 4;
constexpr size_t kTxBufferBytes = kNetHeaderBytes + kMaxFrameBytes;

struct [[gnu::packed]] VirtioNetConfig {
    uint8_t mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
};

virtio_pci::Device g_device = {};
virtio_pci::Queue g_rx_queue = {};
virtio_pci::Queue g_tx_queue = {};
uint8_t g_mac[6] = {};
uint32_t g_tx_frames = 0;
uint32_t g_rx_frames = 0;
uint32_t g_tx_errors = 0;
uint32_t g_rx_errors = 0;
bool g_present = false;
bool g_up = false;
nic::Events g_events = {};

// Prioridad por encima de rtl8139 (100): si algun dia coexistieran los dos
// devices en la linea de comandos de QEMU, este gana. En la practica nunca
// compiten por hardware real: la configuracion de QEMU usa virtio-net-pci O
// rtl8139 sobre el mismo netdev, nunca los dos a la vez.
constexpr int kDriverPriority = 110;

volatile VirtioNetConfig* device_cfg() {
    return reinterpret_cast<volatile VirtioNetConfig*>(virtio_pci::device_cfg_base(g_device));
}

void report(uint32_t net_status) {
    if (g_events.status != nullptr) {
        g_events.status(net_status);
    }
}

size_t rx_slot_offset(uint16_t slot) { return static_cast<size_t>(slot) * kRxBufferBytes; }

bool post_rx_slot(uint16_t slot) {
    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_rx_queue);
    descriptors[slot] = {
        .addr = virtio_pci::queue_extra_physical(g_rx_queue, rx_slot_offset(slot)),
        .len = static_cast<uint32_t>(kRxBufferBytes),
        .flags = virtio_pci::kDescriptorFlagWrite,
        .next = 0,
    };
    return virtio_pci::submit_descriptor_head(g_rx_queue, slot);
}

void prime_rx_slots() {
    for (uint16_t slot = 0; slot < kRxSlots; ++slot) {
        (void)post_rx_slot(slot);
    }
    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_rx_queue);
}

// Receive a bounded batch even when the device continuously refills the ring.
void poll_receive() {
    if (!g_up || !g_rx_queue.enabled) {
        return;
    }

    const volatile virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_rx_queue);
    const virtio_pci::UsedElement* ring = virtio_pci::queue_used_ring(g_rx_queue);
    bool notified = false;

    for (size_t budget = 0; budget < 16 && g_rx_queue.last_used_index != used->idx; ++budget) {
        virtio_pci::memory_barrier();
        const virtio_pci::UsedElement element = ring[g_rx_queue.last_used_index % g_rx_queue.size];
        g_rx_queue.last_used_index = static_cast<uint16_t>(g_rx_queue.last_used_index + 1);

        const uint16_t slot = static_cast<uint16_t>(element.id);
        if (slot < kRxSlots) {
            if (element.len < kNetHeaderBytes || element.len > kRxBufferBytes) {
                ++g_rx_errors;
                report(SAVANXP_NET_STATUS_RX_INVALID);
            } else {
                const uint8_t* buffer = virtio_pci::queue_extra(g_rx_queue, rx_slot_offset(slot));
                // This is redundant with element.len's upper bound today;
                // keep the frame-level defense in depth if the RX buffer and
                // Ethernet limit ever diverge.
                const size_t frame_length = element.len - kNetHeaderBytes;
                if (frame_length > kMaxFrameBytes) {
                    ++g_rx_errors;
                    report(SAVANXP_NET_STATUS_RX_INVALID);
                    if (post_rx_slot(slot)) {
                        notified = true;
                    }
                    continue;
                }
                ++g_rx_frames;
                if (g_events.frame != nullptr) {
                    g_events.frame(buffer + kNetHeaderBytes, frame_length);
                }
            }

            if (post_rx_slot(slot)) {
                notified = true;
            }
        }
    }

    if (notified) {
        virtio_pci::memory_barrier();
        virtio_pci::notify_queue(g_device, g_rx_queue);
    }
}

// Keep the DMA buffer intact until the device returns its descriptor.
bool g_tx_busy = false;
bool transmit(const void* frame, size_t length) {
    if (!g_up || frame == nullptr || length == 0 || length > kMaxFrameBytes) {
        ++g_tx_errors;
        report(SAVANXP_NET_STATUS_TX_FAILED);
        return false;
    }

    if (g_tx_busy) {
        virtio_pci::memory_barrier();
        const volatile auto* used = virtio_pci::queue_used_header(g_tx_queue);
        if (g_tx_queue.last_used_index == used->idx) return false;
        g_tx_queue.last_used_index = static_cast<uint16_t>(g_tx_queue.last_used_index + 1);
        g_tx_busy = false;
    }
    uint8_t* buffer = virtio_pci::queue_extra(g_tx_queue, 0);
    memset(buffer, 0, kNetHeaderBytes);
    memcpy(buffer + kNetHeaderBytes, frame, length);

    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_tx_queue);
    descriptors[0] = {
        .addr = virtio_pci::queue_extra_physical(g_tx_queue, 0),
        .len = static_cast<uint32_t>(kNetHeaderBytes + length),
        .flags = 0,
        .next = 0,
    };

    if (!virtio_pci::submit_descriptor_head(g_tx_queue, 0)) {
        ++g_tx_errors;
        report(SAVANXP_NET_STATUS_TX_FAILED);
        return false;
    }
    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_tx_queue);

    g_tx_busy = true;
    ++g_tx_frames;
    return true;
}

void attach(const nic::Events& events) {
    g_events = events;
}

bool is_up() {
    return g_up;
}

const uint8_t* mac_address() {
    return g_mac;
}

void get_stats(nic::Stats& stats) {
    stats.tx_frames = g_tx_frames;
    stats.rx_frames = g_rx_frames;
    stats.tx_errors = g_tx_errors;
    stats.rx_errors = g_rx_errors;
}

/* ---- start and stop (docs/kernel/DRIVER_LIFECYCLE.md) ---------------------
 *
 * start() takes the transport and negotiates the features, as probe() did;
 * the queues and DRIVER_OK wait for bring_up(), which net:: runs on
 * NET_IOC_UP, and go onto the same record, so stop() gives back whatever the
 * device got. */

constexpr char kDriverName[] = "virtio-net";
// transport, features.
constexpr unsigned kStartStages = 2;

lifecycle::Acquired g_acquired;
bool g_started = false;
// Set while a failed start unwinds: the function is then given back as a
// failed start, not as a clean stop.
bool g_start_failing = false;

void release_transport(void*) {
    virtio_pci::release_device(g_device, kDriverName, g_start_failing);
}

void release_queues(void*) {
    virtio_pci::release_queue(g_rx_queue);
    virtio_pci::release_queue(g_tx_queue);
}

bool stage_failed(unsigned stage, const char* name) {
    if (!lifecycle::injected(kDriverName, stage)) {
        return false;
    }
    console::printf("virtio-net: start stage %u (%s) failed by injection\n", stage, name);
    return true;
}

bool bring_up() {
    if (!g_present) {
        return false;
    }
    if (g_up) {
        return true;
    }

    if (!virtio_pci::setup_queue(g_device, kNetRxQueue, kRxQueueLimit, kRxBufferBytes * kRxSlots, 16, g_rx_queue) ||
        !virtio_pci::setup_queue(g_device, kNetTxQueue, kTxQueueLimit, kTxBufferBytes, 16, g_tx_queue) ||
        !g_acquired.push("queues", &release_queues, nullptr)) {
        console::write_line("virtio-net: failed to initialize queues");
        // The device is reset before its queues go: it never uses them after.
        (void)virtio_pci::quiesce_device(g_device);
        release_queues(nullptr);
        inventory::pci_failed(g_device.pci_device);
        g_present = false;
        report(SAVANXP_NET_STATUS_BRING_UP_FAILED);
        return false;
    }

    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::device_status(g_device) | virtio_pci::kStatusDriverOk));
    g_up = true;
    prime_rx_slots();

    console::printf(
        "virtio-net: up pci=%x:%x.%u mac=%x:%x:%x:%x:%x:%x\n",
        static_cast<unsigned>(g_device.pci_device.bus),
        static_cast<unsigned>(g_device.pci_device.slot),
        static_cast<unsigned>(g_device.pci_device.function),
        static_cast<unsigned>(g_mac[0]),
        static_cast<unsigned>(g_mac[1]),
        static_cast<unsigned>(g_mac[2]),
        static_cast<unsigned>(g_mac[3]),
        static_cast<unsigned>(g_mac[4]),
        static_cast<unsigned>(g_mac[5])
    );
    return true;
}

// The first virtio-net no other driver holds.
bool find_device(pci::DeviceInfo& found) {
    size_t from = 0;
    size_t index = 0;
    pci::DeviceInfo candidate = {};
    while (pci::ready() && virtio_pci::find_modern_device_from(from, kVirtioNetModernDevice,
               kVirtioNetSubsystemDevice, candidate, index)) {
        if (inventory::pci_available(candidate, kDriverName)) {
            found = candidate;
            return true;
        }
        from = index + 1;
    }
    return false;
}

// The transport, then the features and the MAC, without starting queues.
// False with everything given back.
bool start(const pci::DeviceInfo& pci_device) {
    memset(&g_device, 0, sizeof(g_device));
    memset(&g_rx_queue, 0, sizeof(g_rx_queue));
    memset(&g_tx_queue, 0, sizeof(g_tx_queue));
    memset(g_mac, 0, sizeof(g_mac));
    g_present = false;
    g_up = false;
    g_tx_busy = false;

    unsigned stage = 1;
    if (stage_failed(stage, "transport")) {
        return false;
    }
    if (!virtio_pci::initialize_device(pci_device, true, g_device)) {
        console::write_line("virtio-net: missing required MMIO capabilities");
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

            volatile virtio_pci::CommonCfg* cfg = virtio_pci::common_cfg(g_device);
            cfg->device_feature_select = 0;
            virtio_pci::memory_barrier();
            const uint32_t requested_word0 = cfg->device_feature & kFeatureMacBit;
            ok = virtio_pci::negotiate_features(g_device, requested_word0, virtio_pci::kFeatureVersion1Bit);
            if (!ok) {
                console::write_line("virtio-net: feature negotiation failed");
            } else if ((requested_word0 & kFeatureMacBit) != 0) {
                for (size_t index = 0; index < sizeof(g_mac); ++index) {
                    g_mac[index] = device_cfg()->mac[index];
                }
            }
        }
    }
    if (!ok) {
        console::printf("virtio-net: could not start at %x:%x.%u (stage %u); everything it took is given back\n",
            static_cast<unsigned>(pci_device.bus), static_cast<unsigned>(pci_device.slot),
            static_cast<unsigned>(pci_device.function), stage);
        (void)virtio_pci::quiesce_device(g_device);
        g_start_failing = true;
        g_acquired.unwind();
        g_start_failing = false;
        return false;
    }

    g_started = true;
    g_present = true;
    // Recognized and negotiated; DRIVER_OK waits for bring_up().
    inventory::pci_started(pci_device);
    return true;
}

// Reset and read back, then the queues, if it had come up, and the function
// given back. A device that does not reset keeps everything and is listed as
// failed.
bool stop() {
    if (!g_started) {
        return true;
    }
    g_up = false;
    g_present = false;
    if (!virtio_pci::quiesce_device(g_device)) {
        console::write_line("virtio-net: the device did not reset; it keeps its queues");
        inventory::pci_failed(g_device.pci_device);
        return false;
    }
    g_acquired.unwind();
    g_started = false;
    g_tx_busy = false;
    return true;
}

bool idle() {
    return !g_started && g_acquired.count() == 0 && g_rx_queue.allocation.physical_address == 0 &&
        g_tx_queue.allocation.physical_address == 0;
}

const lifecycle::Subject kSubject = {
    kDriverName,
    kStartStages,
    &find_device,
    &start,
    &bring_up,
    &stop,
    &idle,
    nullptr,
};

// Recognizes the device and takes it, without starting queues: NET_IOC_UP
// or a native socket's prepare step brings it up later.
bool probe() {
    pci::DeviceInfo pci_device = {};
    return find_device(pci_device) && start(pci_device);
}

int prepare() { return bring_up() ? 0 : -SX_EIO; }

const nic::Nic kNic = {
    &attach,
    &bring_up,
    &is_up,
    &mac_address,
    &transmit,
    &poll_receive,
    &get_stats,
    &prepare,
    nullptr,
};

const nic::Nic& nic_of() { return kNic; }

const nic::Driver kDriver = {
    "virtio-net",
    kDriverPriority,
    &probe,
    &nic_of,
};

} // namespace

namespace virtio_net {

const nic::Driver& driver() { return kDriver; }

const lifecycle::Subject& lifecycle_subject() { return kSubject; }

} // namespace virtio_net
