#pragma once

// Shared between the files of the USB stack (kernel/usb/). Nothing outside
// kernel/usb includes it: the rest of the kernel sees include/kernel/usb.hpp.
// See docs/roadmaps/done/USB_ROADMAP.md.

#include <stddef.h>
#include <stdint.h>

#include "kernel/physical_memory.hpp"

namespace pci {
struct DeviceInfo;
}

namespace usb {

// The xHCI default speed IDs (PORTSC bits 10-13, slot context bits 20-23).
enum class Speed : uint8_t {
    unknown = 0,
    full = 1,
    low = 2,
    high = 3,
    super = 4,
    super_plus = 5,
};

const char* speed_name(Speed speed);

// Results of a transfer. Negative values are failures.
enum Status : int {
    status_ok = 0,
    status_stall = -1,
    status_timeout = -2,
    status_error = -3,
    status_no_device = -4,
};

struct [[gnu::packed]] SetupPacket {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
};
static_assert(sizeof(SetupPacket) == 8);

struct [[gnu::packed]] DeviceDescriptor {
    uint8_t length;
    uint8_t type;
    uint16_t usb_version;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size0;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
    uint8_t manufacturer_index;
    uint8_t product_index;
    uint8_t serial_index;
    uint8_t configuration_count;
};
static_assert(sizeof(DeviceDescriptor) == 18);

struct [[gnu::packed]] ConfigurationDescriptor {
    uint8_t length;
    uint8_t type;
    uint16_t total_length;
    uint8_t interface_count;
    uint8_t configuration_value;
    uint8_t configuration_index;
    uint8_t attributes;
    uint8_t max_power;
};
static_assert(sizeof(ConfigurationDescriptor) == 9);

struct [[gnu::packed]] InterfaceDescriptor {
    uint8_t length;
    uint8_t type;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_count;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_index;
};
static_assert(sizeof(InterfaceDescriptor) == 9);

struct [[gnu::packed]] EndpointDescriptor {
    uint8_t length;
    uint8_t type;
    uint8_t address;      // bit 7: IN
    uint8_t attributes;   // bits 0-1: transfer type
    uint16_t max_packet_size;
    uint8_t interval;
};
static_assert(sizeof(EndpointDescriptor) == 7);

// Descriptor types.
constexpr uint8_t kDescriptorDevice = 1;
constexpr uint8_t kDescriptorConfiguration = 2;
constexpr uint8_t kDescriptorString = 3;
constexpr uint8_t kDescriptorInterface = 4;
constexpr uint8_t kDescriptorEndpoint = 5;
constexpr uint8_t kDescriptorSsEndpointCompanion = 48;
constexpr uint8_t kDescriptorHid = 0x21;
constexpr uint8_t kDescriptorHidReport = 0x22;
constexpr uint8_t kDescriptorHub = 0x29;
constexpr uint8_t kDescriptorSsHub = 0x2a;

// Standard requests.
constexpr uint8_t kRequestGetStatus = 0;
constexpr uint8_t kRequestClearFeature = 1;
constexpr uint8_t kRequestSetFeature = 3;
constexpr uint8_t kRequestGetDescriptor = 6;
constexpr uint8_t kRequestSetConfiguration = 9;

// bmRequestType.
constexpr uint8_t kRequestIn = 0x80;
constexpr uint8_t kRequestClass = 0x20;
constexpr uint8_t kRequestToInterface = 0x01;
constexpr uint8_t kRequestToEndpoint = 0x02;
constexpr uint8_t kRequestToOther = 0x03;

constexpr uint16_t kFeatureEndpointHalt = 0;

// Endpoint transfer types (bmAttributes bits 0-1).
constexpr uint8_t kTransferControl = 0;
constexpr uint8_t kTransferIsochronous = 1;
constexpr uint8_t kTransferBulk = 2;
constexpr uint8_t kTransferInterrupt = 3;

constexpr uint8_t kClassHid = 0x03;
constexpr uint8_t kClassMassStorage = 0x08;
constexpr uint8_t kClassHub = 0x09;

constexpr size_t kMaxDevices = 32;
constexpr size_t kMaxConfiguration = 1024;
// Hubs nest at most five deep below the root (USB 2.0, 4.1.1).
constexpr uint8_t kMaxHubDepth = 5;

struct Device;

// A class driver. probe() looks at one interface of a configured device and
// claims it by returning true; it may configure endpoints and start
// transfers. detach() is called when the device goes away.
struct ClassDriver {
    const char* name;
    bool (*probe)(Device& device, const InterfaceDescriptor& interface, const uint8_t* extra,
        size_t extra_length);
    void (*detach)(Device& device);
    // Called from the timer tick after the controller's events are drained,
    // outside any of the stack's own callbacks: short work that needs
    // transfers of its own (a keyboard's LEDs).
    void (*poll)();
    // Called from usb::service(), when a core has nothing else to do: long
    // work (a hub enumerating its ports, a pulled stick released). Returns
    // whether it did something.
    bool (*service)();
};

constexpr size_t kMaxClassDrivers = 4;

struct Device {
    bool present;
    // Bumps every time the slot is reused, so a driver can tell a stale
    // pointer from the device now in the slot.
    uint32_t generation;
    uint8_t controller;
    uint8_t slot;
    // The root port the device's branch hangs from, 1-based.
    uint8_t root_port;
    // xHCI route string: 4 bits per hub tier below the root port.
    uint32_t route;
    uint8_t depth;
    Speed speed;
    // The hub this device hangs from (nullptr on a root port), and its port.
    Device* parent;
    uint8_t parent_port;
    // For a low- or full-speed device behind a high-speed hub: that hub's
    // slot and port (the transaction translator).
    uint8_t tt_slot;
    uint8_t tt_port;
    DeviceDescriptor descriptor;
    uint8_t configuration[kMaxConfiguration];
    uint16_t configuration_length;
    // Hub: how many ports, and whether its TT is per port.
    bool hub;
    uint8_t hub_ports;
    bool hub_multi_tt;
    const ClassDriver* drivers[4];
    // Each claiming driver's own per-device state.
    void* driver_data[4];
};

const char* status_name(int status);

// The class drivers, registered by usb::initialize().
void register_hid_driver();
void register_storage_driver();
void register_hub_driver();
const ClassDriver& storage_class_driver();

// The core (usb.cpp).
bool register_class_driver(const ClassDriver& driver);
// A device was found on a port: address it, read its descriptors,
// configure it and offer its interfaces to the class drivers. parent is null
// for a root port.
Device* attach(uint8_t controller, uint8_t root_port, Device* parent, uint8_t parent_port, Speed speed);
// A device went away: tell its drivers, then everything below it, then free
// the slot.
void detach(Device& device);
// The device table, for hubs looking for their children.
Device* device_at(size_t index);
size_t device_index(const Device& device);
void* driver_data(Device& device, const ClassDriver& driver);
void set_driver_data(Device& device, const ClassDriver& driver, void* data);
// Busy-waits, with interrupts as they are. USB timing is in milliseconds.
void delay_ms(uint32_t milliseconds);

// ASCII of a string descriptor, US English, '?' for anything else; empty if
// the device has none.
void read_string(Device& device, uint8_t index, char* out, size_t capacity);

// Standard requests on top of hc::control.
int get_descriptor(Device& device, uint8_t type, uint8_t index, void* buffer, uint16_t length);
int clear_halt(Device& device, uint8_t endpoint_address);

// The host controllers (hc.cpp), whatever their kind: xHCI, EHCI, UHCI or
// OHCI. `controller` is the index in hc.cpp's table, which Device::controller
// holds. Every call is synchronous unless it says otherwise, and completes by
// polling the controller.
namespace hc {

// Finds and starts every controller: xHCI, then EHCI, then the UHCI and OHCI
// companions, so that an EHCI controller has its ports before its companions
// look at theirs. Returns how many started.
size_t initialize();
size_t controller_count();
size_t port_count(uint8_t controller);
// Root-port state.
bool port_connected(uint8_t controller, uint8_t port);
// Resets the port and returns the speed of what is behind it, or
// Speed::unknown if nothing came up.
Speed reset_port(uint8_t controller, uint8_t port);
// Ports whose connection changed, as a bitmap (bit n-1 = port n), with
// every change bit cleared: those the controller reported in an event since
// the last call, or every port with `all`.
uint64_t take_port_changes(uint8_t controller, bool all);
// Whether a port status change event arrived since the last
// take_port_changes(). Cheap: no register is read.
bool port_events_pending(uint8_t controller);

// Gives the device a slot and an address. Fills device.slot.
bool enable_device(Device& device);
// Endpoint 0's packet size, once the device descriptor said it.
bool set_max_packet0(Device& device, uint16_t max_packet);
// Releases the slot and everything allocated for it.
void disable_device(Device& device);
// Tells the controller the device is a hub (slot context), after its hub
// descriptor was read.
bool set_hub(Device& device);

// A control transfer on endpoint 0. data is length bytes, copied through
// the controller's own DMA memory. actual, if given, receives how many bytes
// moved.
int control(Device& device, const SetupPacket& setup, void* data, uint32_t* actual = nullptr);
// Adds an endpoint (bulk or interrupt) to the device's configuration.
bool add_endpoint(Device& device, const EndpointDescriptor& endpoint, uint8_t max_burst = 0);
// A bulk transfer, up to kMaxBulkBytes.
constexpr uint32_t kMaxBulkBytes = 64 * 1024;
int bulk(Device& device, uint8_t endpoint_address, void* buffer, uint32_t length, uint32_t* actual,
    uint32_t timeout_ms);
// After a stall on a bulk endpoint: reset it in the controller. The caller
// also clears the halt on the device (clear_halt).
bool reset_endpoint(Device& device, uint8_t endpoint_address);

// Interrupt IN endpoints run on their own: submit_interrupt queues one read
// of up to length bytes, and when it completes the callback receives the
// data. The callback runs while events are being drained; it may only call
// submit_interrupt again.
using InterruptCallback = void (*)(Device& device, uint8_t endpoint_address, int status,
    const uint8_t* data, uint32_t length);
bool submit_interrupt(Device& device, uint8_t endpoint_address, uint16_t length, InterruptCallback callback);

// Drains every controller's event ring or schedule: completes interrupt
// reads and records port changes.
void poll();

// What a controller driver gives hc.cpp. `local` is the driver's own index of
// the controller; calls about a device find it from Device::controller with
// local_index(). Each entry has the meaning of the hc:: function of that name.
struct ControllerOps {
    const char* name;
    size_t (*port_count)(uint8_t local);
    bool (*port_connected)(uint8_t local, uint8_t port);
    Speed (*reset_port)(uint8_t local, uint8_t port);
    uint64_t (*take_port_changes)(uint8_t local, bool all);
    bool (*port_events_pending)(uint8_t local);
    bool (*enable_device)(Device& device);
    bool (*set_max_packet0)(Device& device, uint16_t max_packet);
    void (*disable_device)(Device& device);
    bool (*set_hub)(Device& device);
    int (*control)(Device& device, const SetupPacket& setup, void* data, uint32_t* actual);
    bool (*add_endpoint)(Device& device, const EndpointDescriptor& endpoint, uint8_t max_burst);
    int (*bulk)(Device& device, uint8_t endpoint_address, void* buffer, uint32_t length, uint32_t* actual,
        uint32_t timeout_ms);
    bool (*reset_endpoint)(Device& device, uint8_t endpoint_address);
    bool (*submit_interrupt)(Device& device, uint8_t endpoint_address, uint16_t length,
        InterruptCallback callback);
    void (*poll)(uint8_t local);
};

// Eight UHCI or OHCI and two EHCI on a 2008 south bridge, and room for xHCI.
constexpr size_t kMaxControllers = 16;

// Called by a driver's start() for each controller it brought up.
bool add_controller(const ControllerOps& ops, uint8_t local);
uint8_t local_index(uint8_t controller);
// "xhci", "ehci", "uhci" or "ohci", for the log.
const char* controller_name(uint8_t controller);

} // namespace hc

// The controller drivers. start() finds and starts every controller of its
// kind and adds each to hc.cpp's table; it returns how many it started.
namespace xhci {
size_t start();
}
namespace ehci {
size_t start();
}
namespace uhci {
size_t start();
}
namespace ohci {
size_t start();
}

// What the PCI-side drivers share (hc.cpp).
namespace hc {
// PCI class of a USB controller: serial bus, USB; the programming interface
// says which kind.
constexpr uint8_t kClassSerialBus = 0x0c;
constexpr uint8_t kSubclassUsb = 0x03;
constexpr uint8_t kProgIfUhci = 0x00;
constexpr uint8_t kProgIfOhci = 0x10;
constexpr uint8_t kProgIfEhci = 0x20;
constexpr uint8_t kProgIfXhci = 0x30;

// Memory or I/O decode and bus mastering on, the interrupt line off (every
// driver here polls), and the function brought to D0 if the firmware left it
// in a low-power state. `name` prefixes the log line.
void prepare_pci(const pci::DeviceInfo& info, const char* name, bool io_space);
// Zeroed pages below 4 GiB, for the controllers that address only 32 bits.
bool allocate_dma32(size_t pages, memory::PageAllocation& allocation);
void release_dma(memory::PageAllocation& allocation);
uint64_t now_ms();

// For the controllers without device slots (EHCI, UHCI, OHCI): picks a free
// address on the device's controller and sends SET_ADDRESS to address 0
// through the controller's own control(); Device::slot then holds the
// address. The driver must already know endpoint 0's packet size.
bool assign_address(Device& device);
void release_address(Device& device);
} // namespace hc

} // namespace usb
