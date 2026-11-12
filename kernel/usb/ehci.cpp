// EHCI host controller driver: USB 2.0 high speed on every PC from about
// 2002 to 2012. Written from the Enhanced Host Controller Interface
// specification, revision 1.0, with OpenBSD's sys/dev/usb/ehci.c and
// sys/dev/pci/ehci_pci.c as reference (see
// docs/third-party/THIRD_PARTY_ADOPTION.md). Polled, like every driver here:
// interrupts stay off and a transfer is finished when its qTDs are no longer
// active. See docs/roadmaps/done/USB_ROADMAP.md.
//
// An EHCI root port only keeps high-speed devices: a low- or full-speed one
// is handed to the companion UHCI or OHCI controller by setting the port's
// owner bit (EHCI 1.0, 4.2.2). Low- and full-speed devices behind a
// high-speed hub stay here, reached through the hub's transaction translator
// with split transactions.
//
// The asynchronous schedule is the reclamation head alone; for each control
// or bulk transfer one queue head is programmed, linked after it, and
// unlinked with the async advance doorbell when the transfer ends. Each
// interrupt endpoint has a queue head of its own on the periodic schedule,
// polled in the first microframe of every frame, whatever its interval.

#include <stddef.h>
#include <stdint.h>

#include "arch/cpu.hpp"
#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/vmm.hpp"
#include "usb_internal.hpp"

