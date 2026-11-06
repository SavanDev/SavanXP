// The USB core: finds devices on the controllers' ports, reads their
// descriptors, configures them and hands their interfaces to the class
// drivers. See docs/roadmaps/open/USB_ROADMAP.md.

#include "kernel/usb.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/cpu.hpp"
#include "kernel/console.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "usb_internal.hpp"

namespace {

using usb::Device;
using usb::Speed;

usb::Device g_devices[usb::kMaxDevices] = {};
const usb::ClassDriver* g_class_drivers[usb::kMaxClassDrivers] = {};
size_t g_class_driver_count = 0;
bool g_ready = false;

Device* allocate_device() {
    for (Device& device : g_devices) {
        if (!device.present) {
            const uint32_t generation = device.generation + 1;
            memset(&device, 0, sizeof(device));
            device.generation = generation;
            device.present = true;
            return &device;
        }
    }
    return nullptr;
}

void free_device(Device& device) {
    const uint32_t generation = device.generation + 1;
    memset(&device, 0, sizeof(device));
    device.generation = generation;
}

// "1" for a root port, "1.3" for port 3 of a hub on root port 1.
void format_path(const Device& device, char* out, size_t capacity) {
    char reversed[32] = {};
    size_t length = 0;
    const Device* node = &device;
    while (node != nullptr && length + 4 < sizeof(reversed)) {
        const unsigned port = node->parent != nullptr ? node->parent_port : node->root_port;
        char digits[4] = {};
        size_t count = 0;
        unsigned value = port;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0 && count < sizeof(digits));
        for (size_t i = 0; i < count; ++i) {
            reversed[length++] = digits[i];
        }
        node = node->parent;
        if (node != nullptr) {
            reversed[length++] = '.';
        }
    }
    size_t written = 0;
    for (size_t i = length; i > 0 && written + 1 < capacity; --i) {
        out[written++] = reversed[i - 1];
    }
    out[written] = 0;
}

} // namespace

// ASCII of a string descriptor in US English, '?' for anything else.
void usb::read_string(Device& device, uint8_t index, char* out, size_t capacity) {
    out[0] = 0;
    if (index == 0) {
        return;
    }
    uint8_t buffer[255] = {};
    if (usb::get_descriptor(device, usb::kDescriptorString, index, buffer, sizeof(buffer)) != usb::status_ok ||
        buffer[0] < 2) {
        return;
    }
    size_t length = 0;
    for (size_t i = 2; i + 1 < buffer[0] && length + 1 < capacity; i += 2) {
        const uint16_t c = static_cast<uint16_t>(buffer[i] | (buffer[i + 1] << 8));
        out[length++] = c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
    }
    while (length > 0 && out[length - 1] == ' ') {
        --length;
    }
    out[length] = 0;
}

