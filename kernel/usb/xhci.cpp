// xHCI host controller driver. Written from the xHCI 1.2 specification, with
// OpenBSD's sys/dev/usb/xhci.c and sys/dev/pci/xhci_pci.c as reference (see
// docs/third-party/THIRD_PARTY_ADOPTION.md). Polled: interrupts stay off and
// every completion is found by draining the event ring, like the other
// drivers here. See docs/roadmaps/open/USB_ROADMAP.md.

#include <stddef.h>
#include <stdint.h>

#include "arch/cpu.hpp"
#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/vmm.hpp"
#include "usb_internal.hpp"

namespace {

using usb::Device;
using usb::Speed;

// PCI class of an xHCI controller: serial bus, USB, xHCI interface.
constexpr uint8_t kClassSerialBus = 0x0c;
constexpr uint8_t kSubclassUsb = 0x03;
constexpr uint8_t kProgIfXhci = 0x30;

constexpr uint16_t kVendorIntel = 0x8086;
// Intel's EHCI/xHCI port switching (8 Series and later): which ports the
// firmware may hand over, and the registers that hand them over.
constexpr uint8_t kIntelXusb2pr = 0xd0;
constexpr uint8_t kIntelXusb2prm = 0xd4;
constexpr uint8_t kIntelUsb3Pssen = 0xd8;
constexpr uint8_t kIntelUsb3prm = 0xdc;
// PCI power management capability: PMCSR bits 0-1 are the power state.
constexpr uint8_t kPciCapabilityPowerManagement = 0x01;

constexpr uint8_t kPciCommandOffset = 0x04;
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;
constexpr uint16_t kPciCommandInterruptDisable = 1u << 10;

// Capability registers.
constexpr uint32_t kCapLength = 0x00;
constexpr uint32_t kHcsParams1 = 0x04;
constexpr uint32_t kHcsParams2 = 0x08;
constexpr uint32_t kHccParams1 = 0x10;
constexpr uint32_t kDbOff = 0x14;
constexpr uint32_t kRtsOff = 0x18;
constexpr uint32_t kHccAc64 = 1u << 0;
constexpr uint32_t kHccContextSize = 1u << 2;

// Operational registers.
constexpr uint32_t kUsbCmd = 0x00;
constexpr uint32_t kUsbSts = 0x04;
constexpr uint32_t kPageSizeReg = 0x08;
constexpr uint32_t kCrcr = 0x18;
constexpr uint32_t kDcbaap = 0x30;
constexpr uint32_t kConfig = 0x38;
constexpr uint32_t kPortBase = 0x400;
constexpr uint32_t kPortStride = 0x10;
constexpr uint32_t kCmdRun = 1u << 0;
constexpr uint32_t kCmdReset = 1u << 1;
constexpr uint32_t kStsHalted = 1u << 0;
constexpr uint32_t kStsNotReady = 1u << 11;

// PORTSC.
constexpr uint32_t kPortConnected = 1u << 0;
constexpr uint32_t kPortEnabled = 1u << 1;
constexpr uint32_t kPortReset = 1u << 4;
constexpr uint32_t kPortPower = 1u << 9;
constexpr uint32_t kPortConnectChange = 1u << 17;
constexpr uint32_t kPortResetChange = 1u << 21;
constexpr uint32_t kPortWarmResetChange = 1u << 19;
constexpr uint32_t kPortWarmReset = 1u << 31;
constexpr uint32_t kPortChangeBits = 0x00fe0000u;
// What a write carries back unchanged: connect status, over-current, link
// state, power, speed, indicators and wake enables. PED, LWS and every change
// bit are left out: writing 1 to them disables the port, moves the link or
// clears the change.
constexpr uint32_t kPortPreserve = 0x0e00fbe9u;

// Runtime registers of interrupter 0.
constexpr uint32_t kInterrupter0 = 0x20;
constexpr uint32_t kIman = 0x00;
constexpr uint32_t kErstSz = 0x08;
constexpr uint32_t kErstBa = 0x10;
constexpr uint32_t kErdp = 0x18;
constexpr uint64_t kErdpBusy = 1u << 3;

// Extended capabilities.
constexpr uint8_t kExtCapLegacy = 1;
constexpr uint8_t kExtCapProtocol = 2;
constexpr uint32_t kLegacyBiosOwned = 1u << 16;
constexpr uint32_t kLegacyOsOwned = 1u << 24;

// TRB types.
constexpr uint32_t kTrbNormal = 1;
constexpr uint32_t kTrbSetup = 2;
constexpr uint32_t kTrbData = 3;
constexpr uint32_t kTrbStatus = 4;
constexpr uint32_t kTrbLink = 6;
constexpr uint32_t kTrbEnableSlot = 9;
constexpr uint32_t kTrbDisableSlot = 10;
constexpr uint32_t kTrbAddressDevice = 11;
constexpr uint32_t kTrbConfigureEndpoint = 12;
constexpr uint32_t kTrbEvaluateContext = 13;
constexpr uint32_t kTrbResetEndpoint = 14;
constexpr uint32_t kTrbStopEndpoint = 15;
constexpr uint32_t kTrbSetDequeue = 16;
constexpr uint32_t kTrbNoOp = 23;
constexpr uint32_t kTrbTransferEvent = 32;
constexpr uint32_t kTrbCommandEvent = 33;
constexpr uint32_t kTrbPortEvent = 34;

// TRB control bits.
constexpr uint32_t kTrbCycle = 1u << 0;
constexpr uint32_t kTrbToggleCycle = 1u << 1;
constexpr uint32_t kTrbShortPacketInterrupt = 1u << 2;
constexpr uint32_t kTrbChain = 1u << 4;
constexpr uint32_t kTrbInterruptOnCompletion = 1u << 5;
constexpr uint32_t kTrbImmediateData = 1u << 6;
constexpr uint32_t kTrbDirectionIn = 1u << 16;

// Completion codes.
constexpr uint8_t kCodeSuccess = 1;
constexpr uint8_t kCodeStall = 6;
constexpr uint8_t kCodeShortPacket = 13;

// Endpoint context types.
constexpr uint32_t kEpControl = 4;
constexpr uint32_t kEpBulkOut = 2;
constexpr uint32_t kEpBulkIn = 6;
constexpr uint32_t kEpInterruptOut = 3;
constexpr uint32_t kEpInterruptIn = 7;

constexpr uint32_t kRingTrbs = memory::kPageSize / 16;
constexpr size_t kMaxControllers = 4;
constexpr size_t kMaxSlots = 32;
constexpr size_t kMaxPorts = 64;
// The specification's ceiling: 1023 pages; the array of their addresses must
// fit one page.
constexpr size_t kMaxScratchpads = memory::kPageSize / sizeof(uint64_t);
constexpr size_t kMaxTdTrbs = 4;
constexpr uint32_t kCommandTimeoutMs = 1000;
constexpr uint32_t kControlTimeoutMs = 1000;

// The tail of every device's input-context page holds the buffers of its
// interrupt IN endpoints: past the 33 contexts of 64 bytes, at most.
constexpr size_t kInterruptBufferOffset = 3072;
constexpr size_t kInterruptBufferSize = 128;
constexpr size_t kInterruptBuffers = (memory::kPageSize - kInterruptBufferOffset) / kInterruptBufferSize;

struct Trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};
static_assert(sizeof(Trb) == 16);

