#include "kernel/virtio_sound.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/device.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "kernel/virtio_pci.hpp"

namespace {

constexpr uint16_t kVirtioSoundModernDevice = 0x1059u;
constexpr uint16_t kVirtioSoundSubsystemDevice = 25u;
constexpr uint16_t kVirtioSoundControlQueue = 0;
constexpr uint16_t kVirtioSoundEventQueue = 1;
constexpr uint16_t kVirtioSoundTxQueue = 2;
constexpr uint16_t kVirtioSoundRxQueue = 3;
constexpr uint16_t kControlQueueDescriptors = 2;
constexpr uint16_t kEventQueueDescriptors = 1;
constexpr uint16_t kTxQueueDescriptors = 4;
constexpr uint16_t kRxQueueDescriptors = 1;
constexpr uint32_t kVirtioSndRPcmInfo = 0x0100u;
constexpr uint32_t kVirtioSndRPcmSetParams = 0x0101u;
constexpr uint32_t kVirtioSndRPcmPrepare = 0x0102u;
constexpr uint32_t kVirtioSndRPcmRelease = 0x0103u;
constexpr uint32_t kVirtioSndRPcmStart = 0x0104u;
constexpr uint32_t kVirtioSndRPcmStop = 0x0105u;
constexpr uint32_t kVirtioSndSOk = 0x8000u;
constexpr uint8_t kVirtioSndDirectionOutput = 0u;
constexpr uint8_t kVirtioSndPcmFmtS16 = 5u;
constexpr uint8_t kVirtioSndPcmRate48000 = 7u;
constexpr size_t kControlRequestBytes = 256;
constexpr size_t kControlResponseBytes = 256;
constexpr size_t kControlResponseOffset = kControlRequestBytes;
constexpr size_t kControlQueueExtraBytes = kControlRequestBytes + kControlResponseBytes;
constexpr size_t kEventQueueExtraBytes = 16;
constexpr uint32_t kAudioSampleRateHz = 48000u;
constexpr uint32_t kAudioChannels = 2u;
constexpr uint32_t kAudioBitsPerSample = 16u;
constexpr uint32_t kAudioFrameBytes = 4u;
constexpr uint32_t kAudioPeriodBytes = 4096u;
constexpr uint32_t kAudioBufferBytes = 16384u;
constexpr size_t kTxHeaderOffset = 0;
constexpr size_t kTxDataOffset = sizeof(uint32_t);
constexpr size_t kTxStatusOffset = kTxDataOffset + kAudioPeriodBytes;
constexpr size_t kTxQueueExtraBytes = kTxStatusOffset + sizeof(uint32_t) * 2u;
constexpr uint32_t kCommandTimeoutSpins = 10000000u;

struct [[gnu::packed]] VirtioSndHdr {
    uint32_t code;
};

struct [[gnu::packed]] VirtioSndConfig {
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
};

struct [[gnu::packed]] VirtioSndQueryInfo {
    VirtioSndHdr hdr;
    uint32_t start_id;
    uint32_t count;
    uint32_t size;
};

struct [[gnu::packed]] VirtioSndInfo {
    uint32_t hda_fn_nid;
};

struct [[gnu::packed]] VirtioSndPcmInfo {
    VirtioSndInfo hdr;
    uint32_t features;
    uint64_t formats;
    uint64_t rates;
    uint8_t direction;
    uint8_t channels_min;
    uint8_t channels_max;
    uint8_t padding[5];
};

struct [[gnu::packed]] VirtioSndPcmInfoResponse {
    VirtioSndHdr hdr;
    VirtioSndPcmInfo info;
};

struct [[gnu::packed]] VirtioSndPcmHdr {
    VirtioSndHdr hdr;
    uint32_t stream_id;
};

struct [[gnu::packed]] VirtioSndPcmSetParams {
    VirtioSndPcmHdr hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t channels;
    uint8_t format;
    uint8_t rate;
    uint8_t padding;
};

struct [[gnu::packed]] VirtioSndPcmXfer {
    uint32_t stream_id;
};

struct [[gnu::packed]] VirtioSndPcmStatus {
    uint32_t status;
    uint32_t latency_bytes;
};

device::Device g_audio_device = {
    .name = "audio0",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
};

virtio_pci::Device g_device = {};
virtio_pci::Queue g_control_queue = {};
virtio_pci::Queue g_event_queue = {};
virtio_pci::Queue g_tx_queue = {};
virtio_pci::Queue g_rx_queue = {};
savanxp_audio_info g_audio_info = {};
uint32_t g_selected_stream_id = 0;
uint32_t g_owner_pid = 0;
uint32_t g_last_status_code = 0;
bool g_ready = false;
bool g_stream_selected = false;
bool g_stream_params_set = false;
bool g_stream_prepared = false;
bool g_stream_started = false;
bool g_event_buffer_armed = false;

int negative_error(savanxp_error_code code) {
    return -static_cast<int>(code);
}

volatile VirtioSndConfig* device_cfg() {
    return reinterpret_cast<volatile VirtioSndConfig*>(virtio_pci::device_cfg_base(g_device));
}

bool wait_for_used_element(virtio_pci::Queue& queue, virtio_pci::UsedElement& element) {
    const volatile virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(queue);
    const virtio_pci::UsedElement* ring = virtio_pci::queue_used_ring(queue);

    for (uint32_t spin = 0; spin < kCommandTimeoutSpins; ++spin) {
        virtio_pci::memory_barrier();
        if (queue.last_used_index == used->idx) {
            continue;
        }

        element = ring[queue.last_used_index % queue.size];
        queue.last_used_index = static_cast<uint16_t>(queue.last_used_index + 1);
        return true;
    }
    return false;
}

bool submit_control_command(const void* request, size_t request_bytes, void* response, size_t response_bytes) {
    if (!g_ready || request == nullptr || response == nullptr || request_bytes == 0 || response_bytes == 0 ||
        request_bytes > kControlRequestBytes || response_bytes > kControlResponseBytes) {
        return false;
    }

    memcpy(virtio_pci::queue_extra(g_control_queue), request, request_bytes);
    memset(virtio_pci::queue_extra(g_control_queue, kControlResponseOffset), 0, response_bytes);

    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_control_queue);
    descriptors[0] = {
        .addr = virtio_pci::queue_extra_physical(g_control_queue),
        .len = static_cast<uint32_t>(request_bytes),
        .flags = virtio_pci::kDescriptorFlagNext,
        .next = 1,
    };
    descriptors[1] = {
        .addr = virtio_pci::queue_extra_physical(g_control_queue, kControlResponseOffset),
        .len = static_cast<uint32_t>(response_bytes),
        .flags = virtio_pci::kDescriptorFlagWrite,
        .next = 0,
    };

