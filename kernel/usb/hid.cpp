// USB HID: keyboards through the boot protocol, pointers (mice, tablets)
// through their report descriptor, read with OpenBSD's parser
// (hid_report.cpp). Reports arrive on an interrupt IN endpoint; the
// callback only queues them, and usb::poll() turns them into input events.
// USB keyboards and pointers coexist with PS/2 and virtio ones: none
// silences the others. See docs/roadmaps/done/USB_ROADMAP.md.

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "kernel/console.hpp"
#include "kernel/input.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/usb.hpp"
#include "usb_internal.hpp"

#include "hid_types.hpp"
#include "hid_report.h"

namespace {

using usb::Device;

constexpr uint8_t kSubclassBoot = 1;
constexpr uint8_t kProtocolKeyboard = 1;
constexpr uint8_t kProtocolMouse = 2;

constexpr uint8_t kRequestSetReport = 0x09;
constexpr uint8_t kRequestSetIdle = 0x0a;
constexpr uint8_t kRequestSetProtocol = 0x0b;
constexpr uint16_t kReportTypeOutput = 2;

constexpr size_t kMaxKeyboards = 4;
constexpr size_t kMaxPointers = 4;
constexpr size_t kReportBytes = 64;
constexpr size_t kQueuedReports = 8;
constexpr size_t kMaxReportDescriptor = 1024;
constexpr size_t kMaxButtons = 5;

// Typematic, since a USB keyboard does not repeat by itself.
constexpr uint64_t kRepeatDelayMs = 500;
constexpr uint64_t kRepeatPeriodMs = 33;

// Reports the callback received and poll() has not handled yet.
struct ReportQueue {
    uint8_t data[kQueuedReports][kReportBytes];
    uint8_t length[kQueuedReports];
    size_t read;
    size_t write;
};

struct Endpoint {
    Device* device;
    uint32_t generation;
    uint8_t interface;
    uint8_t address;
    uint16_t max_packet;
    bool stalled;
    bool dead;
    ReportQueue queue;
};

struct Keyboard {
    bool active;
    Endpoint endpoint;
    uint8_t previous[8];
    bool leds_dirty;
    uint8_t repeat_usage;
    uint64_t repeat_at;
};

struct Pointer {
    bool active;
    Endpoint endpoint;
    bool boot;
    bool absolute;
    uint8_t report_id;
    hid_location x;
    hid_location y;
    hid_location wheel;
    hid_location buttons[kMaxButtons];
    int32_t x_min;
    int32_t x_max;
    int32_t y_min;
    int32_t y_max;
    bool have_position;
    int32_t last_x;
    int32_t last_y;
};

Keyboard g_keyboards[kMaxKeyboards] = {};
Pointer g_pointers[kMaxPointers] = {};
uint32_t g_leds = 0;
uint32_t g_screen_width = 0;
uint32_t g_screen_height = 0;
bool g_input_registered = false;

// HID keyboard usages 0x00-0x67 as PC/AT set 1 positions: low 7 bits the
// code, 0x80 the 0xE0 prefix, 0 for none.
constexpr uint8_t kE0 = 0x80;
const uint8_t kUsageToSet1[0x68] = {
    0, 0, 0, 0,
    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, // a-l
    0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, // m-x
    0x15, 0x2c,                                                             // y z
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,             // 1-0
    0x1c, 0x01, 0x0e, 0x0f, 0x39, 0x0c, 0x0d, 0x1a, 0x1b, 0x2b,             // enter-backslash
    0x2b, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3a,                         // non-US #-caps
    0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58, // F1-F12
    0, 0x46, 0,                                                             // print, scroll, pause
    kE0 | 0x52, kE0 | 0x47, kE0 | 0x49, kE0 | 0x53, kE0 | 0x4f, kE0 | 0x51, // ins-pgdn
    kE0 | 0x4d, kE0 | 0x4b, kE0 | 0x50, kE0 | 0x48,                         // arrows
    0x45, kE0 | 0x35, 0x37, 0x4a, 0x4e, kE0 | 0x1c,                         // num-kp enter
    0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49, 0x52, 0x53,       // kp1-kp.
    0x56, kE0 | 0x5d, 0, 0,                                                 // non-US \, menu
};

// Modifier usages 0xe0-0xe7: left ctrl, shift, alt, gui, then right.
const uint8_t kModifierToSet1[8] = {
    0x1d, 0x2a, 0x38, kE0 | 0x5b, kE0 | 0x1d, 0x36, kE0 | 0x38, kE0 | 0x5c,
};

constexpr uint8_t kUsagePrintScreen = 0x46;
constexpr uint8_t kUsagePause = 0x48;

uint64_t now_ms() {
    return timer::monotonic_ns() / 1000000u;
}

void submit_usage(uint8_t usage, bool pressed) {
    if (usage == kUsagePrintScreen) {
        input::submit_key(SAVANXP_KEY_PRINT_SCREEN, pressed);
        return;
    }
    if (usage == kUsagePause) {
        input::submit_key(SAVANXP_KEY_PAUSE, pressed);
        return;
    }
    uint8_t code = 0;
    if (usage < sizeof(kUsageToSet1)) {
        code = kUsageToSet1[usage];
    } else if (usage >= 0xe0 && usage <= 0xe7) {
        code = kModifierToSet1[usage - 0xe0];
    }
    if (code != 0) {
        input::submit_scancode(code & 0x7f, (code & kE0) != 0, pressed);
    }
}

bool contains(const uint8_t* keys, uint8_t usage) {
    for (size_t i = 2; i < 8; ++i) {
        if (keys[i] == usage) {
            return true;
        }
    }
    return false;
}

void handle_keyboard_report(Keyboard& keyboard, const uint8_t* report, size_t length) {
    uint8_t current[8] = {};
    memcpy(current, report, length < 8 ? length : 8);
    // ErrorRollOver: too many keys down; the report says nothing.
    if (current[2] == 0x01) {
        return;
    }
    const uint8_t changed = static_cast<uint8_t>(current[0] ^ keyboard.previous[0]);
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if ((changed & (1u << bit)) != 0) {
            submit_usage(static_cast<uint8_t>(0xe0 + bit), (current[0] & (1u << bit)) != 0);
        }
    }
    for (size_t i = 2; i < 8; ++i) {
        const uint8_t usage = keyboard.previous[i];
        if (usage > 0x03 && !contains(current, usage)) {
            submit_usage(usage, false);
            if (usage == keyboard.repeat_usage) {
                keyboard.repeat_usage = 0;
            }
        }
    }
    for (size_t i = 2; i < 8; ++i) {
        const uint8_t usage = current[i];
        if (usage > 0x03 && !contains(keyboard.previous, usage)) {
            submit_usage(usage, true);
            keyboard.repeat_usage = usage;
            keyboard.repeat_at = now_ms() + kRepeatDelayMs;
        }
    }
    memcpy(keyboard.previous, current, sizeof(current));
}