struct Ring {
    volatile Trb* trbs;
    uint64_t physical;
    uint32_t enqueue;
    uint32_t cycle;
    memory::PageAllocation page;
};

struct Endpoint {
    bool active;
    uint8_t address;
    uint16_t max_packet;
    Ring ring;
    // A synchronous transfer in flight: the TRBs of its TD, and what the
    // controller said about it.
    bool waiting;
    bool done;
    uint8_t code;
    uint32_t residual;
    uint64_t done_trb;
    uint64_t td[kMaxTdTrbs];
    uint32_t td_length[kMaxTdTrbs];
    size_t td_count;
    // The data stage of a control transfer, which may end short without
    // ending the transfer.
    uint64_t data_trb;
    bool data_seen;
    uint32_t data_residual;
    // Interrupt IN.
    usb::hc::InterruptCallback callback;
    uint8_t* buffer;
    uint64_t buffer_physical;
    uint16_t length;
    uint64_t pending_trb;
    bool pending;
};

// What the controller keeps per device, indexed like the core's table.
struct DeviceState {
    memory::PageAllocation output;
    memory::PageAllocation input;
    Endpoint endpoints[32];
    size_t buffers_used;
};

struct Controller {
    pci::DeviceInfo pci;
    volatile uint8_t* cap;
    volatile uint8_t* op;
    volatile uint8_t* rt;
    volatile uint8_t* db;
    uint8_t slots;
    uint8_t ports;
    size_t context_size;
    bool ac64;
    memory::PageAllocation dcbaa;
    memory::PageAllocation scratch_array;
    memory::PageAllocation scratch;
    Ring commands;
    memory::PageAllocation event_page;
    memory::PageAllocation erst;
    volatile Trb* events;
    uint32_t event_dequeue;
    uint32_t event_cycle;
    // The command in flight.
    uint64_t command_trb;
    bool command_done;
    uint8_t command_code;
    uint8_t command_slot;
    uint8_t port_major[kMaxPorts + 1];
    uint64_t port_changes;
    // Bounce buffers for control and bulk data.
    memory::PageAllocation control_buffer;
    memory::PageAllocation bulk_buffer;
    Device* slot_device[kMaxSlots + 1];
    bool draining;
};

Controller g_controllers[kMaxControllers] = {};
size_t g_controller_count = 0;
DeviceState g_states[usb::kMaxDevices] = {};

uint32_t read32(volatile uint8_t* base, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(base + offset);
}

void write32(volatile uint8_t* base, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(base + offset) = value;
}

// 64-bit registers are written as two halves, low first: some controllers
// do not take a 64-bit access.
void write64(volatile uint8_t* base, uint32_t offset, uint64_t value) {
    write32(base, offset, static_cast<uint32_t>(value));
    write32(base, offset + 4, static_cast<uint32_t>(value >> 32));
}

uint64_t now_ms() {
    return timer::monotonic_ns() / 1000000u;
}

Controller& controller_of(const Device& device) {
    return g_controllers[device.controller];
}

DeviceState& state_of(const Device& device) {
    return g_states[usb::device_index(device)];
}

uint8_t endpoint_index(uint8_t address) {
    // Device context index: endpoint 0 is 1, then OUT n is 2n, IN n is 2n+1.
    const uint8_t number = address & 0x0f;
    if (number == 0) {
        return 1;
    }
    return static_cast<uint8_t>(number * 2 + ((address & 0x80) != 0 ? 1 : 0));
}

bool fits(const Controller& controller, uint64_t physical) {
    return controller.ac64 || (physical >> 32) == 0;
}

bool allocate_zeroed(const Controller& controller, memory::PageAllocation& page) {
    if (!memory::allocate_page(page)) {
        return false;
    }
    if (!fits(controller, page.physical_address)) {
        (void)memory::free_allocation(page);
        page = {};
        return false;
    }
    memset(page.virtual_address, 0, memory::kPageSize);
    return true;
}

void release(memory::PageAllocation& page) {
    if (page.page_count != 0) {
        (void)memory::free_allocation(page);
    }
    page = {};
}

bool ring_create(const Controller& controller, Ring& ring) {
    ring = {};
    if (!allocate_zeroed(controller, ring.page)) {
        return false;
    }
    ring.trbs = static_cast<volatile Trb*>(ring.page.virtual_address);
    ring.physical = ring.page.physical_address;
    ring.cycle = 1;
    return true;
}

// Queues one TRB and returns its physical address. The cycle bit is written
// last, so the controller never sees a half-written TRB as its own.
uint64_t ring_push(Ring& ring, uint64_t parameter, uint32_t status, uint32_t control) {
    volatile Trb& trb = ring.trbs[ring.enqueue];
    trb.parameter = parameter;
    trb.status = status;
    arch::store_barrier();
    trb.control = (control & ~kTrbCycle) | ring.cycle;
    const uint64_t physical = ring.physical + static_cast<uint64_t>(ring.enqueue) * sizeof(Trb);
    if (++ring.enqueue == kRingTrbs - 1) {
        // The last TRB links back to the first and flips the cycle. It keeps
        // the chain bit of the TRB before it, so a TD may cross it.
        volatile Trb& link = ring.trbs[kRingTrbs - 1];
        link.parameter = ring.physical;
        link.status = 0;
        arch::store_barrier();
        link.control = (kTrbLink << 10) | kTrbToggleCycle | (control & kTrbChain) | ring.cycle;
        ring.enqueue = 0;
        ring.cycle ^= 1;
    }
    return physical;
}