namespace {

// Reads the device's first configuration, whole.
bool read_configuration(Device& device) {
    usb::ConfigurationDescriptor header = {};
    if (usb::get_descriptor(device, usb::kDescriptorConfiguration, 0, &header, sizeof(header)) != usb::status_ok ||
        header.total_length < sizeof(header)) {
        return false;
    }
    uint16_t length = header.total_length;
    if (length > usb::kMaxConfiguration) {
        length = usb::kMaxConfiguration;
    }
    if (usb::get_descriptor(device, usb::kDescriptorConfiguration, 0, device.configuration, length) !=
        usb::status_ok) {
        return false;
    }
    device.configuration_length = length;
    return true;
}

bool claim(Device& device, const usb::InterfaceDescriptor& interface, const uint8_t* extra, size_t extra_length) {
    for (size_t i = 0; i < g_class_driver_count; ++i) {
        const usb::ClassDriver& driver = *g_class_drivers[i];
        if (!driver.probe(device, interface, extra, extra_length)) {
            continue;
        }
        for (const usb::ClassDriver*& slot : device.drivers) {
            if (slot == nullptr || slot == &driver) {
                slot = &driver;
                break;
            }
        }
        return true;
    }
    return false;
}

// Offers every interface (alternate setting 0) to the class drivers.
void offer_interfaces(Device& device) {
    const uint8_t* bytes = device.configuration;
    const size_t total = device.configuration_length;
    size_t offset = 0;
    while (offset + 2 <= total) {
        const uint8_t length = bytes[offset];
        if (length < 2 || offset + length > total) {
            break;
        }
        if (bytes[offset + 1] != usb::kDescriptorInterface || length < sizeof(usb::InterfaceDescriptor)) {
            offset += length;
            continue;
        }
        usb::InterfaceDescriptor interface = {};
        memcpy(&interface, bytes + offset, sizeof(interface));
        size_t end = offset + length;
        while (end + 2 <= total && bytes[end] >= 2 && bytes[end + 1] != usb::kDescriptorInterface) {
            end += bytes[end];
        }
        if (end > total) {
            end = total;
        }
        if (interface.alternate_setting == 0 &&
            !claim(device, interface, bytes + offset + length, end - offset - length)) {
            console::printf("usb: %x:%x interface %u class %x.%x.%x: no driver\n",
                static_cast<unsigned>(device.descriptor.vendor_id), static_cast<unsigned>(device.descriptor.product_id),
                static_cast<unsigned>(interface.interface_number), static_cast<unsigned>(interface.interface_class),
                static_cast<unsigned>(interface.interface_subclass), static_cast<unsigned>(interface.interface_protocol));
        }
        offset = end;
    }
}

void scan_root_ports(uint8_t controller, uint64_t ports) {
    for (uint8_t port = 1; port <= usb::hc::port_count(controller) && port <= 64; ++port) {
        if ((ports & (1ull << (port - 1))) == 0) {
            continue;
        }
        // Whatever was on the port before is gone or about to be replaced.
        for (Device& device : g_devices) {
            if (device.present && device.parent == nullptr && device.controller == controller &&
                device.root_port == port) {
                usb::detach(device);
            }
        }
        if (!usb::hc::port_connected(controller, port)) {
            continue;
        }
        const Speed speed = usb::hc::reset_port(controller, port);
        if (speed == Speed::unknown) {
            continue;
        }
        (void)usb::attach(controller, port, nullptr, 0, speed);
    }
}

} // namespace

