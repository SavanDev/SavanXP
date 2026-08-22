#include "kernel/rtl8139.hpp"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/uacpi_glue.hpp"
#include "savanxp/syscall.h"

namespace {

constexpr bool kLogRtl = false;

// Prioridad alta: por ahora es el unico NIC, pero el numero deja lugar para un
// virtio-net que deberia ganarle cuando exista.
constexpr int kDriverPriority = 100;

constexpr uint16_t kVendorRealtek = 0x10ecu;
constexpr uint16_t kDeviceRtl8139 = 0x8139u;
constexpr uint16_t kPciCommandOffset = 0x04;
constexpr uint16_t kPciCommandIo = 1u << 0;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;

constexpr uint16_t kRegMac0 = 0x00;
constexpr uint16_t kRegTxStatus0 = 0x10;
constexpr uint16_t kRegTxAddress0 = 0x20;
constexpr uint16_t kRegRxBuffer = 0x30;
constexpr uint16_t kRegCommand = 0x37;
constexpr uint16_t kRegCapr = 0x38;
constexpr uint16_t kRegCbr = 0x3a;
constexpr uint16_t kRegIsr = 0x3e;
constexpr uint16_t kRegImr = 0x3c;
constexpr uint16_t kRegTcr = 0x40;
constexpr uint16_t kRegRcr = 0x44;
constexpr uint16_t kRegConfig1 = 0x52;

constexpr uint8_t kCommandReset = 0x10;
constexpr uint8_t kCommandRxEnable = 0x08;
constexpr uint8_t kCommandTxEnable = 0x04;
constexpr uint8_t kCommandRxBufferEmpty = 0x01;

constexpr uint32_t kRxBufferSize = 8192;
constexpr uint32_t kRxBufferBytes = kRxBufferSize + 16 + 1500;
constexpr uint32_t kTxBufferBytes = 2048;
constexpr uint16_t kRxOk = 0x0001;
constexpr uint32_t kTxOk = 1u << 15;
constexpr uint32_t kTxErr = 1u << 29;
constexpr uint32_t kRcrWrap = 1u << 7;
constexpr uint32_t kRcrAcceptAllPhysical = 1u << 0;
constexpr uint32_t kRcrAcceptPhysical = 1u << 1;
constexpr uint32_t kRcrAcceptMulticast = 1u << 2;
constexpr uint32_t kRcrAcceptBroadcast = 1u << 3;

pci::DeviceInfo g_pci_device = {};
memory::PageAllocation g_rx_allocation = {};
memory::PageAllocation g_tx_allocation[4] = {};
uint16_t g_io_base = 0;
uint16_t g_cur_rx = 0;
uint8_t g_tx_index = 0;
uint8_t g_mac[6] = {};
uint32_t g_tx_frames = 0;
uint32_t g_rx_frames = 0;
uint32_t g_tx_errors = 0;
uint32_t g_rx_errors = 0;
bool g_present = false;
bool g_up = false;

// Estado del camino interrupt-driven (INTx via _PRT/IOAPIC).
volatile uint32_t g_irq_count = 0;
bool g_irq_routed = false;

nic::Events g_events = {};

void rtl_log(const char* format, ...) {
    if (!kLogRtl) {
        return;
    }

    va_list args;
    va_start(args, format);
    console::vprintf(format, args);
    va_end(args);
}

// Solo los estados que el driver puede diagnosticar; el resto los pone el stack.
void report(uint32_t net_status) {
    if (g_events.status != nullptr) {
        g_events.status(net_status);
    }
}

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline void out16(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

inline void out32(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t in8(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline uint16_t in16(uint16_t port) {
    uint16_t value = 0;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline uint32_t in32(uint16_t port) {
    uint32_t value = 0;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

bool wait_for_clear_reset() {
    for (uint32_t spin = 0; spin < 100000; ++spin) {
        if ((in8(static_cast<uint16_t>(g_io_base + kRegCommand)) & kCommandReset) == 0) {
            return true;
        }
    }
    return false;
}

void clear_interrupt_status() {
    const uint16_t status = in16(static_cast<uint16_t>(g_io_base + kRegIsr));
    if (status != 0) {
        out16(static_cast<uint16_t>(g_io_base + kRegIsr), status);
    }
}

bool transmit(const void* frame, size_t length) {
    if (!g_up || frame == nullptr || length == 0 || length > kTxBufferBytes) {
        ++g_tx_errors;
        report(SAVANXP_NET_STATUS_TX_FAILED);
        return false;
    }

    const size_t tx_slot = g_tx_index % 4;
    auto* tx_buffer = static_cast<uint8_t*>(g_tx_allocation[tx_slot].virtual_address);
    const size_t wire_length = length < 60 ? 60 : length;
    memset(tx_buffer, 0, wire_length);
    memcpy(tx_buffer, frame, length);

    out32(static_cast<uint16_t>(g_io_base + kRegTxAddress0 + tx_slot * 4), static_cast<uint32_t>(g_tx_allocation[tx_slot].physical_address));
    out32(static_cast<uint16_t>(g_io_base + kRegTxStatus0 + tx_slot * 4), static_cast<uint32_t>(wire_length));

    for (uint32_t spin = 0; spin < 200000; ++spin) {
        const uint32_t status = in32(static_cast<uint16_t>(g_io_base + kRegTxStatus0 + tx_slot * 4));
        if ((status & kTxOk) != 0) {
            rtl_log("rtl8139: tx ok slot=%u len=%u status=%x\n", (unsigned)tx_slot, (unsigned)wire_length, (unsigned)status);
            ++g_tx_frames;
            g_tx_index = static_cast<uint8_t>((g_tx_index + 1) % 4);
            return true;
        }
        if ((status & kTxErr) != 0) {
            rtl_log("rtl8139: tx err slot=%u status=%x\n", (unsigned)tx_slot, (unsigned)status);
            ++g_tx_errors;
            report(SAVANXP_NET_STATUS_TX_FAILED);
            return false;
        }
    }
    rtl_log("rtl8139: tx timeout slot=%u\n", (unsigned)tx_slot);
    ++g_tx_errors;
    report(SAVANXP_NET_STATUS_TX_TIMEOUT);
    return false;
}

// Salva/deshabilita y restaura IF localmente. poll_receive corre tanto desde el
// handler de INTx (contexto de IRQ) como desde el main (polling/service). Envolver
// su seccion critica con esto la vuelve atomica en monocore: mientras el main
// drena el ring, el IRQ del NIC no puede preemptarlo (evita corromper g_cur_rx).
inline uint64_t irq_save() {
    uint64_t f;
    asm volatile("pushfq; pop %0; cli" : "=r"(f)::"memory");
    return f;
}

inline void irq_restore(uint64_t f) {
    asm volatile("push %0; popfq" ::"r"(f) : "memory", "cc");
}

void poll_receive() {
    if (!g_up) {
        return;
    }

    const uint64_t flags = irq_save();
    clear_interrupt_status();
    auto* rx = static_cast<uint8_t*>(g_rx_allocation.virtual_address);
    while ((in8(static_cast<uint16_t>(g_io_base + kRegCommand)) & kCommandRxBufferEmpty) == 0) {
        uint8_t* packet = rx + g_cur_rx;
        const uint16_t status = *reinterpret_cast<uint16_t*>(packet + 0);
        const uint16_t length = *reinterpret_cast<uint16_t*>(packet + 2);
        rtl_log(
            "rtl8139: rx status=%x len=%u cur=%u cbr=%u cmd=%x\n",
            (unsigned)status,
            (unsigned)length,
            (unsigned)g_cur_rx,
            (unsigned)in16(static_cast<uint16_t>(g_io_base + kRegCbr)),
            (unsigned)in8(static_cast<uint16_t>(g_io_base + kRegCommand))
        );
        if ((status & kRxOk) == 0 || length < 4) {
            rtl_log("rtl8139: rx invalid status=%x len=%u\n", (unsigned)status, (unsigned)length);
            ++g_rx_errors;
            report(SAVANXP_NET_STATUS_RX_INVALID);
            break;
        }

        const size_t frame_length = static_cast<size_t>(length - 4);
        ++g_rx_frames;
        if (g_events.frame != nullptr) {
            g_events.frame(packet + 4, frame_length);
        }

        g_cur_rx = static_cast<uint16_t>((g_cur_rx + length + 4 + 3) & ~3u);
        g_cur_rx %= kRxBufferSize;
        out16(
            static_cast<uint16_t>(g_io_base + kRegCapr),
            static_cast<uint16_t>(g_cur_rx == 0 ? 0xfff0u : (g_cur_rx - 16u))
        );
    }
    irq_restore(flags);
}

// Handler de la INTx del rtl8139 (ruteada por _PRT -> IOAPIC). Sirve el device
// (drena RX + limpia el ISR, desasertando la linea level-triggered) para no
// generar tormenta. Corre en contexto de IRQ.
void irq_handler() {
    g_irq_count = g_irq_count + 1;
    if (g_irq_count == 1) {
        console::write("rtl8139: primer INTx recibido via IOAPIC (interrupt-driven ok)\n");
    }
    poll_receive();
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

bool bring_up() {
    if (!g_present) {
        return false;
    }
    if (g_up) {
        return true;
    }

    if (g_io_base == 0) {
        g_io_base = static_cast<uint16_t>(g_pci_device.bar[0] & ~0x3u);
    }
    if (g_io_base == 0) {
        report(SAVANXP_NET_STATUS_BRING_UP_FAILED);
        return false;
    }

    uint16_t command = pci::read_config_u16(g_pci_device.bus, g_pci_device.slot, g_pci_device.function, kPciCommandOffset);
    command = static_cast<uint16_t>(command | kPciCommandIo | kPciCommandBusMaster);
    pci::write_config_u16(g_pci_device.bus, g_pci_device.slot, g_pci_device.function, kPciCommandOffset, command);

    if (g_rx_allocation.physical_address == 0 &&
        !memory::allocate_contiguous_pages((kRxBufferBytes + memory::kPageSize - 1) / memory::kPageSize, g_rx_allocation)) {
        report(SAVANXP_NET_STATUS_BRING_UP_FAILED);
        return false;
    }
    memset(g_rx_allocation.virtual_address, 0, g_rx_allocation.page_count * memory::kPageSize);

    for (size_t index = 0; index < 4; ++index) {
        if (g_tx_allocation[index].physical_address == 0 && !memory::allocate_page(g_tx_allocation[index])) {
            report(SAVANXP_NET_STATUS_BRING_UP_FAILED);
            return false;
        }
        memset(g_tx_allocation[index].virtual_address, 0, memory::kPageSize);
    }

    out8(static_cast<uint16_t>(g_io_base + kRegConfig1), 0x00);
    out8(static_cast<uint16_t>(g_io_base + kRegCommand), kCommandReset);
    if (!wait_for_clear_reset()) {
        report(SAVANXP_NET_STATUS_BRING_UP_FAILED);
        return false;
    }

    for (uint8_t index = 0; index < 6; ++index) {
        g_mac[index] = in8(static_cast<uint16_t>(g_io_base + kRegMac0 + index));
    }

    out32(static_cast<uint16_t>(g_io_base + kRegRxBuffer), static_cast<uint32_t>(g_rx_allocation.physical_address));
    out16(static_cast<uint16_t>(g_io_base + kRegImr), 0x0000);
    out16(static_cast<uint16_t>(g_io_base + kRegIsr), 0xffff);
    out32(static_cast<uint16_t>(g_io_base + kRegTcr), 0x03000700u);
    out32(
        static_cast<uint16_t>(g_io_base + kRegRcr),
        kRcrWrap | kRcrAcceptAllPhysical | kRcrAcceptPhysical | kRcrAcceptMulticast | kRcrAcceptBroadcast
    );
    out16(static_cast<uint16_t>(g_io_base + kRegCapr), 0xfff0);
    out8(static_cast<uint16_t>(g_io_base + kRegCommand), static_cast<uint8_t>(kCommandRxEnable | kCommandTxEnable));

    g_cur_rx = 0;
    g_up = true;

    // Si la INTx quedo ruteada (en probe), habilitar las interrupciones del
    // rtl8139 (ROK|TOK|RER|TER) => interrupt-driven. Si no, IMR=0 y seguimos en
    // polling (fallback). El IMR ya se puso en 0 arriba (linea kRegImr).
    if (g_irq_routed) {
        out16(static_cast<uint16_t>(g_io_base + kRegImr), 0x000F);
    }

    rtl_log(
        "rtl8139: up io=%x irq=%u mac=%x:%x:%x:%x:%x:%x\n",
        (unsigned)g_io_base,
        (unsigned)g_pci_device.irq_line,
        (unsigned)g_mac[0],
        (unsigned)g_mac[1],
        (unsigned)g_mac[2],
        (unsigned)g_mac[3],
        (unsigned)g_mac[4],
        (unsigned)g_mac[5]
    );
    return true;
}

// Sondea el bus y deja el device reconocido, sin levantarlo: la subida real es
// bring_up(), cuando alguien pide NET_IOC_UP sobre /dev/net0.
bool probe() {
    g_present = pci::find_device(kVendorRealtek, kDeviceRtl8139, g_pci_device);
    if (!g_present) {
        return false;
    }

    g_io_base = static_cast<uint16_t>(g_pci_device.bar[0] & ~0x3u);
    // Rutear la INTx del NIC via _PRT/IOAPIC (una vez, al detectarlo). El
    // handler queda registrado; el device recien asertara INTx cuando bring_up
    // habilite el IMR. Si falla, g_irq_routed=false y se usa polling.
    const uint8_t vec = uacpi_glue::route_pci_intx(
        g_pci_device.bus, g_pci_device.slot, g_pci_device.function, &irq_handler);
    g_irq_routed = (vec != 0);
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
    "rtl8139",
    kDriverPriority,
    &probe,
    &nic_of,
};

} // namespace

namespace rtl8139 {

const nic::Driver& driver() { return kDriver; }

} // namespace rtl8139
