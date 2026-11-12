// OHCI host controller driver: the USB 1.1 controller of AMD, ATI, NVIDIA,
// SiS and ALi chipsets, and the low- and full-speed companion of their EHCI.
// Written from the OpenHCI specification, release 1.0a, with OpenBSD's
// sys/dev/usb/ohci.c and sys/dev/pci/ohci_pci.c as reference (see
// docs/third-party/THIRD_PARTY_ADOPTION.md). Polled, like every driver here:
// interrupts stay off and a transfer is finished when its endpoint
// descriptor's queue is empty or halted. See docs/roadmaps/done/USB_ROADMAP.md.
//
// One endpoint descriptor on the control list and one on the bulk list carry
// the transfer in flight; they are reprogrammed for each transfer while
// skipped. Each interrupt endpoint has an ED of its own on the periodic list,
// which every slot of the HCCA's interrupt table points to: it is polled
// every frame, whatever its interval.

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

// Registers.
constexpr uint32_t kRevision = 0x00;
constexpr uint32_t kControl = 0x04;
constexpr uint32_t kCommandStatus = 0x08;
constexpr uint32_t kInterruptStatus = 0x0c;
constexpr uint32_t kInterruptDisable = 0x14;
constexpr uint32_t kHcca = 0x18;
constexpr uint32_t kControlHead = 0x20;
constexpr uint32_t kControlCurrent = 0x24;
constexpr uint32_t kBulkHead = 0x28;
constexpr uint32_t kBulkCurrent = 0x2c;
constexpr uint32_t kFrameInterval = 0x34;
constexpr uint32_t kFrameNumber = 0x3c;
constexpr uint32_t kPeriodicStart = 0x40;
constexpr uint32_t kLowSpeedThreshold = 0x44;
constexpr uint32_t kRootDescriptorA = 0x48;
constexpr uint32_t kRootStatus = 0x50;
constexpr uint32_t kPortBase = 0x54;

// HcControl.
constexpr uint32_t kControlRatio4 = 3u << 0;
constexpr uint32_t kControlPeriodic = 1u << 2;
constexpr uint32_t kControlList = 1u << 4;
constexpr uint32_t kControlBulk = 1u << 5;
constexpr uint32_t kControlStateMask = 3u << 6;
constexpr uint32_t kStateReset = 0u << 6;
constexpr uint32_t kStateResume = 1u << 6;
constexpr uint32_t kStateOperational = 2u << 6;
constexpr uint32_t kControlInterruptRouting = 1u << 8;
constexpr uint32_t kControlRemoteWakeConnected = 1u << 9;

// HcCommandStatus.
constexpr uint32_t kCommandReset = 1u << 0;
constexpr uint32_t kCommandControlFilled = 1u << 1;
constexpr uint32_t kCommandBulkFilled = 1u << 2;
constexpr uint32_t kCommandOwnershipChange = 1u << 3;

// HcInterruptStatus.
constexpr uint32_t kInterruptWriteDoneHead = 1u << 1;
constexpr uint32_t kInterruptRootHubChange = 1u << 6;
constexpr uint32_t kInterruptAll = 0xc000007fu;

// HcRhDescriptorA and HcRhStatus.
constexpr uint32_t kRootPortCountMask = 0xffu;
constexpr uint32_t kRootNoPowerSwitching = 1u << 9;
constexpr uint32_t kRootSetGlobalPower = 1u << 16;

// HcRhPortStatus. Writing 1 acts and writing 0 does nothing; bit 0 written
// disables the port and bit 9 written turns its power off.
constexpr uint32_t kPortConnected = 1u << 0;
constexpr uint32_t kPortEnabled = 1u << 1;
constexpr uint32_t kPortSetReset = 1u << 4;
constexpr uint32_t kPortSetPower = 1u << 8;
constexpr uint32_t kPortLowSpeed = 1u << 9;
constexpr uint32_t kPortConnectChange = 1u << 16;
constexpr uint32_t kPortResetChange = 1u << 20;
constexpr uint32_t kPortChanges = 0x001f0000u;
constexpr uint8_t kMaxPorts = 15;

