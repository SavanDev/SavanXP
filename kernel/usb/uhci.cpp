// UHCI host controller driver: the USB 1.1 controller of Intel and VIA
// chipsets, and the low- and full-speed companion of their EHCI. Written from
// the Universal Host Controller Interface Design Guide, revision 1.1, with
// OpenBSD's sys/dev/usb/uhci.c and sys/dev/pci/uhci_pci.c as reference (see
// docs/third-party/THIRD_PARTY_ADOPTION.md). Polled, like every driver here:
// interrupts stay off and a transfer is finished when its TDs are no longer
// active. See docs/roadmaps/done/USB_ROADMAP.md.
//
// The schedule is fixed: every entry of the frame list points to the first
// of kInterruptSlots queue heads, one per interrupt endpoint, chained to the
// one queue head that carries the control or bulk transfer in flight. An
// interrupt endpoint is polled every frame, whatever its interval: a HID
// device answers NAK when it has nothing to say, which costs nothing here.

#include <stddef.h>
#include <stdint.h>

#include "arch/cpu.hpp"
#include "arch/x86/io.hpp"
#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "usb_internal.hpp"

namespace {

using namespace arch::x86::port_io;
using usb::Device;
using usb::Speed;

// I/O registers.
constexpr uint16_t kCommand = 0x00;
constexpr uint16_t kStatus = 0x02;
constexpr uint16_t kInterruptEnable = 0x04;
constexpr uint16_t kFrameNumber = 0x06;
constexpr uint16_t kFrameListBase = 0x08;
constexpr uint16_t kStartOfFrame = 0x0c;
constexpr uint16_t kPortBase = 0x10;

constexpr uint16_t kCommandRun = 1u << 0;
constexpr uint16_t kCommandHostReset = 1u << 1;
constexpr uint16_t kCommandGlobalReset = 1u << 2;
constexpr uint16_t kCommandConfigured = 1u << 6;
constexpr uint16_t kCommandMaxPacket64 = 1u << 7;
constexpr uint16_t kStatusHalted = 1u << 5;
constexpr uint16_t kStatusClear = 0x003f;

// PORTSC.
constexpr uint16_t kPortConnected = 1u << 0;
constexpr uint16_t kPortConnectChange = 1u << 1;
constexpr uint16_t kPortEnabled = 1u << 2;
constexpr uint16_t kPortEnableChange = 1u << 3;
constexpr uint16_t kPortAlwaysOne = 1u << 7;
constexpr uint16_t kPortLowSpeed = 1u << 8;
constexpr uint16_t kPortReset = 1u << 9;
constexpr uint16_t kPortWriteClear = kPortConnectChange | kPortEnableChange;
// The design guide gives two ports; some controllers have more, and a port
// that is not there reads without bit 7 (OpenBSD and Linux probe the same way).
constexpr uint8_t kMaxPorts = 8;

// PCI configuration: LEGSUP. Writing 0x8f00 clears the SMI status bits and
// turns off the firmware's legacy keyboard and mouse emulation and the
// controller's PCI interrupt (uhci_pci.c, UHCI_LEGSUP_ALL_OFF).
constexpr uint8_t kPciLegacySupport = 0xc0;
constexpr uint16_t kLegacyOff = 0x8f00;

// Link pointers.
constexpr uint32_t kLinkTerminate = 1u << 0;
constexpr uint32_t kLinkQueueHead = 1u << 1;
constexpr uint32_t kLinkDepthFirst = 1u << 2;

// TD control and status.
constexpr uint32_t kTdLengthMask = 0x7ffu;
constexpr uint32_t kTdBitstuff = 1u << 17;
constexpr uint32_t kTdCrcTimeout = 1u << 18;
constexpr uint32_t kTdBabble = 1u << 20;
constexpr uint32_t kTdBufferError = 1u << 21;
constexpr uint32_t kTdStalled = 1u << 22;
constexpr uint32_t kTdActive = 1u << 23;
constexpr uint32_t kTdLowSpeed = 1u << 26;
constexpr uint32_t kTdThreeErrors = 3u << 27;
constexpr uint32_t kTdShortPacketDetect = 1u << 29;
constexpr uint32_t kTdErrors = kTdBitstuff | kTdCrcTimeout | kTdBabble | kTdBufferError | kTdStalled;

// TD token.
constexpr uint32_t kPidSetup = 0x2d;
constexpr uint32_t kPidIn = 0x69;
constexpr uint32_t kPidOut = 0xe1;
constexpr uint32_t kTokenToggle = 1u << 19;

struct Td {
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;
};
static_assert(sizeof(Td) == 16);

struct Qh {
    volatile uint32_t head;
    volatile uint32_t element;
    uint32_t unused[2];
};
static_assert(sizeof(Qh) == 16);

constexpr size_t kMaxControllers = 8;
constexpr size_t kInterruptSlots = 32;
constexpr size_t kInterruptBufferSize = 64;
// The queue page: the control/bulk queue head, then the interrupt queue
// heads, their TDs and their buffers.
constexpr size_t kInterruptQhOffset = sizeof(Qh);
constexpr size_t kInterruptTdOffset = 1024;
constexpr size_t kInterruptBufferOffset = 2048;
static_assert(kInterruptQhOffset + kInterruptSlots * sizeof(Qh) <= kInterruptTdOffset);
static_assert(kInterruptTdOffset + kInterruptSlots * sizeof(Td) <= kInterruptBufferOffset);
static_assert(kInterruptBufferOffset + kInterruptSlots * kInterruptBufferSize <= memory::kPageSize);

// The TDs of the transfer in flight, shared by every controller: the kernel
// runs one synchronous transfer at a time. 64 KiB in 64-byte packets.
constexpr size_t kPoolPages = 5;
constexpr size_t kPoolTds = kPoolPages * memory::kPageSize / sizeof(Td);
constexpr size_t kBulkPages = usb::hc::kMaxBulkBytes / memory::kPageSize;
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
    uint16_t io;
    uint8_t ports;
    memory::PageAllocation frames;
    memory::PageAllocation queues;
    InterruptSlot slots[kInterruptSlots];
    uint64_t next_port_check;
    bool port_change;
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

Qh& async_qh(Controller& controller) {
    return *static_cast<Qh*>(controller.queues.virtual_address);
}

Qh& interrupt_qh(Controller& controller, size_t slot) {
    return *reinterpret_cast<Qh*>(static_cast<uint8_t*>(controller.queues.virtual_address) + kInterruptQhOffset +
        slot * sizeof(Qh));
}

Td& interrupt_td(Controller& controller, size_t slot) {
    return *reinterpret_cast<Td*>(static_cast<uint8_t*>(controller.queues.virtual_address) + kInterruptTdOffset +
        slot * sizeof(Td));
}

uint8_t* interrupt_buffer(Controller& controller, size_t slot) {
    return static_cast<uint8_t*>(controller.queues.virtual_address) + kInterruptBufferOffset +
        slot * kInterruptBufferSize;
}

Td* pool() {
    return static_cast<Td*>(g_pool.virtual_address);
}

uint32_t pool_physical(size_t index) {
    return physical32(g_pool, index * sizeof(Td));
}

// The controller may still be looking at TDs taken off the schedule until
// the frame it is in ends.
void wait_frame(Controller& controller) {
    const uint16_t frame = in16(static_cast<uint16_t>(controller.io + kFrameNumber));
    const uint64_t deadline = now_ms() + 3;
    while (in16(static_cast<uint16_t>(controller.io + kFrameNumber)) == frame && now_ms() < deadline) {
        arch::cpu_relax();
    }
}

uint16_t port_register(const Controller& controller, uint8_t port) {
    return static_cast<uint16_t>(controller.io + kPortBase + (port - 1) * 2);
}

uint16_t read_port(const Controller& controller, uint8_t port) {
    return in16(port_register(controller, port));
}

// The write-to-clear bits are left out unless asked for; PE is carried back.
void write_port(const Controller& controller, uint8_t port, uint16_t portsc, uint16_t set) {
    out16(port_register(controller, port), static_cast<uint16_t>((portsc & kPortEnabled) | set));
}

// Encodes a TD's length field: n - 1, with 0x7ff for no data.
uint32_t encode_length(uint32_t length) {
    return (length - 1) & kTdLengthMask;
}

uint32_t decode_length(uint32_t field) {
    return (field + 1) & kTdLengthMask;
}

uint32_t td_status(const Device& device, bool short_packet_detect) {
    return kTdActive | kTdThreeErrors | (device.speed == Speed::low ? kTdLowSpeed : 0u) |
        (short_packet_detect ? kTdShortPacketDetect : 0u);
}

uint32_t td_token(const Device& device, uint8_t endpoint, uint32_t pid, uint32_t length, bool toggle) {
    return pid | (static_cast<uint32_t>(device.slot) << 8) | (static_cast<uint32_t>(endpoint & 0x0f) << 15) |
        (toggle ? kTokenToggle : 0u) | (encode_length(length) << 21);
}

// Queues one stage's packets after the TDs already in the pool. Returns the
// new TD count, or 0 if the pool is full.
size_t queue_packets(size_t count, const Device& device, uint8_t endpoint, uint32_t pid, uint32_t physical,
    uint32_t length, uint16_t max_packet, bool& toggle, bool short_packet_detect) {
    uint32_t offset = 0;
    do {
        if (count >= kPoolTds) {
            return 0;
        }
        const uint32_t piece = length - offset > max_packet ? max_packet : length - offset;
        Td& td = pool()[count];
        td.link = kLinkTerminate;
        td.buffer = piece == 0 ? 0u : physical + offset;
        td.token = td_token(device, endpoint, pid, piece, toggle);
        td.status = td_status(device, short_packet_detect && pid == kPidIn);
        if (count != 0) {
            pool()[count - 1].link = pool_physical(count) | kLinkDepthFirst;
        }
        toggle = !toggle;
        offset += piece;
        ++count;
    } while (offset < length);
    return count;
}

struct RunResult {
    int status;
    // TDs that completed without error, the short one included.
    size_t completed;
    uint32_t bytes;
    bool short_packet;
};

// Puts the pool's first `count` TDs on the control/bulk queue head and waits
// until they are done, one of them fails or ends short, or time runs out.
RunResult run(Controller& controller, size_t count, uint32_t timeout_ms) {
    RunResult result = {usb::status_timeout, 0, 0, false};
    Qh& qh = async_qh(controller);
    arch::store_barrier();
    qh.element = pool_physical(0);
    const uint64_t deadline = now_ms() + timeout_ms;
    bool finished = false;
    while (!finished) {
        result.completed = 0;
        result.bytes = 0;
        bool active = false;
        for (size_t i = 0; i < count; ++i) {
            const Td& td = pool()[i];
            const uint32_t status = td.status;
            if ((status & kTdActive) != 0) {
                active = true;
                break;
            }
            if ((status & kTdErrors) != 0) {
                result.status = (status & kTdStalled) != 0 ? usb::status_stall : usb::status_error;
                finished = true;
                break;
            }
            const uint32_t moved = decode_length(status);
            result.bytes += moved;
            ++result.completed;
            if (moved < decode_length(td.token >> 21)) {
                result.short_packet = true;
                result.status = usb::status_ok;
                finished = true;
                break;
            }
        }
        // Every TD done: finished once the controller has also moved the
        // queue head past the last one. It writes a TD's status before it
        // advances the element pointer, and a new transfer put on the queue
        // head in between would be overwritten.
        if (!active && !finished && (qh.element & kLinkTerminate) != 0) {
            result.status = usb::status_ok;
            finished = true;
        }
        if (!finished && now_ms() > deadline) {
            result.status = usb::status_timeout;
            break;
        }
        if (!finished) {
            arch::cpu_relax();
        }
    }
    if (result.status != usb::status_ok || result.short_packet) {
        // The queue head still points into the TDs: take them off, let the
        // controller finish the frame it is in, and take them off again in
        // case it wrote the pointer back meanwhile.
        qh.element = kLinkTerminate;
        wait_frame(controller);
        qh.element = kLinkTerminate;
    }
    return result;
}

bool start_controller(const pci::DeviceInfo& info, Controller& controller) {
    controller = {};
    controller.pci = info;
    pci::BarInfo bar = {};
    if (!pci::bar_info(info, 4, bar) || !bar.io_space || bar.base == 0 || bar.base > 0xffff) {
        console::write_line("uhci: controller without an I/O BAR");
        return false;
    }
    controller.io = static_cast<uint16_t>(bar.base);
    usb::hc::prepare_pci(info, "uhci", true);
    pci::write_config_u16(info.bus, info.slot, info.function, kPciLegacySupport, kLegacyOff);
    const uint16_t io = controller.io;
    if (in16(static_cast<uint16_t>(io + kCommand)) == 0xffff) {
        console::printf("uhci: %x:%x.%u does not answer\n", static_cast<unsigned>(info.bus),
            static_cast<unsigned>(info.slot), static_cast<unsigned>(info.function));
        return false;
    }

    // Stop, reset the bus (the design guide asks for 10 ms; OpenBSD gives
    // it 50), then the controller.
    out16(static_cast<uint16_t>(io + kCommand), 0);
    out16(static_cast<uint16_t>(io + kInterruptEnable), 0);
    out16(static_cast<uint16_t>(io + kCommand), kCommandGlobalReset);
    usb::delay_ms(50);
    out16(static_cast<uint16_t>(io + kCommand), 0);
    usb::delay_ms(10);
    out16(static_cast<uint16_t>(io + kCommand), kCommandHostReset);
    const uint64_t deadline = now_ms() + 100;
    while ((in16(static_cast<uint16_t>(io + kCommand)) & kCommandHostReset) != 0) {
        if (now_ms() > deadline) {
            console::write_line("uhci: the controller does not leave reset");
            return false;
        }
        arch::cpu_relax();
    }
    out16(static_cast<uint16_t>(io + kInterruptEnable), 0);
    out16(static_cast<uint16_t>(io + kStatus), kStatusClear);

    for (uint8_t port = 1; port <= kMaxPorts; ++port) {
        const uint16_t portsc = in16(static_cast<uint16_t>(io + kPortBase + (port - 1) * 2));
        if ((portsc & kPortAlwaysOne) == 0 || portsc == 0xffff) {
            break;
        }
        controller.ports = port;
    }
    if (controller.ports == 0) {
        console::write_line("uhci: no root ports");
        return false;
    }

    if (!usb::hc::allocate_dma32(1, controller.frames) || !usb::hc::allocate_dma32(1, controller.queues)) {
        console::write_line("uhci: out of memory below 4 GiB");
        usb::hc::release_dma(controller.frames);
        return false;
    }
    Qh& async = async_qh(controller);
    async.head = kLinkTerminate;
    async.element = kLinkTerminate;
    for (size_t slot = 0; slot < kInterruptSlots; ++slot) {
        Qh& qh = interrupt_qh(controller, slot);
        qh.element = kLinkTerminate;
        qh.head = slot + 1 < kInterruptSlots
            ? physical32(controller.queues, kInterruptQhOffset + (slot + 1) * sizeof(Qh)) | kLinkQueueHead
            : physical32(controller.queues, 0) | kLinkQueueHead;
    }
    auto* frames = static_cast<volatile uint32_t*>(controller.frames.virtual_address);
    for (size_t frame = 0; frame < 1024; ++frame) {
        frames[frame] = physical32(controller.queues, kInterruptQhOffset) | kLinkQueueHead;
    }
    arch::store_barrier();
    out32(static_cast<uint16_t>(io + kFrameListBase), physical32(controller.frames, 0));
    out16(static_cast<uint16_t>(io + kFrameNumber), 0);
    out8(static_cast<uint16_t>(io + kStartOfFrame), 64);
    out16(static_cast<uint16_t>(io + kCommand), kCommandRun | kCommandConfigured | kCommandMaxPacket64);
    const uint64_t running = now_ms() + 100;
    while ((in16(static_cast<uint16_t>(io + kStatus)) & kStatusHalted) != 0) {
        if (now_ms() > running) {
            console::write_line("uhci: the controller does not start");
            return false;
        }
        arch::cpu_relax();
    }
    console::printf("uhci: %x:%x.%u %x:%x at I/O 0x%x, %u ports\n", static_cast<unsigned>(info.bus),
        static_cast<unsigned>(info.slot), static_cast<unsigned>(info.function), static_cast<unsigned>(info.vendor_id),
        static_cast<unsigned>(info.device_id), static_cast<unsigned>(io), static_cast<unsigned>(controller.ports));
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

namespace usb::uhci {

using hc::InterruptCallback;
using hc::kMaxBulkBytes;

size_t port_count(uint8_t local) {
    return local < g_controller_count ? g_controllers[local].ports : 0;
}

bool port_connected(uint8_t local, uint8_t port) {
    return (read_port(g_controllers[local], port) & kPortConnected) != 0;
}

Speed reset_port(uint8_t local, uint8_t port) {
    Controller& controller = g_controllers[local];
    uint16_t portsc = read_port(controller, port);
    if ((portsc & kPortConnected) == 0) {
        return Speed::unknown;
    }
    // Root port reset: 50 ms (USB 2.0, 7.1.7.5), then the port is enabled
    // by hand; some controllers need a few tries (OpenBSD's uhci_portreset).
    write_port(controller, port, portsc, kPortReset);
    usb::delay_ms(50);
    write_port(controller, port, read_port(controller, port), 0);
    usb::delay_ms(1);
    for (int attempt = 0; attempt < 10; ++attempt) {
        portsc = read_port(controller, port);
        if ((portsc & kPortConnected) == 0) {
            return Speed::unknown;
        }
        if ((portsc & kPortWriteClear) != 0) {
            write_port(controller, port, portsc, portsc & kPortWriteClear);
            continue;
        }
        if ((portsc & kPortEnabled) != 0) {
            break;
        }
        write_port(controller, port, portsc, kPortEnabled);
        usb::delay_ms(10);
    }
    portsc = read_port(controller, port);
    if ((portsc & kPortEnabled) == 0) {
        console::printf("uhci: port %u does not enable\n", static_cast<unsigned>(port));
        return Speed::unknown;
    }
    // Reset recovery (USB 2.0, 7.1.7.5).
    usb::delay_ms(10);
    return (portsc & kPortLowSpeed) != 0 ? Speed::low : Speed::full;
}

uint64_t take_port_changes(uint8_t local, bool all) {
    Controller& controller = g_controllers[local];
    controller.port_change = false;
    uint64_t changes = 0;
    for (uint8_t port = 1; port <= controller.ports; ++port) {
        const uint16_t portsc = read_port(controller, port);
        if ((portsc & kPortConnectChange) != 0) {
            changes |= 1ull << (port - 1);
        }
        if ((portsc & kPortWriteClear) != 0) {
            write_port(controller, port, portsc, portsc & kPortWriteClear);
        }
    }
    (void)all;
    return changes;
}

// No event ring to drain: the ports are read, at most every 20 ms.
bool port_events_pending(uint8_t local) {
    Controller& controller = g_controllers[local];
    const uint64_t now = now_ms();
    if (!controller.port_change && now >= controller.next_port_check) {
        controller.next_port_check = now + 20;
        for (uint8_t port = 1; port <= controller.ports; ++port) {
            if ((read_port(controller, port) & kPortConnectChange) != 0) {
                controller.port_change = true;
            }
        }
    }
    return controller.port_change;
}

int control(Device& device, const SetupPacket& setup, void* data, uint32_t* actual);

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
            interrupt_qh(controller, slot).element = kLinkTerminate;
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

// A full-speed hub needs nothing from this controller: there is no
// transaction translator, and a low-speed device behind it is reached with
// the LS bit, for which the controller sends the PRE packet itself.
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
    uint8_t* buffer = static_cast<uint8_t*>(g_control_buffer.virtual_address);
    memcpy(buffer, &setup, sizeof(setup));
    if (!in && setup.length != 0) {
        memcpy(buffer + 8, data, setup.length);
    }
    const uint16_t max_packet = state_of(device).max_packet[0] == 0 ? 8 : state_of(device).max_packet[0];
    const uint32_t physical = physical32(g_control_buffer, 0);

    // SETUP and DATA first: a short IN data stage ends there, and the status
    // stage runs on its own.
    bool toggle = false;
    size_t count = queue_packets(0, device, 0, kPidSetup, physical, 8, 8, toggle, false);
    if (setup.length != 0) {
        count = queue_packets(count, device, 0, in ? kPidIn : kPidOut, physical + 8, setup.length, max_packet,
            toggle, true);
    }
    if (count == 0) {
        return status_error;
    }
    RunResult result = run(controller, count, kControlTimeoutMs);
    if (result.status != status_ok) {
        return result.status;
    }
    const uint32_t moved = result.bytes >= 8 ? result.bytes - 8 : 0u;

    toggle = true;
    count = queue_packets(0, device, 0, in && setup.length != 0 ? kPidOut : kPidIn, 0, 0, max_packet, toggle, false);
    result = run(controller, count, kControlTimeoutMs);
    if (result.status != status_ok) {
        return result.status;
    }
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
    for (InterruptSlot& entry : controller.slots) {
        if (!entry.used) {
            entry = {};
            entry.used = true;
            entry.device = &device;
            entry.generation = device.generation;
            entry.endpoint = descriptor.address;
            return true;
        }
    }
    console::write_line("uhci: no room for another interrupt endpoint");
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
    // As many packets as the pool holds at a time; a short packet ends the
    // transfer.
    uint32_t done = 0;
    int status = status_ok;
    do {
        uint32_t piece = length - done;
        const uint32_t room = static_cast<uint32_t>(kPoolTds) * max_packet;
        if (piece > room) {
            piece = room;
        }
        bool toggle = (state.toggles & (1u << key)) != 0;
        const size_t count = queue_packets(0, device, address, in ? kPidIn : kPidOut,
            physical32(g_bulk_buffer, done), piece, max_packet, toggle, true);
        if (count == 0) {
            return status_error;
        }
        const RunResult result = run(controller, count, timeout_ms);
        // The toggle moves once per packet the device took or gave.
        if ((result.completed & 1u) != 0) {
            state.toggles ^= 1u << key;
        }
        done += result.bytes;
        status = result.status;
        if (status != status_ok || result.short_packet) {
            break;
        }
    } while (done < length);
    if (status != status_ok) {
        return status;
    }
    if (in && done != 0) {
        memcpy(buffer, bounce, done);
    }
    if (actual != nullptr) {
        *actual = done;
    }
    return status_ok;
}

bool reset_endpoint(Device& device, uint8_t address) {
    state_of(device).toggles &= ~(1u << endpoint_key(address));
    return true;
}

bool submit_interrupt(Device& device, uint8_t address, uint16_t length, InterruptCallback callback) {
    Controller& controller = controller_of(device);
    size_t index = 0;
    InterruptSlot* entry = find_slot(controller, device, address, &index);
    if (entry == nullptr || entry->pending) {
        return false;
    }
    DeviceState& state = state_of(device);
    const size_t key = endpoint_key(address);
    uint16_t size = length;
    if (size > state.max_packet[key]) {
        size = state.max_packet[key];
    }
    if (size > kInterruptBufferSize) {
        size = kInterruptBufferSize;
    }
    entry->callback = callback;
    entry->length = size;
    entry->pending = true;
    Td& td = interrupt_td(controller, index);
    td.link = kLinkTerminate;
    td.buffer = physical32(controller.queues, kInterruptBufferOffset + index * kInterruptBufferSize);
    td.token = td_token(device, address, kPidIn, size, (state.toggles & (1u << key)) != 0);
    td.status = td_status(device, false);
    arch::store_barrier();
    interrupt_qh(controller, index).element = physical32(controller.queues, kInterruptTdOffset + index * sizeof(Td));
    return true;
}

void poll(uint8_t local) {
    Controller& controller = g_controllers[local];
    for (size_t slot = 0; slot < kInterruptSlots; ++slot) {
        InterruptSlot& entry = controller.slots[slot];
        if (!entry.used || !entry.pending) {
            continue;
        }
        const uint32_t status = interrupt_td(controller, slot).status;
        if ((status & kTdActive) != 0) {
            continue;
        }
        // As in run(): a TD that completed is finished once the controller
        // has advanced the queue head past it, so that submit_interrupt's
        // new TD is not overwritten. A failed one stays where it is.
        Qh& qh = interrupt_qh(controller, slot);
        if ((status & kTdErrors) == 0 && (qh.element & kLinkTerminate) == 0) {
            continue;
        }
        entry.pending = false;
        Device& device = *entry.device;
        int result = status_ok;
        uint32_t moved = 0;
        if ((status & kTdErrors) != 0) {
            result = (status & kTdStalled) != 0 ? status_stall : status_error;
            qh.element = kLinkTerminate;
        } else {
            moved = decode_length(status);
            state_of(device).toggles ^= 1u << endpoint_key(entry.endpoint);
        }
        if (entry.callback != nullptr) {
            entry.callback(device, entry.endpoint, result, interrupt_buffer(controller, slot), moved);
        }
    }
}

const hc::ControllerOps kOps = {
    "uhci",
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
            info.subclass != hc::kSubclassUsb || info.prog_if != hc::kProgIfUhci) {
            continue;
        }
        if (g_pool.page_count == 0 &&
            (!hc::allocate_dma32(kPoolPages, g_pool) || !hc::allocate_dma32(1, g_control_buffer) ||
                !hc::allocate_dma32(kBulkPages, g_bulk_buffer))) {
            console::write_line("uhci: out of memory below 4 GiB");
            return g_controller_count;
        }
        if (start_controller(info, g_controllers[g_controller_count]) &&
            hc::add_controller(kOps, static_cast<uint8_t>(g_controller_count))) {
            ++g_controller_count;
        }
    }
    return g_controller_count;
}

} // namespace usb::uhci