uint64_t ring_dequeue_pointer(const Ring& ring) {
    return (ring.physical + static_cast<uint64_t>(ring.enqueue) * sizeof(Trb)) | ring.cycle;
}

void ring_doorbell(Controller& controller, uint8_t slot, uint8_t target) {
    arch::store_barrier();
    write32(controller.db, static_cast<uint32_t>(slot) * 4u, target);
}

volatile uint32_t* context(const memory::PageAllocation& page, size_t context_size, size_t index) {
    return reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(page.virtual_address) + index * context_size);
}

void handle_transfer_event(Controller& controller, const Trb& event) {
    const uint8_t slot = static_cast<uint8_t>(event.control >> 24);
    const uint8_t index = static_cast<uint8_t>((event.control >> 16) & 0x1f);
    const uint8_t code = static_cast<uint8_t>(event.status >> 24);
    const uint32_t residual = event.status & 0xffffffu;
    if (slot == 0 || slot > kMaxSlots || controller.slot_device[slot] == nullptr) {
        return;
    }
    Device& device = *controller.slot_device[slot];
    Endpoint& endpoint = state_of(device).endpoints[index];

    if (endpoint.pending && event.parameter == endpoint.pending_trb) {
        endpoint.pending = false;
        int status = usb::status_ok;
        if (code == kCodeStall) {
            status = usb::status_stall;
        } else if (code != kCodeSuccess && code != kCodeShortPacket) {
            status = usb::status_error;
        }
        const uint32_t length = residual > endpoint.length ? 0u : endpoint.length - residual;
        if (endpoint.callback != nullptr) {
            endpoint.callback(device, endpoint.address, status, endpoint.buffer, length);
        }
        return;
    }
    if (!endpoint.waiting) {
        return;
    }
    const bool error = code != kCodeSuccess && code != kCodeShortPacket;
    if (endpoint.data_trb != 0 && event.parameter == endpoint.data_trb && !error) {
        endpoint.data_seen = true;
        endpoint.data_residual = residual;
        return;
    }
    bool ours = endpoint.data_trb != 0 && event.parameter == endpoint.data_trb;
    for (size_t i = 0; i < endpoint.td_count && !ours; ++i) {
        ours = event.parameter == endpoint.td[i];
    }
    if (!ours) {
        return;
    }
    endpoint.done = true;
    endpoint.code = code;
    endpoint.residual = residual;
    endpoint.done_trb = event.parameter;
}

void drain(Controller& controller) {
    bool consumed = false;
    for (;;) {
        volatile Trb& slot = controller.events[controller.event_dequeue];
        const uint32_t control = slot.control;
        if ((control & kTrbCycle) != controller.event_cycle) {
            break;
        }
        // Copy and advance before handling: a callback that queues work may
        // drain again, and must not see this event a second time.
        Trb event = {};
        event.parameter = slot.parameter;
        event.status = slot.status;
        event.control = control;
        if (++controller.event_dequeue == kRingTrbs) {
            controller.event_dequeue = 0;
            controller.event_cycle ^= 1;
        }
        consumed = true;
        switch ((event.control >> 10) & 0x3f) {
        case kTrbTransferEvent:
            handle_transfer_event(controller, event);
            break;
        case kTrbCommandEvent:
            if (event.parameter == controller.command_trb) {
                controller.command_done = true;
                controller.command_code = static_cast<uint8_t>(event.status >> 24);
                controller.command_slot = static_cast<uint8_t>(event.control >> 24);
            }
            break;
        case kTrbPortEvent: {
            const uint32_t port = static_cast<uint32_t>(event.parameter >> 24) & 0xffu;
            if (port >= 1 && port <= kMaxPorts) {
                controller.port_changes |= 1ull << (port - 1);
            }
            break;
        }
        default:
            break;
        }
    }
    if (consumed) {
        write64(controller.rt, kInterrupter0 + kErdp,
            (controller.event_page.physical_address +
                static_cast<uint64_t>(controller.event_dequeue) * sizeof(Trb)) | kErdpBusy);
    }
}

// Runs a command and returns its completion code (0 if it never completed).
uint8_t command(Controller& controller, uint64_t parameter, uint32_t control, uint8_t* slot_out = nullptr) {
    controller.command_done = false;
    controller.command_trb = ring_push(controller.commands, parameter, 0, control);
    ring_doorbell(controller, 0, 0);
    const uint64_t deadline = now_ms() + kCommandTimeoutMs;
    while (!controller.command_done) {
        drain(controller);
        if (controller.command_done) {
            break;
        }
        if (now_ms() > deadline) {
            console::printf("xhci: command %u timed out\n", static_cast<unsigned>((control >> 10) & 0x3f));
            return 0;
        }
        arch::cpu_relax();
    }
    if (slot_out != nullptr) {
        *slot_out = controller.command_slot;
    }
    return controller.command_code;
}

// After a stall or a timeout the endpoint's ring position is unknown to the
// controller: stop or reset it, then point it at the next free TRB.
bool recover_endpoint(Controller& controller, Device& device, uint8_t index, bool halted) {
    Endpoint& endpoint = state_of(device).endpoints[index];
    const uint32_t target = (static_cast<uint32_t>(device.slot) << 24) | (static_cast<uint32_t>(index) << 16);
    if (halted) {
        (void)command(controller, 0, (kTrbResetEndpoint << 10) | target);
    } else {
        (void)command(controller, 0, (kTrbStopEndpoint << 10) | target);
    }
    return command(controller, ring_dequeue_pointer(endpoint.ring), (kTrbSetDequeue << 10) | target) ==
        kCodeSuccess;
}

// Waits for the transfer on `endpoint` to finish. false on timeout.
bool wait_transfer(Controller& controller, Endpoint& endpoint, uint32_t timeout_ms) {
    const uint64_t deadline = now_ms() + timeout_ms;
    while (!endpoint.done) {
        drain(controller);
        if (endpoint.done) {
            break;
        }
        if (now_ms() > deadline) {
            return false;
        }
        arch::cpu_relax();
    }
    return true;
}

int finish_transfer(Controller& controller, Device& device, uint8_t index, bool completed) {
    Endpoint& endpoint = state_of(device).endpoints[index];
    endpoint.waiting = false;
    if (!completed) {
        (void)recover_endpoint(controller, device, index, false);
        return usb::status_timeout;
    }
    if (endpoint.code == kCodeSuccess || endpoint.code == kCodeShortPacket) {
        return usb::status_ok;
    }
    (void)recover_endpoint(controller, device, index, true);
    return endpoint.code == kCodeStall ? usb::status_stall : usb::status_error;
}