void release_keys(Keyboard& keyboard) {
    const uint8_t none[8] = {};
    handle_keyboard_report(keyboard, none, sizeof(none));
}

int32_t normalize_axis(int32_t value, int32_t minimum, int32_t maximum, uint32_t extent) {
    if (extent <= 1 || maximum <= minimum) {
        return 0;
    }
    if (value < minimum) {
        value = minimum;
    } else if (value > maximum) {
        value = maximum;
    }
    const int64_t range = static_cast<int64_t>(maximum) - minimum;
    return static_cast<int32_t>(((static_cast<int64_t>(value) - minimum) * (extent - 1) + range / 2) / range);
}

void handle_pointer_report(Pointer& pointer, const uint8_t* report, size_t length) {
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t wheel = 0;
    uint32_t buttons = 0;
    if (pointer.boot) {
        if (length < 3) {
            return;
        }
        buttons = report[0] & 0x07u;
        dx = static_cast<int8_t>(report[1]);
        dy = static_cast<int8_t>(report[2]);
        wheel = length > 3 ? static_cast<int8_t>(report[3]) : 0;
    } else {
        if (pointer.report_id != 0) {
            if (length == 0 || report[0] != pointer.report_id) {
                return;
            }
            ++report;
            --length;
        }
        const int size = static_cast<int>(length);
        for (size_t i = 0; i < kMaxButtons; ++i) {
            if (pointer.buttons[i].size != 0 && hid_get_udata(report, size, &pointer.buttons[i]) != 0) {
                buttons |= 1u << i;
            }
        }
        if (pointer.absolute) {
            dx = static_cast<int32_t>(hid_get_udata(report, size, &pointer.x));
            dy = static_cast<int32_t>(hid_get_udata(report, size, &pointer.y));
        } else {
            dx = hid_get_data(report, size, &pointer.x);
            dy = hid_get_data(report, size, &pointer.y);
        }
        if (pointer.wheel.size != 0) {
            wheel = hid_get_data(report, size, &pointer.wheel);
        }
    }
    // HID numbers buttons left, right, middle, as the ABI does.
    const uint32_t mask = (buttons & 1u ? SAVANXP_MOUSE_BUTTON_LEFT : 0u) |
        (buttons & 2u ? SAVANXP_MOUSE_BUTTON_RIGHT : 0u) | (buttons & 4u ? SAVANXP_MOUSE_BUTTON_MIDDLE : 0u);
    if (!pointer.absolute) {
        input::submit_mouse_event({dx, dy, wheel, mask, 0, 0, false});
        return;
    }
    // An absolute position is the truth; the delta is for consumers that only
    // add, and the first sample after a reset is a jump.
    const int32_t x = normalize_axis(dx, pointer.x_min, pointer.x_max, g_screen_width);
    const int32_t y = normalize_axis(dy, pointer.y_min, pointer.y_max, g_screen_height);
    const int32_t delta_x = pointer.have_position ? x - pointer.last_x : x;
    const int32_t delta_y = pointer.have_position ? y - pointer.last_y : y;
    pointer.have_position = true;
    pointer.last_x = x;
    pointer.last_y = y;
    input::submit_mouse_event({delta_x, delta_y, wheel, mask, x, y, true});
}

