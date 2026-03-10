#include "kernel/ui.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/device.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "shared/syscall.h"

namespace {

constexpr size_t kInputQueueCapacity = 128;

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

boot::FramebufferInfo g_framebuffer = {};
savanxp_input_event g_input_queue[kInputQueueCapacity] = {};
size_t g_input_read_index = 0;
size_t g_input_write_index = 0;
size_t g_input_count = 0;
uint32_t g_owner_pid = 0;
bool g_ready = false;

int negative_error(savanxp_error_code code) {
    return -static_cast<int>(code);
}

void reset_input_queue() {
    memset(g_input_queue, 0, sizeof(g_input_queue));
    g_input_read_index = 0;
    g_input_write_index = 0;
    g_input_count = 0;
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

void release_graphics_session() {
    if (g_owner_pid == 0) {
        return;
    }

    g_owner_pid = 0;
    reset_input_queue();
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

int framebuffer_ioctl(uint64_t request, uint64_t argument) {
    switch (request) {
        case FB_IOC_GET_INFO: {
            if (!process::validate_user_range(argument, sizeof(savanxp_fb_info), true)) {
                return negative_error(SAVANXP_EINVAL);
            }

            savanxp_fb_info info = {};
            info.width = static_cast<uint32_t>(g_framebuffer.width);
            info.height = static_cast<uint32_t>(g_framebuffer.height);
            info.pitch = static_cast<uint32_t>(g_framebuffer.pitch);
            info.bpp = g_framebuffer.bpp;
            info.buffer_size = static_cast<uint32_t>(g_framebuffer.pitch * g_framebuffer.height);
            return process::copy_to_user(argument, &info, sizeof(info)) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        case FB_IOC_ACQUIRE: {
            if (!console::framebuffer_ready()) {
                return negative_error(SAVANXP_ENODEV);
            }

            const uint32_t pid = process::current_pid();
            if (pid == 0) {
                return negative_error(SAVANXP_EBADF);
            }
            if (g_owner_pid != 0 && g_owner_pid != pid) {
                return negative_error(SAVANXP_EBUSY);
            }

            g_owner_pid = pid;
            reset_input_queue();
            console::set_framebuffer_console_enabled(false);
            return 0;
        }
        case FB_IOC_RELEASE:
            if (g_owner_pid != 0 && g_owner_pid != process::current_pid()) {
                return negative_error(SAVANXP_EBUSY);
            }
            release_graphics_session();
            return 0;
        case FB_IOC_PRESENT: {
            if (g_owner_pid == 0 || g_owner_pid != process::current_pid()) {
                return negative_error(SAVANXP_EBUSY);
            }

            const size_t byte_count = static_cast<size_t>(g_framebuffer.pitch * g_framebuffer.height);
            if (!process::validate_user_range(argument, byte_count, false)) {
                return negative_error(SAVANXP_EINVAL);
            }

            return console::present_pixels(reinterpret_cast<const void*>(argument), byte_count)
                ? 0
                : negative_error(SAVANXP_EINVAL);
        }
        default:
            return negative_error(SAVANXP_ENOSYS);
    }
}

void framebuffer_close() {
    if (g_owner_pid != 0 && g_owner_pid == process::current_pid()) {
        release_graphics_session();
    }
}

} // namespace

namespace ui {

void initialize(const boot::FramebufferInfo& framebuffer) {
    g_framebuffer = framebuffer;
    reset_input_queue();
    g_owner_pid = 0;

    g_framebuffer_device.ioctl = framebuffer_ioctl;
    g_framebuffer_device.close = framebuffer_close;
    g_input_device.read = read_input_device;
    g_input_device.close = nullptr;

    if (!device::register_node("/dev/fb0", &g_framebuffer_device, true)) {
        return;
    }
    if (!device::register_node("/dev/input0", &g_input_device, false)) {
        return;
    }

    g_ready = true;
}

bool ready() {
    return g_ready;
}

bool graphics_active() {
    return g_owner_pid != 0;
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

bool framebuffer_available() {
    return console::framebuffer_ready();
}

} // namespace ui
