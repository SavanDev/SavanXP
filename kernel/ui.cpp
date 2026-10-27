#include "kernel/ui.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/boot_screen.hpp"
#include "kernel/console.hpp"
#include "kernel/device.hpp"
#include "kernel/display.hpp"
#include "kernel/input.hpp"
#include "kernel/object.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "abi/savanxp_abi.h"

namespace {

constexpr size_t kInputQueueCapacity = 512;
constexpr size_t kMouseQueueCapacity = 512;

device::Device g_input_device = {
    .name = "input0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
    .can_read = nullptr,
};

device::Device g_mouse_device = {
    .name = "mouse0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
    .can_read = nullptr,
};

boot::FramebufferInfo g_framebuffer = {};
savanxp_fb_info g_framebuffer_info = {};
savanxp_input_event g_input_queue[kInputQueueCapacity] = {};
size_t g_input_read_index = 0;
size_t g_input_write_index = 0;
size_t g_input_count = 0;
savanxp_mouse_event g_mouse_queue[kMouseQueueCapacity] = {};
size_t g_mouse_read_index = 0;
size_t g_mouse_write_index = 0;
size_t g_mouse_count = 0;
uint32_t g_owner_pid = 0;
bool g_ready = false;
bool g_mouse_available = false;

int negative_error(sx_status code) {
    return -static_cast<int>(code);
}

void reset_input_queue() {
    memset(g_input_queue, 0, sizeof(g_input_queue));
    g_input_read_index = 0;
    g_input_write_index = 0;
    g_input_count = 0;
}

void reset_mouse_queue() {
    memset(g_mouse_queue, 0, sizeof(g_mouse_queue));
    g_mouse_read_index = 0;
    g_mouse_write_index = 0;
    g_mouse_count = 0;
}

void enqueue_event(uint32_t type, uint32_t key, int32_t ascii, uint32_t modifiers) {
    if (g_input_count == kInputQueueCapacity) {
        g_input_read_index = (g_input_read_index + 1) % kInputQueueCapacity;
        g_input_count -= 1;
    }

    g_input_queue[g_input_write_index] = {
        .type = type,
        .key = key,
        .ascii = ascii,
        .modifiers = modifiers,
    };
    g_input_write_index = (g_input_write_index + 1) % kInputQueueCapacity;
    g_input_count += 1;
}

void enqueue_mouse_event(int32_t delta_x, int32_t delta_y, int32_t wheel, uint32_t buttons, int32_t absolute_x, int32_t absolute_y, bool has_absolute) {
    if (g_mouse_count == kMouseQueueCapacity) {
        // Al descartar el mas viejo se arrastran sus ticks de rueda al que pasa
        // a ser el mas viejo, porque un tick de rueda perdido es scroll que no
        // ocurre nunca. Un delta de movimiento perdido, en cambio, si se
        // recupera: el evento que entra trae la posicion verdadera del puntero
        // y el consumidor la asigna, asi que los deltas descartados en el
        // camino dejan de sumar sobre una base equivocada. Por eso el
        // descartado se lleva solo la rueda y nada mas.
        const int32_t dropped_wheel = g_mouse_queue[g_mouse_read_index].wheel;
        g_mouse_read_index = (g_mouse_read_index + 1) % kMouseQueueCapacity;
        g_mouse_count -= 1;
        g_mouse_queue[g_mouse_read_index].wheel += dropped_wheel;
    }

    g_mouse_queue[g_mouse_write_index] = {
        .delta_x = delta_x,
        .delta_y = delta_y,
        .wheel = wheel,
        .buttons = buttons,
        .absolute_x = absolute_x,
        .absolute_y = absolute_y,
        .flags = has_absolute ? SAVANXP_MOUSE_FLAG_ABSOLUTE : 0u,
    };
    g_mouse_write_index = (g_mouse_write_index + 1) % kMouseQueueCapacity;
    g_mouse_count += 1;
}

bool input_device_can_read() {
    return g_input_count != 0;
}

int input_ioctl(uint64_t request, uint64_t argument, uint32_t granted_access) {
    if (request == INPUT_IOC_SET_LAYOUT && (granted_access & object::access_write) == 0) {
        return negative_error(SX_EACCES);
    }
    switch (request) {
        case INPUT_IOC_SET_LAYOUT:
            if (!input::set_keyboard_layout(static_cast<int>(argument))) {
                return negative_error(SX_EINVAL);
            }
            return 0;
        case INPUT_IOC_GET_LAYOUT:
            return input::keyboard_layout();
        default:
            return negative_error(SX_ENOSYS);
    }
}

bool mouse_device_can_read() {
    return g_mouse_count != 0;
}

void release_graphics_session() {
    if (g_owner_pid == 0) {
        return;
    }

    g_owner_pid = 0;
    reset_input_queue();
    reset_mouse_queue();
    console::set_framebuffer_console_enabled(true);
}

int read_input_device(uint64_t user_buffer, size_t count) {
    if (count < sizeof(savanxp_input_event)) {
        return negative_error(SX_EINVAL);
    }
    if (!process::validate_user_range(user_buffer, count, true)) {
        return negative_error(SX_EINVAL);
    }

    size_t copied = 0;
    while (g_input_count != 0 && copied + sizeof(savanxp_input_event) <= count) {
        const savanxp_input_event& event = g_input_queue[g_input_read_index];
        if (!process::copy_to_user(user_buffer + copied, &event, sizeof(event))) {
            return negative_error(SX_EINVAL);
        }
        g_input_read_index = (g_input_read_index + 1) % kInputQueueCapacity;
        g_input_count -= 1;
        copied += sizeof(savanxp_input_event);
    }

    return static_cast<int>(copied);
}

int read_mouse_device(uint64_t user_buffer, size_t count) {
    input::poll_pointers();

    if (count < sizeof(savanxp_mouse_event)) {
        return negative_error(SX_EINVAL);
    }
    if (!process::validate_user_range(user_buffer, count, true)) {
        return negative_error(SX_EINVAL);
    }

    size_t copied = 0;
    while (g_mouse_count != 0 && copied + sizeof(savanxp_mouse_event) <= count) {
        const savanxp_mouse_event& event = g_mouse_queue[g_mouse_read_index];
        if (!process::copy_to_user(user_buffer + copied, &event, sizeof(event))) {
            return negative_error(SX_EINVAL);
        }
        g_mouse_read_index = (g_mouse_read_index + 1) % kMouseQueueCapacity;
        g_mouse_count -= 1;
        copied += sizeof(savanxp_mouse_event);
    }

    return static_cast<int>(copied);
}

} // namespace

