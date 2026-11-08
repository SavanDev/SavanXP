#include "kernel/audio_device.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/audio.hpp"
#include "kernel/device.hpp"
#include "kernel/object.hpp"
#include "kernel/process.hpp"
#include "kernel/timer.hpp"
#include "abi/savanxp_abi.h"
#include "audio/peak_meter.h"

namespace {

device::Device g_audio_device = {
    .name = "audio0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
    .can_read = nullptr,
};

// The first writer owns playback until it closes a writable device handle.
// Another process gets EBUSY; acquisition is implicit on the first write.
uint32_t g_owner_pid = 0;
// Capture has an independent owner and DMA stream on HDA and virtio-sound.
// Closing one direction must leave the other direction running.
uint32_t g_capture_owner_pid = 0;

int negative_error(sx_status code) {
    return -static_cast<int>(code);
}

int16_t g_capture_scratch[2048];
savanxp_audio_peak_meter g_capture_peaks = {};

int capture_period(uint64_t user_buffer, uint32_t bytes) {
    if (bytes%4 || bytes > sizeof(g_capture_scratch)) return negative_error(SX_EINVAL);
    const int result = audio::capture_read_period(g_capture_scratch,bytes);
    if (result) return result < 0 ? result : negative_error(SX_EIO);
    audio::apply_capture_gain(g_capture_scratch,bytes);
    savanxp_audio_peak_observe(&g_capture_peaks,g_capture_scratch,bytes/4,timer::monotonic_ns());
    return process::copy_to_user(user_buffer,g_capture_scratch,bytes) ? 0 : negative_error(SX_EINVAL);
}
int capture_start() {
    if (!audio::capture_ready()) return negative_error(SX_ENODEV);
    const uint32_t pid = process::current_pid();
    if (!pid) return negative_error(SX_EBADF);
    if (g_capture_owner_pid && g_capture_owner_pid != pid) return negative_error(SX_EBUSY);
    if (!audio::capture_configure()) return negative_error(SX_EIO);
    g_capture_owner_pid = pid;
    return 0;
}
int capture_stop() {
    if (g_capture_owner_pid && g_capture_owner_pid != process::current_pid()) return negative_error(SX_EBUSY);
    if (!audio::capture_stop()) return negative_error(SX_EIO);
    g_capture_owner_pid = 0;
    savanxp_audio_peak_reset(&g_capture_peaks);
    return 0;
}

int audio_read(uint64_t user_buffer, size_t count) {
    savanxp_audio_info info = {};
    if (!audio::get_capture_info(info)) {
        return negative_error(SX_ENODEV);
    }
    if (count == 0) {
        return 0;
    }
    if (info.frame_bytes == 0 || (count % info.frame_bytes) != 0) {
        return negative_error(SX_EINVAL);
    }
    if (!process::validate_user_range(user_buffer, count, true)) {
        return negative_error(SX_EINVAL);
    }

    const uint32_t pid = process::current_pid();
    if (pid == 0) {
        return negative_error(SX_EBADF);
    }
    if (g_capture_owner_pid != 0 && g_capture_owner_pid != pid) {
        return negative_error(SX_EBUSY);
    }

    const bool acquired_owner = g_capture_owner_pid == 0;
    if (acquired_owner) {
        g_capture_owner_pid = pid;
    }

    if (!audio::capture_configure()) {
        if (acquired_owner) {
            g_capture_owner_pid = 0;
        }
        audio::capture_stop();
        return negative_error(SX_EIO);
    }

    const uint32_t period = info.period_bytes != 0 ? info.period_bytes : static_cast<uint32_t>(count);
    size_t transferred = 0;
    while (transferred < count) {
        const size_t remaining = count - transferred;
        const uint32_t chunk = static_cast<uint32_t>(remaining > period ? period : remaining);
        const int result = capture_period(user_buffer + transferred, chunk);
        if (result != 0) {
            return result < 0 ? result : negative_error(SX_EIO);
        }
        transferred += chunk;
    }

    return static_cast<int>(count);
}

int audio_write(uint64_t user_buffer, size_t count) {
    savanxp_audio_info info = {};
    if (!audio::ready() || !audio::get_info(info)) {
        return negative_error(SX_ENODEV);
    }
    if (count == 0) {
        return 0;
    }
    if (info.frame_bytes == 0 || (count % info.frame_bytes) != 0) {
        return negative_error(SX_EINVAL);
    }
    if (!process::validate_user_range(user_buffer, count, false)) {
        return negative_error(SX_EINVAL);
    }

    const uint32_t pid = process::current_pid();
    if (pid == 0) {
        return negative_error(SX_EBADF);
    }
    if (g_owner_pid != 0 && g_owner_pid != pid) {
        return negative_error(SX_EBUSY);
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
        return negative_error(SX_EIO);
    }

    const uint32_t period = info.period_bytes != 0 ? info.period_bytes : static_cast<uint32_t>(count);
    size_t transferred = 0;
    while (transferred < count) {
        const size_t remaining = count - transferred;
        const uint32_t chunk = static_cast<uint32_t>(remaining > period ? period : remaining);
        const int result = audio::submit_period(user_buffer + transferred, chunk);
        if (result != 0) {
            return result < 0 ? result : negative_error(SX_EIO);
        }
        transferred += chunk;
    }

    return static_cast<int>(count);
}

int audio_ioctl(uint64_t request, uint64_t argument, uint32_t granted_access) {
    switch (request) {
        case AUDIO_IOC_GET_INFO: {
            savanxp_audio_info info = {};
            if (!audio::ready() || !audio::get_info(info)) {
                return negative_error(SX_ENODEV);
            }
            if (!process::validate_user_range(argument, sizeof(info), true)) {
                return negative_error(SX_EINVAL);
            }
            return process::copy_to_user(argument, &info, sizeof(info))
                ? 0
                : negative_error(SX_EINVAL);
        }
        case AUDIO_IOC_CAPTURE_GET_INFO: {
            savanxp_audio_info info = {};
            if (!audio::get_capture_info(info)) return negative_error(SX_ENODEV);
            if (!process::validate_user_range(argument,sizeof(info),true)) return negative_error(SX_EINVAL);
            return process::copy_to_user(argument,&info,sizeof(info)) ? 0 : negative_error(SX_EINVAL);
        }
        case AUDIO_IOC_CAPTURE_START:
        case AUDIO_IOC_CAPTURE_STOP:
        case AUDIO_IOC_CAPTURE_READ: {
            if (!(granted_access & object::access_read)) return negative_error(SX_EACCES);
            if (request == AUDIO_IOC_CAPTURE_START) return capture_start();
            if (request == AUDIO_IOC_CAPTURE_STOP) return capture_stop();
            if (!g_capture_owner_pid) return negative_error(SX_EBADF);
            if (g_capture_owner_pid != process::current_pid()) return negative_error(SX_EBUSY);
            savanxp_audio_capture_read read = {};
            savanxp_audio_info info = {};
            if (!audio::get_capture_info(info)) return negative_error(SX_ENODEV);
            if (!process::validate_user_range(argument,sizeof(read),false) ||
                !process::copy_from_user(&read,argument,sizeof(read)) || read.reserved ||
                !read.byte_count || read.byte_count != info.period_bytes || read.byte_count > sizeof(g_capture_scratch) ||
                !process::validate_user_range(read.buffer,read.byte_count,true)) return negative_error(SX_EINVAL);
            const int available = audio::capture_available();
            if (available < 0) return available;
            if (static_cast<uint32_t>(available) < read.byte_count) return negative_error(SX_EAGAIN);
            return capture_period(read.buffer,read.byte_count);
        }
        case AUDIO_IOC_CAPTURE_GET_STATE: {
            if (!audio::capture_ready()) return negative_error(SX_ENODEV);
            savanxp_audio_capture_state state = {audio::capture_volume(),audio::capture_muted() ? 1u : 0u,
                0,0,g_capture_owner_pid ? 1u : 0u};
            savanxp_audio_peak_read(&g_capture_peaks,timer::monotonic_ns(),&state.peak_left,&state.peak_right);
            if (!process::validate_user_range(argument,sizeof(state),true)) return negative_error(SX_EINVAL);
            return process::copy_to_user(argument,&state,sizeof(state)) ? 0 : negative_error(SX_EINVAL);
        }
        case AUDIO_IOC_CAPTURE_SET_VOLUME:
        case AUDIO_IOC_CAPTURE_SET_MUTED: {
            if (!(granted_access & object::access_write)) return negative_error(SX_EACCES);
            if (!audio::capture_ready()) return negative_error(SX_ENODEV);
            if (request == AUDIO_IOC_CAPTURE_SET_VOLUME) {
                if (argument > 100 || !audio::set_capture_volume(argument)) return negative_error(SX_EINVAL);
            } else {
                if (argument > 1) return negative_error(SX_EINVAL);
                audio::set_capture_muted(argument != 0);
            }
            savanxp_audio_peak_reset(&g_capture_peaks);
            return 0;
        }
        case AUDIO_IOC_GET_ENDPOINT: {
            savanxp_audio_endpoint_query query = {};
            if (!process::validate_user_range(argument, sizeof(query), true) ||
                !process::copy_from_user(&query, argument, sizeof(query)) || query.reserved)
                return negative_error(SX_EINVAL);
            if (!audio::endpoint(query.index, query.endpoint)) return negative_error(SX_ENOENT);
            return process::copy_to_user(argument, &query, sizeof(query)) ? 0 : negative_error(SX_EINVAL);
        }
        case AUDIO_IOC_SELECT_ENDPOINT: {
            if (!(granted_access & object::access_write)) return negative_error(SX_EACCES);
            savanxp_audio_endpoint_selection selection = {};
            if (!process::validate_user_range(argument, sizeof(selection), false) ||
                !process::copy_from_user(&selection, argument, sizeof(selection)) || selection.reserved ||
                (selection.direction != SAVANXP_AUDIO_PLAYBACK && selection.direction != SAVANXP_AUDIO_CAPTURE))
                return negative_error(SX_EINVAL);
            const uint32_t owner = selection.direction == SAVANXP_AUDIO_PLAYBACK ? g_owner_pid : g_capture_owner_pid;
            if (owner && owner != process::current_pid()) return negative_error(SX_EBUSY);
            return audio::select_endpoint(selection.id, selection.direction);
        }
        case AUDIO_IOC_SET_VOLUME:
        case AUDIO_IOC_SET_MUTED: {
            // Mismo corte que INPUT_IOC_SET_LAYOUT: mutar el estado
            // compartido exige escritura. Leerlo, no.
            if ((granted_access & object::access_write) == 0) {
                return negative_error(SX_EACCES);
            }
            if (request == AUDIO_IOC_SET_VOLUME) {
                if (!audio::set_master_volume(static_cast<uint32_t>(argument))) {
                    return negative_error(SX_EINVAL);
                }
                return 0;
            }
            if (argument != 0 && argument != 1) {
                return negative_error(SX_EINVAL);
            }
            audio::set_master_muted(argument != 0);
            return 0;
        }
        case AUDIO_IOC_GET_VOLUME:
            return static_cast<int>(audio::master_volume());
        case AUDIO_IOC_GET_MUTED:
            return audio::master_muted() ? 1 : 0;
        default:
            return negative_error(SX_ENOSYS);
    }
}

void audio_close(uint32_t access) {
    const uint32_t pid = process::current_pid();
    if ((access & object::access_write) && g_owner_pid != 0 && g_owner_pid == pid) {
        audio::stop();
        g_owner_pid = 0;
    }
    if ((access & object::access_read) && g_capture_owner_pid != 0 && g_capture_owner_pid == pid) {
        audio::capture_stop();
        g_capture_owner_pid = 0;
    }
}

} // namespace

namespace audio_device {

void release_session_for(uint32_t pid) {
    if (pid == 0) {
        return;
    }
    if (g_owner_pid == pid) {
        audio::stop();
        g_owner_pid = 0;
    }
    if (g_capture_owner_pid == pid) {
        audio::capture_stop();
        g_capture_owner_pid = 0;
    }
}

bool initialize() {
    if (!audio::ready() && !audio::capture_ready()) {
        return false;
    }

    g_audio_device.read = audio_read;
    g_audio_device.write = audio_write;
    g_audio_device.ioctl = audio_ioctl;
    g_audio_device.close_access = audio_close;
    return device::register_node("/dev/audio0", &g_audio_device, true);
}

} // namespace audio_device