    if (!virtio_pci::submit_descriptor_head(g_control_queue, 0)) {
        return false;
    }
    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_control_queue);

    virtio_pci::UsedElement element = {};
    if (!wait_for_used_element(g_control_queue, element) || element.id != 0) {
        return false;
    }

    memcpy(response, virtio_pci::queue_extra(g_control_queue, kControlResponseOffset), response_bytes);
    return true;
}

bool send_status_only_command(const void* request, size_t request_bytes) {
    VirtioSndHdr response = {};
    if (!submit_control_command(request, request_bytes, &response, sizeof(response))) {
        g_last_status_code = 0;
        return false;
    }

    g_last_status_code = response.code;
    return response.code == kVirtioSndSOk;
}

bool submit_simple_stream_command(uint32_t request_code) {
    VirtioSndPcmHdr request = {};
    request.hdr.code = request_code;
    request.stream_id = g_selected_stream_id;
    return send_status_only_command(&request, sizeof(request));
}

bool queue_event_buffer() {
    if (!g_ready) {
        return false;
    }
    if (g_event_buffer_armed) {
        return true;
    }

    memset(virtio_pci::queue_extra(g_event_queue), 0, kEventQueueExtraBytes);
    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_event_queue);
    descriptors[0] = {
        .addr = virtio_pci::queue_extra_physical(g_event_queue),
        .len = static_cast<uint32_t>(kEventQueueExtraBytes),
        .flags = virtio_pci::kDescriptorFlagWrite,
        .next = 0,
    };

    if (!virtio_pci::submit_descriptor_head(g_event_queue, 0)) {
        return false;
    }
    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_event_queue);
    g_event_buffer_armed = true;
    return true;
}