// Endpoint descriptor, dword 0.
constexpr uint32_t kEdLowSpeed = 1u << 13;
constexpr uint32_t kEdSkip = 1u << 14;
// Dword 2 (HeadP): halted and the toggle carry.
constexpr uint32_t kEdHalted = 1u << 0;
constexpr uint32_t kEdToggleCarry = 1u << 1;
constexpr uint32_t kEdPointerMask = ~0xfu;

// General transfer descriptor, dword 0.
constexpr uint32_t kTdRounding = 1u << 18;
constexpr uint32_t kTdSetup = 0u << 19;
constexpr uint32_t kTdOut = 1u << 19;
constexpr uint32_t kTdIn = 2u << 19;
constexpr uint32_t kTdNoInterrupt = 7u << 21;
constexpr uint32_t kTdData0 = 2u << 24;
constexpr uint32_t kTdData1 = 3u << 24;
constexpr uint32_t kTdNotAccessed = 15u << 28;

// Condition codes.
constexpr uint32_t kCodeNoError = 0;
constexpr uint32_t kCodeStall = 4;
constexpr uint32_t kCodeDataUnderrun = 9;
constexpr uint32_t kCodeNotAccessed = 14;

struct Ed {
    volatile uint32_t control;
    volatile uint32_t tail;
    volatile uint32_t head;
    volatile uint32_t next;
};
static_assert(sizeof(Ed) == 16);

struct Td {
    volatile uint32_t control;
    volatile uint32_t current;
    volatile uint32_t next;
    volatile uint32_t end;
};
static_assert(sizeof(Td) == 16);

constexpr size_t kMaxControllers = 8;
constexpr size_t kInterruptSlots = 32;
constexpr size_t kInterruptBufferSize = 64;
// The controller page: the HCCA (256-byte aligned), the control and bulk
// EDs, the interrupt EDs, their two TDs each, their buffers.
constexpr size_t kControlEdOffset = 256;
constexpr size_t kBulkEdOffset = 272;
constexpr size_t kInterruptEdOffset = 288;
constexpr size_t kInterruptTdOffset = 1024;
constexpr size_t kInterruptBufferOffset = 2048;
static_assert(kInterruptEdOffset + kInterruptSlots * sizeof(Ed) <= kInterruptTdOffset);
static_assert(kInterruptTdOffset + kInterruptSlots * 2 * sizeof(Td) <= kInterruptBufferOffset);
static_assert(kInterruptBufferOffset + kInterruptSlots * kInterruptBufferSize <= memory::kPageSize);

// The TDs of the transfer in flight, shared by every controller: the kernel
// runs one synchronous transfer at a time. A bulk transfer takes one TD per
// page of the bounce buffer, plus the dummy TD every queue ends in.
constexpr size_t kBulkPages = usb::hc::kMaxBulkBytes / memory::kPageSize;
constexpr size_t kPoolTds = memory::kPageSize / sizeof(Td);
static_assert(kBulkPages + 1 <= kPoolTds);
constexpr uint32_t kControlTimeoutMs = 1000;

struct InterruptSlot {
    bool used;
    bool pending;
    Device* device;
    uint32_t generation;
    uint8_t endpoint;
    uint16_t length;
    // Which of the slot's two TDs is the dummy at the queue's tail.
    uint8_t tail;
    usb::hc::InterruptCallback callback;
};