namespace {

using usb::Device;
using usb::Speed;

// Capability registers.
constexpr uint32_t kCapLength = 0x00;
constexpr uint32_t kHcsParams = 0x04;
constexpr uint32_t kHccParams = 0x08;
constexpr uint32_t kHcsPortCountMask = 0xfu;
constexpr uint32_t kHcsPortPowerControl = 1u << 4;
constexpr uint32_t kHcc64Bit = 1u << 0;

// Operational registers.
constexpr uint32_t kUsbCmd = 0x00;
constexpr uint32_t kUsbSts = 0x04;
constexpr uint32_t kUsbIntr = 0x08;
constexpr uint32_t kSegment = 0x10;
constexpr uint32_t kPeriodicBase = 0x14;
constexpr uint32_t kAsyncAddress = 0x18;
constexpr uint32_t kConfigFlag = 0x40;
constexpr uint32_t kPortBase = 0x44;

constexpr uint32_t kCmdRun = 1u << 0;
constexpr uint32_t kCmdReset = 1u << 1;
constexpr uint32_t kCmdPeriodic = 1u << 4;
constexpr uint32_t kCmdAsync = 1u << 5;
constexpr uint32_t kCmdAsyncDoorbell = 1u << 6;
constexpr uint32_t kCmdThreshold8 = 8u << 16;

constexpr uint32_t kStsPortChange = 1u << 2;
constexpr uint32_t kStsAsyncAdvance = 1u << 5;
constexpr uint32_t kStsHalted = 1u << 12;
constexpr uint32_t kStsPeriodicRunning = 1u << 14;
constexpr uint32_t kStsAsyncRunning = 1u << 15;
constexpr uint32_t kStsClear = 0x3fu;

// PORTSC.
constexpr uint32_t kPortConnected = 1u << 0;
constexpr uint32_t kPortConnectChange = 1u << 1;
constexpr uint32_t kPortEnabled = 1u << 2;
constexpr uint32_t kPortEnableChange = 1u << 3;
constexpr uint32_t kPortOvercurrentChange = 1u << 5;
constexpr uint32_t kPortReset = 1u << 8;
constexpr uint32_t kPortLineMask = 3u << 10;
constexpr uint32_t kPortLineK = 1u << 10;
constexpr uint32_t kPortPower = 1u << 12;
constexpr uint32_t kPortOwner = 1u << 13;
constexpr uint32_t kPortWriteClear = kPortConnectChange | kPortEnableChange | kPortOvercurrentChange;
constexpr uint8_t kMaxPorts = 15;

// PCI configuration: the legacy support extended capability.
constexpr uint8_t kExtCapLegacy = 1;
constexpr uint32_t kLegacyBiosOwned = 1u << 16;
constexpr uint32_t kLegacyOsOwned = 1u << 24;

// Link pointers.
constexpr uint32_t kLinkTerminate = 1u << 0;
constexpr uint32_t kLinkQh = 1u << 1;

// Queue head, endpoint characteristics.
constexpr uint32_t kQhSpeedFull = 0u << 12;
constexpr uint32_t kQhSpeedLow = 1u << 12;
constexpr uint32_t kQhSpeedHigh = 2u << 12;
constexpr uint32_t kQhToggleFromQtd = 1u << 14;
constexpr uint32_t kQhReclamationHead = 1u << 15;
constexpr uint32_t kQhControlEndpoint = 1u << 27;
constexpr uint32_t kQhNakReload4 = 4u << 28;
// Endpoint capabilities.
constexpr uint32_t kQhMult1 = 1u << 30;
constexpr uint32_t kQhStartMicroframe0 = 0x01u;
constexpr uint32_t kQhCompleteMicroframes234 = 0x1cu << 8;

// qTD token.
constexpr uint32_t kQtdActive = 1u << 7;
constexpr uint32_t kQtdHalted = 1u << 6;
constexpr uint32_t kQtdBufferError = 1u << 5;
constexpr uint32_t kQtdBabble = 1u << 4;
constexpr uint32_t kQtdTransactionError = 1u << 3;
constexpr uint32_t kQtdPidOut = 0u << 8;
constexpr uint32_t kQtdPidIn = 1u << 8;
constexpr uint32_t kQtdPidSetup = 2u << 8;
constexpr uint32_t kQtdThreeErrors = 3u << 10;
constexpr uint32_t kQtdToggle = 1u << 31;
constexpr uint32_t kQtdLengthShift = 16;
constexpr uint32_t kQtdLengthMask = 0x7fffu;

// qTD and queue head as the controller sees them, with the 64-bit
// controllers' extra buffer words (EHCI 1.0, appendix B), which a 32-bit
// controller ignores.
struct Qtd {
    volatile uint32_t next;
    volatile uint32_t alternate;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t buffer_high[5];
};

struct Qh {
    volatile uint32_t horizontal;
    volatile uint32_t characteristics;
    volatile uint32_t capabilities;
    volatile uint32_t current;
    // The overlay: the qTD in progress.
    volatile uint32_t next;
    volatile uint32_t alternate;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t buffer_high[5];
};

constexpr size_t kQtdStride = 64;
constexpr size_t kQhStride = 128;
static_assert(sizeof(Qtd) <= kQtdStride && sizeof(Qh) <= kQhStride);

constexpr size_t kMaxControllers = 4;
constexpr size_t kInterruptSlots = 32;
constexpr size_t kInterruptBufferSize = 64;
// The controller's block: the reclamation head, the transfer queue head,
// the interrupt queue heads, their qTDs and their buffers.
constexpr size_t kHeadQhOffset = 0;
constexpr size_t kAsyncQhOffset = kQhStride;
constexpr size_t kInterruptQhOffset = 2 * kQhStride;
constexpr size_t kInterruptQtdOffset = 8192;
constexpr size_t kInterruptBufferOffset = 12288;
constexpr size_t kBlockPages = 4;
static_assert(kInterruptQhOffset + kInterruptSlots * kQhStride <= kInterruptQtdOffset);
static_assert(kInterruptQtdOffset + kInterruptSlots * kQtdStride <= kInterruptBufferOffset);
static_assert(kInterruptBufferOffset + kInterruptSlots * kInterruptBufferSize <= kBlockPages * memory::kPageSize);

// The qTDs of the transfer in flight, shared by every controller: the kernel
// runs one synchronous transfer at a time. A qTD carries up to five pages;
// a bulk transfer takes one per four pages of the bounce buffer.
constexpr size_t kPoolQtds = memory::kPageSize / kQtdStride;
constexpr size_t kBulkPages = usb::hc::kMaxBulkBytes / memory::kPageSize;
constexpr uint32_t kQtdBulkBytes = 4 * memory::kPageSize;
constexpr uint32_t kControlTimeoutMs = 1000;

struct InterruptSlot {
    bool used;
    bool pending;
    Device* device;
    uint32_t generation;
    uint8_t endpoint;
    uint16_t length;
    usb::hc::InterruptCallback callback;
};

struct Controller {
    pci::DeviceInfo pci;
    volatile uint8_t* cap;
    volatile uint8_t* op;
    uint8_t ports;
    memory::PageAllocation frames;
    memory::PageAllocation block;
    InterruptSlot slots[kInterruptSlots];
};

// Per device, indexed like the core's table. Endpoint n OUT is bit n of the
// toggles and entry n of max_packet; IN n is bit and entry 16 + n.
struct DeviceState {
    uint16_t max_packet[32];
    uint32_t toggles;
};

Controller g_controllers[kMaxControllers] = {};
size_t g_controller_count = 0;
DeviceState g_states[usb::kMaxDevices] = {};
memory::PageAllocation g_pool = {};
memory::PageAllocation g_control_buffer = {};
memory::PageAllocation g_bulk_buffer = {};

using usb::hc::now_ms;

uint32_t read32(volatile uint8_t* base, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(base + offset);
}

void write32(volatile uint8_t* base, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(base + offset) = value;
}

Controller& controller_of(const Device& device) {
    return g_controllers[usb::hc::local_index(device.controller)];
}

DeviceState& state_of(const Device& device) {
    return g_states[usb::device_index(device)];
}

size_t endpoint_key(uint8_t address) {
    return (address & 0x0fu) + ((address & 0x80u) != 0 ? 16u : 0u);
}

uint32_t physical32(const memory::PageAllocation& page, size_t offset) {
    return static_cast<uint32_t>(page.physical_address + offset);
}

template <typename T>
T& at(const memory::PageAllocation& page, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(page.virtual_address) + offset);
}