void start_transfer(Endpoint& endpoint) {
    endpoint.waiting = true;
    endpoint.done = false;
    endpoint.code = 0;
    endpoint.residual = 0;
    endpoint.done_trb = 0;
    endpoint.td_count = 0;
    endpoint.data_trb = 0;
    endpoint.data_seen = false;
    endpoint.data_residual = 0;
}

uint32_t port_register(uint8_t port) {
    return kPortBase + static_cast<uint32_t>(port - 1) * kPortStride;
}

uint32_t read_port(Controller& controller, uint8_t port) {
    return read32(controller.op, port_register(port));
}

void write_port(Controller& controller, uint8_t port, uint32_t portsc, uint32_t set) {
    write32(controller.op, port_register(port), (portsc & kPortPreserve) | set);
}

bool wait_port(Controller& controller, uint8_t port, uint32_t mask, uint32_t timeout_ms) {
    const uint64_t deadline = now_ms() + timeout_ms;
    while ((read_port(controller, port) & mask) == 0) {
        if (now_ms() > deadline) {
            return false;
        }
        arch::cpu_relax();
    }
    return true;
}

// The firmware may be using the controller (legacy keyboard emulation): ask
// for it through USBLEGSUP and turn its SMIs off.
void take_from_firmware(Controller& controller) {
    uint32_t offset = ((read32(controller.cap, kHccParams1) >> 16) & 0xffffu) * 4u;
    while (offset != 0) {
        const uint32_t value = read32(controller.cap, offset);
        if (value == 0xffffffffu) {
            return;
        }
        if ((value & 0xff) == kExtCapLegacy) {
            if ((value & kLegacyBiosOwned) != 0) {
                write32(controller.cap, offset, value | kLegacyOsOwned);
                // OpenBSD's xhci_pci.c gives the firmware 5 s; some take
                // seconds to stop their legacy emulation.
                const uint64_t deadline = now_ms() + 5000;
                while ((read32(controller.cap, offset) & kLegacyBiosOwned) != 0 && now_ms() < deadline) {
                    arch::cpu_relax();
                }
                if ((read32(controller.cap, offset) & kLegacyBiosOwned) != 0) {
                    console::write_line("xhci: the firmware did not release the controller");
                    // Take it anyway, as other systems do.
                    write32(controller.cap, offset, read32(controller.cap, offset) & ~kLegacyBiosOwned);
                }
            }
            // USBLEGCTLSTS: SMI enables off, SMI status bits cleared.
            const uint32_t control = read32(controller.cap, offset + 4);
            write32(controller.cap, offset + 4, (control & 0x000e1feeu) | 0xe0000000u);
        }
        const uint32_t next = (value >> 8) & 0xff;
        offset = next == 0 ? 0 : offset + next * 4u;
    }
}

// Which ports are USB 2 and which USB 3, from the Supported Protocol
// capabilities: they reset differently.
void read_protocols(Controller& controller) {
    uint32_t offset = ((read32(controller.cap, kHccParams1) >> 16) & 0xffffu) * 4u;
    while (offset != 0) {
        const uint32_t value = read32(controller.cap, offset);
        if (value == 0xffffffffu) {
            return;
        }
        if ((value & 0xff) == kExtCapProtocol) {
            const uint8_t major = static_cast<uint8_t>(value >> 24);
            const uint32_t ports = read32(controller.cap, offset + 8);
            const uint32_t first = ports & 0xff;
            const uint32_t count = (ports >> 8) & 0xff;
            for (uint32_t port = first; port < first + count && port <= kMaxPorts; ++port) {
                if (port >= 1) {
                    controller.port_major[port] = major;
                }
            }
        }
        const uint32_t next = (value >> 8) & 0xff;
        offset = next == 0 ? 0 : offset + next * 4u;
    }
}

bool wait_status(Controller& controller, uint32_t mask, bool set, uint32_t timeout_ms) {
    const uint64_t deadline = now_ms() + timeout_ms;
    while (((read32(controller.op, kUsbSts) & mask) != 0) != set) {
        if (now_ms() > deadline) {
            return false;
        }
        arch::cpu_relax();
    }
    return true;
}

bool reset_controller(Controller& controller) {
    write32(controller.op, kUsbCmd, read32(controller.op, kUsbCmd) & ~kCmdRun);
    if (!wait_status(controller, kStsHalted, true, 100)) {
        console::write_line("xhci: the controller does not halt");
        return false;
    }
    write32(controller.op, kUsbCmd, kCmdReset);
    // Some Intel controllers hang if touched within 1 ms of the reset.
    usb::delay_ms(1);
    const uint64_t deadline = now_ms() + 1000;
    while ((read32(controller.op, kUsbCmd) & kCmdReset) != 0) {
        if (now_ms() > deadline) {
            console::write_line("xhci: the controller does not leave reset");
            return false;
        }
        arch::cpu_relax();
    }
    if (!wait_status(controller, kStsNotReady, false, 1000)) {
        console::write_line("xhci: the controller never becomes ready");
        return false;
    }
    return true;
}

