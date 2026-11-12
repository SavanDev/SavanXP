// The table of host controllers: whatever their kind, the USB core reaches
// them through usb::hc::, and each call goes to the driver that started the
// controller. See docs/roadmaps/done/USB_ROADMAP.md.

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/pci.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "usb_internal.hpp"

namespace {

struct Entry {
    const usb::hc::ControllerOps* ops;
    uint8_t local;
};

Entry g_controllers[usb::hc::kMaxControllers] = {};
size_t g_count = 0;
// USB addresses in use, 1-127, per controller: bit n of word n / 32.
uint32_t g_addresses[usb::hc::kMaxControllers][4] = {};

constexpr uint8_t kRequestSetAddress = 5;

const usb::hc::ControllerOps& ops_of(uint8_t controller) {
    return *g_controllers[controller].ops;
}

constexpr uint8_t kPciCommandOffset = 0x04;
constexpr uint16_t kPciCommandIo = 1u << 0;
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;
constexpr uint16_t kPciCommandInterruptDisable = 1u << 10;
constexpr uint8_t kPciCapabilityPowerManagement = 0x01;

} // namespace

namespace usb::hc {

bool add_controller(const ControllerOps& ops, uint8_t local) {
    if (g_count >= kMaxControllers) {
        console::printf("usb: more than %u controllers, %s %u left alone\n", static_cast<unsigned>(kMaxControllers),
            ops.name, static_cast<unsigned>(local));
        return false;
    }
    g_controllers[g_count].ops = &ops;
    g_controllers[g_count].local = local;
    ++g_count;
    return true;
}

uint8_t local_index(uint8_t controller) {
    return g_controllers[controller].local;
}

const char* controller_name(uint8_t controller) {
    return g_controllers[controller].ops->name;
}

void prepare_pci(const pci::DeviceInfo& info, const char* name, bool io_space) {
    uint16_t command = pci::read_config_u16(info.bus, info.slot, info.function, kPciCommandOffset);
    command = static_cast<uint16_t>(command | (io_space ? kPciCommandIo : kPciCommandMemory) | kPciCommandBusMaster |
        kPciCommandInterruptDisable);
    pci::write_config_u16(info.bus, info.slot, info.function, kPciCommandOffset, command);

    // A controller in D1-D3 answers every read with all ones (PCI PM 1.2,
    // 10 ms to recover from D3).
    uint8_t capability = 0;
    if (!pci::find_capability(info, kPciCapabilityPowerManagement, capability)) {
        return;
    }
    const uint8_t pmcsr = static_cast<uint8_t>(capability + 4);
    const uint16_t control = pci::read_config_u16(info.bus, info.slot, info.function, pmcsr);
    if ((control & 0x3u) == 0) {
        return;
    }
    console::printf("%s: %x:%x.%u was in D%u, waking it\n", name, static_cast<unsigned>(info.bus),
        static_cast<unsigned>(info.slot), static_cast<unsigned>(info.function), static_cast<unsigned>(control & 0x3u));
    pci::write_config_u16(info.bus, info.slot, info.function, pmcsr, static_cast<uint16_t>(control & ~0x3u));
    usb::delay_ms(10);
}

bool allocate_dma32(size_t pages, memory::PageAllocation& allocation) {
    allocation = {};
    if (!memory::allocate_contiguous_pages_below(pages, memory::kDma32Limit, allocation)) {
        allocation = {};
        return false;
    }
    memset(allocation.virtual_address, 0, pages * memory::kPageSize);
    return true;
}

void release_dma(memory::PageAllocation& allocation) {
    if (allocation.page_count != 0) {
        (void)memory::free_allocation(allocation);
    }
    allocation = {};
}

uint64_t now_ms() {
    return timer::monotonic_ns() / 1000000u;
}

bool assign_address(Device& device) {
    uint32_t* used = g_addresses[device.controller];
    uint8_t address = 0;
    for (uint8_t candidate = 1; candidate < 128; ++candidate) {
        if ((used[candidate / 32] & (1u << (candidate % 32))) == 0) {
            address = candidate;
            break;
        }
    }
    if (address == 0) {
        console::write_line("usb: no free address on the controller");
        return false;
    }
    device.slot = 0;
    SetupPacket setup = {};
    setup.request = kRequestSetAddress;
    setup.value = address;
    const int status = ops_of(device.controller).control(device, setup, nullptr, nullptr);
    if (status != status_ok) {
        console::printf("usb: SET_ADDRESS %u failed: %s\n", static_cast<unsigned>(address), status_name(status));
        return false;
    }
    used[address / 32] |= 1u << (address % 32);
    device.slot = address;
    // SET_ADDRESS recovery (USB 2.0, 9.2.6.3).
    usb::delay_ms(2);
    return true;
}

void release_address(Device& device) {
    if (device.slot != 0 && device.slot < 128) {
        g_addresses[device.controller][device.slot / 32] &= ~(1u << (device.slot % 32));
    }
    device.slot = 0;
}

size_t initialize() {
    g_count = 0;
    if (!pci::ready()) {
        return 0;
    }
    // EHCI before its companions: once it sets CONFIGFLAG every port is its
    // own, and it hands the low- and full-speed devices over (EHCI 1.0,
    // 4.2). The companions then find them on their ports.
    (void)xhci::start();
    (void)ehci::start();
    (void)uhci::start();
    (void)ohci::start();
    return g_count;
}

size_t controller_count() {
    return g_count;
}

size_t port_count(uint8_t controller) {
    return controller < g_count ? ops_of(controller).port_count(local_index(controller)) : 0;
}

bool port_connected(uint8_t controller, uint8_t port) {
    return ops_of(controller).port_connected(local_index(controller), port);
}

Speed reset_port(uint8_t controller, uint8_t port) {
    return ops_of(controller).reset_port(local_index(controller), port);
}

uint64_t take_port_changes(uint8_t controller, bool all) {
    return ops_of(controller).take_port_changes(local_index(controller), all);
}

bool port_events_pending(uint8_t controller) {
    return ops_of(controller).port_events_pending(local_index(controller));
}

bool enable_device(Device& device) {
    return ops_of(device.controller).enable_device(device);
}

bool set_max_packet0(Device& device, uint16_t max_packet) {
    return ops_of(device.controller).set_max_packet0(device, max_packet);
}

void disable_device(Device& device) {
    ops_of(device.controller).disable_device(device);
}

bool set_hub(Device& device) {
    return ops_of(device.controller).set_hub(device);
}

int control(Device& device, const SetupPacket& setup, void* data, uint32_t* actual) {
    return ops_of(device.controller).control(device, setup, data, actual);
}

bool add_endpoint(Device& device, const EndpointDescriptor& endpoint, uint8_t max_burst) {
    return ops_of(device.controller).add_endpoint(device, endpoint, max_burst);
}

int bulk(Device& device, uint8_t endpoint_address, void* buffer, uint32_t length, uint32_t* actual,
    uint32_t timeout_ms) {
    return ops_of(device.controller).bulk(device, endpoint_address, buffer, length, actual, timeout_ms);
}

bool reset_endpoint(Device& device, uint8_t endpoint_address) {
    return ops_of(device.controller).reset_endpoint(device, endpoint_address);
}

bool submit_interrupt(Device& device, uint8_t endpoint_address, uint16_t length, InterruptCallback callback) {
    return ops_of(device.controller).submit_interrupt(device, endpoint_address, length, callback);
}

void poll() {
    for (size_t index = 0; index < g_count; ++index) {
        g_controllers[index].ops->poll(g_controllers[index].local);
    }
}

} // namespace usb::hc
