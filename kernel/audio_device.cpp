#include "kernel/audio_device.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/audio.hpp"
#include "kernel/device.hpp"
#include "kernel/process.hpp"
#include "savanxp/syscall.h"

namespace {

device::Device g_audio_device = {
    .name = "audio0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
    .can_read = nullptr,
};

// Un unico escritor a la vez. El primero que escribe adquiere el stream y lo
// mantiene hasta cerrar el fd; cualquier otro proceso recibe EBUSY. Espeja la
// semantica de sesion de /dev/gpu0, pero implicita: no hay ioctl de acquire,
// la primera escritura es la que toma posesion.
uint32_t g_owner_pid = 0;

int negative_error(savanxp_error_code code) {
    return -static_cast<int>(code);
}

int audio_write(uint64_t user_buffer, size_t count) {
    savanxp_audio_info info = {};
    if (!audio::ready() || !audio::get_info(info)) {
        return negative_error(SAVANXP_ENODEV);
    }
    if (count == 0) {
        return 0;
    }
    if (info.frame_bytes == 0 || (count % info.frame_bytes) != 0) {
        return negative_error(SAVANXP_EINVAL);
    }
    if (!process::validate_user_range(user_buffer, count, false)) {
        return negative_error(SAVANXP_EINVAL);
    }

    const uint32_t pid = process::current_pid();
    if (pid == 0) {
        return negative_error(SAVANXP_EBADF);
    }
    if (g_owner_pid != 0 && g_owner_pid != pid) {
        return negative_error(SAVANXP_EBUSY);
    }

    const bool acquired_owner = g_owner_pid == 0;
    if (acquired_owner) {
        g_owner_pid = pid;
    }

    if (!audio::configure()) {
        if (acquired_owner) {
            g_owner_pid = 0;
        }
        audio::stop();
        return negative_error(SAVANXP_EIO);
    }

    const uint32_t period = info.period_bytes != 0 ? info.period_bytes : static_cast<uint32_t>(count);
    size_t transferred = 0;
    while (transferred < count) {
        const size_t remaining = count - transferred;
        const uint32_t chunk = static_cast<uint32_t>(remaining > period ? period : remaining);
        const int result = audio::submit_period(user_buffer + transferred, chunk);
        if (result != 0) {
            return result < 0 ? result : negative_error(SAVANXP_EIO);
        }
        transferred += chunk;
    }

    return static_cast<int>(count);
}

int audio_ioctl(uint64_t request, uint64_t argument) {
    switch (request) {
        case AUDIO_IOC_GET_INFO: {
            savanxp_audio_info info = {};
            if (!audio::ready() || !audio::get_info(info)) {
                return negative_error(SAVANXP_ENODEV);
            }
            if (!process::validate_user_range(argument, sizeof(info), true)) {
                return negative_error(SAVANXP_EINVAL);
            }
            return process::copy_to_user(argument, &info, sizeof(info))
                ? 0
                : negative_error(SAVANXP_EINVAL);
        }
        default:
            return negative_error(SAVANXP_ENOSYS);
    }
}

void audio_close() {
    const uint32_t pid = process::current_pid();
    if (g_owner_pid == 0 || g_owner_pid != pid) {
        return;
    }
    audio::stop();
    g_owner_pid = 0;
}

} // namespace

namespace audio_device {

bool initialize() {
    if (!audio::ready()) {
        return false;
    }

    g_audio_device.write = audio_write;
    g_audio_device.ioctl = audio_ioctl;
    g_audio_device.close = audio_close;
    return device::register_node("/dev/audio0", &g_audio_device, true);
}

} // namespace audio_device
