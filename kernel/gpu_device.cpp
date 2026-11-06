#include "kernel/gpu_device.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/device.hpp"
#include "kernel/display.hpp"
#include "kernel/heap.hpp"
#include "kernel/process.hpp"
#include "kernel/ui.hpp"
#include "abi/savanxp_abi.h"

namespace {

device::Device g_gpu_device = {
    .name = "gpu0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
    .can_read = nullptr,
};

int negative_error(sx_status code) {
    return -static_cast<int>(code);
}

bool owns_session() {
    return display::owns_session(process::current_pid());
}

bool gpu_ioctl_is_query(uint64_t request) {
    switch (request) {
        case GPU_IOC_GET_INFO:
        case GPU_IOC_GET_STATS:
        case GPU_IOC_GET_PRESENT_TIMELINE:
        case GPU_IOC_WAIT_PRESENT:
        case GPU_IOC_GET_CONNECTOR_PROPERTIES:
        case GPU_IOC_GET_SCANOUTS:
        case GPU_IOC_3D_GET_CAPSET:
            return true;
        default:
            return false;
    }
}

// The 3D ioctls (docs/roadmaps/open/VIRGL_ROADMAP.md). None of them looks at
// the display session: a context belongs to the process that made it.
int gpu3d_ioctl(uint64_t request, uint64_t argument) {
    const uint32_t pid = process::current_pid();
    switch (request) {
        case GPU_IOC_3D_GET_CAPSET: {
            savanxp_gpu_3d_capset capset = {};
            if (!process::validate_user_range(argument, sizeof(capset), true) ||
                !process::copy_from_user(&capset, argument, sizeof(capset))) {
                return negative_error(SX_EINVAL);
            }
            const uint8_t* data = nullptr;
            uint32_t size = 0;
            const int status = display::gpu3d_capset(capset.capset_id, capset.capset_version, data, size);
            if (status != 0) {
                return status;
            }
            if (capset.data != 0) {
                const uint32_t copied = size < capset.size ? size : capset.size;
                if (!process::validate_user_range(capset.data, copied, true) ||
                    !process::copy_to_user(capset.data, data, copied)) {
                    return negative_error(SX_EINVAL);
                }
            }
            capset.size = size;
            return process::copy_to_user(argument, &capset, sizeof(capset)) ? 0 : negative_error(SX_EINVAL);
        }
        case GPU_IOC_3D_CREATE_CONTEXT: {
            savanxp_gpu_3d_context context = {};
            if (!process::validate_user_range(argument, sizeof(context), true) ||
                !process::copy_from_user(&context, argument, sizeof(context))) {
                return negative_error(SX_EINVAL);
            }
            context.debug_name[sizeof(context.debug_name) - 1u] = '\0';
            const int status = display::gpu3d_create_context(pid, context.debug_name, context.context_id);
            if (status != 0) {
                return status;
            }
            return process::copy_to_user(argument, &context, sizeof(context)) ? 0 : negative_error(SX_EINVAL);
        }
        case GPU_IOC_3D_DESTROY_CONTEXT:
            return display::gpu3d_destroy_context(pid, static_cast<uint32_t>(argument));
        case GPU_IOC_3D_CREATE_RESOURCE: {
            savanxp_gpu_3d_resource resource = {};
            if (!process::validate_user_range(argument, sizeof(resource), true) ||
                !process::copy_from_user(&resource, argument, sizeof(resource))) {
                return negative_error(SX_EINVAL);
            }
            const int status = display::gpu3d_create_resource(pid, resource);
            if (status != 0) {
                return status;
            }
            return process::copy_to_user(argument, &resource, sizeof(resource)) ? 0 : negative_error(SX_EINVAL);
        }
        case GPU_IOC_3D_DESTROY_RESOURCE: {
            savanxp_gpu_3d_resource_ref ref = {};
            if (!process::validate_user_range(argument, sizeof(ref), false) ||
                !process::copy_from_user(&ref, argument, sizeof(ref))) {
                return negative_error(SX_EINVAL);
            }
            return display::gpu3d_destroy_resource(pid, ref.context_id, ref.resource_id);
        }
        case GPU_IOC_3D_SUBMIT: {
            savanxp_gpu_3d_submit submit = {};
            if (!process::validate_user_range(argument, sizeof(submit), false) ||
                !process::copy_from_user(&submit, argument, sizeof(submit))) {
                return negative_error(SX_EINVAL);
            }
            if (submit.size == 0 || (submit.size % 4u) != 0 || submit.size > SAVANXP_GPU_3D_MAX_SUBMIT_BYTES ||
                !process::validate_user_range(submit.commands, submit.size, false)) {
                return negative_error(SX_EINVAL);
            }
            void* commands = heap::allocate(submit.size, alignof(uint32_t));
            if (commands == nullptr) {
                return negative_error(SX_ENOMEM);
            }
            int status = negative_error(SX_EINVAL);
            if (process::copy_from_user(commands, submit.commands, submit.size)) {
                status = display::gpu3d_submit(pid, submit.context_id, commands, submit.size);
            }
            heap::free(commands);
            return status;
        }
        case GPU_IOC_3D_TRANSFER: {
            savanxp_gpu_3d_transfer transfer = {};
            if (!process::validate_user_range(argument, sizeof(transfer), false) ||
                !process::copy_from_user(&transfer, argument, sizeof(transfer))) {
                return negative_error(SX_EINVAL);
            }
            return display::gpu3d_transfer(pid, transfer);
        }
        default:
            return negative_error(SX_ENOSYS);
    }
}

bool gpu_ioctl_is_3d(uint64_t request) {
    return request >= GPU_IOC_3D_GET_CAPSET && request <= GPU_IOC_3D_TRANSFER;
}

int gpu_ioctl(uint64_t request, uint64_t argument, uint32_t granted_access) {
    if (!gpu_ioctl_is_query(request) && (granted_access & object::access_write) == 0) {
        return negative_error(SX_EACCES);
    }
    if (gpu_ioctl_is_3d(request)) {
        return gpu3d_ioctl(request, argument);
    }
    switch (request) {
        case GPU_IOC_GET_INFO: {
            savanxp_gpu_info info = {};
            if (!process::validate_user_range(argument, sizeof(info), true)) {
                return negative_error(SX_EINVAL);
            }
            if (!display::get_info(info)) {
                return negative_error(SX_ENODEV);
            }
            return process::copy_to_user(argument, &info, sizeof(info)) ? 0 : negative_error(SX_EINVAL);
        }
        case GPU_IOC_ACQUIRE: {
            const uint32_t pid = process::current_pid();
            if (pid == 0) {
                return negative_error(SX_EBADF);
            }
            return display::acquire_session(pid) ? 0 : negative_error(SX_EBUSY);
        }
        case GPU_IOC_RELEASE: {
            const uint32_t pid = process::current_pid();
            if (!display::owns_session(pid)) {
                return negative_error(SX_EBUSY);
            }
            display::release_session_resources();
            display::release_session(pid);
            return 0;
        }
        case GPU_IOC_PRESENT: {
            if (!owns_session()) {
                return negative_error(SX_EBUSY);
            }
            const savanxp_fb_info& fb = display::framebuffer_info();
            if (!process::validate_user_range(argument, fb.buffer_size, false)) {
                return negative_error(SX_EINVAL);
            }
            return display::present(reinterpret_cast<const void*>(argument), fb.buffer_size)
                ? 0
                : negative_error(SX_EIO);
        }
        case GPU_IOC_PRESENT_REGION: {
            savanxp_gpu_present_region region = {};
            if (!owns_session()) {
                return negative_error(SX_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(region), true) ||
                !process::copy_from_user(&region, argument, sizeof(region))) {
                return negative_error(SX_EINVAL);
            }
            const savanxp_fb_info& fb = display::framebuffer_info();
            if (region.pixels == 0 || region.source_pitch == 0 || region.width == 0 || region.height == 0 ||
                region.x >= fb.width || region.y >= fb.height ||
                region.width > (fb.width - region.x) ||
                region.height > (fb.height - region.y)) {
                return negative_error(SX_EINVAL);
            }

            const uint64_t row_bytes = static_cast<uint64_t>(region.width) * sizeof(uint32_t);
            const uint64_t last_row = static_cast<uint64_t>(region.y + region.height - 1);
            const uint64_t touched_bytes = (last_row * region.source_pitch) +
                (static_cast<uint64_t>(region.x) * sizeof(uint32_t)) + row_bytes;
            if (!process::validate_user_range(region.pixels, touched_bytes, false)) {
                return negative_error(SX_EINVAL);
            }

            return display::present_region(
                reinterpret_cast<const void*>(region.pixels),
                region.source_pitch,
                region.x,
                region.y,
                region.width,
                region.height
            ) ? 0 : negative_error(SX_EIO);
        }
        case GPU_IOC_SET_MODE: {
            savanxp_gpu_mode mode = {};
            if (!owns_session()) {
                return negative_error(SX_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(mode), true) ||
                !process::copy_from_user(&mode, argument, sizeof(mode))) {
                return negative_error(SX_EINVAL);
            }
            if (!display::set_mode(mode)) {
                return negative_error(SX_EIO);
            }
            // Unico punto por donde userland muta el modo: aca se repone la
            // geometria que otros subsistemas tienen cacheada.
            ui::sync_framebuffer_geometry();
            return process::copy_to_user(argument, &mode, sizeof(mode)) ? 0 : negative_error(SX_EINVAL);
        }
        case GPU_IOC_IMPORT_SECTION: {
            savanxp_gpu_surface_import import_request = {};
            if (!owns_session()) {
                return negative_error(SX_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(import_request), true) ||
                !process::copy_from_user(&import_request, argument, sizeof(import_request))) {
                return negative_error(SX_EINVAL);
            }
            if (!display::import_surface(import_request)) {
                return negative_error(SX_EIO);
            }
            return process::copy_to_user(argument, &import_request, sizeof(import_request)) ? 0 : negative_error(SX_EINVAL);
        }
        case GPU_IOC_RELEASE_SURFACE: {
            if (!owns_session()) {
                return negative_error(SX_EBUSY);
            }
            return display::release_surface(static_cast<uint32_t>(argument)) ? 0 : negative_error(SX_EBADF);
        }
        case GPU_IOC_PRESENT_SURFACE_REGION: {
            savanxp_gpu_surface_present present = {};
            if (!owns_session()) {
                return negative_error(SX_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(present), true) ||
                !process::copy_from_user(&present, argument, sizeof(present))) {
                return negative_error(SX_EINVAL);
            }
            return display::present_surface_region(present) ? 0 : negative_error(SX_EIO);
        }
        case GPU_IOC_WAIT_IDLE: {
            if (!owns_session()) {
                return negative_error(SX_EBUSY);
            }
            display::wait_for_idle();
            return 0;
        }
        case GPU_IOC_GET_STATS: {
            savanxp_gpu_stats stats = {};
            if (!process::validate_user_range(argument, sizeof(stats), true)) {
                return negative_error(SX_EINVAL);
            }
            if (!display::get_stats(stats)) {
                return negative_error(SX_ENODEV);
            }
            return process::copy_to_user(argument, &stats, sizeof(stats)) ? 0 : negative_error(SX_EINVAL);
        }
        case GPU_IOC_GET_PRESENT_TIMELINE: {
            savanxp_gpu_present_timeline timeline = {};
            if (!process::validate_user_range(argument, sizeof(timeline), true)) {
                return negative_error(SX_EINVAL);
            }
            if (!display::get_present_timeline(timeline)) {
                return negative_error(SX_ENODEV);
            }
            return process::copy_to_user(argument, &timeline, sizeof(timeline)) ? 0 : negative_error(SX_EINVAL);
        }
        case GPU_IOC_WAIT_PRESENT: {
            savanxp_gpu_present_wait wait_request = {};
            if (!process::validate_user_range(argument, sizeof(wait_request), true) ||
                !process::copy_from_user(&wait_request, argument, sizeof(wait_request))) {
                return negative_error(SX_EINVAL);
            }
            if (!display::wait_present(wait_request)) {
                return negative_error(SX_EIO);
            }
            return process::copy_to_user(argument, &wait_request, sizeof(wait_request)) ? 0 : negative_error(SX_EINVAL);
        }
        case GPU_IOC_GET_CONNECTOR_PROPERTIES: {
            savanxp_gpu_connector_properties properties = {};
            if (!process::validate_user_range(argument, sizeof(properties), true)) {
                return negative_error(SX_EINVAL);
            }
            if (!display::get_connector_properties(properties)) {
                return negative_error(SX_ENODEV);
            }
            return process::copy_to_user(argument, &properties, sizeof(properties)) ? 0 : negative_error(SX_EINVAL);
        }
        case GPU_IOC_GET_SCANOUTS: {
            savanxp_gpu_scanout_state state = {};
            if (!process::validate_user_range(argument, sizeof(state), true)) {
                return negative_error(SX_EINVAL);
            }
            if (!display::get_scanouts(state)) {
                return negative_error(SX_ENODEV);
            }
            return process::copy_to_user(argument, &state, sizeof(state)) ? 0 : negative_error(SX_EINVAL);
        }
        case GPU_IOC_REFRESH_SCANOUTS:
            // Re-arma el scanout activo (muta estado del dispositivo), asi que
            // exige la sesion grafica igual que el resto de ioctls mutantes.
            if (!owns_session()) {
                return negative_error(SX_EBUSY);
            }
            return display::refresh_scanouts() ? 0 : negative_error(SX_EIO);
        case GPU_IOC_PRESENT_SURFACE_BATCH: {
            savanxp_gpu_surface_present_batch batch = {};
            if (!owns_session()) {
                return negative_error(SX_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(batch), true) ||
                !process::copy_from_user(&batch, argument, sizeof(batch))) {
                return negative_error(SX_EINVAL);
            }
            return display::present_surface_batch(batch) ? 0 : negative_error(SX_EIO);
        }
        case GPU_IOC_CREATE_PRESENT_EVENT:
            return display::create_present_event();
        case GPU_IOC_CREATE_SCANOUT_EVENT:
            return display::create_scanout_event();
        case GPU_IOC_SET_CURSOR: {
            savanxp_gpu_cursor_image image = {};
            if (!owns_session()) {
                return negative_error(SX_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(image), true) ||
                !process::copy_from_user(&image, argument, sizeof(image))) {
                return negative_error(SX_EINVAL);
            }
            return display::set_cursor(image) ? 0 : negative_error(SX_ENODEV);
        }
        case GPU_IOC_MOVE_CURSOR: {
            savanxp_gpu_cursor_position position = {};
            if (!owns_session()) {
                return negative_error(SX_EBUSY);
            }
            if (!process::validate_user_range(argument, sizeof(position), true) ||
                !process::copy_from_user(&position, argument, sizeof(position))) {
                return negative_error(SX_EINVAL);
            }
            return display::move_cursor(position) ? 0 : negative_error(SX_ENODEV);
        }
        default:
            return negative_error(SX_ENOSYS);
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