void drain_event_queue() {
    if (!g_ready || !g_event_queue.enabled) {
        return;
    }

    const volatile virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_event_queue);
    while (g_event_queue.last_used_index != used->idx) {
        virtio_pci::UsedElement element = {};
        if (!wait_for_used_element(g_event_queue, element)) {
            break;
        }
        g_event_buffer_armed = false;
        memset(virtio_pci::queue_extra(g_event_queue), 0, kEventQueueExtraBytes);
    }

    (void)queue_event_buffer();
}

bool query_stream_info(uint32_t stream_id, VirtioSndPcmInfo& info) {
    VirtioSndQueryInfo request = {};
    request.hdr.code = kVirtioSndRPcmInfo;
    request.start_id = stream_id;
    request.count = 1;
    request.size = sizeof(VirtioSndPcmInfo);

    VirtioSndPcmInfoResponse response = {};
    if (!submit_control_command(&request, sizeof(request), &response, sizeof(response))) {
        g_last_status_code = 0;
        return false;
    }

    g_last_status_code = response.hdr.code;
    if (response.hdr.code != kVirtioSndSOk) {
        return false;
    }

    info = response.info;
    return true;
}

bool stream_supports_required_format(const VirtioSndPcmInfo& info) {
    const uint64_t format_mask = 1ull << kVirtioSndPcmFmtS16;
    const uint64_t rate_mask = 1ull << kVirtioSndPcmRate48000;

    return info.direction == kVirtioSndDirectionOutput &&
        (info.formats & format_mask) != 0 &&
        (info.rates & rate_mask) != 0 &&
        info.channels_min <= kAudioChannels &&
        info.channels_max >= kAudioChannels;
}

bool select_output_stream() {
    const uint32_t stream_count = device_cfg()->streams;
    for (uint32_t stream_id = 0; stream_id < stream_count; ++stream_id) {
        VirtioSndPcmInfo info = {};
        if (!query_stream_info(stream_id, info)) {
            return false;
        }
        if (!stream_supports_required_format(info)) {
            continue;
        }

        g_selected_stream_id = stream_id;
        g_stream_selected = true;
        g_audio_info = {
            .sample_rate_hz = kAudioSampleRateHz,
            .channels = kAudioChannels,
            .bits_per_sample = kAudioBitsPerSample,
            .frame_bytes = kAudioFrameBytes,
            .period_bytes = kAudioPeriodBytes,
            .buffer_bytes = kAudioBufferBytes,
            .backend = SAVANXP_AUDIO_BACKEND_VIRTIO,
            .flags = 0,
        };
        return true;
    }

    return true;
}

bool set_stream_params() {
    VirtioSndPcmSetParams request = {};
    request.hdr.hdr.code = kVirtioSndRPcmSetParams;
    request.hdr.stream_id = g_selected_stream_id;
    request.buffer_bytes = kAudioBufferBytes;
    request.period_bytes = kAudioPeriodBytes;
    request.features = 0;
    request.channels = static_cast<uint8_t>(kAudioChannels);
    request.format = kVirtioSndPcmFmtS16;
    request.rate = kVirtioSndPcmRate48000;
    request.padding = 0;
    return send_status_only_command(&request, sizeof(request));
}

bool ensure_stream_ready() {
    if (!g_stream_selected) {
        return false;
    }
    if (!g_stream_params_set) {
        if (!set_stream_params()) {
            return false;
        }
        g_stream_params_set = true;
    }
    if (!g_stream_prepared) {
        if (!submit_simple_stream_command(kVirtioSndRPcmPrepare)) {
            return false;
        }
        g_stream_prepared = true;
    }
    if (!g_stream_started) {
        if (!submit_simple_stream_command(kVirtioSndRPcmStart)) {
            return false;
        }
        g_stream_started = true;
    }
    return true;
}

void reset_stream_state() {
    g_stream_params_set = false;
    g_stream_prepared = false;
    g_stream_started = false;
}