Endpoint* endpoint_for(Device& device, uint8_t address) {
    for (Keyboard& keyboard : g_keyboards) {
        if (keyboard.active && keyboard.endpoint.device == &device && keyboard.endpoint.address == address) {
            return &keyboard.endpoint;
        }
    }
    for (Pointer& pointer : g_pointers) {
        if (pointer.active && pointer.endpoint.device == &device && pointer.endpoint.address == address) {
            return &pointer.endpoint;
        }
    }
    return nullptr;
}

// Runs while the controller's events are drained: queue and read again.
void report_arrived(Device& device, uint8_t address, int status, const uint8_t* data, uint32_t length) {
    Endpoint* endpoint = endpoint_for(device, address);
    if (endpoint == nullptr) {
        return;
    }
    if (status == usb::status_stall) {
        endpoint->stalled = true;
        return;
    }
    if (status != usb::status_ok) {
        // Most likely the device is gone; the port change will say so.
        endpoint->dead = true;
        return;
    }
    ReportQueue& queue = endpoint->queue;
    const size_t next = (queue.write + 1) % kQueuedReports;
    if (next != queue.read && length != 0) {
        const size_t bytes = length < kReportBytes ? length : kReportBytes;
        memcpy(queue.data[queue.write], data, bytes);
        queue.length[queue.write] = static_cast<uint8_t>(bytes);
        queue.write = next;
    }
    (void)usb::hc::submit_interrupt(device, address, endpoint->max_packet, &report_arrived);
}

bool stale(const Endpoint& endpoint) {
    return endpoint.device == nullptr || !endpoint.device->present ||
        endpoint.device->generation != endpoint.generation;
}

// Clears a stalled endpoint and starts reading it again.
void recover(Endpoint& endpoint) {
    if (!endpoint.stalled || stale(endpoint)) {
        return;
    }
    endpoint.stalled = false;
    (void)usb::hc::reset_endpoint(*endpoint.device, endpoint.address);
    (void)usb::clear_halt(*endpoint.device, endpoint.address);
    (void)usb::hc::submit_interrupt(*endpoint.device, endpoint.address, endpoint.max_packet, &report_arrived);
}