bool set_up_memory(Controller& controller) {
    if (!allocate_zeroed(controller, controller.dcbaa)) {
        return false;
    }
    auto* dcbaa = static_cast<uint64_t*>(controller.dcbaa.virtual_address);

    const uint32_t params2 = read32(controller.cap, kHcsParams2);
    const size_t scratchpads = ((params2 >> 27) & 0x1f) | (((params2 >> 21) & 0x1f) << 5);
    if (scratchpads > kMaxScratchpads) {
        console::printf("xhci: wants %u scratchpad pages, more than supported\n", static_cast<unsigned>(scratchpads));
        return false;
    }
    if (scratchpads != 0) {
        if (!allocate_zeroed(controller, controller.scratch_array)) {
            return false;
        }
        // One contiguous run: the controller only needs page-aligned pages,
        // and one allocation is one thing to keep.
        if (!memory::allocate_contiguous_pages(scratchpads, controller.scratch) ||
            !fits(controller, controller.scratch.physical_address)) {
            return false;
        }
        memset(controller.scratch.virtual_address, 0, scratchpads * memory::kPageSize);
        auto* array = static_cast<uint64_t*>(controller.scratch_array.virtual_address);
        for (size_t i = 0; i < scratchpads; ++i) {
            array[i] = controller.scratch.physical_address + i * memory::kPageSize;
        }
        dcbaa[0] = controller.scratch_array.physical_address;
    }

    if (!ring_create(controller, controller.commands) || !allocate_zeroed(controller, controller.event_page) ||
        !allocate_zeroed(controller, controller.erst) || !allocate_zeroed(controller, controller.control_buffer)) {
        return false;
    }
    if (!memory::allocate_contiguous_pages(usb::hc::kMaxBulkBytes / memory::kPageSize, controller.bulk_buffer) ||
        !fits(controller, controller.bulk_buffer.physical_address)) {
        return false;
    }
    controller.events = static_cast<volatile Trb*>(controller.event_page.virtual_address);
    controller.event_cycle = 1;
    auto* erst = static_cast<uint32_t*>(controller.erst.virtual_address);
    erst[0] = static_cast<uint32_t>(controller.event_page.physical_address);
    erst[1] = static_cast<uint32_t>(controller.event_page.physical_address >> 32);
    erst[2] = kRingTrbs;
    erst[3] = 0;

    write32(controller.op, kConfig, controller.slots);
    write64(controller.op, kDcbaap, controller.dcbaa.physical_address);
    write64(controller.op, kCrcr, controller.commands.physical | 1u);
    write32(controller.rt, kInterrupter0 + kErstSz, 1);
    write64(controller.rt, kInterrupter0 + kErdp, controller.event_page.physical_address);
    write64(controller.rt, kInterrupter0 + kErstBa, controller.erst.physical_address);
    // Interrupter disabled; clear a pending flag left by the firmware.
    write32(controller.rt, kInterrupter0 + kIman, 1u);
    return true;
}

// A controller the firmware left in a low-power state answers every read
// with all ones: bring it to D0 first (PCI PM 1.2, 10 ms to recover from D3).
void power_up(const pci::DeviceInfo& info) {
    uint8_t capability = 0;
    if (!pci::find_capability(info, kPciCapabilityPowerManagement, capability)) {
        return;
    }
    const uint8_t pmcsr = static_cast<uint8_t>(capability + 4);
    const uint16_t control = pci::read_config_u16(info.bus, info.slot, info.function, pmcsr);
    if ((control & 0x3u) == 0) {
        return;
    }
    console::printf("xhci: %x:%x.%u was in D%u, waking it\n", static_cast<unsigned>(info.bus),
        static_cast<unsigned>(info.slot), static_cast<unsigned>(info.function), static_cast<unsigned>(control & 0x3u));
    pci::write_config_u16(info.bus, info.slot, info.function, pmcsr, static_cast<uint16_t>(control & ~0x3u));
    usb::delay_ms(10);
}

// Intel 8 Series and later boot with their USB 2 and USB 3 ports routed to
// the EHCI controller; the firmware says which may move to xHCI, and this
// moves them (OpenBSD's xhci_pci_port_route).
void route_intel_ports(const pci::DeviceInfo& info) {
    if (info.vendor_id != kVendorIntel) {
        return;
    }
    pci::write_config_u32(info.bus, info.slot, info.function, kIntelUsb3Pssen,
        pci::read_config_u32(info.bus, info.slot, info.function, kIntelUsb3prm));
    pci::write_config_u32(info.bus, info.slot, info.function, kIntelXusb2pr,
        pci::read_config_u32(info.bus, info.slot, info.function, kIntelXusb2prm));
}

bool start_controller(const pci::DeviceInfo& info, Controller& controller) {
    controller = {};
    controller.pci = info;
    pci::BarInfo bar = {};
    if (!pci::bar_info(info, 0, bar) || bar.io_space || bar.base == 0) {
        console::write_line("xhci: controller without a memory BAR");
        return false;
    }
    void* base = nullptr;
    if (!vm::map_kernel_mmio(bar.base, static_cast<size_t>(bar.size), vm::kPageWriteThrough | vm::kPageCacheDisable,
            &base)) {
        console::write_line("xhci: cannot map the registers");
        return false;
    }
    uint16_t pci_command = pci::read_config_u16(info.bus, info.slot, info.function, kPciCommandOffset);
    pci_command = static_cast<uint16_t>(pci_command | kPciCommandMemory | kPciCommandBusMaster |
        kPciCommandInterruptDisable);
    pci::write_config_u16(info.bus, info.slot, info.function, kPciCommandOffset, pci_command);
    power_up(info);

    controller.cap = static_cast<volatile uint8_t*>(base);
    if (read32(controller.cap, kHccParams1) == 0xffffffffu) {
        console::printf("xhci: %x:%x.%u does not answer\n", static_cast<unsigned>(info.bus),
            static_cast<unsigned>(info.slot), static_cast<unsigned>(info.function));
        return false;
    }
    controller.op = controller.cap + (read32(controller.cap, kCapLength) & 0xff);
    controller.rt = controller.cap + (read32(controller.cap, kRtsOff) & ~0x1fu);
    controller.db = controller.cap + (read32(controller.cap, kDbOff) & ~0x3u);
    const uint32_t params1 = read32(controller.cap, kHcsParams1);
    const uint32_t hcc = read32(controller.cap, kHccParams1);
    controller.slots = static_cast<uint8_t>((params1 & 0xff) < kMaxSlots ? (params1 & 0xff) : kMaxSlots);
    controller.ports = static_cast<uint8_t>(params1 >> 24);
    if (controller.ports > kMaxPorts) {
        console::printf("xhci: %u ports, only the first %u are used\n", static_cast<unsigned>(controller.ports),
            static_cast<unsigned>(kMaxPorts));
        controller.ports = kMaxPorts;
    }
    controller.context_size = (hcc & kHccContextSize) != 0 ? 64 : 32;
    controller.ac64 = (hcc & kHccAc64) != 0;

    take_from_firmware(controller);
    if (!reset_controller(controller)) {
        return false;
    }
    if ((read32(controller.op, kPageSizeReg) & 1u) == 0) {
        console::write_line("xhci: the controller does not do 4 KiB pages");
        return false;
    }
    if (!set_up_memory(controller)) {
        console::write_line("xhci: out of memory for the controller");
        return false;
    }
    read_protocols(controller);
    write32(controller.op, kUsbCmd, kCmdRun);
    if (!wait_status(controller, kStsHalted, false, 100)) {
        console::write_line("xhci: the controller does not start");
        return false;
    }
    if (command(controller, 0, kTrbNoOp << 10) != kCodeSuccess) {
        console::write_line("xhci: no answer to a No Op command");
        return false;
    }
    route_intel_ports(info);
    // Ports without power (no Port Power Control, or the firmware left it
    // off) get it now; a device needs 20 ms after power to connect.
    bool powered = false;
    for (uint8_t port = 1; port <= controller.ports; ++port) {
        const uint32_t portsc = read_port(controller, port);
        if ((portsc & kPortPower) == 0) {
            write_port(controller, port, portsc, kPortPower);
            powered = true;
        }
    }
    if (powered) {
        usb::delay_ms(20);
    }
    console::printf("xhci: %x:%x.%u %x:%x, %u ports, %u slots, %u-byte contexts\n",
        static_cast<unsigned>(info.bus), static_cast<unsigned>(info.slot), static_cast<unsigned>(info.function),
        static_cast<unsigned>(info.vendor_id), static_cast<unsigned>(info.device_id),
        static_cast<unsigned>(controller.ports), static_cast<unsigned>(controller.slots),
        static_cast<unsigned>(controller.context_size));
    return true;
}

