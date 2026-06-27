#include "kernel/gpu_device.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/device.hpp"
#include "kernel/display.hpp"
#include "kernel/process.hpp"
#include "savanxp/syscall.h"

namespace {

device::Device g_gpu_device = {
    .name = "gpu0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
    .can_read = nullptr,
};

int negative_error(savanxp_error_code code) {
    return -static_cast<int>(code);
}

bool owns_session() {
    return display::owns_session(process::current_pid());
}

int gpu_ioctl(uint64_t request, uint64_t argument) {
    switch (request) {
        case GPU_IOC_GET_INFO: {
            savanxp_gpu_info info = {};
            if (!process::validate_user_range(argument, sizeof(info), true)) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (!display::get_info(info)) {
                return negative_error(SAVANXP_ENODEV);
            }
            return process::copy_to_user(argument, &info, sizeof(info)) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        case GPU_IOC_ACQUIRE: {
            const uint32_t pid = process::current_pid();
            if (pid == 0) {
                return negative_error(SAVANXP_EBADF);
            }
            return display::acquire_session(pid) ? 0 : negative_error(SAVANXP_EBUSY);
        }
        case GPU_IOC_RELEASE: {
            const uint32_t pid = process::current_pid();
            if (!display::owns_session(pid)) {
                return negative_error(SAVANXP_EBUSY);
            }
            display::release_session_resources();
            display::release_session(pid);
            return 0;
        }
        case GPU_IOC_PRESENT: {
            if (!owns_session()) {
                return negative_error(SAVANXP_EBUSY);
            }
            const savanxp_fb_info& fb = display::framebuffer_info();
            if (!process::validate_user_range(argument, fb.buffer_size, false)) {
                return negative_error(SAVANXP_EINVAL);
            }
            return display::present(reinterpret_cast<const void*>(argument), fb.buffer_size)
                ? 0
                : negative_error(SAVANXP_EIO);
        }
        case GPU_IOC_PRESENT_REGION: {
            savanxp_gpu_present_region region = {};
            if (!owns_session()) {
                return negative_error(SAVANXP_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(region), true) ||
                !process::copy_from_user(&region, argument, sizeof(region))) {
                return negative_error(SAVANXP_EINVAL);
            }
            const savanxp_fb_info& fb = display::framebuffer_info();
            if (region.pixels == 0 || region.source_pitch == 0 || region.width == 0 || region.height == 0 ||
                region.x >= fb.width || region.y >= fb.height ||
                region.width > (fb.width - region.x) ||
                region.height > (fb.height - region.y)) {
                return negative_error(SAVANXP_EINVAL);
            }

            const uint64_t row_bytes = static_cast<uint64_t>(region.width) * sizeof(uint32_t);
            const uint64_t last_row = static_cast<uint64_t>(region.y + region.height - 1);
            const uint64_t touched_bytes = (last_row * region.source_pitch) +
                (static_cast<uint64_t>(region.x) * sizeof(uint32_t)) + row_bytes;
            if (!process::validate_user_range(region.pixels, touched_bytes, false)) {
                return negative_error(SAVANXP_EINVAL);
            }

            return display::present_region(
                reinterpret_cast<const void*>(region.pixels),
                region.source_pitch,
                region.x,
                region.y,
                region.width,
                region.height
            ) ? 0 : negative_error(SAVANXP_EIO);
        }
        case GPU_IOC_SET_MODE: {
            savanxp_gpu_mode mode = {};
            if (!owns_session()) {
                return negative_error(SAVANXP_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(mode), true) ||
                !process::copy_from_user(&mode, argument, sizeof(mode))) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (!display::set_mode(mode)) {
                return negative_error(SAVANXP_EIO);
            }
            return process::copy_to_user(argument, &mode, sizeof(mode)) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        case GPU_IOC_IMPORT_SECTION: {
            savanxp_gpu_surface_import import_request = {};
            if (!owns_session()) {
                return negative_error(SAVANXP_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(import_request), true) ||
                !process::copy_from_user(&import_request, argument, sizeof(import_request))) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (!display::import_surface(import_request)) {
                return negative_error(SAVANXP_EIO);
            }
            return process::copy_to_user(argument, &import_request, sizeof(import_request)) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        case GPU_IOC_RELEASE_SURFACE: {
            if (!owns_session()) {
                return negative_error(SAVANXP_EBUSY);
            }
            return display::release_surface(static_cast<uint32_t>(argument)) ? 0 : negative_error(SAVANXP_EBADF);
        }
        case GPU_IOC_PRESENT_SURFACE_REGION: {
            savanxp_gpu_surface_present present = {};
            if (!owns_session()) {
                return negative_error(SAVANXP_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(present), true) ||
                !process::copy_from_user(&present, argument, sizeof(present))) {
                return negative_error(SAVANXP_EINVAL);
            }
            return display::present_surface_region(present) ? 0 : negative_error(SAVANXP_EIO);
        }
        case GPU_IOC_WAIT_IDLE: {
            if (!owns_session()) {
                return negative_error(SAVANXP_EBUSY);
            }
            display::wait_for_idle();
            return 0;
        }
        case GPU_IOC_GET_STATS: {
            savanxp_gpu_stats stats = {};
            if (!process::validate_user_range(argument, sizeof(stats), true)) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (!display::get_stats(stats)) {
                return negative_error(SAVANXP_ENODEV);
            }
            return process::copy_to_user(argument, &stats, sizeof(stats)) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        case GPU_IOC_GET_PRESENT_TIMELINE: {
            savanxp_gpu_present_timeline timeline = {};
            if (!process::validate_user_range(argument, sizeof(timeline), true)) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (!display::get_present_timeline(timeline)) {
                return negative_error(SAVANXP_ENODEV);
            }
            return process::copy_to_user(argument, &timeline, sizeof(timeline)) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        case GPU_IOC_WAIT_PRESENT: {
            savanxp_gpu_present_wait wait_request = {};
            if (!process::validate_user_range(argument, sizeof(wait_request), true) ||
                !process::copy_from_user(&wait_request, argument, sizeof(wait_request))) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (!display::wait_present(wait_request)) {
                return negative_error(SAVANXP_EIO);
            }
            return process::copy_to_user(argument, &wait_request, sizeof(wait_request)) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        case GPU_IOC_GET_CONNECTOR_PROPERTIES: {
            savanxp_gpu_connector_properties properties = {};
            if (!process::validate_user_range(argument, sizeof(properties), true)) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (!display::get_connector_properties(properties)) {
                return negative_error(SAVANXP_ENODEV);
            }
            return process::copy_to_user(argument, &properties, sizeof(properties)) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        case GPU_IOC_GET_SCANOUTS: {
            savanxp_gpu_scanout_state state = {};
            if (!process::validate_user_range(argument, sizeof(state), true)) {
                return negative_error(SAVANXP_EINVAL);
            }
            if (!display::get_scanouts(state)) {
                return negative_error(SAVANXP_ENODEV);
            }
            return process::copy_to_user(argument, &state, sizeof(state)) ? 0 : negative_error(SAVANXP_EINVAL);
        }
        case GPU_IOC_REFRESH_SCANOUTS:
            return display::refresh_scanouts() ? 0 : negative_error(SAVANXP_EIO);
        case GPU_IOC_PRESENT_SURFACE_BATCH: {
            savanxp_gpu_surface_present_batch batch = {};
            if (!owns_session()) {
                return negative_error(SAVANXP_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(batch), true) ||
                !process::copy_from_user(&batch, argument, sizeof(batch))) {
                return negative_error(SAVANXP_EINVAL);
            }
            return display::present_surface_batch(batch) ? 0 : negative_error(SAVANXP_EIO);
        }
        case GPU_IOC_CREATE_PRESENT_EVENT:
            return display::create_present_event();
        case GPU_IOC_CREATE_SCANOUT_EVENT:
            return display::create_scanout_event();
        case GPU_IOC_SET_CURSOR: {
            savanxp_gpu_cursor_image image = {};
            if (!owns_session()) {
                return negative_error(SAVANXP_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(image), true) ||
                !process::copy_from_user(&image, argument, sizeof(image))) {
                return negative_error(SAVANXP_EINVAL);
            }
            return display::set_cursor(image) ? 0 : negative_error(SAVANXP_ENODEV);
        }
        case GPU_IOC_MOVE_CURSOR: {
            savanxp_gpu_cursor_position position = {};
            if (!owns_session()) {
                return negative_error(SAVANXP_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(position), true) ||
                !process::copy_from_user(&position, argument, sizeof(position))) {
                return negative_error(SAVANXP_EINVAL);
            }
            return display::move_cursor(position) ? 0 : negative_error(SAVANXP_ENODEV);
        }
        default:
            return negative_error(SAVANXP_ENOSYS);
    }
}

void gpu_close() {
    const uint32_t pid = process::current_pid();
    if (display::owns_session(pid)) {
        display::release_session_resources();
        display::release_session(pid);
    }
}

} // namespace

namespace gpu_device {

bool initialize() {
    g_gpu_device.ioctl = gpu_ioctl;
    g_gpu_device.close = gpu_close;
    return device::register_node("/dev/gpu0", &g_gpu_device, true);
}

} // namespace gpu_device