void stop_and_release_stream() {
    if (!g_stream_selected) {
        reset_stream_state();
        return;
    }

    if (g_stream_started &&
        !submit_simple_stream_command(kVirtioSndRPcmStop)) {
        console::printf("virtio-sound: STOP failed (status=0x%x)\n", static_cast<unsigned>(g_last_status_code));
    }
    if ((g_stream_prepared || g_stream_params_set) &&
        !submit_simple_stream_command(kVirtioSndRPcmRelease)) {
        console::printf("virtio-sound: RELEASE failed (status=0x%x)\n", static_cast<unsigned>(g_last_status_code));
    }

    reset_stream_state();
}

bool submit_tx_chunk(uint64_t user_buffer, uint32_t byte_count) {
    if (byte_count == 0 || byte_count > kAudioPeriodBytes) {
        return false;
    }

    auto* header = reinterpret_cast<VirtioSndPcmXfer*>(virtio_pci::queue_extra(g_tx_queue, kTxHeaderOffset));
    void* payload = virtio_pci::queue_extra(g_tx_queue, kTxDataOffset);
    auto* status = reinterpret_cast<VirtioSndPcmStatus*>(virtio_pci::queue_extra(g_tx_queue, kTxStatusOffset));

    header->stream_id = g_selected_stream_id;
    if (!process::copy_from_user(payload, user_buffer, byte_count)) {
        return false;
    }
    memset(status, 0, sizeof(*status));

    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_tx_queue);
    descriptors[0] = {
        .addr = virtio_pci::queue_extra_physical(g_tx_queue, kTxHeaderOffset),
        .len = static_cast<uint32_t>(sizeof(*header)),
        .flags = virtio_pci::kDescriptorFlagNext,
        .next = 1,
    };
    descriptors[1] = {
        .addr = virtio_pci::queue_extra_physical(g_tx_queue, kTxDataOffset),
        .len = byte_count,
        .flags = virtio_pci::kDescriptorFlagNext,
        .next = 2,
    };
    descriptors[2] = {
        .addr = virtio_pci::queue_extra_physical(g_tx_queue, kTxStatusOffset),
        .len = static_cast<uint32_t>(sizeof(*status)),
        .flags = virtio_pci::kDescriptorFlagWrite,
        .next = 0,
    };

    if (!virtio_pci::submit_descriptor_head(g_tx_queue, 0)) {
        return false;
    }
    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_tx_queue);

    virtio_pci::UsedElement element = {};
    if (!wait_for_used_element(g_tx_queue, element) || element.id != 0) {
        return false;
    }
    return status->status == kVirtioSndSOk;
}

int audio_write(uint64_t user_buffer, size_t count) {
    if (!g_ready || !g_stream_selected) {
        return negative_error(SAVANXP_ENODEV);
    }
    if (count == 0) {
        return 0;
    }
    if ((count % g_audio_info.frame_bytes) != 0) {
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

    drain_event_queue();
    if (!ensure_stream_ready()) {
        if (acquired_owner) {
            g_owner_pid = 0;
        }
        stop_and_release_stream();
        return negative_error(SAVANXP_EIO);
    }

    size_t transferred = 0;
    while (transferred < count) {
        const size_t remaining = count - transferred;
        const uint32_t chunk = static_cast<uint32_t>(remaining > g_audio_info.period_bytes ? g_audio_info.period_bytes : remaining);
        if (!submit_tx_chunk(user_buffer + transferred, chunk)) {
            return negative_error(SAVANXP_EIO);
        }
        transferred += chunk;
        drain_event_queue();
    }

    return static_cast<int>(count);
}

int audio_ioctl(uint64_t request, uint64_t argument) {
    switch (request) {
        case AUDIO_IOC_GET_INFO:
            if (!g_ready || !g_stream_selected) {
                return negative_error(SAVANXP_ENODEV);
            }
            if (!process::validate_user_range(argument, sizeof(g_audio_info), true)) {
                return negative_error(SAVANXP_EINVAL);
            }
            return process::copy_to_user(argument, &g_audio_info, sizeof(g_audio_info))
                ? 0
                : negative_error(SAVANXP_EINVAL);
        default:
            return negative_error(SAVANXP_ENOSYS);
    }
}

void audio_close() {
    const uint32_t pid = process::current_pid();
    if (g_owner_pid == 0 || g_owner_pid != pid) {
        return;
    }

    drain_event_queue();
    stop_and_release_stream();
    g_owner_pid = 0;
}

void fail_device(const char* reason) {
    virtio_pci::fail_device(g_device);
    if (reason != nullptr) {
        console::printf("virtio-sound: %s\n", reason);
    }
    g_ready = false;
}

} // namespace

