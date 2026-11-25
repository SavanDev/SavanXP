#include "kernel/rtl8139.hpp"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "arch/cpu.hpp"
#include "arch/x86/io.hpp"
#include "kernel/console.hpp"
#include "kernel/inventory.hpp"
#include "kernel/ioapic.hpp"
#include "kernel/iommu.hpp"
#include "kernel/lifecycle.hpp"
#include "kernel/lifecycle_selftest.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/uacpi_glue.hpp"

namespace {

using namespace arch::x86::port_io;

constexpr bool kLogRtl = false;

// Prioridad por debajo de virtio-net (110): en la practica nunca compiten por
// hardware real (la maquina usa uno u otro segun la configuracion de QEMU,
// nunca los dos juntos), pero esto fija el orden si algun dia coexistieran.
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
// The hardware length includes the four-byte CRC. The extra tail is the
// contiguous wrap area promised by RCR.wrap, not part of the logical ring.
constexpr uint32_t kRxHeaderBytes = 4;
constexpr uint32_t kMaxRxRecordBytes = kRxHeaderBytes + 1514;
constexpr uint32_t kRxBufferBytes = kRxBufferSize + 16 + kMaxRxRecordBytes;
constexpr uint32_t kTxBufferBytes = 2048;
constexpr uint16_t kRxOk = 0x0001;
constexpr uint32_t kTxOk = 1u << 15;
constexpr uint32_t kTxErr = 1u << 29;
constexpr uint32_t kRcrWrap = 1u << 7;
constexpr uint32_t kRcrAcceptAllPhysical = 1u << 0;
constexpr uint32_t kRcrAcceptPhysical = 1u << 1;
constexpr uint32_t kRcrAcceptMulticast = 1u << 2;
constexpr uint32_t kRcrAcceptBroadcast = 1u << 3;

/* With the IOMMU translating, the card gets a domain of its own
 * (docs/kernel/IOMMU.md) and sees its buffers here, not at their physical
 * addresses. 2 GiB is not memory on the QEMU boards, so a card left in the
 * shared identity domain by mistake faults instead of writing somewhere. */
constexpr uint32_t kRxDeviceAddress = 0x80000000u;
constexpr uint32_t kTxDeviceAddress = 0x80010000u;

pci::DeviceInfo g_pci_device = {};
memory::PageAllocation g_rx_allocation = {};
memory::PageAllocation g_tx_allocation[4] = {};
// What the card is told: physical addresses, or its domain's.
uint32_t g_rx_device_address = 0;
uint32_t g_tx_device_address[4] = {};
iommu::Domain g_domain;
// Boot option rtl8139-unmapped-tx: transmit buffer 0 is left out of the
// domain, so the first frame sent faults (the M3 fault scenario).
bool g_leave_tx_unmapped = false;
// A DMA fault stopped the card for good.
bool g_failed = false;
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

void clear_interrupt_status() {
    const uint16_t status = in16(static_cast<uint16_t>(g_io_base + kRegIsr));
    if (status != 0) {
        out16(static_cast<uint16_t>(g_io_base + kRegIsr), status);
    }
}

bool g_tx_busy[4] = {};

bool transmit(const void* frame, size_t length) {
    if (!g_up || frame == nullptr || length == 0 || length > kTxBufferBytes) {
        ++g_tx_errors;
        report(SAVANXP_NET_STATUS_TX_FAILED);
        return false;
    }

    const size_t tx_slot = g_tx_index % 4;
    if (g_tx_busy[tx_slot]) {
        const uint32_t status = in32(static_cast<uint16_t>(g_io_base + kRegTxStatus0 + tx_slot * 4));
        if ((status & (kTxOk | kTxErr)) == 0) return false;
        if (status & kTxErr) { ++g_tx_errors; report(SAVANXP_NET_STATUS_TX_FAILED); }
        g_tx_busy[tx_slot] = false;
    }
    auto* tx_buffer = static_cast<uint8_t*>(g_tx_allocation[tx_slot].virtual_address);
    const size_t wire_length = length < 60 ? 60 : length;
    memset(tx_buffer, 0, wire_length);
    memcpy(tx_buffer, frame, length);

    out32(static_cast<uint16_t>(g_io_base + kRegTxAddress0 + tx_slot * 4), g_tx_device_address[tx_slot]);
    out32(static_cast<uint16_t>(g_io_base + kRegTxStatus0 + tx_slot * 4), static_cast<uint32_t>(wire_length));

    g_tx_busy[tx_slot] = true;
    ++g_tx_frames;
    g_tx_index = static_cast<uint8_t>((g_tx_index + 1) % 4);
    return true;
}

void poll_receive() {
    if (!g_up) {
        return;
    }

    // Kernel entry serialization protects this ring across CPUs and IRQs.
    // A fixed budget also bounds a pass while the device keeps receiving.
    const uint64_t flags = arch::save_and_disable_interrupts();
    clear_interrupt_status();
    auto* rx = static_cast<uint8_t*>(g_rx_allocation.virtual_address);
    for (size_t budget = 0; budget < 16 &&
         (in8(static_cast<uint16_t>(g_io_base + kRegCommand)) & kCommandRxBufferEmpty) == 0; ++budget) {
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
        const uint64_t record_end = static_cast<uint64_t>(g_cur_rx) +
            kRxHeaderBytes + length;
        const bool fits_buffer = g_cur_rx < kRxBufferSize &&
            length >= kRxHeaderBytes && length <= kMaxRxRecordBytes &&
            record_end <= kRxBufferBytes;
        if ((status & kRxOk) == 0 || !fits_buffer) {
            rtl_log("rtl8139: rx invalid status=%x len=%u\n", (unsigned)status, (unsigned)length);
            ++g_rx_errors;
            report(SAVANXP_NET_STATUS_RX_INVALID);
            // The card has already advanced its write pointer. Resynchronize
            // with it instead of breaking with CAPR unchanged and repeatedly
            // parsing the same malformed record forever.
            g_cur_rx = static_cast<uint16_t>(
                in16(static_cast<uint16_t>(g_io_base + kRegCbr)) & (kRxBufferSize - 1));
            out16(
                static_cast<uint16_t>(g_io_base + kRegCapr),
                static_cast<uint16_t>(g_cur_rx == 0 ? 0xfff0u : (g_cur_rx - 16u))
            );
            break;
        }

        const size_t frame_length = static_cast<size_t>(length - kRxHeaderBytes);
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
    arch::restore_interrupts(flags);
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

bool interrupts(uint32_t& delivered) {
    delivered = g_irq_count;
    return g_irq_routed;
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

/* The IOMMU's report of a DMA the card's domain does not allow: the card
 * is stopped and moved to the blocked domain, and stays down; the rest of
 * the system carries on without it. */
void dma_fault(void*, uint16_t, uint64_t, bool write) {
    if (g_failed) {
        return;
    }
    g_failed = true;
    g_up = false;
    out16(static_cast<uint16_t>(g_io_base + kRegImr), 0x0000);
    out8(static_cast<uint16_t>(g_io_base + kRegCommand), 0x00);
    const uint16_t command = pci::read_config_u16(g_pci_device.bus, g_pci_device.slot, g_pci_device.function, kPciCommandOffset);
    pci::write_config_u16(g_pci_device.bus, g_pci_device.slot, g_pci_device.function, kPciCommandOffset,
        static_cast<uint16_t>(command & ~kPciCommandBusMaster));
    iommu::block(g_pci_device);
    console::printf("rtl8139: a DMA %s outside the card's domain; the card is stopped and blocked\n",
        write ? "write" : "read");
}

/* Where the card is told its buffers are. Without translation, or when no
 * domain can be had, the physical addresses, as before; otherwise the card
 * moves into a domain that maps its buffers and nothing else. */
void set_up_dma() {
    g_rx_device_address = static_cast<uint32_t>(g_rx_allocation.physical_address);
    for (size_t index = 0; index < 4; ++index) {
        g_tx_device_address[index] = static_cast<uint32_t>(g_tx_allocation[index].physical_address);
    }
    if (g_domain.valid() || !iommu::translating() || !iommu::create_domain(&dma_fault, nullptr, g_domain)) {
        return;
    }
    // The card only writes what it receives and only reads what it sends.
    bool mapped = iommu::map(g_domain, kRxDeviceAddress, g_rx_allocation.physical_address,
        g_rx_allocation.page_count * memory::kPageSize, iommu::kDeviceWrites);
    for (size_t index = 0; index < 4 && mapped; ++index) {
        if (index == 0 && g_leave_tx_unmapped) {
            console::printf("rtl8139: transmit buffer 0 left out of the card's domain (rtl8139-unmapped-tx)\n");
            continue;
        }
        mapped = iommu::map(g_domain, kTxDeviceAddress + index * memory::kPageSize,
            g_tx_allocation[index].physical_address, memory::kPageSize, iommu::kDeviceReads);
    }
    if (!mapped || !iommu::attach(g_domain, g_pci_device)) {
        console::printf("rtl8139: no IOMMU domain of its own; the card stays in the shared one\n");
        iommu::destroy_domain(g_domain);
        return;
    }
    g_rx_device_address = kRxDeviceAddress;
    for (size_t index = 0; index < 4; ++index) {
        g_tx_device_address[index] = static_cast<uint32_t>(kTxDeviceAddress + index * memory::kPageSize);
    }
}

// The reset in progress, across calls of prepare().
unsigned g_phase = 0;
uint64_t g_deadline = 0;

int prepare() {
    if (!g_present || g_failed) {
        return -SX_EIO;
    }
    if (g_up) {
        return 0;
    }

    if (g_phase == 0) {
        uint16_t command = pci::read_config_u16(g_pci_device.bus, g_pci_device.slot, g_pci_device.function, kPciCommandOffset);
        command = static_cast<uint16_t>(command | kPciCommandIo | kPciCommandBusMaster);
        pci::write_config_u16(g_pci_device.bus, g_pci_device.slot, g_pci_device.function, kPciCommandOffset, command);

        memset(g_rx_allocation.virtual_address, 0, g_rx_allocation.page_count * memory::kPageSize);
        for (size_t index = 0; index < 4; ++index) {
            memset(g_tx_allocation[index].virtual_address, 0, memory::kPageSize);
        }

        out8(static_cast<uint16_t>(g_io_base + kRegConfig1), 0x00);
        out8(static_cast<uint16_t>(g_io_base + kRegCommand), kCommandReset);
        g_deadline = timer::monotonic_ns() + 100000000ull;
        g_phase = 1;
        return -SX_EAGAIN;
    }
    if ((in8(static_cast<uint16_t>(g_io_base + kRegCommand)) & kCommandReset) != 0) {
        if (timer::monotonic_ns() < g_deadline) return -SX_EAGAIN;
        report(SAVANXP_NET_STATUS_BRING_UP_FAILED);
        return -SX_EIO;
    }

    for (uint8_t index = 0; index < 6; ++index) {
        g_mac[index] = in8(static_cast<uint16_t>(g_io_base + kRegMac0 + index));
    }

    out32(static_cast<uint16_t>(g_io_base + kRegRxBuffer), g_rx_device_address);
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

    // Enable ROK/TOK/RER/TER when start routed INTx; otherwise retain polling.
    // The interrupt mask was cleared above before resetting the device.
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
    return 0;
}

bool bring_up() {
    int result;
    do { result = prepare(); asm volatile("pause"); } while (result == -SX_EAGAIN);
    // Started is taken; up or not is what the inventory shows from here.
    if (result == 0) {
        inventory::pci_started(g_pci_device);
    } else {
        inventory::pci_failed(g_pci_device);
    }
    return result == 0;
}

// The first RTL8139 no other driver holds. One card: nic:: drives one NIC.
bool find_card(pci::DeviceInfo& found) {
    for (size_t index = 0; index < pci::device_count(); ++index) {
        pci::DeviceInfo info = {};
        if (pci::device_info(index, info) && info.vendor_id == kVendorRealtek && info.device_id == kDeviceRtl8139 &&
            inventory::pci_available(info, "rtl8139")) {
            found = info;
            return true;
        }
    }
    return false;
}

/* ---- start and stop (docs/kernel/DRIVER_LIFECYCLE.md) ---------------------
 *
 * start() takes the card in stages, each pushing onto g_acquired how to give
 * back what it took; a failed stage unwinds exactly that, and stop() unwinds
 * the rest once quiesce() has proved the card quiet. The reset and the
 * receive set-up stay in prepare(), which net:: runs on NET_IOC_UP. */

constexpr char kDriverName[] = "rtl8139";
// The stages of start(), for failure injection: claim, registers,
// interrupt, buffers, translation.
constexpr unsigned kStartStages = 5;

lifecycle::Acquired g_acquired;
bool g_started = false;
// Set while a failed start unwinds: the function is then given back as a
// failed start, not as a clean stop.
bool g_start_failing = false;
uint32_t g_gsi = 0;
uint8_t g_vector = 0;

void release_function(void*) {
    if (g_start_failing) {
        (void)inventory::pci_release_failed(g_pci_device, kDriverName);
    } else {
        (void)inventory::pci_release(g_pci_device, kDriverName);
    }
    g_io_base = 0;
}

void release_interrupt(void*) {
    if (!ioapic::unroute_gsi(g_gsi, &irq_handler)) {
        console::printf("rtl8139: the handler was not on GSI %u\n", static_cast<unsigned>(g_gsi));
    }
    g_irq_routed = false;
    g_vector = 0;
}

void release_buffers(void*) {
    if (g_rx_allocation.physical_address != 0) {
        (void)memory::free_allocation(g_rx_allocation);
        g_rx_allocation = {};
    }
    for (size_t index = 0; index < 4; ++index) {
        if (g_tx_allocation[index].physical_address != 0) {
            (void)memory::free_allocation(g_tx_allocation[index]);
            g_tx_allocation[index] = {};
        }
    }
}

void release_translation(void*) {
    // The card goes to the blocked domain and the tables are freed.
    iommu::destroy_domain(g_domain);
}

bool stage_failed(unsigned stage, const char* name) {
    if (!lifecycle::injected(kDriverName, stage)) {
        return false;
    }
    console::printf("rtl8139: start stage %u (%s) failed by injection\n", stage, name);
    return true;
}

// The card's registers, interrupt line, DMA buffers and IOMMU domain, in
// that order. False with everything given back; the card is left alone if
// another driver holds it.
bool start(const pci::DeviceInfo& card) {
    g_pci_device = card;
    unsigned stage = 1;
    bool ok = !stage_failed(stage, "claim") && inventory::pci_claim(g_pci_device, kDriverName);
    if (!ok) {
        return false;
    }
    ok = g_acquired.push("claim", &release_function, nullptr);

    pci::BarInfo io = {};
    if (ok) {
        ++stage;
        ok = !stage_failed(stage, "registers") && pci::bar_info(g_pci_device, 0, io) && io.io_space &&
            inventory::pci_claim_bar(g_pci_device, io);
        if (ok) {
            g_io_base = static_cast<uint16_t>(io.base);
            // Port decoding, so quiesce() can reach the registers even if
            // prepare() never ran; bus mastering waits for prepare().
            const uint16_t command = pci::read_config_u16(g_pci_device.bus, g_pci_device.slot,
                g_pci_device.function, kPciCommandOffset);
            pci::write_config_u16(g_pci_device.bus, g_pci_device.slot, g_pci_device.function, kPciCommandOffset,
                static_cast<uint16_t>(command | kPciCommandIo));
        }
    }
    if (ok) {
        // INTx through _PRT and the IOAPIC. The device asserts it only after
        // prepare() enables IMR; a line that cannot be routed leaves polling.
        ++stage;
        ok = !stage_failed(stage, "interrupt");
        if (ok) {
            g_vector = uacpi_glue::route_pci_intx(
                g_pci_device.bus, g_pci_device.slot, g_pci_device.function, &irq_handler, &g_gsi);
            g_irq_routed = g_vector != 0;
            if (g_irq_routed) {
                ok = g_acquired.push("interrupt", &release_interrupt, nullptr);
                if (!ok) {
                    release_interrupt(nullptr);
                }
            }
            if (ok && g_irq_routed) {
                ok = inventory::pci_claim_resource(g_pci_device, SXN_DEVICE_RESOURCE_IRQ, g_gsi, 1,
                    SXN_DEVICE_RESOURCE_SHARED);
            }
        }
    }
    if (ok) {
        // Recorded first, so a partial allocation is freed too.
        ++stage;
        ok = !stage_failed(stage, "buffers") && g_acquired.push("buffers", &release_buffers, nullptr);
        if (ok) {
            ok = memory::allocate_contiguous_pages(
                (kRxBufferBytes + memory::kPageSize - 1) / memory::kPageSize, g_rx_allocation);
            for (size_t index = 0; index < 4 && ok; ++index) {
                ok = memory::allocate_page(g_tx_allocation[index]);
            }
        }
    }
    if (ok) {
        ++stage;
        ok = !stage_failed(stage, "translation");
        if (ok) {
            set_up_dma();
            if (g_domain.valid()) {
                ok = g_acquired.push("translation", &release_translation, nullptr);
                if (!ok) {
                    release_translation(nullptr);
                }
            }
        }
    }
    if (!ok) {
        console::printf("rtl8139: could not start at %x:%x.%u (stage %u); everything it took is given back\n",
            static_cast<unsigned>(card.bus), static_cast<unsigned>(card.slot),
            static_cast<unsigned>(card.function), stage);
        g_start_failing = true;
        g_acquired.unwind();
        g_start_failing = false;
        return false;
    }
    g_failed = false;
    g_started = true;
    g_present = true;
    inventory::pci_started(g_pci_device);
    return true;
}

// Interrupts masked, receive and transmit off, bus mastering off, and read
// back: true only when the card can neither raise an interrupt nor reach
// memory.
bool quiesce() {
    g_up = false;
    out16(static_cast<uint16_t>(g_io_base + kRegImr), 0x0000);
    out8(static_cast<uint16_t>(g_io_base + kRegCommand), 0x00);
    clear_interrupt_status();
    const uint16_t command =
        pci::read_config_u16(g_pci_device.bus, g_pci_device.slot, g_pci_device.function, kPciCommandOffset);
    pci::write_config_u16(g_pci_device.bus, g_pci_device.slot, g_pci_device.function, kPciCommandOffset,
        static_cast<uint16_t>(command & ~kPciCommandBusMaster));
    const bool engines_off =
        (in8(static_cast<uint16_t>(g_io_base + kRegCommand)) & (kCommandRxEnable | kCommandTxEnable)) == 0;
    const bool masked = in16(static_cast<uint16_t>(g_io_base + kRegImr)) == 0;
    const bool no_master = (pci::read_config_u16(g_pci_device.bus, g_pci_device.slot, g_pci_device.function,
        kPciCommandOffset) & kPciCommandBusMaster) == 0;
    return engines_off && masked && no_master;
}

// Quiet, then everything start() took given back. A card that will not
// quiesce keeps all of it and is listed as failed: its buffers may still be
// written.
bool stop() {
    if (!g_started) {
        return true;
    }
    if (!quiesce()) {
        console::write_line("rtl8139: the card did not quiesce; it keeps its buffers");
        inventory::pci_failed(g_pci_device);
        return false;
    }
    g_acquired.unwind();
    g_started = false;
    g_present = false;
    g_phase = 0;
    g_cur_rx = 0;
    g_tx_index = 0;
    for (bool& busy : g_tx_busy) {
        busy = false;
    }
    return true;
}

// Recognizes the card and takes it, without bringing the link up: NET_IOC_UP
// runs bring_up(), and native socket calls advance the reset through
// prepare().
bool probe() {
    pci::DeviceInfo card = {};
    g_present = find_card(card) && start(card);
    return g_present;
}

const nic::Nic kNic = {
    &attach,
    &bring_up,
    &is_up,
    &mac_address,
    &transmit,
    &poll_receive,
    &get_stats,
    &prepare,
    &interrupts,
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

void leave_transmit_buffer_unmapped(bool leave) { g_leave_tx_unmapped = leave; }

namespace {

bool idle() {
    return !g_started && g_acquired.count() == 0 && !g_domain.valid();
}

uint8_t vector() {
    return g_vector;
}

const lifecycle::Subject kSubject = {
    kDriverName,
    kStartStages,
    &find_card,
    &start,
    &bring_up,
    &stop,
    &idle,
    &vector,
};

} // namespace

const lifecycle::Subject& lifecycle_subject() { return kSubject; }

} // namespace rtl8139