struct Controller {
    pci::DeviceInfo pci;
    volatile uint8_t* registers;
    uint8_t ports;
    memory::PageAllocation page;
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

uint32_t read32(const Controller& controller, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(controller.registers + offset);
}

void write32(const Controller& controller, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(controller.registers + offset) = value;
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

Ed& interrupt_ed(Controller& controller, size_t slot) {
    return at<Ed>(controller.page, kInterruptEdOffset + slot * sizeof(Ed));
}

size_t interrupt_td_offset(size_t slot, size_t which) {
    return kInterruptTdOffset + (slot * 2 + which) * sizeof(Td);
}

Td* pool() {
    return static_cast<Td*>(g_pool.virtual_address);
}

uint32_t pool_physical(size_t index) {
    return physical32(g_pool, index * sizeof(Td));
}

// The controller may still be looking at an ED just skipped until the frame
// it is in ends (OpenHCI 1.0a, 5.2.7.1.2).
void wait_frame(const Controller& controller) {
    const uint32_t frame = read32(controller, kFrameNumber) & 0xffffu;
    const uint64_t deadline = now_ms() + 3;
    while ((read32(controller, kFrameNumber) & 0xffffu) == frame && now_ms() < deadline) {
        arch::cpu_relax();
    }
}

uint32_t port_register(uint8_t port) {
    return kPortBase + static_cast<uint32_t>(port - 1) * 4u;
}

uint32_t ed_control(const Device& device, uint8_t endpoint, uint16_t max_packet, bool skip) {
    return static_cast<uint32_t>(device.slot & 0x7f) | (static_cast<uint32_t>(endpoint & 0x0f) << 7) |
        (device.speed == Speed::low ? kEdLowSpeed : 0u) | (skip ? kEdSkip : 0u) |
        (static_cast<uint32_t>(max_packet & 0x7ff) << 16);
}

void fill_td(Td& td, uint32_t control, uint32_t physical, uint32_t length, uint32_t next) {
    td.control = control | kTdNoInterrupt | kTdNotAccessed;
    td.current = length == 0 ? 0u : physical;
    td.end = length == 0 ? 0u : physical + length - 1;
    td.next = next;
}

struct RunResult {
    int status;
    uint32_t bytes;
    bool short_packet;
};

// Runs the pool's first `count` TDs, the last of which is the dummy, on
// `ed` (the control or the bulk one): programs it while skipped, waits for
// its queue to empty or halt, skips it again.
RunResult run(Controller& controller, Ed& ed, uint32_t ed_word, uint32_t toggle, size_t count,
    const uint32_t* lengths, uint32_t filled, uint32_t timeout_ms, uint32_t* toggle_out) {
    RunResult result = {usb::status_timeout, 0, false};
    ed.tail = pool_physical(count - 1);
    ed.head = pool_physical(0) | toggle;
    arch::store_barrier();
    ed.control = ed_word & ~kEdSkip;
    arch::store_barrier();
    write32(controller, kCommandStatus, filled);

    const uint64_t deadline = now_ms() + timeout_ms;
    bool finished = false;
    while (!finished) {
        const uint32_t head = ed.head;
        if ((head & kEdHalted) != 0 || (head & kEdPointerMask) == (ed.tail & kEdPointerMask)) {
            finished = true;
            break;
        }
        if (now_ms() > deadline) {
            break;
        }
        arch::cpu_relax();
    }
    ed.control = ed_word | kEdSkip;
    wait_frame(controller);
    const uint32_t head = ed.head;
    if (toggle_out != nullptr) {
        *toggle_out = head & kEdToggleCarry;
    }
    if (!finished) {
        ed.head = ed.tail | (head & kEdToggleCarry);
        return result;
    }

    // What each TD did, in order: one that was never reached ends the walk.
    result.status = usb::status_ok;
    for (size_t i = 0; i + 1 < count; ++i) {
        const Td& td = pool()[i];
        const uint32_t code = td.control >> 28;
        if (code >= kCodeNotAccessed) {
            break;
        }
        const uint32_t start = td.end == 0 ? 0u : td.end + 1 - lengths[i];
        const uint32_t moved = td.current == 0 ? lengths[i] : td.current - start;
        if (code == kCodeNoError || code == kCodeDataUnderrun) {
            // A short packet ends a bulk transfer (the TDs after it were
            // never reached), and a control data stage, which the status
            // stage still follows.
            result.bytes += moved;
            if (moved < lengths[i]) {
                result.short_packet = true;
            }
            continue;
        }
        result.status = code == kCodeStall ? usb::status_stall : usb::status_error;
        break;
    }
    // A halted queue (an error, or a short packet without rounding) is
    // emptied and restarted for the next transfer.
    ed.head = ed.tail | (head & kEdToggleCarry);
    return result;
}

void release_from_firmware(Controller& controller) {
    const uint32_t control = read32(controller, kControl);
    if ((control & kControlInterruptRouting) != 0) {
        // An SMM driver owns it (the firmware's legacy keyboard emulation):
        // ask for it (OpenHCI 1.0a, 5.1.1.3.3).
        write32(controller, kCommandStatus, kCommandOwnershipChange);
        const uint64_t deadline = now_ms() + 500;
        while ((read32(controller, kControl) & kControlInterruptRouting) != 0 && now_ms() < deadline) {
            arch::cpu_relax();
        }
        if ((read32(controller, kControl) & kControlInterruptRouting) != 0) {
            console::write_line("ohci: the firmware did not release the controller");
        }
    } else if ((control & kControlStateMask) != kStateReset) {
        // A BIOS driver ran it: resume, if it was not operational.
        if ((control & kControlStateMask) != kStateOperational) {
            write32(controller, kControl, (control & ~kControlStateMask) | kStateResume);
            usb::delay_ms(20);
        }
    } else {
        // Cold: let the bus reset settle.
        usb::delay_ms(50);
    }
}

bool start_controller(const pci::DeviceInfo& info, Controller& controller) {
    controller = {};
    controller.pci = info;
    pci::BarInfo bar = {};
    if (!pci::bar_info(info, 0, bar) || bar.io_space || bar.base == 0) {
        console::write_line("ohci: controller without a memory BAR");
        return false;
    }
    usb::hc::prepare_pci(info, "ohci", false);
    void* base = nullptr;
    const size_t size = bar.size < memory::kPageSize ? memory::kPageSize : static_cast<size_t>(bar.size);
    if (!vm::map_kernel_mmio(bar.base, size, vm::kPageWriteThrough | vm::kPageCacheDisable, &base)) {
        console::write_line("ohci: cannot map the registers");
        return false;
    }
    controller.registers = static_cast<volatile uint8_t*>(base);
    const uint32_t revision = read32(controller, kRevision);
    if (revision == 0xffffffffu) {
        console::printf("ohci: %x:%x.%u does not answer\n", static_cast<unsigned>(info.bus),
            static_cast<unsigned>(info.slot), static_cast<unsigned>(info.function));
        return false;
    }

    release_from_firmware(controller);
    write32(controller, kInterruptDisable, kInterruptAll);
    uint32_t interval = read32(controller, kFrameInterval) & 0x3fffu;
    if (interval == 0) {
        interval = 11999;
    }
    write32(controller, kCommandStatus, kCommandReset);
    const uint64_t deadline = now_ms() + 30;
    while ((read32(controller, kCommandStatus) & kCommandReset) != 0) {
        if (now_ms() > deadline) {
            console::write_line("ohci: the controller does not leave reset");
            return false;
        }
        arch::cpu_relax();
    }
    // The controller is now suspended, and has 2 ms to be made operational.
    if (!usb::hc::allocate_dma32(1, controller.page)) {
        console::write_line("ohci: out of memory below 4 GiB");
        return false;
    }
    at<Ed>(controller.page, kControlEdOffset).control = kEdSkip;
    at<Ed>(controller.page, kBulkEdOffset).control = kEdSkip;
    for (size_t slot = 0; slot < kInterruptSlots; ++slot) {
        Ed& ed = interrupt_ed(controller, slot);
        ed.control = kEdSkip;
        const uint32_t dummy = physical32(controller.page, interrupt_td_offset(slot, 0));
        ed.tail = dummy;
        ed.head = dummy;
        ed.next = slot + 1 < kInterruptSlots ? physical32(controller.page, kInterruptEdOffset + (slot + 1) * sizeof(Ed))
                                             : 0u;
    }
    auto* table = &at<volatile uint32_t>(controller.page, 0);
    for (size_t entry = 0; entry < 32; ++entry) {
        table[entry] = physical32(controller.page, kInterruptEdOffset);
    }
    arch::store_barrier();
    write32(controller, kHcca, physical32(controller.page, 0));
    write32(controller, kControlHead, physical32(controller.page, kControlEdOffset));
    write32(controller, kControlCurrent, 0);
    write32(controller, kBulkHead, physical32(controller.page, kBulkEdOffset));
    write32(controller, kBulkCurrent, 0);
    write32(controller, kInterruptStatus, kInterruptAll);
    const uint32_t largest_packet = ((interval - 210) * 6) / 7;
    const uint32_t toggle = (read32(controller, kFrameInterval) & 0x80000000u) ^ 0x80000000u;
    write32(controller, kFrameInterval, interval | (largest_packet << 16) | toggle);
    write32(controller, kPeriodicStart, (interval * 9) / 10);
    write32(controller, kLowSpeedThreshold, 0x628);
    write32(controller, kControl,
        (read32(controller, kControl) & kControlRemoteWakeConnected) | kControlRatio4 | kControlPeriodic |
            kControlList | kControlBulk | kStateOperational);

    // Power the ports: globally, and each one, whichever the root hub does
    // (some boards say per port and wire it globally). Then wait the root
    // hub's power-on-to-power-good time, 2 ms a unit.
    const uint32_t descriptor = read32(controller, kRootDescriptorA);
    controller.ports = static_cast<uint8_t>(descriptor & kRootPortCountMask);
    if (controller.ports > kMaxPorts) {
        controller.ports = kMaxPorts;
    }
    if (controller.ports == 0) {
        console::write_line("ohci: no root ports");
        return false;
    }
    if ((descriptor & kRootNoPowerSwitching) == 0) {
        write32(controller, kRootStatus, kRootSetGlobalPower);
        for (uint8_t port = 1; port <= controller.ports; ++port) {
            write32(controller, port_register(port), kPortSetPower);
        }
        uint32_t wait = (descriptor >> 24) * 2u;
        if (wait < 20) {
            wait = 20;
        }
        usb::delay_ms(wait);
    }
    console::printf("ohci: %x:%x.%u %x:%x, revision %x, %u ports\n", static_cast<unsigned>(info.bus),
        static_cast<unsigned>(info.slot), static_cast<unsigned>(info.function), static_cast<unsigned>(info.vendor_id),
        static_cast<unsigned>(info.device_id), static_cast<unsigned>(revision & 0xffu),
        static_cast<unsigned>(controller.ports));
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

} // namespace

namespace usb::ohci {

using hc::InterruptCallback;
using hc::kMaxBulkBytes;

size_t port_count(uint8_t local) {
    return local < g_controller_count ? g_controllers[local].ports : 0;
}

bool port_connected(uint8_t local, uint8_t port) {
    return (read32(g_controllers[local], port_register(port)) & kPortConnected) != 0;
}

Speed reset_port(uint8_t local, uint8_t port) {
    Controller& controller = g_controllers[local];
    const uint32_t reg = port_register(port);
    if ((read32(controller, reg) & kPortConnected) == 0) {
        return Speed::unknown;
    }
    write32(controller, reg, kPortSetReset);
    const uint64_t deadline = now_ms() + 100;
    while ((read32(controller, reg) & kPortResetChange) == 0) {
        if (now_ms() > deadline) {
            console::printf("ohci: port %u does not finish its reset\n", static_cast<unsigned>(port));
            break;
        }
        arch::cpu_relax();
    }
    const uint32_t portsc = read32(controller, reg);
    write32(controller, reg, portsc & kPortChanges);
    if ((portsc & kPortEnabled) == 0 || (portsc & kPortConnected) == 0) {
        return Speed::unknown;
    }
    // Reset recovery (USB 2.0, 7.1.7.5).
    usb::delay_ms(10);
    return (portsc & kPortLowSpeed) != 0 ? Speed::low : Speed::full;
}

uint64_t take_port_changes(uint8_t local, bool) {
    Controller& controller = g_controllers[local];
    write32(controller, kInterruptStatus, kInterruptRootHubChange);
    uint64_t changes = 0;
    for (uint8_t port = 1; port <= controller.ports; ++port) {
        const uint32_t portsc = read32(controller, port_register(port));
        if ((portsc & kPortConnectChange) != 0) {
            changes |= 1ull << (port - 1);
        }
        if ((portsc & kPortChanges) != 0) {
            write32(controller, port_register(port), portsc & kPortChanges);
        }
    }
    return changes;
}

// The root hub status change bit says whether any port changed; it is set
// even with interrupts off.
bool port_events_pending(uint8_t local) {
    return (read32(g_controllers[local], kInterruptStatus) & kInterruptRootHubChange) != 0;
}

bool enable_device(Device& device) {
    DeviceState& state = state_of(device);
    state = {};
    state.max_packet[0] = 8;
    state.max_packet[16] = 8;
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
            interrupt_ed(controller, slot).control = kEdSkip;
            entry = {};
            removed = true;
        }
    }
    if (removed) {
        wait_frame(controller);
    }
    hc::release_address(device);
    state_of(device) = {};
}

// A full-speed hub needs nothing from this controller: a low-speed device
// behind it is reached with the ED's speed bit, and the controller sends the
// PRE packet itself.
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

