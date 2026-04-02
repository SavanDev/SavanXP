#include "kernel/ui.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/device.hpp"
#include "kernel/ps2.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "kernel/virtio_gpu.hpp"
#include "kernel/virtio_input.hpp"
#include "savanxp/syscall.h"

namespace {

constexpr size_t kInputQueueCapacity = 128;
constexpr size_t kMouseQueueCapacity = 128;

device::Device g_input_device = {
    .name = "input0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
};

device::Device g_mouse_device = {
    .name = "mouse0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
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

int negative_error(savanxp_error_code code) {
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

void enqueue_event(uint32_t type, uint32_t key, int32_t ascii) {
    if (g_input_count == kInputQueueCapacity) {
        g_input_read_index = (g_input_read_index + 1) % kInputQueueCapacity;
        g_input_count -= 1;
    }

    g_input_queue[g_input_write_index] = {
        .type = type,
        .key = key,
        .ascii = ascii,
    };
    g_input_write_index = (g_input_write_index + 1) % kInputQueueCapacity;
    g_input_count += 1;
}

void enqueue_mouse_event(int32_t delta_x, int32_t delta_y, uint32_t buttons) {
    if (g_mouse_count == kMouseQueueCapacity) {
        g_mouse_read_index = (g_mouse_read_index + 1) % kMouseQueueCapacity;
        g_mouse_count -= 1;
    }

    g_mouse_queue[g_mouse_write_index] = {
        .delta_x = delta_x,
        .delta_y = delta_y,
        .buttons = buttons,
    };
    g_mouse_write_index = (g_mouse_write_index + 1) % kMouseQueueCapacity;
    g_mouse_count += 1;
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
        return negative_error(SAVANXP_EINVAL);
    }
    if (!process::validate_user_range(user_buffer, count, true)) {
        return negative_error(SAVANXP_EINVAL);
    }

    size_t copied = 0;
    while (g_input_count != 0 && copied + sizeof(savanxp_input_event) <= count) {
        const savanxp_input_event& event = g_input_queue[g_input_read_index];
        if (!process::copy_to_user(user_buffer + copied, &event, sizeof(event))) {
            return negative_error(SAVANXP_EINVAL);
        }
        g_input_read_index = (g_input_read_index + 1) % kInputQueueCapacity;
        g_input_count -= 1;
        copied += sizeof(savanxp_input_event);
    }

    return static_cast<int>(copied);
}

int read_mouse_device(uint64_t user_buffer, size_t count) {
    virtio_input::poll();

    if (count < sizeof(savanxp_mouse_event)) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (!process::validate_user_range(user_buffer, count, true)) {
        return negative_error(SAVANXP_EINVAL);
    }

    size_t copied = 0;
    while (g_mouse_count != 0 && copied + sizeof(savanxp_mouse_event) <= count) {
        const savanxp_mouse_event& event = g_mouse_queue[g_mouse_read_index];
        if (!process::copy_to_user(user_buffer + copied, &event, sizeof(event))) {
            return negative_error(SAVANXP_EINVAL);
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

    if (virtio_gpu::ready()) {
        g_framebuffer_info = virtio_gpu::framebuffer_info();
    }
    virtio_input::set_framebuffer_extent(g_framebuffer_info.width, g_framebuffer_info.height);

    g_input_device.read = read_input_device;
    g_input_device.close = nullptr;
    g_mouse_device.read = read_mouse_device;
    g_mouse_device.close = nullptr;
    if (!device::register_node("/dev/input0", &g_input_device, false)) {
        return;
    }
    if (ps2::mouse_ready() || virtio_input::mouse_ready()) {
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
    virtio_input::begin_graphics_session();
    console::set_framebuffer_console_enabled(false);
    return true;
}

void release_graphics_session(uint32_t pid) {
    if (g_owner_pid == 0 || g_owner_pid != pid) {
        return;
    }
    if (virtio_gpu::ready()) {
        virtio_gpu::wait_for_idle();
    }
    virtio_input::end_graphics_session();
    ::release_graphics_session();
}

bool owns_graphics_session(uint32_t pid) {
    return g_owner_pid != 0 && g_owner_pid == pid;
}

void handle_key_event(uint32_t key, bool pressed, char ascii) {
    if (!graphics_active()) {
        return;
    }
    enqueue_event(
        pressed ? SAVANXP_INPUT_EVENT_KEY_DOWN : SAVANXP_INPUT_EVENT_KEY_UP,
        key,
        ascii
    );
}

void handle_mouse_event(int32_t delta_x, int32_t delta_y, uint32_t buttons) {
    if (!graphics_active() || !g_mouse_available) {
        return;
    }
    enqueue_mouse_event(delta_x, delta_y, buttons);
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