uint16_t default_max_packet(Speed speed) {
    switch (speed) {
    case Speed::low:
    case Speed::full: return 8;
    case Speed::high: return 64;
    default: return 512;
    }
}

// The endpoint interval, in 125 us frames as a power of two.
uint32_t interval_for(const Device& device, const usb::EndpointDescriptor& endpoint) {
    const uint8_t type = endpoint.attributes & 0x03;
    if (type != usb::kTransferInterrupt && type != usb::kTransferIsochronous) {
        return 0;
    }
    if (device.speed == Speed::high || device.speed == Speed::super || device.speed == Speed::super_plus ||
        type == usb::kTransferIsochronous) {
        const uint32_t interval = endpoint.interval == 0 ? 1 : endpoint.interval;
        return interval > 16 ? 15 : interval - 1;
    }
    // Low and full speed: milliseconds, so 8 frames each.
    uint32_t frames = static_cast<uint32_t>(endpoint.interval == 0 ? 1 : endpoint.interval) * 8;
    uint32_t exponent = 0;
    while ((2u << exponent) <= frames) {
        ++exponent;
    }
    if (exponent < 3) {
        exponent = 3;
    }
    return exponent > 10 ? 10 : exponent;
}

// Fills the slot context of the input context from the device.
void fill_slot_context(const Controller& controller, const Device& device, const memory::PageAllocation& input,
    uint32_t context_entries) {
    volatile uint32_t* slot = context(input, controller.context_size, 1);
    slot[0] = (device.route & 0xfffffu) | (static_cast<uint32_t>(device.speed) << 20) |
        (device.hub && device.hub_multi_tt ? (1u << 25) : 0u) | (device.hub ? (1u << 26) : 0u) |
        (context_entries << 27);
    slot[1] = (static_cast<uint32_t>(device.root_port) << 16) |
        (device.hub ? (static_cast<uint32_t>(device.hub_ports) << 24) : 0u);
    slot[2] = static_cast<uint32_t>(device.tt_slot) | (static_cast<uint32_t>(device.tt_port) << 8);
    slot[3] = 0;
}

uint32_t context_entries_of(const Controller& controller, const DeviceState& state) {
    const volatile uint32_t* slot = context(state.output, controller.context_size, 0);
    const uint32_t entries = slot[0] >> 27;
    return entries == 0 ? 1 : entries;
}

} // namespace