    // SETUP, DATA (rounding on: a short IN data stage is fine, and the
    // controller goes on to the status stage), STATUS, the dummy.
    uint32_t lengths[4] = {8, setup.length, 0, 0};
    size_t count = 0;
    fill_td(pool()[count], kTdSetup | kTdData0, physical, 8, pool_physical(count + 1));
    ++count;
    if (setup.length != 0) {
        fill_td(pool()[count], (in ? kTdIn : kTdOut) | kTdData1 | kTdRounding, physical + 8, setup.length,
            pool_physical(count + 1));
        lengths[count] = setup.length;
        ++count;
    }
    const bool status_in = setup.length == 0 || !in;
    fill_td(pool()[count], (status_in ? kTdIn : kTdOut) | kTdData1, 0, 0, pool_physical(count + 1));
    lengths[count] = 0;
    ++count;
    fill_td(pool()[count], 0, 0, 0, 0);
    ++count;

    Ed& ed = at<Ed>(controller.page, kControlEdOffset);
    const RunResult result = run(controller, ed, ed_control(device, 0, max_packet, true), 0, count, lengths,
        kCommandControlFilled, kControlTimeoutMs, nullptr);
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
        // The ED was skipped since its last user left, a frame ago at least.
        Ed& ed = interrupt_ed(controller, slot);
        const uint32_t dummy = physical32(controller.page, interrupt_td_offset(slot, 0));
        ed.tail = dummy;
        ed.head = dummy;
        arch::store_barrier();
        ed.control = ed_control(device, descriptor.address, state.max_packet[key], false) | (2u << 11);
        return true;
    }
    console::write_line("ohci: no room for another interrupt endpoint");
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
    // One TD per page. Rounding only on the last: a short packet earlier
    // halts the queue with a data underrun, which ends the transfer.
    uint32_t lengths[kBulkPages + 1] = {};
    size_t count = 0;
    uint32_t offset = 0;
    do {
        const uint32_t piece = length - offset > memory::kPageSize ? static_cast<uint32_t>(memory::kPageSize)
                                                                   : length - offset;
        const bool last = offset + piece >= length;
        fill_td(pool()[count], (in ? kTdIn : kTdOut) | (last ? kTdRounding : 0u),
            physical32(g_bulk_buffer, offset), piece, pool_physical(count + 1));
        lengths[count] = piece;
        ++count;
        offset += piece;
    } while (offset < length);
    fill_td(pool()[count], 0, 0, 0, 0);
    ++count;

    Ed& ed = at<Ed>(controller.page, kBulkEdOffset);
    uint32_t toggle = (state.toggles & (1u << key)) != 0 ? kEdToggleCarry : 0u;
    const RunResult result = run(controller, ed, ed_control(device, address, max_packet, true) | ((in ? 2u : 1u) << 11),
        toggle, count, lengths, kCommandBulkFilled, timeout_ms, &toggle);
    if (toggle != 0) {
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
        // Clears the halt and the toggle carry; the queue is empty.
        Ed& ed = interrupt_ed(controller, index);
        ed.head = ed.tail & kEdPointerMask;
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
    // The dummy at the tail becomes the TD, and the other one the new dummy.
    const size_t use = entry->tail;
    const size_t next = use ^ 1u;
    Td& dummy = at<Td>(controller.page, interrupt_td_offset(index, next));
    fill_td(dummy, 0, 0, 0, 0);
    Td& td = at<Td>(controller.page, interrupt_td_offset(index, use));
    fill_td(td, kTdIn | kTdRounding, physical32(controller.page, kInterruptBufferOffset + index * kInterruptBufferSize),
        size, physical32(controller.page, interrupt_td_offset(index, next)));
    entry->tail = static_cast<uint8_t>(next);
    arch::store_barrier();
    interrupt_ed(controller, index).tail = physical32(controller.page, interrupt_td_offset(index, next));
    return true;
}