namespace ui {

void initialize(const boot::FramebufferInfo& framebuffer) {
    g_framebuffer = framebuffer;
    g_framebuffer_info = {
        .width = static_cast<uint32_t>(framebuffer.width),
        .height = static_cast<uint32_t>(framebuffer.height),
        .pitch = static_cast<uint32_t>(framebuffer.pitch),
        .bpp = framebuffer.bpp,
        .buffer_size = static_cast<uint32_t>(framebuffer.pitch * framebuffer.height),
    };
    reset_input_queue();
    reset_mouse_queue();
    g_owner_pid = 0;
    g_mouse_available = false;

    if (display::ready()) {
        g_framebuffer_info = display::framebuffer_info();
    }
    input::set_screen_extent(g_framebuffer_info.width, g_framebuffer_info.height);

    g_input_device.read = read_input_device;
    g_input_device.can_read = input_device_can_read;
    g_input_device.ioctl = input_ioctl;
    g_input_device.close = nullptr;
    g_mouse_device.read = read_mouse_device;
    g_mouse_device.can_read = mouse_device_can_read;
    g_mouse_device.close = nullptr;
    if (!device::register_node("/dev/input0", &g_input_device, false)) {
        return;
    }
    if (input::pointer_available()) {
        if (!device::register_node("/dev/mouse0", &g_mouse_device, false)) {
            return;
        }
        g_mouse_available = true;
    }

    g_ready = true;
}

bool ready() {
    return g_ready;
}

bool graphics_active() {
    return g_owner_pid != 0;
}

bool acquire_graphics_session(uint32_t pid) {
    if (pid == 0) {
        return false;
    }
    if (g_owner_pid != 0) {
        return g_owner_pid == pid;
    }

    g_owner_pid = pid;
    reset_input_queue();
    reset_mouse_queue();
    input::reset_pointers();
    console::set_framebuffer_console_enabled(false);
    // A partir de aca la pantalla es del cliente grafico: el splash del
    // arranque tiene que dejar de repintar su barra por debajo.
    boot_screen::finish();
    return true;
}

void release_graphics_session(uint32_t pid) {
    if (g_owner_pid == 0 || g_owner_pid != pid) {
        return;
    }
    if (display::ready()) {
        display::wait_for_idle();
    }
    input::reset_pointers();
    ::release_graphics_session();
}

bool owns_graphics_session(uint32_t pid) {
    return g_owner_pid != 0 && g_owner_pid == pid;
}

void handle_key_event(uint32_t key, bool pressed, char ascii, uint32_t modifiers) {
    if (!graphics_active()) {
        return;
    }
    enqueue_event(
        pressed ? SAVANXP_INPUT_EVENT_KEY_DOWN : SAVANXP_INPUT_EVENT_KEY_UP,
        key,
        ascii,
        modifiers
    );
}

void handle_mouse_event(int32_t delta_x, int32_t delta_y, int32_t wheel, uint32_t buttons, int32_t absolute_x, int32_t absolute_y, bool has_absolute) {
    if (!graphics_active() || !g_mouse_available) {
        return;
    }
    enqueue_mouse_event(delta_x, delta_y, wheel, buttons, absolute_x, absolute_y, has_absolute);
}

void sync_framebuffer_geometry() {
    if (!display::ready()) {
        return;
    }
    g_framebuffer_info = display::framebuffer_info();
    // An absolute pointer maps its range onto the screen size: with the old
    // extent, after a mode change the cursor would point somewhere else.
    input::set_screen_extent(g_framebuffer_info.width, g_framebuffer_info.height);
}

bool framebuffer_available() {
    return g_framebuffer_info.width != 0 && g_framebuffer_info.height != 0 && g_framebuffer_info.pitch != 0;
}

const savanxp_fb_info& framebuffer_info() {
    return g_framebuffer_info;
}

bool mouse_available() {
    return g_mouse_available;
}

} // namespace ui