Qh& interrupt_qh(Controller& controller, size_t slot) {
    return at<Qh>(controller.block, kInterruptQhOffset + slot * kQhStride);
}

Qtd& pool_qtd(size_t index) {
    return at<Qtd>(g_pool, index * kQtdStride);
}

uint32_t pool_physical(size_t index) {
    return physical32(g_pool, index * kQtdStride);
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

// The periodic schedule is read anew every microframe: what was taken off
// a queue head is left alone once the frame in progress ends.
void wait_frame() {
    usb::delay_ms(2);
}

// An interrupt queue head nobody uses stays on the schedule, valid and idle:
// a start microframe (a zero start mask there is undefined, EHCI 1.0, 3.6.2)
// and no qTD.
void idle_interrupt_qh(Qh& qh) {
    qh.next = kLinkTerminate;
    qh.alternate = kLinkTerminate;
    qh.token = 0;
    qh.current = 0;
    qh.characteristics = kQhSpeedHigh;
    qh.capabilities = kQhMult1 | kQhStartMicroframe0;
}

uint32_t port_register(uint8_t port) {
    return kPortBase + static_cast<uint32_t>(port - 1) * 4u;
}

uint32_t read_port(Controller& controller, uint8_t port) {
    return read32(controller.op, port_register(port));
}

// The write-to-clear bits are left out unless asked for.
void write_port(Controller& controller, uint8_t port, uint32_t portsc, uint32_t set) {
    write32(controller.op, port_register(port), (portsc & ~kPortWriteClear) | set);
}

// Fills one qTD with `length` bytes at `physical`, which may cross up to
// four page boundaries.
void fill_qtd(Qtd& qtd, uint32_t pid, bool toggle, uint32_t physical, uint32_t length, uint32_t next,
    uint32_t alternate) {
    qtd.next = next;
    qtd.alternate = alternate;
    for (size_t i = 0; i < 5; ++i) {
        qtd.buffer[i] = 0;
        qtd.buffer_high[i] = 0;
    }
    if (length != 0) {
        qtd.buffer[0] = physical;
        uint32_t page = physical & ~static_cast<uint32_t>(memory::kPageSize - 1);
        for (size_t i = 1; i < 5; ++i) {
            page += static_cast<uint32_t>(memory::kPageSize);
            qtd.buffer[i] = page;
        }
    }
    arch::store_barrier();
    qtd.token = kQtdActive | pid | kQtdThreeErrors | (toggle ? kQtdToggle : 0u) |
        ((length & kQtdLengthMask) << kQtdLengthShift);
}

// Endpoint characteristics and capabilities of a queue head for one of the
// device's endpoints. A low- or full-speed device is reached through its
// hub's transaction translator.
uint32_t characteristics(const Device& device, uint8_t endpoint, uint16_t max_packet, bool control) {
    uint32_t value = static_cast<uint32_t>(device.slot & 0x7f) | (static_cast<uint32_t>(endpoint & 0x0f) << 8) |
        (static_cast<uint32_t>(max_packet & 0x7ff) << 16);
    if (device.speed == Speed::high) {
        value |= kQhSpeedHigh;
    } else {
        value |= device.speed == Speed::low ? kQhSpeedLow : kQhSpeedFull;
        if (control) {
            value |= kQhControlEndpoint;
        }
    }
    return value;
}

uint32_t capabilities(const Device& device, bool periodic) {
    uint32_t value = kQhMult1;
    if (device.speed != Speed::high) {
        value |= (static_cast<uint32_t>(device.tt_slot & 0x7f) << 16) |
            (static_cast<uint32_t>(device.tt_port & 0x7f) << 23);
        if (periodic) {
            value |= kQhStartMicroframe0 | kQhCompleteMicroframes234;
        }
    } else if (periodic) {
        value |= kQhStartMicroframe0;
    }
    return value;
}

// Takes the transfer queue head off the asynchronous schedule and waits
// until the controller has let go of it (EHCI 1.0, 4.8.2).
void unlink_async(Controller& controller) {
    Qh& head = at<Qh>(controller.block, kHeadQhOffset);
    head.horizontal = physical32(controller.block, kHeadQhOffset) | kLinkQh;
    arch::store_barrier();
    write32(controller.op, kUsbSts, kStsAsyncAdvance);
    write32(controller.op, kUsbCmd, read32(controller.op, kUsbCmd) | kCmdAsyncDoorbell);
    if (!wait_status(controller, kStsAsyncAdvance, true, 50)) {
        console::write_line("ehci: no async advance after a transfer");
    }
    write32(controller.op, kUsbSts, kStsAsyncAdvance);
}

struct RunResult {
    int status;
    uint32_t bytes;
    bool short_packet;
    uint32_t overlay_token;
};

// Runs the pool's first `count` qTDs on the transfer queue head.
RunResult run(Controller& controller, uint32_t endpoint_characteristics, uint32_t endpoint_capabilities, bool toggle,
    size_t count, const uint32_t* lengths, uint32_t timeout_ms) {
    RunResult result = {usb::status_timeout, 0, false, 0};
    Qh& qh = at<Qh>(controller.block, kAsyncQhOffset);
    qh.characteristics = endpoint_characteristics | kQhNakReload4;
    qh.capabilities = endpoint_capabilities;
    qh.current = 0;
    qh.next = pool_physical(0);
    qh.alternate = kLinkTerminate;
    for (size_t i = 0; i < 5; ++i) {
        qh.buffer[i] = 0;
        qh.buffer_high[i] = 0;
    }
    qh.token = toggle ? kQtdToggle : 0u;
    qh.horizontal = physical32(controller.block, kHeadQhOffset) | kLinkQh;
    arch::store_barrier();
    at<Qh>(controller.block, kHeadQhOffset).horizontal = physical32(controller.block, kAsyncQhOffset) | kLinkQh;

    const uint64_t deadline = now_ms() + timeout_ms;
    bool finished = false;
    while (!finished) {
        result.bytes = 0;
        result.short_packet = false;
        bool active = false;
        bool stopped = false;
        for (size_t i = 0; i < count; ++i) {
            const Qtd& qtd = pool_qtd(i);
            const uint32_t token = qtd.token;
            if ((token & kQtdActive) != 0) {
                active = true;
                break;
            }
            if ((token & kQtdHalted) != 0) {
                result.status = (token & (kQtdBabble | kQtdBufferError | kQtdTransactionError)) != 0
                    ? usb::status_error
                    : usb::status_stall;
                stopped = true;
                break;
            }
            const uint32_t remaining = (token >> kQtdLengthShift) & kQtdLengthMask;
            result.bytes += lengths[i] - (remaining > lengths[i] ? lengths[i] : remaining);
            if (remaining != 0) {
                result.short_packet = true;
                // Where the queue went from here: the alternate pointer.
                if ((qtd.alternate & kLinkTerminate) != 0) {
                    stopped = true;
                    result.status = usb::status_ok;
                    break;
                }
            }
        }
        if (stopped || !active) {
            if (!stopped) {
                result.status = usb::status_ok;
            }
            finished = true;
            break;
        }
        if (now_ms() > deadline) {
            break;
        }
        arch::cpu_relax();
    }
    unlink_async(controller);
    result.overlay_token = qh.token;
    return result;
}

// The firmware may be using the controller (legacy keyboard emulation): ask
// for it through USBLEGSUP and turn its SMIs off (ehci_pci.c, ehci_takecontroller).
void take_from_firmware(Controller& controller) {
    const pci::DeviceInfo& info = controller.pci;
    uint32_t offset = (read32(controller.cap, kHccParams) >> 8) & 0xffu;
    for (int guard = 0; offset >= 0x40 && guard < 16; ++guard) {
        const uint32_t value =
            pci::read_config_u32(info.bus, info.slot, info.function, static_cast<uint8_t>(offset));
        if (value == 0xffffffffu) {
            return;
        }
        if ((value & 0xff) == kExtCapLegacy) {
            if ((value & kLegacyBiosOwned) != 0) {
                pci::write_config_u32(info.bus, info.slot, info.function, static_cast<uint8_t>(offset),
                    value | kLegacyOsOwned);
                const uint64_t deadline = now_ms() + 5000;
                while ((pci::read_config_u32(info.bus, info.slot, info.function, static_cast<uint8_t>(offset)) &
                           kLegacyBiosOwned) != 0 &&
                    now_ms() < deadline) {
                    usb::delay_ms(1);
                }
                const uint32_t after =
                    pci::read_config_u32(info.bus, info.slot, info.function, static_cast<uint8_t>(offset));
                if ((after & kLegacyBiosOwned) != 0) {
                    console::write_line("ehci: the firmware did not release the controller");
                    pci::write_config_u32(info.bus, info.slot, info.function, static_cast<uint8_t>(offset),
                        (after & ~kLegacyBiosOwned) | kLegacyOsOwned);
                }
            }
            // USBLEGCTLSTS: every SMI enable off, every SMI status cleared.
            pci::write_config_u32(info.bus, info.slot, info.function, static_cast<uint8_t>(offset + 4), 0xe0000000u);
        }
        offset = (value >> 8) & 0xffu;
    }
}

bool start_controller(const pci::DeviceInfo& info, Controller& controller) {
    controller = {};
    controller.pci = info;
    pci::BarInfo bar = {};
    if (!pci::bar_info(info, 0, bar) || bar.io_space || bar.base == 0) {
        console::write_line("ehci: controller without a memory BAR");
        return false;
    }
    usb::hc::prepare_pci(info, "ehci", false);
    void* base = nullptr;
    const size_t size = bar.size < memory::kPageSize ? memory::kPageSize : static_cast<size_t>(bar.size);
    if (!vm::map_kernel_mmio(bar.base, size, vm::kPageWriteThrough | vm::kPageCacheDisable, &base)) {
        console::write_line("ehci: cannot map the registers");
        return false;
    }
    controller.cap = static_cast<volatile uint8_t*>(base);
    if (read32(controller.cap, kHcsParams) == 0xffffffffu) {
        console::printf("ehci: %x:%x.%u does not answer\n", static_cast<unsigned>(info.bus),
            static_cast<unsigned>(info.slot), static_cast<unsigned>(info.function));
        return false;
    }
    controller.op = controller.cap + (read32(controller.cap, kCapLength) & 0xff);
    const uint32_t hcs = read32(controller.cap, kHcsParams);
    controller.ports = static_cast<uint8_t>(hcs & kHcsPortCountMask);
    if (controller.ports == 0 || controller.ports > kMaxPorts) {
        console::write_line("ehci: no root ports");
        return false;
    }

    take_from_firmware(controller);
    write32(controller.op, kUsbIntr, 0);
    write32(controller.op, kUsbCmd, read32(controller.op, kUsbCmd) & ~kCmdRun);
    if (!wait_status(controller, kStsHalted, true, 20)) {
        console::write_line("ehci: the controller does not halt");
        return false;
    }
    write32(controller.op, kUsbCmd, kCmdReset);
    const uint64_t deadline = now_ms() + 250;
    while ((read32(controller.op, kUsbCmd) & kCmdReset) != 0) {
        if (now_ms() > deadline) {
            console::write_line("ehci: the controller does not leave reset");
            return false;
        }
        arch::cpu_relax();
    }

    if (!usb::hc::allocate_dma32(1, controller.frames) || !usb::hc::allocate_dma32(kBlockPages, controller.block)) {
        console::write_line("ehci: out of memory below 4 GiB");
        usb::hc::release_dma(controller.frames);
        return false;
    }
    // The reclamation head, alone in the asynchronous ring.
    Qh& head = at<Qh>(controller.block, kHeadQhOffset);
    head.horizontal = physical32(controller.block, kHeadQhOffset) | kLinkQh;
    head.characteristics = kQhReclamationHead | kQhSpeedHigh;
    head.capabilities = kQhMult1;
    head.next = kLinkTerminate;
    head.alternate = kLinkTerminate;
    head.token = kQtdHalted;
    Qh& transfer = at<Qh>(controller.block, kAsyncQhOffset);
    transfer.next = kLinkTerminate;
    transfer.alternate = kLinkTerminate;
    // The interrupt queue heads, chained, idle until an endpoint takes one.
    for (size_t slot = 0; slot < kInterruptSlots; ++slot) {
        Qh& qh = interrupt_qh(controller, slot);
        qh.horizontal = slot + 1 < kInterruptSlots
            ? physical32(controller.block, kInterruptQhOffset + (slot + 1) * kQhStride) | kLinkQh
            : kLinkTerminate;
        idle_interrupt_qh(qh);
    }
    auto* frames = static_cast<volatile uint32_t*>(controller.frames.virtual_address);
    for (size_t frame = 0; frame < 1024; ++frame) {
        frames[frame] = physical32(controller.block, kInterruptQhOffset) | kLinkQh;
    }
    arch::store_barrier();
    if ((read32(controller.cap, kHccParams) & kHcc64Bit) != 0) {
        write32(controller.op, kSegment, 0);
    }
    write32(controller.op, kPeriodicBase, physical32(controller.frames, 0));
    write32(controller.op, kAsyncAddress, physical32(controller.block, kHeadQhOffset));
    write32(controller.op, kUsbSts, kStsClear);
    write32(controller.op, kUsbCmd, kCmdThreshold8 | kCmdRun | kCmdPeriodic | kCmdAsync);
    if (!wait_status(controller, kStsHalted, false, 20) ||
        !wait_status(controller, kStsAsyncRunning | kStsPeriodicRunning, true, 20)) {
        console::write_line("ehci: the controller does not start");
        return false;
    }
    // Every port is now this controller's (EHCI 1.0, 4.1).
    write32(controller.op, kConfigFlag, 1);
    usb::delay_ms(5);
    if ((hcs & kHcsPortPowerControl) != 0) {
        for (uint8_t port = 1; port <= controller.ports; ++port) {
            const uint32_t portsc = read_port(controller, port);
            if ((portsc & kPortPower) == 0) {
                write_port(controller, port, portsc, kPortPower);
            }
        }
        usb::delay_ms(20);
    }
    console::printf("ehci: %x:%x.%u %x:%x, %u ports, %u companions\n", static_cast<unsigned>(info.bus),
        static_cast<unsigned>(info.slot), static_cast<unsigned>(info.function), static_cast<unsigned>(info.vendor_id),
        static_cast<unsigned>(info.device_id), static_cast<unsigned>(controller.ports),
        static_cast<unsigned>((hcs >> 12) & 0xfu));
    return true;
}

InterruptSlot* find_slot(Controller& controller, const Device& device, uint8_t endpoint, size_t* index) {
    for (size_t slot = 0; slot < kInterruptSlots; ++slot) {
        InterruptSlot& entry = controller.slots[slot];
        if (entry.used && entry.device == &device && entry.generation == device.generation &&
            entry.endpoint == endpoint) {
            if (index != nullptr) {
                *index = slot;
            }
            return &entry;
        }
    }
    return nullptr;
}

// Hands the port to the companion controller, for a device that is not
// high speed.
void release_port(Controller& controller, uint8_t port, const char* why) {
    const uint32_t portsc = read_port(controller, port);
    write_port(controller, port, portsc, kPortOwner);
    console::printf("ehci: port %u: %s device, handed to the companion\n", static_cast<unsigned>(port), why);
}

} // namespace