void poll(uint8_t local) {
    Controller& controller = g_controllers[local];
    // The done queue is never read; let the controller keep writing it.
    if ((read32(controller, kInterruptStatus) & kInterruptWriteDoneHead) != 0) {
        write32(controller, kInterruptStatus, kInterruptWriteDoneHead);
    }
    for (size_t slot = 0; slot < kInterruptSlots; ++slot) {
        InterruptSlot& entry = controller.slots[slot];
        if (!entry.used || !entry.pending) {
            continue;
        }
        Ed& ed = interrupt_ed(controller, slot);
        const uint32_t head = ed.head;
        if ((head & kEdHalted) == 0 && (head & kEdPointerMask) != (ed.tail & kEdPointerMask)) {
            continue;
        }
        entry.pending = false;
        const Td& td = at<Td>(controller.page, interrupt_td_offset(slot, entry.tail ^ 1u));
        const uint32_t code = td.control >> 28;
        int result = status_ok;
        uint32_t moved = 0;
        if (code == kCodeNoError || code == kCodeDataUnderrun) {
            const uint32_t start = physical32(controller.page, kInterruptBufferOffset + slot * kInterruptBufferSize);
            moved = td.current == 0 ? entry.length : td.current - start;
        } else {
            result = code == kCodeStall ? status_stall : status_error;
        }
        if ((head & kEdHalted) != 0 && result == status_ok) {
            // Not expected with rounding on; restart the queue all the same.
            ed.head = (ed.tail & kEdPointerMask) | (head & kEdToggleCarry);
        }
        if (entry.callback != nullptr) {
            entry.callback(*entry.device, entry.endpoint, result,
                &at<uint8_t>(controller.page, kInterruptBufferOffset + slot * kInterruptBufferSize), moved);
        }
    }
}

const hc::ControllerOps kOps = {
    "ohci",
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
            info.subclass != hc::kSubclassUsb || info.prog_if != hc::kProgIfOhci) {
            continue;
        }
        if (g_pool.page_count == 0 &&
            (!hc::allocate_dma32(1, g_pool) || !hc::allocate_dma32(1, g_control_buffer) ||
                !hc::allocate_dma32(kBulkPages, g_bulk_buffer))) {
            console::write_line("ohci: out of memory below 4 GiB");
            return g_controller_count;
        }
        if (start_controller(info, g_controllers[g_controller_count]) &&
            hc::add_controller(kOps, static_cast<uint8_t>(g_controller_count))) {
            ++g_controller_count;
        }
    }
    return g_controller_count;
}

} // namespace usb::ohci
