#include "kernel/virtio_net.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/string.hpp"
#include "kernel/virtio_pci.hpp"
#include "savanxp/syscall.h"

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

// TX queda sincronico (un solo frame en vuelo, transmit() espera su
// confirmacion antes de volver) igual que rtl8139 -- que tambien bloquea
// hasta ver TX_OK/TX_ERR --, asi que alcanza con un solo buffer/descriptor;
// no hace falta un pool de slots como en la cola RX.
constexpr uint16_t kTxQueueLimit = 4;
constexpr size_t kTxBufferBytes = kNetHeaderBytes + kMaxFrameBytes;
constexpr uint32_t kCommandTimeoutSpins = 10000000u;

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
// compiten por hardware real: build.ps1 arma la maquina con virtio-net-pci O
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

bool wait_for_used_element(virtio_pci::Queue& queue, virtio_pci::UsedElement& element) {
    const volatile virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(queue);
    const virtio_pci::UsedElement* ring = virtio_pci::queue_used_ring(queue);

    for (uint32_t spin = 0; spin < kCommandTimeoutSpins; ++spin) {
        virtio_pci::memory_barrier();
        if (queue.last_used_index == used->idx) {
            continue;
        }

        element = ring[queue.last_used_index % queue.size];
        queue.last_used_index = static_cast<uint16_t>(queue.last_used_index + 1);
        return true;
    }
    return false;
}

// La llaman tanto el handler de IRQ (si algun dia se suma interrupt-driven,
// mismo patron que virtio_gpu/virtio_input) como los loops de espera de
// net:: -- por ahora este driver es polling-only, como rtl8139 cuando no
// logra rutear su INTx, asi que es el unico camino de recepcion.
void poll_receive() {
    if (!g_up || !g_rx_queue.enabled) {
        return;
    }

    const volatile virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_rx_queue);
    const virtio_pci::UsedElement* ring = virtio_pci::queue_used_ring(g_rx_queue);
    bool notified = false;

    while (g_rx_queue.last_used_index != used->idx) {
        virtio_pci::memory_barrier();
        const virtio_pci::UsedElement element = ring[g_rx_queue.last_used_index % g_rx_queue.size];
        g_rx_queue.last_used_index = static_cast<uint16_t>(g_rx_queue.last_used_index + 1);

        const uint16_t slot = static_cast<uint16_t>(element.id);
        if (slot < kRxSlots) {
            if (element.len < kNetHeaderBytes) {
                ++g_rx_errors;
                report(SAVANXP_NET_STATUS_RX_INVALID);
            } else {
                const uint8_t* buffer = virtio_pci::queue_extra(g_rx_queue, rx_slot_offset(slot));
                const size_t frame_length = element.len - kNetHeaderBytes;
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

// Sincronico, como rtl8139::transmit: arma el unico buffer de TX, notifica y
// espera (polling acotado) la confirmacion del device antes de volver. QEMU
// despacha virtio-net practicamente al instante, asi que esto no bloquea de
// verdad en la practica.
bool transmit(const void* frame, size_t length) {
    if (!g_up || frame == nullptr || length == 0 || length > kMaxFrameBytes) {
        ++g_tx_errors;
        report(SAVANXP_NET_STATUS_TX_FAILED);
        return false;
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

    virtio_pci::UsedElement element = {};
    if (!wait_for_used_element(g_tx_queue, element)) {
        ++g_tx_errors;
        report(SAVANXP_NET_STATUS_TX_TIMEOUT);
        return false;
    }

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

void fail_device(const char* reason) {
    virtio_pci::fail_device(g_device);
    if (reason != nullptr) {
        console::printf("virtio-net: %s\n", reason);
    }
    g_up = false;
    g_present = false;
}

bool bring_up() {
    if (!g_present) {
        return false;
    }
    if (g_up) {
        return true;
    }

    if (!virtio_pci::setup_queue(g_device, kNetRxQueue, kRxQueueLimit, kRxBufferBytes * kRxSlots, 16, g_rx_queue) ||
        !virtio_pci::setup_queue(g_device, kNetTxQueue, kTxQueueLimit, kTxBufferBytes, 16, g_tx_queue)) {
        fail_device("failed to initialize queues");
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

// Sondea el bus, negocia features y deja la MAC leida, SIN levantar las colas:
// la subida real es bring_up(), cuando alguien pide NET_IOC_UP sobre
// /dev/net0 -- mismo contrato que rtl8139::probe().
bool probe() {
    memset(&g_device, 0, sizeof(g_device));
    memset(&g_rx_queue, 0, sizeof(g_rx_queue));
    memset(&g_tx_queue, 0, sizeof(g_tx_queue));
    memset(g_mac, 0, sizeof(g_mac));
    g_present = false;
    g_up = false;

    pci::DeviceInfo pci_device = {};
    if (!pci::ready() || !virtio_pci::find_modern_device(kVirtioNetModernDevice, kVirtioNetSubsystemDevice, pci_device)) {
        return false;
    }

    if (!virtio_pci::initialize_device(pci_device, true, g_device)) {
        console::write_line("virtio-net: missing required MMIO capabilities");
        return false;
    }

    virtio_pci::set_device_status(g_device, 0);
    virtio_pci::memory_barrier();
    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::kStatusAcknowledge | virtio_pci::kStatusDriver));

    volatile virtio_pci::CommonCfg* cfg = virtio_pci::common_cfg(g_device);
    cfg->device_feature_select = 0;
    virtio_pci::memory_barrier();
    const uint32_t requested_word0 = cfg->device_feature & kFeatureMacBit;

    if (!virtio_pci::negotiate_features(g_device, requested_word0, virtio_pci::kFeatureVersion1Bit)) {
        console::write_line("virtio-net: feature negotiation failed");
        virtio_pci::fail_device(g_device);
        return false;
    }

    if ((requested_word0 & kFeatureMacBit) != 0) {
        for (size_t index = 0; index < sizeof(g_mac); ++index) {
            g_mac[index] = device_cfg()->mac[index];
        }
    }

    g_present = true;
    return true;
}

const nic::Nic kNic = {
    &attach,
    &bring_up,
    &is_up,
    &mac_address,
    &transmit,
    &poll_receive,
    &get_stats,
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

} // namespace virtio_net