namespace usb::ehci {

using hc::InterruptCallback;
using hc::kMaxBulkBytes;

size_t port_count(uint8_t local) {
    return local < g_controller_count ? g_controllers[local].ports : 0;
}

bool port_connected(uint8_t local, uint8_t port) {
    const uint32_t portsc = read_port(g_controllers[local], port);
    return (portsc & kPortConnected) != 0 && (portsc & kPortOwner) == 0;
}

Speed reset_port(uint8_t local, uint8_t port) {
    Controller& controller = g_controllers[local];
    uint32_t portsc = read_port(controller, port);
    if ((portsc & kPortConnected) == 0 || (portsc & kPortOwner) != 0) {
        return Speed::unknown;
    }
    // A low-speed device pulls the line to the K state before any reset.
    if ((portsc & kPortLineMask) == kPortLineK) {
        release_port(controller, port, "low-speed");
        return Speed::unknown;
    }
    write_port(controller, port, portsc & ~kPortEnabled, kPortReset);
    usb::delay_ms(50);
    write_port(controller, port, read_port(controller, port) & ~kPortReset, 0);
    const uint64_t deadline = now_ms() + 10;
    while ((read_port(controller, port) & kPortReset) != 0) {
        if (now_ms() > deadline) {
            console::printf("ehci: port %u does not finish its reset\n", static_cast<unsigned>(port));
            return Speed::unknown;
        }
        arch::cpu_relax();
    }
    usb::delay_ms(2);
    portsc = read_port(controller, port);
    if ((portsc & kPortConnected) == 0) {
        return Speed::unknown;
    }
    // Only a high-speed device's chirp leaves the port enabled.
    if ((portsc & kPortEnabled) == 0) {
        release_port(controller, port, "full-speed");
        return Speed::unknown;
    }
    write_port(controller, port, portsc, portsc & kPortWriteClear);
    // Reset recovery (USB 2.0, 7.1.7.5).
    usb::delay_ms(10);
    return Speed::high;
}

uint64_t take_port_changes(uint8_t local, bool) {
    Controller& controller = g_controllers[local];
    write32(controller.op, kUsbSts, kStsPortChange);
    uint64_t changes = 0;
    for (uint8_t port = 1; port <= controller.ports; ++port) {
        const uint32_t portsc = read_port(controller, port);
        if ((portsc & kPortConnectChange) != 0) {
            changes |= 1ull << (port - 1);
        }
        if ((portsc & kPortWriteClear) != 0) {
            write_port(controller, port, portsc, portsc & kPortWriteClear);
        }
    }
    return changes;
}

// USBSTS keeps the port change bit with interrupts off.
bool port_events_pending(uint8_t local) {
    return (read32(g_controllers[local].op, kUsbSts) & kStsPortChange) != 0;
}

bool enable_device(Device& device) {
    DeviceState& state = state_of(device);
    state = {};
    const uint16_t max_packet = device.speed == Speed::high ? 64 : 8;
    state.max_packet[0] = max_packet;
    state.max_packet[16] = max_packet;
    return hc::assign_address(device);
}

bool set_max_packet0(Device& device, uint16_t max_packet) {
    DeviceState& state = state_of(device);
    state.max_packet[0] = max_packet;
    state.max_packet[16] = max_packet;
    return true;
}

void disable_device(Device& device) {
    Controller& controller = controller_of(device);
    bool removed = false;
    for (size_t slot = 0; slot < kInterruptSlots; ++slot) {
        InterruptSlot& entry = controller.slots[slot];
        if (entry.used && entry.device == &device) {
            Qh& qh = interrupt_qh(controller, slot);
            qh.next = kLinkTerminate;
            qh.token = 0;
            entry = {};
            removed = true;
        }
    }
    if (removed) {
        // A transaction in progress may write the overlay back once more.
        wait_frame();
        for (size_t slot = 0; slot < kInterruptSlots; ++slot) {
            if (!controller.slots[slot].used) {
                idle_interrupt_qh(interrupt_qh(controller, slot));
            }
        }
    }
    hc::release_address(device);
    state_of(device) = {};
}

// The queue heads carry the hub's address and port for the devices behind
// it, from the core's Device; the controller needs nothing else.
bool set_hub(Device&) {
    return true;
}

int control(Device& device, const SetupPacket& setup, void* data, uint32_t* actual) {
    Controller& controller = controller_of(device);
    if (actual != nullptr) {
        *actual = 0;
    }
    // The setup packet and the data share one page.
    if (setup.length > memory::kPageSize - sizeof(setup)) {
        return status_error;
    }
    const bool in = (setup.request_type & kRequestIn) != 0;
    auto* buffer = static_cast<uint8_t*>(g_control_buffer.virtual_address);
    memcpy(buffer, &setup, sizeof(setup));
    if (!in && setup.length != 0) {
        memcpy(buffer + 8, data, setup.length);
    }
    const uint32_t physical = physical32(g_control_buffer, 0);
    const uint16_t max_packet = state_of(device).max_packet[0] == 0 ? 8 : state_of(device).max_packet[0];

    // SETUP, DATA, STATUS. A short data stage goes on to the status stage
    // through its alternate pointer.
    uint32_t lengths[3] = {8, setup.length, 0};
    size_t count = 0;
    const size_t status_index = setup.length != 0 ? 2 : 1;
    fill_qtd(pool_qtd(count), kQtdPidSetup, false, physical, 8, pool_physical(count + 1), kLinkTerminate);
    ++count;
    if (setup.length != 0) {
        fill_qtd(pool_qtd(count), in ? kQtdPidIn : kQtdPidOut, true, physical + 8, setup.length,
            pool_physical(count + 1), pool_physical(status_index));
        ++count;
    }
    const bool status_in = setup.length == 0 || !in;
    fill_qtd(pool_qtd(count), status_in ? kQtdPidIn : kQtdPidOut, true, 0, 0, kLinkTerminate, kLinkTerminate);
    lengths[count] = 0;
    ++count;

    const RunResult result = run(controller, characteristics(device, 0, max_packet, true) | kQhToggleFromQtd,
        capabilities(device, false), false, count, lengths, kControlTimeoutMs);
    if (result.status != status_ok) {
        return result.status;
    }
    const uint32_t moved = result.bytes >= 8 ? result.bytes - 8 : 0u;
    if (in && moved != 0) {
        memcpy(data, buffer + 8, moved);
    }
    if (actual != nullptr) {
        *actual = moved;
    }
    return status_ok;
}

bool add_endpoint(Device& device, const EndpointDescriptor& descriptor, uint8_t) {
    Controller& controller = controller_of(device);
    DeviceState& state = state_of(device);
    const uint8_t type = descriptor.attributes & 0x03;
    if (type != kTransferBulk && type != kTransferInterrupt) {
        return false;
    }
    const size_t key = endpoint_key(descriptor.address);
    state.max_packet[key] = static_cast<uint16_t>(descriptor.max_packet_size & 0x7ff);
    if (state.max_packet[key] == 0) {
        state.max_packet[key] = 8;
    }
    state.toggles &= ~(1u << key);
    if (type != kTransferInterrupt || (descriptor.address & 0x80) == 0 ||
        find_slot(controller, device, descriptor.address, nullptr) != nullptr) {
        return true;
    }
    for (size_t slot = 0; slot < kInterruptSlots; ++slot) {
        InterruptSlot& entry = controller.slots[slot];
        if (entry.used) {
            continue;
        }
        entry = {};
        entry.used = true;
        entry.device = &device;
        entry.generation = device.generation;
        entry.endpoint = descriptor.address;
        // The queue head is idle, with no qTD to run: what it says about the
        // endpoint is read only once submit_interrupt gives it one.
        Qh& qh = interrupt_qh(controller, slot);
        qh.characteristics = characteristics(device, descriptor.address, state.max_packet[key], false);
        qh.capabilities = capabilities(device, true);
        return true;
    }
    console::write_line("ehci: no room for another interrupt endpoint");
    return false;
}

int bulk(Device& device, uint8_t address, void* buffer, uint32_t length, uint32_t* actual, uint32_t timeout_ms) {
    Controller& controller = controller_of(device);
    DeviceState& state = state_of(device);
    if (actual != nullptr) {
        *actual = 0;
    }
    const size_t key = endpoint_key(address);
    const uint16_t max_packet = state.max_packet[key];
    if (max_packet == 0 || length > kMaxBulkBytes) {
        return status_error;
    }
    const bool in = (address & 0x80) != 0;
    auto* bounce = static_cast<uint8_t*>(g_bulk_buffer.virtual_address);
    if (!in && length != 0) {
        memcpy(bounce, buffer, length);
    }
    // 16 KiB per qTD, a multiple of every packet size. A short packet ends
    // the transfer: every alternate pointer terminates.
    uint32_t lengths[kBulkPages + 1] = {};
    size_t count = 0;
    uint32_t offset = 0;
    do {
        const uint32_t piece = length - offset > kQtdBulkBytes ? kQtdBulkBytes : length - offset;
        const bool last = offset + piece >= length;
        fill_qtd(pool_qtd(count), in ? kQtdPidIn : kQtdPidOut, false, physical32(g_bulk_buffer, offset), piece,
            last ? kLinkTerminate : pool_physical(count + 1), kLinkTerminate);
        lengths[count] = piece;
        ++count;
        offset += piece;
    } while (offset < length && count < kPoolQtds);

    const RunResult result = run(controller, characteristics(device, address, max_packet, false),
        capabilities(device, false), (state.toggles & (1u << key)) != 0, count, lengths, timeout_ms);
    if ((result.overlay_token & kQtdToggle) != 0) {
        state.toggles |= 1u << key;
    } else {
        state.toggles &= ~(1u << key);
    }
    if (result.status != status_ok) {
        return result.status;
    }
    if (in && result.bytes != 0) {
        memcpy(buffer, bounce, result.bytes);
    }
    if (actual != nullptr) {
        *actual = result.bytes;
    }
    return status_ok;
}

bool reset_endpoint(Device& device, uint8_t address) {
    Controller& controller = controller_of(device);
    state_of(device).toggles &= ~(1u << endpoint_key(address));
    size_t index = 0;
    InterruptSlot* entry = find_slot(controller, device, address, &index);
    if (entry != nullptr && !entry->pending) {
        // Clears the halt and the toggle; the queue is empty.
        Qh& qh = interrupt_qh(controller, index);
        qh.next = kLinkTerminate;
        qh.token = 0;
    }
    return true;
}

bool submit_interrupt(Device& device, uint8_t address, uint16_t length, InterruptCallback callback) {
    Controller& controller = controller_of(device);
    size_t index = 0;
    InterruptSlot* entry = find_slot(controller, device, address, &index);
    if (entry == nullptr || entry->pending) {
        return false;
    }
    const size_t key = endpoint_key(address);
    uint16_t size = length;
    if (size > state_of(device).max_packet[key]) {
        size = state_of(device).max_packet[key];
    }
    if (size > kInterruptBufferSize) {
        size = kInterruptBufferSize;
    }
    entry->callback = callback;
    entry->length = size;
    entry->pending = true;
    Qtd& qtd = at<Qtd>(controller.block, kInterruptQtdOffset + index * kQtdStride);
    fill_qtd(qtd, kQtdPidIn, false, physical32(controller.block, kInterruptBufferOffset + index * kInterruptBufferSize),
        size, kLinkTerminate, kLinkTerminate);
    arch::store_barrier();
    // The queue head is idle: its overlay takes the qTD on the next visit.
    interrupt_qh(controller, index).next = physical32(controller.block, kInterruptQtdOffset + index * kQtdStride);
    return true;
}

void poll(uint8_t local) {
    Controller& controller = g_controllers[local];
    for (size_t slot = 0; slot < kInterruptSlots; ++slot) {
        InterruptSlot& entry = controller.slots[slot];
        if (!entry.used || !entry.pending) {
            continue;
        }
        const Qtd& qtd = at<Qtd>(controller.block, kInterruptQtdOffset + slot * kQtdStride);
        const uint32_t token = qtd.token;
        if ((token & kQtdActive) != 0) {
            continue;
        }
        entry.pending = false;
        int result = status_ok;
        uint32_t moved = 0;
        if ((token & kQtdHalted) != 0) {
            result = (token & (kQtdBabble | kQtdBufferError | kQtdTransactionError)) != 0 ? status_error
                                                                                          : status_stall;
        } else {
            const uint32_t remaining = (token >> kQtdLengthShift) & kQtdLengthMask;
            moved = remaining > entry.length ? 0u : entry.length - remaining;
        }
        if (entry.callback != nullptr) {
            entry.callback(*entry.device, entry.endpoint, result,
                &at<uint8_t>(controller.block, kInterruptBufferOffset + slot * kInterruptBufferSize), moved);
        }
    }
}

const hc::ControllerOps kOps = {
    "ehci",
    &port_count,
    &port_connected,
    &reset_port,
    &take_port_changes,
    &port_events_pending,
    &enable_device,
    &set_max_packet0,
    &disable_device,
    &set_hub,
    &control,
    &add_endpoint,
    &bulk,
    &reset_endpoint,
    &submit_interrupt,
    &poll,
};

size_t start() {
    g_controller_count = 0;
    for (size_t index = 0; index < pci::device_count() && g_controller_count < kMaxControllers; ++index) {
        pci::DeviceInfo info = {};
        if (!pci::device_info(index, info) || info.class_code != hc::kClassSerialBus ||
            info.subclass != hc::kSubclassUsb || info.prog_if != hc::kProgIfEhci) {
            continue;
        }
        if (g_pool.page_count == 0 &&
            (!hc::allocate_dma32(1, g_pool) || !hc::allocate_dma32(1, g_control_buffer) ||
                !hc::allocate_dma32(kBulkPages, g_bulk_buffer))) {
            console::write_line("ehci: out of memory below 4 GiB");
            return g_controller_count;
        }
        if (start_controller(info, g_controllers[g_controller_count]) &&
            hc::add_controller(kOps, static_cast<uint8_t>(g_controller_count))) {
            ++g_controller_count;
        }
    }
    return g_controller_count;
}

} // namespace usb::ehci