namespace usb {

const char* speed_name(Speed speed) {
    switch (speed) {
    case Speed::low: return "low";
    case Speed::full: return "full";
    case Speed::high: return "high";
    case Speed::super: return "super";
    case Speed::super_plus: return "super+";
    default: return "?";
    }
}

const char* status_name(int status) {
    switch (status) {
    case status_ok: return "ok";
    case status_stall: return "stall";
    case status_timeout: return "timeout";
    case status_no_device: return "no device";
    default: return "error";
    }
}

void delay_ms(uint32_t milliseconds) {
    const uint64_t end = timer::monotonic_ns() + static_cast<uint64_t>(milliseconds) * 1000000u;
    while (timer::monotonic_ns() < end) {
        arch::cpu_relax();
    }
}

bool register_class_driver(const ClassDriver& driver) {
    if (g_class_driver_count >= kMaxClassDrivers) {
        return false;
    }
    g_class_drivers[g_class_driver_count++] = &driver;
    return true;
}

Device* device_at(size_t index) {
    return index < kMaxDevices && g_devices[index].present ? &g_devices[index] : nullptr;
}

size_t device_index(const Device& device) {
    return static_cast<size_t>(&device - g_devices);
}

void* driver_data(Device& device, const ClassDriver& driver) {
    for (size_t i = 0; i < 4; ++i) {
        if (device.drivers[i] == &driver) {
            return device.driver_data[i];
        }
    }
    return nullptr;
}

void set_driver_data(Device& device, const ClassDriver& driver, void* data) {
    for (size_t i = 0; i < 4; ++i) {
        if (device.drivers[i] == &driver || device.drivers[i] == nullptr) {
            device.drivers[i] = &driver;
            device.driver_data[i] = data;
            return;
        }
    }
}

int get_descriptor(Device& device, uint8_t type, uint8_t index, void* buffer, uint16_t length) {
    SetupPacket setup = {};
    setup.request_type = kRequestIn;
    setup.request = kRequestGetDescriptor;
    setup.value = static_cast<uint16_t>((type << 8) | index);
    setup.index = type == kDescriptorString ? 0x0409 : 0;
    setup.length = length;
    return hc::control(device, setup, buffer);
}

int clear_halt(Device& device, uint8_t endpoint_address) {
    SetupPacket setup = {};
    setup.request_type = kRequestToEndpoint;
    setup.request = kRequestClearFeature;
    setup.value = kFeatureEndpointHalt;
    setup.index = endpoint_address;
    return hc::control(device, setup, nullptr);
}

Device* attach(uint8_t controller, uint8_t root_port, Device* parent, uint8_t parent_port, Speed speed) {
    Device* slot = allocate_device();
    if (slot == nullptr) {
        console::write_line("usb: device table full");
        return nullptr;
    }
    Device& device = *slot;
    device.controller = controller;
    device.root_port = root_port;
    device.speed = speed;
    device.parent = parent;
    device.parent_port = parent_port;
    if (parent != nullptr) {
        const uint8_t port = parent_port > 15 ? 15 : parent_port;
        device.depth = static_cast<uint8_t>(parent->depth + 1);
        device.route = parent->route | (static_cast<uint32_t>(port) << (4 * parent->depth));
        // A low- or full-speed device behind a high-speed hub talks through
        // that hub's transaction translator.
        if (speed == Speed::low || speed == Speed::full) {
            if (parent->speed == Speed::high) {
                device.tt_slot = parent->slot;
                device.tt_port = parent_port;
            } else {
                device.tt_slot = parent->tt_slot;
                device.tt_port = parent->tt_port;
            }
        }
    }

    char path[24] = {};
    format_path(device, path, sizeof(path));
    if (!hc::enable_device(device)) {
        console::printf("usb: port %s: cannot address the device\n", path);
        free_device(device);
        return nullptr;
    }
    // A full-speed device's endpoint 0 may take more than 8 bytes; the
    // first 8 bytes of the descriptor say how many.
    if (get_descriptor(device, kDescriptorDevice, 0, &device.descriptor, 8) != status_ok) {
        console::printf("usb: port %s: no device descriptor\n", path);
        hc::disable_device(device);
        free_device(device);
        return nullptr;
    }
    if (speed == Speed::full && device.descriptor.max_packet_size0 != 8 &&
        (device.descriptor.max_packet_size0 == 16 || device.descriptor.max_packet_size0 == 32 ||
            device.descriptor.max_packet_size0 == 64)) {
        (void)hc::set_max_packet0(device, device.descriptor.max_packet_size0);
    }
    if (get_descriptor(device, kDescriptorDevice, 0, &device.descriptor, sizeof(device.descriptor)) != status_ok ||
        !read_configuration(device)) {
        console::printf("usb: port %s: cannot read the descriptors\n", path);
        hc::disable_device(device);
        free_device(device);
        return nullptr;
    }
    usb::ConfigurationDescriptor configuration = {};
    memcpy(&configuration, device.configuration, sizeof(configuration));
    SetupPacket setup = {};
    setup.request = kRequestSetConfiguration;
    setup.value = configuration.configuration_value;
    if (hc::control(device, setup, nullptr) != status_ok) {
        console::printf("usb: port %s: SET_CONFIGURATION failed\n", path);
        hc::disable_device(device);
        free_device(device);
        return nullptr;
    }

    char product[48] = {};
    usb::read_string(device, device.descriptor.product_index, product, sizeof(product));
    console::printf("usb: port %s %x:%x class %x %s-speed \"%s\"\n", path,
        static_cast<unsigned>(device.descriptor.vendor_id), static_cast<unsigned>(device.descriptor.product_id),
        static_cast<unsigned>(device.descriptor.device_class), speed_name(speed), product);
    offer_interfaces(device);
    return &device;
}

void detach(Device& device) {
    if (!device.present) {
        return;
    }
    for (Device& child : g_devices) {
        if (child.present && child.parent == &device) {
            detach(child);
        }
    }
    for (const ClassDriver* driver : device.drivers) {
        if (driver != nullptr && driver->detach != nullptr) {
            driver->detach(device);
        }
    }
    char path[24] = {};
    format_path(device, path, sizeof(path));
    console::printf("usb: port %s: %x:%x removed\n", path, static_cast<unsigned>(device.descriptor.vendor_id),
        static_cast<unsigned>(device.descriptor.product_id));
    hc::disable_device(device);
    free_device(device);
}

void initialize() {
    if (hc::initialize() == 0) {
        return;
    }
    g_ready = true;
    register_hid_driver();
    register_storage_driver();
    for (uint8_t controller = 0; controller < hc::controller_count(); ++controller) {
        // Every port once: what is there at boot. Its change bits go with it.
        (void)hc::take_port_changes(controller);
        scan_root_ports(controller, ~0ull);
    }
}

bool ready() {
    return g_ready;
}

void poll() {
    if (!g_ready) {
        return;
    }
    hc::poll();
    for (size_t i = 0; i < g_class_driver_count; ++i) {
        if (g_class_drivers[i]->poll != nullptr) {
            g_class_drivers[i]->poll();
        }
    }
}

} // namespace usb