bool class_request(Device& device, uint8_t interface, uint8_t request, uint16_t value, void* data, uint16_t length) {
    usb::SetupPacket setup = {};
    setup.request_type = usb::kRequestClass | usb::kRequestToInterface;
    setup.request = request;
    setup.value = value;
    setup.index = interface;
    setup.length = length;
    return usb::hc::control(device, setup, data) == usb::status_ok;
}

void send_leds(Keyboard& keyboard) {
    keyboard.leds_dirty = false;
    if (stale(keyboard.endpoint)) {
        return;
    }
    uint8_t report = static_cast<uint8_t>(((g_leds & input::modifier_num_lock) != 0 ? 0x01u : 0u) |
        ((g_leds & input::modifier_caps_lock) != 0 ? 0x02u : 0u) |
        ((g_leds & input::modifier_scroll_lock) != 0 ? 0x04u : 0u));
    (void)class_request(*keyboard.endpoint.device, keyboard.endpoint.interface, kRequestSetReport,
        static_cast<uint16_t>(kReportTypeOutput << 8), &report, 1);
}

// The interrupt IN endpoint among the interface's descriptors.
bool find_interrupt_in(const uint8_t* extra, size_t length, usb::EndpointDescriptor& out) {
    size_t offset = 0;
    while (offset + 2 <= length && extra[offset] >= 2) {
        if (extra[offset + 1] == usb::kDescriptorEndpoint && extra[offset] >= sizeof(usb::EndpointDescriptor)) {
            usb::EndpointDescriptor endpoint = {};
            memcpy(&endpoint, extra + offset, sizeof(endpoint));
            if ((endpoint.address & 0x80) != 0 && (endpoint.attributes & 0x03) == usb::kTransferInterrupt) {
                out = endpoint;
                return true;
            }
        }
        offset += extra[offset];
    }
    return false;
}

// The report descriptor's length, from the HID descriptor.
uint16_t report_descriptor_length(const uint8_t* extra, size_t length) {
    size_t offset = 0;
    while (offset + 2 <= length && extra[offset] >= 2) {
        if (extra[offset + 1] == usb::kDescriptorHid && extra[offset] >= 9) {
            const uint8_t count = extra[offset + 5];
            for (uint8_t i = 0; i < count && offset + 9 + i * 3u <= offset + extra[offset]; ++i) {
                const uint8_t* entry = extra + offset + 6 + i * 3;
                if (entry[0] == usb::kDescriptorHidReport) {
                    return static_cast<uint16_t>(entry[1] | (entry[2] << 8));
                }
            }
        }
        offset += extra[offset];
    }
    return 0;
}

// Finds the pointer's fields in its report descriptor. false if there is no
// X and Y to read.
bool parse_pointer(Pointer& pointer, const uint8_t* descriptor, int length) {
    bool have_x = false;
    bool have_y = false;
    hid_item item = {};
    hid_data* data = hid_start_parse(descriptor, length, hid_input);
    if (data == nullptr) {
        return false;
    }
    while (hid_get_item(data, &item) != 0) {
        if (item.kind != hid_input || (item.flags & HIO_CONST) != 0) {
            continue;
        }
        if (!have_x && item.usage == HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X)) {
            have_x = true;
            pointer.x = item.loc;
            pointer.report_id = static_cast<uint8_t>(item.report_ID);
            pointer.absolute = (item.flags & HIO_RELATIVE) == 0;
            pointer.x_min = item.logical_minimum;
            pointer.x_max = item.logical_maximum;
        } else if (!have_y && item.usage == HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y) &&
            item.report_ID == pointer.report_id) {
            have_y = true;
            pointer.y = item.loc;
            pointer.y_min = item.logical_minimum;
            pointer.y_max = item.logical_maximum;
        } else if (item.usage == HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_WHEEL) && pointer.wheel.size == 0) {
            pointer.wheel = item.loc;
        } else if ((item.usage >> 16) == HUP_BUTTON) {
            const uint32_t button = item.usage & 0xffffu;
            if (button >= 1 && button <= kMaxButtons && pointer.buttons[button - 1].size == 0) {
                pointer.buttons[button - 1] = item.loc;
            }
        }
    }
    hid_end_parse(data);
    // The fields found before X may belong to another report; a pointer
    // whose buttons and position share one report is what this reads.
    return have_x && have_y;
}

