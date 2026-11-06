// USB hubs: USB 2 hubs (with the transaction translator low- and full-speed
// devices behind a high-speed hub need) and USB 3 hubs. Written from USB 2.0
// chapter 11 and USB 3.2 chapter 10, with OpenBSD's uhub.c as reference.
// Ports are scanned once when the hub attaches; after that the hub's status
// change endpoint says which port changed, and usb::service() handles it.
// See docs/roadmaps/open/USB_ROADMAP.md.

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "usb_internal.hpp"

namespace {

using usb::Device;
using usb::Speed;

constexpr uint8_t kRequestSetHubDepth = 12;

// Port features.
constexpr uint16_t kFeaturePortReset = 4;
constexpr uint16_t kFeaturePortPower = 8;
constexpr uint16_t kFeatureChangeConnection = 16;
constexpr uint16_t kFeatureChangeEnable = 17;
constexpr uint16_t kFeatureChangeSuspend = 18;
constexpr uint16_t kFeatureChangeOverCurrent = 19;
constexpr uint16_t kFeatureChangeReset = 20;
constexpr uint16_t kFeatureChangeLinkState = 25;
constexpr uint16_t kFeatureChangeConfigError = 26;
constexpr uint16_t kFeatureChangeWarmReset = 29;

// wPortStatus.
constexpr uint16_t kStatusConnected = 1u << 0;
constexpr uint16_t kStatusEnabled = 1u << 1;
constexpr uint16_t kStatusLowSpeed = 1u << 9;
constexpr uint16_t kStatusHighSpeed = 1u << 10;
// wPortChange.
constexpr uint16_t kChangeConnection = 1u << 0;
constexpr uint16_t kChangeReset = 1u << 4;

constexpr size_t kMaxHubs = 8;
constexpr uint8_t kMaxHubPorts = 15;
// Attach debounce (USB 2.0, 7.1.7.3).
constexpr uint32_t kDebounceMs = 100;

struct Hub {
    bool active;
    Device* device;
    uint32_t generation;
    bool super_speed;
    uint8_t ports;
    uint8_t endpoint;
    uint16_t max_packet;
    // Ports the status change endpoint flagged, bit n for port n.
    uint32_t pending;
    bool stalled;
};

Hub g_hubs[kMaxHubs] = {};

bool stale(const Hub& hub) {
    return hub.device == nullptr || !hub.device->present || hub.device->generation != hub.generation;
}

int port_request(Hub& hub, uint8_t request, uint16_t feature, uint8_t port) {
    usb::SetupPacket setup = {};
    setup.request_type = usb::kRequestClass | usb::kRequestToOther;
    setup.request = request;
    setup.value = feature;
    setup.index = port;
    return usb::hc::control(*hub.device, setup, nullptr);
}

bool port_status(Hub& hub, uint8_t port, uint16_t& status, uint16_t& change) {
    usb::SetupPacket setup = {};
    setup.request_type = usb::kRequestIn | usb::kRequestClass | usb::kRequestToOther;
    setup.request = usb::kRequestGetStatus;
    setup.index = port;
    setup.length = 4;
    uint8_t answer[4] = {};
    if (usb::hc::control(*hub.device, setup, answer) != usb::status_ok) {
        return false;
    }
    status = static_cast<uint16_t>(answer[0] | (answer[1] << 8));
    change = static_cast<uint16_t>(answer[2] | (answer[3] << 8));
    return true;
}

// Acknowledges every change bit the hub reported for a port. wPortChange
// bits 0-4 are the same on both kinds of hub (bits 1 and 2 unused on USB 3);
// bits 5-7 are USB 3 only.
void clear_changes(Hub& hub, uint8_t port, uint16_t change) {
    static const uint16_t kUsb2[5] = {
        kFeatureChangeConnection, kFeatureChangeEnable, kFeatureChangeSuspend, kFeatureChangeOverCurrent,
        kFeatureChangeReset,
    };
    static const uint16_t kUsb3[3] = {kFeatureChangeWarmReset, kFeatureChangeLinkState, kFeatureChangeConfigError};
    for (uint16_t bit = 0; bit < 8; ++bit) {
        if ((change & (1u << bit)) == 0) {
            continue;
        }
        const uint16_t feature = bit < 5 ? kUsb2[bit] : (hub.super_speed ? kUsb3[bit - 5] : 0);
        if (feature != 0) {
            (void)port_request(hub, usb::kRequestClearFeature, feature, port);
        }
    }
}

Device* child_at(const Hub& hub, uint8_t port) {
    for (size_t i = 0; i < usb::kMaxDevices; ++i) {
        Device* device = usb::device_at(i);
        if (device != nullptr && device->parent == hub.device && device->parent_port == port) {
            return device;
        }
    }
    return nullptr;
}

// Resets the port and attaches what is behind it.
void bring_up_port(Hub& hub, uint8_t port) {
    if (hub.device->depth + 1 > usb::kMaxHubDepth) {
        console::write_line("usb-hub: hubs nested too deep");
        return;
    }
    if (port_request(hub, usb::kRequestSetFeature, kFeaturePortReset, port) != usb::status_ok) {
        return;
    }
    uint16_t status = 0;
    uint16_t change = 0;
    const uint64_t deadline = timer::monotonic_ms() + 500;
    do {
        usb::delay_ms(10);
        if (!port_status(hub, port, status, change)) {
            return;
        }
    } while ((change & kChangeReset) == 0 && timer::monotonic_ms() < deadline);
    clear_changes(hub, port, change);
    // Reset recovery (USB 2.0, 7.1.7.5).
    usb::delay_ms(10);
    if (!port_status(hub, port, status, change) || (status & kStatusConnected) == 0 ||
        (status & kStatusEnabled) == 0) {
        return;
    }
    Speed speed = Speed::full;
    if (hub.super_speed) {
        speed = Speed::super;
    } else if ((status & kStatusLowSpeed) != 0) {
        speed = Speed::low;
    } else if ((status & kStatusHighSpeed) != 0) {
        speed = Speed::high;
    }
    (void)usb::attach(hub.device->controller, hub.device->root_port, hub.device, port, speed);
}

// A port whose connection changed: what was there goes, what is there now
// comes.
void port_changed(Hub& hub, uint8_t port) {
    uint16_t status = 0;
    uint16_t change = 0;
    if (!port_status(hub, port, status, change)) {
        return;
    }
    clear_changes(hub, port, change);
    if ((change & kChangeConnection) == 0) {
        return;
    }
    if (Device* child = child_at(hub, port)) {
        usb::detach(*child);
    }
    if ((status & kStatusConnected) != 0) {
        usb::delay_ms(kDebounceMs);
        bring_up_port(hub, port);
    }
}

void status_changed(Device& device, uint8_t address, int status, const uint8_t* data, uint32_t length) {
    for (Hub& hub : g_hubs) {
        if (!hub.active || hub.device != &device || hub.endpoint != address) {
            continue;
        }
        if (status == usb::status_stall) {
            hub.stalled = true;
            return;
        }
        if (status != usb::status_ok) {
            return;
        }
        for (uint32_t i = 0; i < length && i < 4; ++i) {
            hub.pending |= static_cast<uint32_t>(data[i]) << (8 * i);
        }
        // Bit 0 is the hub itself; nothing about it is acted on.
        hub.pending &= ~1u;
        (void)usb::hc::submit_interrupt(device, address, hub.max_packet, &status_changed);
        return;
    }
}

bool probe(Device& device, const usb::InterfaceDescriptor& interface, const uint8_t* extra, size_t extra_length) {
    if (interface.interface_class != usb::kClassHub) {
        return false;
    }
    Hub* slot = nullptr;
    for (Hub& candidate : g_hubs) {
        if (!candidate.active) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        console::write_line("usb-hub: hub table full");
        return false;
    }
    usb::EndpointDescriptor endpoint = {};
    bool found = false;
    for (size_t offset = 0; offset + 2 <= extra_length && extra[offset] >= 2; offset += extra[offset]) {
        if (extra[offset + 1] == usb::kDescriptorEndpoint && extra[offset] >= sizeof(endpoint)) {
            memcpy(&endpoint, extra + offset, sizeof(endpoint));
            found = (endpoint.address & 0x80) != 0 && (endpoint.attributes & 0x03) == usb::kTransferInterrupt;
            if (found) {
                break;
            }
        }
    }
    if (!found) {
        return false;
    }
    Hub& hub = *slot;
    hub = {};
    hub.device = &device;
    hub.generation = device.generation;
    hub.super_speed = device.speed == Speed::super || device.speed == Speed::super_plus;

    // The hub descriptor: how many ports, and how long power takes.
    usb::SetupPacket setup = {};
    setup.request_type = usb::kRequestIn | usb::kRequestClass;
    setup.request = usb::kRequestGetDescriptor;
    setup.value = static_cast<uint16_t>((hub.super_speed ? usb::kDescriptorSsHub : usb::kDescriptorHub) << 8);
    setup.length = hub.super_speed ? 12 : 9;
    uint8_t descriptor[12] = {};
    uint32_t actual = 0;
    if (usb::hc::control(device, setup, descriptor, &actual) != usb::status_ok || actual < 7) {
        console::write_line("usb-hub: no hub descriptor");
        return false;
    }
    hub.ports = descriptor[2] > kMaxHubPorts ? kMaxHubPorts : descriptor[2];
    const uint32_t power_good_ms = static_cast<uint32_t>(descriptor[5]) * 2;

    device.hub = true;
    device.hub_ports = hub.ports;
    if (!usb::hc::set_hub(device)) {
        console::write_line("usb-hub: the controller refused the hub");
        return false;
    }
    if (hub.super_speed) {
        // Where the hub sits, so it can read its tier of the route string.
        usb::SetupPacket depth = {};
        depth.request_type = usb::kRequestClass;
        depth.request = kRequestSetHubDepth;
        depth.value = device.depth;
        (void)usb::hc::control(device, depth, nullptr);
    }
    hub.endpoint = endpoint.address;
    hub.max_packet = static_cast<uint16_t>(endpoint.max_packet_size & 0x7ff);
    if (hub.max_packet > 4) {
        hub.max_packet = 4;
    }
    if (!usb::hc::add_endpoint(device, endpoint)) {
        return false;
    }
    hub.active = true;
    console::printf("usb-hub: %u ports on %x:%x\n", static_cast<unsigned>(hub.ports),
        static_cast<unsigned>(device.descriptor.vendor_id), static_cast<unsigned>(device.descriptor.product_id));

    for (uint8_t port = 1; port <= hub.ports; ++port) {
        (void)port_request(hub, usb::kRequestSetFeature, kFeaturePortPower, port);
    }
    usb::delay_ms(power_good_ms + kDebounceMs);
    // What is plugged in now, at once: a keyboard or a stick behind a hub has
    // to be there by the time input and storage are probed.
    for (uint8_t port = 1; port <= hub.ports; ++port) {
        uint16_t status = 0;
        uint16_t change = 0;
        if (!port_status(hub, port, status, change)) {
            continue;
        }
        clear_changes(hub, port, change);
        if ((status & kStatusConnected) != 0) {
            bring_up_port(hub, port);
        }
    }
    (void)usb::hc::submit_interrupt(device, hub.endpoint, hub.max_packet, &status_changed);
    return true;
}

void detach(Device& device) {
    for (Hub& hub : g_hubs) {
        if (hub.active && hub.device == &device) {
            hub = {};
        }
    }
}

bool service() {
    bool worked = false;
    for (Hub& hub : g_hubs) {
        if (!hub.active || stale(hub)) {
            continue;
        }
        if (hub.stalled) {
            hub.stalled = false;
            (void)usb::hc::reset_endpoint(*hub.device, hub.endpoint);
            (void)usb::clear_halt(*hub.device, hub.endpoint);
            (void)usb::hc::submit_interrupt(*hub.device, hub.endpoint, hub.max_packet, &status_changed);
            worked = true;
        }
        while (hub.active && hub.pending != 0) {
            const uint32_t pending = hub.pending;
            hub.pending = 0;
            for (uint8_t port = 1; port <= hub.ports && hub.active; ++port) {
                if ((pending & (1u << port)) != 0) {
                    port_changed(hub, port);
                }
            }
            worked = true;
        }
    }
    return worked;
}

const usb::ClassDriver kHubDriver = {
    "usb-hub",
    &probe,
    &detach,
    nullptr,
    &service,
};

} // namespace

namespace usb {

void register_hub_driver() {
    (void)register_class_driver(kHubDriver);
}

} // namespace usb