namespace virtio_sound {

void initialize() {
    memset(&g_device, 0, sizeof(g_device));
    memset(&g_control_queue, 0, sizeof(g_control_queue));
    memset(&g_event_queue, 0, sizeof(g_event_queue));
    memset(&g_tx_queue, 0, sizeof(g_tx_queue));
    memset(&g_rx_queue, 0, sizeof(g_rx_queue));
    memset(&g_audio_info, 0, sizeof(g_audio_info));
    g_selected_stream_id = 0;
    g_owner_pid = 0;
    g_last_status_code = 0;
    g_ready = false;
    g_stream_selected = false;
    g_event_buffer_armed = false;
    reset_stream_state();

    pci::DeviceInfo pci_device = {};
    if (!pci::ready() || !virtio_pci::find_modern_device(kVirtioSoundModernDevice, kVirtioSoundSubsystemDevice, pci_device)) {
        return;
    }

    if (!virtio_pci::initialize_device(pci_device, true, g_device)) {
        console::write_line("virtio-sound: missing required MMIO capabilities");
        return;
    }

    virtio_pci::set_device_status(g_device, 0);
    virtio_pci::memory_barrier();
    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::kStatusAcknowledge | virtio_pci::kStatusDriver));

    if (!virtio_pci::negotiate_features(g_device, 0, virtio_pci::kFeatureVersion1Bit)) {
        fail_device("feature negotiation failed");
        return;
    }
    if (!virtio_pci::setup_queue(g_device, kVirtioSoundControlQueue, kControlQueueDescriptors, kControlQueueExtraBytes, 16, g_control_queue) ||
        !virtio_pci::setup_queue(g_device, kVirtioSoundEventQueue, kEventQueueDescriptors, kEventQueueExtraBytes, 16, g_event_queue) ||
        !virtio_pci::setup_queue(g_device, kVirtioSoundTxQueue, kTxQueueDescriptors, kTxQueueExtraBytes, 16, g_tx_queue) ||
        !virtio_pci::setup_queue(g_device, kVirtioSoundRxQueue, kRxQueueDescriptors, 0, 16, g_rx_queue)) {
        fail_device("failed to setup queues");
        return;
    }

    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::device_status(g_device) | virtio_pci::kStatusDriverOk));
    g_ready = true;

    if (device_cfg()->streams == 0) {
        fail_device("device exposes no PCM streams");
        return;
    }
    if (!select_output_stream()) {
        fail_device("failed to query PCM stream info");
        return;
    }
    if (!g_stream_selected) {
        g_ready = false;
        return;
    }
    if (!queue_event_buffer()) {
        fail_device("failed to arm event queue");
        return;
    }

    g_audio_device.write = audio_write;
    g_audio_device.ioctl = audio_ioctl;
    g_audio_device.close = audio_close;
    if (!device::register_node("/dev/audio0", &g_audio_device, true)) {
        fail_device("failed to register /dev/audio0");
        return;
    }

    console::printf(
        "virtio-sound: ready pci=%x:%x.%u stream=%u pcm=%uHz/%uch/%ubit\n",
        static_cast<unsigned>(g_device.pci_device.bus),
        static_cast<unsigned>(g_device.pci_device.slot),
        static_cast<unsigned>(g_device.pci_device.function),
        static_cast<unsigned>(g_selected_stream_id),
        static_cast<unsigned>(g_audio_info.sample_rate_hz),
        static_cast<unsigned>(g_audio_info.channels),
        static_cast<unsigned>(g_audio_info.bits_per_sample)
    );
}

bool ready() {
    return g_ready && g_stream_selected;
}

} // namespace virtio_sound