bool start_endpoint(Endpoint& endpoint, Device& device, uint8_t interface, const usb::EndpointDescriptor& descriptor) {
    endpoint = {};
    endpoint.device = &device;
    endpoint.generation = device.generation;
    endpoint.interface = interface;
    endpoint.address = descriptor.address;
    endpoint.max_packet = static_cast<uint16_t>(descriptor.max_packet_size & 0x7ff);
    if (endpoint.max_packet > kReportBytes) {
        endpoint.max_packet = kReportBytes;
    }
    return usb::hc::add_endpoint(device, descriptor) &&
        usb::hc::submit_interrupt(device, descriptor.address, endpoint.max_packet, &report_arrived);
}

bool probe_keyboard(Device& device, const usb::InterfaceDescriptor& interface,
    const usb::EndpointDescriptor& endpoint) {
    Keyboard* keyboard = nullptr;
    for (Keyboard& candidate : g_keyboards) {
        if (!candidate.active) {
            keyboard = &candidate;
            break;
        }
    }
    if (keyboard == nullptr) {
        return false;
    }
    *keyboard = {};
    (void)class_request(device, interface.interface_number, kRequestSetProtocol, 0, nullptr, 0);
    (void)class_request(device, interface.interface_number, kRequestSetIdle, 0, nullptr, 0);
    if (!start_endpoint(keyboard->endpoint, device, interface.interface_number, endpoint)) {
        return false;
    }
    keyboard->active = true;
    keyboard->leds_dirty = g_leds != 0;
    console::printf("usb-hid: keyboard on %x:%x\n", static_cast<unsigned>(device.descriptor.vendor_id),
        static_cast<unsigned>(device.descriptor.product_id));
    return true;
}

bool probe_pointer(Device& device, const usb::InterfaceDescriptor& interface,
    const usb::EndpointDescriptor& endpoint, const uint8_t* extra, size_t extra_length) {
    Pointer* pointer = nullptr;
    for (Pointer& candidate : g_pointers) {
        if (!candidate.active) {
            pointer = &candidate;
            break;
        }
    }
    if (pointer == nullptr) {
        return false;
    }
    *pointer = {};
    // The report descriptor first: it gives a mouse its wheel, and a tablet
    // has nothing else. Only a boot mouse without one falls back to the
    // fixed boot report.
    const uint16_t length = report_descriptor_length(extra, extra_length);
    bool parsed = false;
    if (length != 0 && length <= kMaxReportDescriptor) {
        static uint8_t descriptor[kMaxReportDescriptor];
        usb::SetupPacket setup = {};
        setup.request_type = usb::kRequestIn | usb::kRequestToInterface;
        setup.request = usb::kRequestGetDescriptor;
        setup.value = static_cast<uint16_t>(usb::kDescriptorHidReport << 8);
        setup.index = interface.interface_number;
        setup.length = length;
        uint32_t actual = 0;
        if (usb::hc::control(device, setup, descriptor, &actual) == usb::status_ok) {
            parsed = parse_pointer(*pointer, descriptor, static_cast<int>(actual));
        }
    }
    if (!parsed) {
        if (interface.interface_subclass != kSubclassBoot || interface.interface_protocol != kProtocolMouse) {
            return false;
        }
        *pointer = {};
        pointer->boot = true;
        (void)class_request(device, interface.interface_number, kRequestSetProtocol, 0, nullptr, 0);
    } else if (interface.interface_subclass == kSubclassBoot) {
        // Report protocol, which is what the descriptor describes.
        (void)class_request(device, interface.interface_number, kRequestSetProtocol, 1, nullptr, 0);
    }
    if (!start_endpoint(pointer->endpoint, device, interface.interface_number, endpoint)) {
        return false;
    }
    pointer->active = true;
    console::printf("usb-hid: %s on %x:%x\n",
        pointer->boot ? "boot mouse" : (pointer->absolute ? "absolute pointer" : "mouse"),
        static_cast<unsigned>(device.descriptor.vendor_id), static_cast<unsigned>(device.descriptor.product_id));
    return true;
}

