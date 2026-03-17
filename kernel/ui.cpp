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
#include "shared/syscall.h"

namespace {

constexpr size_t kInputQueueCapacity = 128;
constexpr size_t kMouseQueueCapacity = 128;

device::Device g_framebuffer_device = {
    .name = "fb0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
};

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

bool present_pixels_internal(const void* pixels, size_t byte_count) {
    if (virtio_gpu::ready()) {
        return virtio_gpu::present_from_kernel(pixels, byte_count);
    }
    return console::present_pixels(pixels, byte_count);
}

bool present_region_internal(const void* pixels, uint32_t source_pitch, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (virtio_gpu::ready()) {
        return virtio_gpu::present_region_from_kernel(pixels, source_pitch, x, y, width, height);
    }
    return console::present_region(pixels, source_pitch, x, y, width, height);
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

int framebuffer_ioctl(uint64_t request, uint64_t argument) {
    switch (request) {
        case FB_IOC_GET_INFO: {
            if (!process::validate_user_range(argument, sizeof(savanxp_fb_info), true)) {
                return negative_error(SAVANXP_EINVAL);
            }

            savanxp_fb_info info = {};
            info = g_framebuffer_info;
            return process::copy_to_user(argument, &info, sizeof(info)) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        case FB_IOC_ACQUIRE: {
            if (g_framebuffer_info.width == 0 || g_framebuffer_info.height == 0 || g_framebuffer_info.pitch == 0) {
                return negative_error(SAVANXP_ENODEV);
            }

            const uint32_t pid = process::current_pid();
            if (pid == 0) {
                return negative_error(SAVANXP_EBADF);
            }
            if (!ui::acquire_graphics_session(pid)) {
                return negative_error(SAVANXP_EBUSY);
            }
            return 0;
        }
        case FB_IOC_RELEASE:
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            ui::release_graphics_session(process::current_pid());
            return 0;
        case FB_IOC_PRESENT: {
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }

            const size_t byte_count = static_cast<size_t>(g_framebuffer_info.pitch) * g_framebuffer_info.height;
            if (!process::validate_user_range(argument, byte_count, false)) {
                return negative_error(SAVANXP_EINVAL);
            }

            return present_pixels_internal(reinterpret_cast<const void*>(argument), byte_count)
                ? 0
                : negative_error(SAVANXP_EINVAL);
        }
        case FB_IOC_PRESENT_REGION: {
            savanxp_fb_present_region region = {};
            if (!ui::owns_graphics_session(process::current_pid())) {
                return negative_error(SAVANXP_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(region), true) ||
                !process::copy_from_user(&region, argument, sizeof(region))) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (region.pixels == 0 || region.source_pitch == 0 || region.width == 0 || region.height == 0) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (region.x >= g_framebuffer_info.width || region.y >= g_framebuffer_info.height) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (region.width > (g_framebuffer_info.width - region.x) || region.height > (g_framebuffer_info.height - region.y)) {
                return negative_error(SAVANXP_EINVAL);
            }

            const uint64_t row_bytes = static_cast<uint64_t>(region.width) * sizeof(uint32_t);
            if (region.source_pitch < row_bytes) {
                return negative_error(SAVANXP_EINVAL);
            }

            const uint64_t last_row = static_cast<uint64_t>(region.y + region.height - 1);
            const uint64_t touched_bytes = (last_row * region.source_pitch) +
                (static_cast<uint64_t>(region.x) * sizeof(uint32_t)) +
                row_bytes;
            if (!process::validate_user_range(region.pixels, touched_bytes, false)) {
                return negative_error(SAVANXP_EINVAL);
            }

            return present_region_internal(
                reinterpret_cast<const void*>(region.pixels),
                region.source_pitch,
                region.x,
                region.y,
                region.width,
                region.height
            ) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        default:
            return negative_error(SAVANXP_ENOSYS);
    }
}

void framebuffer_close() {
    if (ui::owns_graphics_session(process::current_pid())) {
        ui::release_graphics_session(process::current_pid());
    }
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

    g_framebuffer_device.ioctl = framebuffer_ioctl;
    g_framebuffer_device.close = framebuffer_close;
    g_input_device.read = read_input_device;
    g_input_device.close = nullptr;
    g_mouse_device.read = read_mouse_device;
    g_mouse_device.close = nullptr;

    if (!device::register_node("/dev/fb0", &g_framebuffer_device, true)) {
        return;
    }
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