namespace usb::hc {

size_t initialize() {
    g_controller_count = 0;
    if (!pci::ready()) {
        return 0;
    }
    for (size_t index = 0; index < pci::device_count() && g_controller_count < kMaxControllers; ++index) {
        pci::DeviceInfo info = {};
        if (!pci::device_info(index, info) || info.class_code != kClassSerialBus || info.subclass != kSubclassUsb ||
            info.prog_if != kProgIfXhci) {
            continue;
        }
        if (start_controller(info, g_controllers[g_controller_count])) {
            ++g_controller_count;
        }
    }
    return g_controller_count;
}

size_t controller_count() {
    return g_controller_count;
}

size_t port_count(uint8_t controller) {
    return controller < g_controller_count ? g_controllers[controller].ports : 0;
}

bool port_connected(uint8_t controller, uint8_t port) {
    return (read_port(g_controllers[controller], port) & kPortConnected) != 0;
}

Speed reset_port(uint8_t index, uint8_t port) {
    Controller& controller = g_controllers[index];
    uint32_t portsc = read_port(controller, port);
    if ((portsc & kPortConnected) == 0) {
        return Speed::unknown;
    }
    if (controller.port_major[port] == 3) {
        // A USB 3 port trains its link by itself; a warm reset is for one
        // that did not.
        if ((portsc & kPortEnabled) == 0) {
            write_port(controller, port, portsc, kPortWarmReset);
            (void)wait_port(controller, port, kPortResetChange | kPortWarmResetChange, 500);
        }
    } else {
        write_port(controller, port, portsc, kPortReset);
        if (!wait_port(controller, port, kPortResetChange, 500)) {
            console::printf("xhci: port %u does not finish its reset\n", static_cast<unsigned>(port));
        }
        // Reset recovery (USB 2.0, 7.1.7.5).
        usb::delay_ms(10);
    }
    portsc = read_port(controller, port);
    write_port(controller, port, portsc, portsc & kPortChangeBits);
    if ((portsc & kPortEnabled) == 0 || (portsc & kPortConnected) == 0) {
        return Speed::unknown;
    }
    return static_cast<Speed>((portsc >> 10) & 0x0f);
}

uint64_t take_port_changes(uint8_t index, bool all) {
    Controller& controller = g_controllers[index];
    drain(controller);
    const uint64_t flagged = all ? ~0ull : controller.port_changes;
    controller.port_changes = 0;
    uint64_t connect_changes = 0;
    for (uint8_t port = 1; port <= controller.ports; ++port) {
        if ((flagged & (1ull << (port - 1))) == 0) {
            continue;
        }
        const uint32_t portsc = read_port(controller, port);
        // Only a connect change means a device came or went: the reset of a
        // port flags a change too, and must not set off another enumeration.
        if ((portsc & kPortConnectChange) != 0) {
            connect_changes |= 1ull << (port - 1);
        }
        if ((portsc & kPortChangeBits) != 0) {
            write_port(controller, port, portsc, portsc & kPortChangeBits);
        }
    }
    return connect_changes;
}

bool port_events_pending(uint8_t index) {
    return g_controllers[index].port_changes != 0;
}

bool enable_device(Device& device) {
    Controller& controller = controller_of(device);
    DeviceState& state = state_of(device);
    state = {};
    uint8_t slot = 0;
    const uint8_t code = command(controller, 0, kTrbEnableSlot << 10, &slot);
    if (code != kCodeSuccess || slot == 0 || slot > controller.slots) {
        console::printf("xhci: no slot for a device (code %u)\n", static_cast<unsigned>(code));
        return false;
    }
    device.slot = slot;
    if (!allocate_zeroed(controller, state.output) || !allocate_zeroed(controller, state.input) ||
        !ring_create(controller, state.endpoints[1].ring)) {
        disable_device(device);
        return false;
    }
    controller.slot_device[slot] = &device;
    static_cast<uint64_t*>(controller.dcbaa.virtual_address)[slot] = state.output.physical_address;

    Endpoint& zero = state.endpoints[1];
    zero.active = true;
    zero.address = 0;
    zero.max_packet = default_max_packet(device.speed);

    volatile uint32_t* control = context(state.input, controller.context_size, 0);
    control[0] = 0;
    control[1] = 0x3; // add the slot and endpoint 0
    fill_slot_context(controller, device, state.input, 1);
    volatile uint32_t* ep0 = context(state.input, controller.context_size, 2);
    ep0[0] = 0;
    ep0[1] = (3u << 1) | (kEpControl << 3) | (static_cast<uint32_t>(zero.max_packet) << 16);
    ep0[2] = static_cast<uint32_t>(zero.ring.physical) | 1u;
    ep0[3] = static_cast<uint32_t>(zero.ring.physical >> 32);
    ep0[4] = 8;
    const uint8_t addressed = command(controller, state.input.physical_address,
        (kTrbAddressDevice << 10) | (static_cast<uint32_t>(slot) << 24));
    if (addressed != kCodeSuccess) {
        console::printf("xhci: Address Device failed (code %u)\n", static_cast<unsigned>(addressed));
        disable_device(device);
        return false;
    }
    return true;
}

bool set_max_packet0(Device& device, uint16_t max_packet) {
    Controller& controller = controller_of(device);
    DeviceState& state = state_of(device);
    state.endpoints[1].max_packet = max_packet;
    volatile uint32_t* control = context(state.input, controller.context_size, 0);
    control[0] = 0;
    control[1] = 0x2;
    volatile uint32_t* ep0 = context(state.input, controller.context_size, 2);
    ep0[1] = (ep0[1] & 0x0000ffffu) | (static_cast<uint32_t>(max_packet) << 16);
    return command(controller, state.input.physical_address,
               (kTrbEvaluateContext << 10) | (static_cast<uint32_t>(device.slot) << 24)) == kCodeSuccess;
}

void disable_device(Device& device) {
    Controller& controller = controller_of(device);
    DeviceState& state = state_of(device);
    if (device.slot != 0) {
        (void)command(controller, 0, (kTrbDisableSlot << 10) | (static_cast<uint32_t>(device.slot) << 24));
        static_cast<uint64_t*>(controller.dcbaa.virtual_address)[device.slot] = 0;
        controller.slot_device[device.slot] = nullptr;
    }
    for (Endpoint& endpoint : state.endpoints) {
        release(endpoint.ring.page);
    }
    release(state.output);
    release(state.input);
    state = {};
    device.slot = 0;
}

bool set_hub(Device& device) {
    Controller& controller = controller_of(device);
    DeviceState& state = state_of(device);
    volatile uint32_t* control = context(state.input, controller.context_size, 0);
    control[0] = 0;
    control[1] = 0x1;
    fill_slot_context(controller, device, state.input, context_entries_of(controller, state));
    return command(controller, state.input.physical_address,
               (kTrbConfigureEndpoint << 10) | (static_cast<uint32_t>(device.slot) << 24)) == kCodeSuccess;
}

int control(Device& device, const SetupPacket& setup, void* data, uint32_t* actual) {
    Controller& controller = controller_of(device);
    if (setup.length > memory::kPageSize) {
        return status_error;
    }
    Endpoint& endpoint = state_of(device).endpoints[1];
    const bool in = (setup.request_type & kRequestIn) != 0;
    if (!in && setup.length != 0) {
        memcpy(controller.control_buffer.virtual_address, data, setup.length);
    }
    uint64_t packet = 0;
    memcpy(&packet, &setup, sizeof(packet));
    const uint32_t transfer_type = setup.length == 0 ? 0u : (in ? 3u : 2u);

    start_transfer(endpoint);
    endpoint.td[endpoint.td_count++] = ring_push(endpoint.ring, packet, 8,
        (kTrbSetup << 10) | kTrbImmediateData | (transfer_type << 16));
    if (setup.length != 0) {
        endpoint.data_trb = ring_push(endpoint.ring, controller.control_buffer.physical_address, setup.length,
            (kTrbData << 10) | kTrbShortPacketInterrupt | (in ? kTrbDirectionIn : 0u));
    }
    const bool status_in = setup.length == 0 || !in;
    endpoint.td[endpoint.td_count++] = ring_push(endpoint.ring, 0, 0,
        (kTrbStatus << 10) | kTrbInterruptOnCompletion | (status_in ? kTrbDirectionIn : 0u));
    ring_doorbell(controller, device.slot, 1);

    const bool completed = wait_transfer(controller, endpoint, kControlTimeoutMs);
    const int status = finish_transfer(controller, device, 1, completed);
    uint32_t moved = setup.length;
    if (endpoint.data_seen) {
        moved = endpoint.data_residual > setup.length ? 0u : setup.length - endpoint.data_residual;
    }
    if (status == status_ok && in && moved != 0) {
        memcpy(data, controller.control_buffer.virtual_address, moved);
    }
    if (actual != nullptr) {
        *actual = status == status_ok ? moved : 0u;
    }
    return status;
}

bool add_endpoint(Device& device, const EndpointDescriptor& descriptor, uint8_t max_burst) {
    Controller& controller = controller_of(device);
    DeviceState& state = state_of(device);
    const uint8_t index = endpoint_index(descriptor.address);
    const uint8_t type = descriptor.attributes & 0x03;
    const bool in = (descriptor.address & 0x80) != 0;
    uint32_t endpoint_type = 0;
    if (type == kTransferBulk) {
        endpoint_type = in ? kEpBulkIn : kEpBulkOut;
    } else if (type == kTransferInterrupt) {
        endpoint_type = in ? kEpInterruptIn : kEpInterruptOut;
    } else {
        return false;
    }
    Endpoint& endpoint = state.endpoints[index];
    if (endpoint.active) {
        return true;
    }
    endpoint = {};
    if (!ring_create(controller, endpoint.ring)) {
        return false;
    }
    if (type == kTransferInterrupt && in) {
        if (state.buffers_used >= kInterruptBuffers) {
            release(endpoint.ring.page);
            return false;
        }
        const size_t offset = kInterruptBufferOffset + state.buffers_used * kInterruptBufferSize;
        endpoint.buffer = static_cast<uint8_t*>(state.input.virtual_address) + offset;
        endpoint.buffer_physical = state.input.physical_address + offset;
        ++state.buffers_used;
    }
    endpoint.address = descriptor.address;
    endpoint.max_packet = descriptor.max_packet_size & 0x7ff;
    // High-speed interrupt endpoints carry extra transactions per frame in
    // bits 11-12 of wMaxPacketSize.
    uint32_t burst = max_burst;
    if (device.speed == Speed::high && type == kTransferInterrupt) {
        burst = (descriptor.max_packet_size >> 11) & 0x3;
    }

    uint32_t entries = context_entries_of(controller, state);
    if (index > entries) {
        entries = index;
    }
    volatile uint32_t* control = context(state.input, controller.context_size, 0);
    control[0] = 0;
    control[1] = 1u | (1u << index);
    fill_slot_context(controller, device, state.input, entries);
    volatile uint32_t* ep = context(state.input, controller.context_size, static_cast<size_t>(index) + 1);
    const uint32_t payload = static_cast<uint32_t>(endpoint.max_packet) * (burst + 1);
    ep[0] = interval_for(device, descriptor) << 16;
    ep[1] = (3u << 1) | (endpoint_type << 3) | (burst << 8) | (static_cast<uint32_t>(endpoint.max_packet) << 16);
    ep[2] = static_cast<uint32_t>(endpoint.ring.physical) | 1u;
    ep[3] = static_cast<uint32_t>(endpoint.ring.physical >> 32);
    ep[4] = type == kTransferInterrupt ? (payload & 0xffffu) | ((payload & 0xffffu) << 16) : 3072u;
    const uint8_t code = command(controller, state.input.physical_address,
        (kTrbConfigureEndpoint << 10) | (static_cast<uint32_t>(device.slot) << 24));
    // The next command that uses the input context must not add it again.
    for (size_t i = 0; i < 5; ++i) {
        ep[i] = 0;
    }
    if (code != kCodeSuccess) {
        console::printf("xhci: Configure Endpoint %x failed (code %u)\n",
            static_cast<unsigned>(descriptor.address), static_cast<unsigned>(code));
        release(endpoint.ring.page);
        endpoint = {};
        return false;
    }
    endpoint.active = true;
    return true;
}

int bulk(Device& device, uint8_t address, void* buffer, uint32_t length, uint32_t* actual, uint32_t timeout_ms) {
    Controller& controller = controller_of(device);
    const uint8_t index = endpoint_index(address);
    Endpoint& endpoint = state_of(device).endpoints[index];
    if (actual != nullptr) {
        *actual = 0;
    }
    if (!endpoint.active || length > kMaxBulkBytes) {
        return status_error;
    }
    const bool in = (address & 0x80) != 0;
    if (!in && length != 0) {
        memcpy(controller.bulk_buffer.virtual_address, buffer, length);
    }

    start_transfer(endpoint);
    // One TRB per piece that does not cross a 64 KiB boundary.
    uint64_t physical = controller.bulk_buffer.physical_address;
    uint32_t remaining = length;
    do {
        uint32_t piece = 0x10000u - static_cast<uint32_t>(physical & 0xffffu);
        if (piece > remaining) {
            piece = remaining;
        }
        const bool last = piece == remaining;
        const uint32_t max_packet = endpoint.max_packet == 0 ? 512 : endpoint.max_packet;
        uint32_t td_size = last ? 0 : (remaining - piece + max_packet - 1) / max_packet;
        if (td_size > 31) {
            td_size = 31;
        }
        const uint32_t control = (kTrbNormal << 10) | kTrbShortPacketInterrupt |
            (last ? kTrbInterruptOnCompletion : kTrbChain);
        endpoint.td_length[endpoint.td_count] = piece;
        endpoint.td[endpoint.td_count++] = ring_push(endpoint.ring, physical, piece | (td_size << 17), control);
        physical += piece;
        remaining -= piece;
    } while (remaining != 0 && endpoint.td_count < kMaxTdTrbs);
    ring_doorbell(controller, device.slot, index);

    const bool completed = wait_transfer(controller, endpoint, timeout_ms);
    const int status = finish_transfer(controller, device, index, completed);
    if (status != status_ok) {
        return status;
    }
    uint32_t moved = 0;
    for (size_t i = 0; i < endpoint.td_count; ++i) {
        if (endpoint.td[i] == endpoint.done_trb) {
            const uint32_t residual = endpoint.residual > endpoint.td_length[i] ? endpoint.td_length[i]
                                                                                : endpoint.residual;
            moved += endpoint.td_length[i] - residual;
            break;
        }
        moved += endpoint.td_length[i];
    }
    if (in && moved != 0) {
        memcpy(buffer, controller.bulk_buffer.virtual_address, moved);
    }
    if (actual != nullptr) {
        *actual = moved;
    }
    return status_ok;
}

bool reset_endpoint(Device& device, uint8_t address) {
    return recover_endpoint(controller_of(device), device, endpoint_index(address), true);
}

bool submit_interrupt(Device& device, uint8_t address, uint16_t length, InterruptCallback callback) {
    Controller& controller = controller_of(device);
    const uint8_t index = endpoint_index(address);
    Endpoint& endpoint = state_of(device).endpoints[index];
    if (!endpoint.active || endpoint.buffer == nullptr || endpoint.pending) {
        return false;
    }
    endpoint.callback = callback;
    endpoint.length = length > kInterruptBufferSize ? kInterruptBufferSize : length;
    endpoint.pending = true;
    endpoint.pending_trb = ring_push(endpoint.ring, endpoint.buffer_physical, endpoint.length,
        (kTrbNormal << 10) | kTrbShortPacketInterrupt | kTrbInterruptOnCompletion);
    ring_doorbell(controller, device.slot, index);
    return true;
}

void poll() {
    for (size_t index = 0; index < g_controller_count; ++index) {
        drain(g_controllers[index]);
    }
}

} // namespace usb::hc