bool probe(Device& device, const usb::InterfaceDescriptor& interface, const uint8_t* extra, size_t extra_length) {
    if (interface.interface_class != usb::kClassHid) {
        return false;
    }
    usb::EndpointDescriptor endpoint = {};
    if (!find_interrupt_in(extra, extra_length, endpoint)) {
        return false;
    }
    if (interface.interface_subclass == kSubclassBoot && interface.interface_protocol == kProtocolKeyboard) {
        return probe_keyboard(device, interface, endpoint);
    }
    return probe_pointer(device, interface, endpoint, extra, extra_length);
}

void detach(Device& device) {
    for (Keyboard& keyboard : g_keyboards) {
        if (keyboard.active && keyboard.endpoint.device == &device) {
            // Whatever it held down is up now.
            release_keys(keyboard);
            keyboard = {};
        }
    }
    for (Pointer& pointer : g_pointers) {
        if (pointer.active && pointer.endpoint.device == &device) {
            pointer = {};
        }
    }
}

bool next_report(Endpoint& endpoint, uint8_t* out, size_t& length) {
    ReportQueue& queue = endpoint.queue;
    if (queue.read == queue.write) {
        return false;
    }
    length = queue.length[queue.read];
    memcpy(out, queue.data[queue.read], length);
    queue.read = (queue.read + 1) % kQueuedReports;
    return true;
}

void poll() {
    uint8_t report[kReportBytes] = {};
    size_t length = 0;
    const uint64_t now = now_ms();
    for (Keyboard& keyboard : g_keyboards) {
        if (!keyboard.active) {
            continue;
        }
        while (next_report(keyboard.endpoint, report, length)) {
            handle_keyboard_report(keyboard, report, length);
        }
        recover(keyboard.endpoint);
        if (keyboard.leds_dirty) {
            send_leds(keyboard);
        }
        if (keyboard.repeat_usage != 0 && now >= keyboard.repeat_at) {
            submit_usage(keyboard.repeat_usage, true);
            keyboard.repeat_at = now + kRepeatPeriodMs;
        }
    }
    for (Pointer& pointer : g_pointers) {
        if (!pointer.active) {
            continue;
        }
        while (next_report(pointer.endpoint, report, length)) {
            handle_pointer_report(pointer, report, length);
        }
        recover(pointer.endpoint);
    }
}

const usb::ClassDriver kHidDriver = {
    "usb-hid",
    &probe,
    &detach,
    &poll,
    nullptr,
};

void input_poll() {
    usb::poll();
}

// Called with the kernel lock held, maybe from an interrupt: only record.
void set_keyboard_leds(uint32_t lock_modifiers) {
    g_leds = lock_modifiers;
    for (Keyboard& keyboard : g_keyboards) {
        if (keyboard.active) {
            keyboard.leds_dirty = true;
        }
    }
}

void set_screen_extent(uint32_t width, uint32_t height) {
    g_screen_width = width;
    g_screen_height = height;
}

void reset_pointer() {
    for (Pointer& pointer : g_pointers) {
        pointer.have_position = false;
    }
}

} // namespace

namespace usb {

void register_hid_driver() {
    (void)register_class_driver(kHidDriver);
}

void start_input() {
    if (!ready() || g_input_registered) {
        return;
    }
    // Registered after ps2 and virtio-input, so it never silences them; and
    // registered even with no keyboard plugged in, so one plugged in later
    // works.
    g_input_registered = input::register_driver({
        .name = "usb-hid",
        .keyboard = true,
        .pointer = true,
        .poll = &input_poll,
        .set_keyboard_leds = &set_keyboard_leds,
        .set_screen_extent = &set_screen_extent,
        .reset_pointer = &reset_pointer,
    });
}

} // namespace usb
