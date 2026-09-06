#include "kernel/virtio_sound.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/audio.hpp"
#include "kernel/console.hpp"
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
// Reproduccion multi-buffer: kTxSlots periodos en vuelo, cada uno con su cadena
// de 3 descriptores (header + data + status). choose_queue_size redondea el
// limite a la potencia de 2 mas cercana hacia abajo, asi que pedimos 32 para
// alojar 8*3=24 descriptores. Sin espera bloqueante: el device consume los
// buffers mientras el productor sigue alimentando.
constexpr uint16_t kTxSlots = 8;
constexpr uint16_t kTxPrimeSlots = 4;
constexpr uint16_t kTxDescriptorsPerSlot = 3;
constexpr uint16_t kTxQueueLimit = 32;
constexpr uint32_t kVirtioSndRPcmInfo = 0x0100u;
constexpr uint32_t kVirtioSndRPcmSetParams = 0x0101u;
constexpr uint32_t kVirtioSndRPcmPrepare = 0x0102u;
constexpr uint32_t kVirtioSndRPcmRelease = 0x0103u;
constexpr uint32_t kVirtioSndRPcmStart = 0x0104u;
constexpr uint32_t kVirtioSndRPcmStop = 0x0105u;
constexpr uint32_t kVirtioSndSOk = 0x8000u;
constexpr uint8_t kVirtioSndDirectionOutput = 0u;
constexpr uint8_t kVirtioSndDirectionInput = 1u;
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
// Layout por slot en la memoria extra de la cola TX: header (stream_id) + data
// (un periodo) + status. Los slots van consecutivos; slot i en i*kTxSlotStride.
constexpr size_t kTxSlotHeaderBytes = sizeof(uint32_t);
constexpr size_t kTxSlotStatusBytes = sizeof(uint32_t) * 2u;
constexpr size_t kTxSlotStride = kTxSlotHeaderBytes + kAudioPeriodBytes + kTxSlotStatusBytes;
constexpr size_t kTxQueueExtraBytes = kTxSlotStride * kTxSlots;
// Captura: mismo layout por slot que TX (header + un periodo + status), pero
// el descriptor de datos queda escribible por el device en vez de por el
// driver. Menos slots que TX (4 en vez de 8) porque no hay que absorber
// jitter de reproduccion, solo mantener uno o dos periodos de profundidad
// mientras el lector no llama a read().
constexpr uint16_t kRxSlots = 4;
constexpr uint16_t kRxDescriptorsPerSlot = 3;
constexpr uint16_t kRxQueueLimit = 16;
constexpr size_t kRxSlotHeaderBytes = sizeof(uint32_t);
constexpr size_t kRxSlotStatusBytes = sizeof(uint32_t) * 2u;
constexpr size_t kRxSlotStride = kRxSlotHeaderBytes + kAudioPeriodBytes + kRxSlotStatusBytes;
constexpr size_t kRxQueueExtraBytes = kRxSlotStride * kRxSlots;
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

// Ciclo de vida de un stream PCM (SET_PARAMS -> PREPARE -> START, y a la
// inversa STOP -> RELEASE). Playback y capture son dos streams virtio-sound
// independientes -- distinto stream_id, distinto estado -- pero comparten
// exactamente esta maquina de comandos, asi que las funciones que la manejan
// quedan parametrizadas por cual StreamState les toca en vez de duplicarse.
struct StreamState {
    uint32_t stream_id;
    bool selected;
    bool params_set;
    bool prepared;
    bool started;
};

virtio_pci::Device g_device = {};
virtio_pci::Queue g_control_queue = {};
virtio_pci::Queue g_event_queue = {};
virtio_pci::Queue g_tx_queue = {};
virtio_pci::Queue g_rx_queue = {};
savanxp_audio_info g_audio_info = {};
StreamState g_playback = {};
StreamState g_capture = {};
uint32_t g_last_status_code = 0;
bool g_ready = false;
bool g_event_buffer_armed = false;
bool g_tx_slot_busy[kTxSlots] = {};   // slot ocupado por un periodo en vuelo
uint16_t g_tx_in_flight = 0;          // periodos encolados sin consumir
uint16_t g_tx_usable_slots = 0;       // min(kTxSlots, size_de_la_cola/3)
bool g_tx_primed = false;             // colchon de silencio ya cargado
uint32_t g_tx_drops = 0;              // periodos descartados por ring lleno
bool g_rx_primed = false;             // buffers de captura ya posteados

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

bool submit_simple_stream_command(StreamState& stream, uint32_t request_code) {
    VirtioSndPcmHdr request = {};
    request.hdr.code = request_code;
    request.stream_id = stream.stream_id;
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

bool stream_matches_format(const VirtioSndPcmInfo& info, uint8_t direction) {
    const uint64_t format_mask = 1ull << kVirtioSndPcmFmtS16;
    const uint64_t rate_mask = 1ull << kVirtioSndPcmRate48000;

    return info.direction == direction &&
        (info.formats & format_mask) != 0 &&
        (info.rates & rate_mask) != 0 &&
        info.channels_min <= kAudioChannels &&
        info.channels_max >= kAudioChannels;
}

// false solo ante un error real de comunicacion con el device (query_stream_info
// fallo). No encontrar un stream de esa direccion NO es un error aca -- el
// caller decide si le hace falta: playback lo trata como fatal, capture como
// opcional (streams=1 en el perfil sin -Virtio-record, por ejemplo).
bool select_stream(uint8_t direction, StreamState& state) {
    const uint32_t stream_count = device_cfg()->streams;
    for (uint32_t stream_id = 0; stream_id < stream_count; ++stream_id) {
        VirtioSndPcmInfo info = {};
        if (!query_stream_info(stream_id, info)) {
            return false;
        }
        if (!stream_matches_format(info, direction)) {
            continue;
        }

        state.stream_id = stream_id;
        state.selected = true;
        return true;
    }

    return true;
}

bool set_stream_params(StreamState& stream) {
    VirtioSndPcmSetParams request = {};
    request.hdr.hdr.code = kVirtioSndRPcmSetParams;
    request.hdr.stream_id = stream.stream_id;
    request.buffer_bytes = kAudioBufferBytes;
    request.period_bytes = kAudioPeriodBytes;
    request.features = 0;
    request.channels = static_cast<uint8_t>(kAudioChannels);
    request.format = kVirtioSndPcmFmtS16;
    request.rate = kVirtioSndPcmRate48000;
    request.padding = 0;
    return send_status_only_command(&request, sizeof(request));
}

bool ensure_stream_ready(StreamState& stream) {
    if (!stream.selected) {
        return false;
    }
    if (!stream.params_set) {
        if (!set_stream_params(stream)) {
            return false;
        }
        stream.params_set = true;
    }
    if (!stream.prepared) {
        if (!submit_simple_stream_command(stream, kVirtioSndRPcmPrepare)) {
            return false;
        }
        stream.prepared = true;
    }
    if (!stream.started) {
        if (!submit_simple_stream_command(stream, kVirtioSndRPcmStart)) {
            return false;
        }
        stream.started = true;
    }
    return true;
}

void reset_stream_state(StreamState& stream) {
    stream.params_set = false;
    stream.prepared = false;
    stream.started = false;
}

void stop_and_release_stream(StreamState& stream) {
    if (!stream.selected) {
        reset_stream_state(stream);
        return;
    }

    if (stream.started &&
        !submit_simple_stream_command(stream, kVirtioSndRPcmStop)) {
        console::printf("virtio-sound: STOP failed (status=0x%x)\n", static_cast<unsigned>(g_last_status_code));
    }
    if ((stream.prepared || stream.params_set) &&
        !submit_simple_stream_command(stream, kVirtioSndRPcmRelease)) {
        console::printf("virtio-sound: RELEASE failed (status=0x%x)\n", static_cast<unsigned>(g_last_status_code));
    }

    reset_stream_state(stream);
}

size_t tx_slot_header_offset(uint16_t slot) { return static_cast<size_t>(slot) * kTxSlotStride; }
size_t tx_slot_data_offset(uint16_t slot) { return tx_slot_header_offset(slot) + kTxSlotHeaderBytes; }
size_t tx_slot_status_offset(uint16_t slot) { return tx_slot_data_offset(slot) + kAudioPeriodBytes; }

// Reclama los periodos que el device ya consumio, liberando sus slots. El id del
// elemento usado es el indice del descriptor cabeza de la cadena = slot * 3.
void reclaim_tx() {
    const volatile virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_tx_queue);
    const virtio_pci::UsedElement* ring = virtio_pci::queue_used_ring(g_tx_queue);
    while (g_tx_queue.last_used_index != used->idx) {
        virtio_pci::memory_barrier();
        const virtio_pci::UsedElement element = ring[g_tx_queue.last_used_index % g_tx_queue.size];
        g_tx_queue.last_used_index = static_cast<uint16_t>(g_tx_queue.last_used_index + 1);
        const uint16_t slot = static_cast<uint16_t>(element.id / kTxDescriptorsPerSlot);
        if (slot < kTxSlots && g_tx_slot_busy[slot]) {
            g_tx_slot_busy[slot] = false;
            if (g_tx_in_flight > 0) {
                --g_tx_in_flight;
            }
        }
    }
}

int find_free_tx_slot() {
    for (uint16_t slot = 0; slot < g_tx_usable_slots; ++slot) {
        if (!g_tx_slot_busy[slot]) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

// Encola un periodo en el slot dado sin esperar a que el device lo consuma. Con
// silence=true rellena con ceros (colchon); si no, copia desde memoria de usuario.
bool submit_tx_slot(uint16_t slot, uint64_t user_buffer, uint32_t byte_count, bool silence) {
    auto* header = reinterpret_cast<VirtioSndPcmXfer*>(virtio_pci::queue_extra(g_tx_queue, tx_slot_header_offset(slot)));
    void* payload = virtio_pci::queue_extra(g_tx_queue, tx_slot_data_offset(slot));
    auto* status = reinterpret_cast<VirtioSndPcmStatus*>(virtio_pci::queue_extra(g_tx_queue, tx_slot_status_offset(slot)));

    header->stream_id = g_playback.stream_id;
    if (silence) {
        memset(payload, 0, byte_count);
    } else if (!process::copy_from_user(payload, user_buffer, byte_count)) {
        return false;
    }
    memset(status, 0, sizeof(*status));

    const uint16_t base = static_cast<uint16_t>(slot * kTxDescriptorsPerSlot);
    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_tx_queue);
    descriptors[base] = {
        .addr = virtio_pci::queue_extra_physical(g_tx_queue, tx_slot_header_offset(slot)),
        .len = static_cast<uint32_t>(sizeof(*header)),
        .flags = virtio_pci::kDescriptorFlagNext,
        .next = static_cast<uint16_t>(base + 1),
    };
    descriptors[base + 1] = {
        .addr = virtio_pci::queue_extra_physical(g_tx_queue, tx_slot_data_offset(slot)),
        .len = byte_count,
        .flags = virtio_pci::kDescriptorFlagNext,
        .next = static_cast<uint16_t>(base + 2),
    };
    descriptors[base + 2] = {
        .addr = virtio_pci::queue_extra_physical(g_tx_queue, tx_slot_status_offset(slot)),
        .len = static_cast<uint32_t>(sizeof(*status)),
        .flags = virtio_pci::kDescriptorFlagWrite,
        .next = 0,
    };

    if (!virtio_pci::submit_descriptor_head(g_tx_queue, base)) {
        return false;
    }
    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_tx_queue);
    g_tx_slot_busy[slot] = true;
    ++g_tx_in_flight;
    return true;
}

// Precarga kTxPrimeSlots periodos de silencio como colchon para absorber el
// jitter del productor sin quedar al borde del underrun.
void prime_tx_silence() {
    for (uint16_t i = 0; i < kTxPrimeSlots; ++i) {
        const int slot = find_free_tx_slot();
        if (slot < 0) {
            break;
        }
        (void)submit_tx_slot(static_cast<uint16_t>(slot), 0, kAudioPeriodBytes, true);
    }
}

void reset_tx_ring() {
    for (uint16_t i = 0; i < kTxSlots; ++i) {
        g_tx_slot_busy[i] = false;
    }
    g_tx_in_flight = 0;
    g_tx_primed = false;
    g_tx_drops = 0;
}

size_t rx_slot_header_offset(uint16_t slot) { return static_cast<size_t>(slot) * kRxSlotStride; }
size_t rx_slot_data_offset(uint16_t slot) { return rx_slot_header_offset(slot) + kRxSlotHeaderBytes; }
size_t rx_slot_status_offset(uint16_t slot) { return rx_slot_data_offset(slot) + kAudioPeriodBytes; }

// Postea (o re-postea) un buffer vacio para que el device lo llene con audio
// capturado. A diferencia de submit_tx_slot no hay datos que copiar antes de
// notificar: el header solo identifica el stream, el payload lo escribe el
// device (por eso ese descriptor lleva kDescriptorFlagWrite).
bool post_rx_slot(uint16_t slot) {
    auto* header = reinterpret_cast<VirtioSndPcmXfer*>(virtio_pci::queue_extra(g_rx_queue, rx_slot_header_offset(slot)));
    auto* status = reinterpret_cast<VirtioSndPcmStatus*>(virtio_pci::queue_extra(g_rx_queue, rx_slot_status_offset(slot)));

    header->stream_id = g_capture.stream_id;
    memset(status, 0, sizeof(*status));

    const uint16_t base = static_cast<uint16_t>(slot * kRxDescriptorsPerSlot);
    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_rx_queue);
    descriptors[base] = {
        .addr = virtio_pci::queue_extra_physical(g_rx_queue, rx_slot_header_offset(slot)),
        .len = static_cast<uint32_t>(sizeof(*header)),
        .flags = virtio_pci::kDescriptorFlagNext,
        .next = static_cast<uint16_t>(base + 1),
    };
    descriptors[base + 1] = {
        .addr = virtio_pci::queue_extra_physical(g_rx_queue, rx_slot_data_offset(slot)),
        .len = static_cast<uint32_t>(kAudioPeriodBytes),
        .flags = static_cast<uint16_t>(virtio_pci::kDescriptorFlagNext | virtio_pci::kDescriptorFlagWrite),
        .next = static_cast<uint16_t>(base + 2),
    };
    descriptors[base + 2] = {
        .addr = virtio_pci::queue_extra_physical(g_rx_queue, rx_slot_status_offset(slot)),
        .len = static_cast<uint32_t>(sizeof(*status)),
        .flags = virtio_pci::kDescriptorFlagWrite,
        .next = 0,
    };

    if (!virtio_pci::submit_descriptor_head(g_rx_queue, base)) {
        return false;
    }
    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_rx_queue);
    return true;
}

void prime_rx_slots() {
    for (uint16_t slot = 0; slot < kRxSlots; ++slot) {
        (void)post_rx_slot(slot);
    }
}

// Descarta periodos capturados antes de un stop/restart de la sesion: no
// corresponden a ningun read() todavia pedido, y wait_for_used_element ya deja
// avanzado last_used_index al consumirlos.
void drain_stale_rx() {
    if (!g_rx_queue.enabled) {
        return;
    }
    const volatile virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_rx_queue);
    while (g_rx_queue.last_used_index != used->idx) {
        virtio_pci::UsedElement element = {};
        if (!wait_for_used_element(g_rx_queue, element)) {
            break;
        }
    }
}

// --- Implementacion del backend audio::Backend ------------------------------
// La logica comun (owner-pid, validacion de usuario, troceado en periodos y el
// registro de /dev/audio0) vive en audio_device.cpp; aca solo quedan las
// operaciones dependientes de virtio.

bool vs_ready() {
    return g_ready && g_playback.selected;
}

bool vs_get_info(savanxp_audio_info& info) {
    if (!g_ready || !g_playback.selected) {
        return false;
    }
    info = g_audio_info;
    return true;
}

bool vs_configure() {
    if (!g_ready || !g_playback.selected) {
        return false;
    }
    reclaim_tx();
    drain_event_queue();
    if (!ensure_stream_ready(g_playback)) {
        return false;
    }
    if (!g_tx_primed) {
        prime_tx_silence();
        g_tx_primed = true;
    }
    return true;
}

int vs_submit_period(uint64_t user_buffer, uint32_t byte_count) {
    if (byte_count == 0 || byte_count > kAudioPeriodBytes) {
        return -static_cast<int>(SAVANXP_EINVAL);
    }

    reclaim_tx();

    // Nota: a diferencia de AC97 (que drena a ritmo de reproduccion), QEMU
    // completa cada TX de virtio-sound al instante y bufferea del lado del host,
    // asi que g_tx_in_flight vuelve a 0 enseguida y eso NO es un underrun real
    // ni hay que reinyectar silencio (inundaria el stream). El colchon inicial
    // queda como seguro por si un device real drenara a ritmo de reproduccion.
    const int slot = find_free_tx_slot();
    if (slot < 0) {
        // Ring lleno: descartar en vez de bloquear. Un spin esperando aca (con
        // interrupciones off, como corren los syscalls) congelaria el timer y el
        // reloj del guest; el productor, que se guia por ese reloj para dosificar,
        // colapsaria. Ese era el motivo del audio entrecortado por virtio.
        ++g_tx_drops;
        return 0;
    }
    if (!submit_tx_slot(static_cast<uint16_t>(slot), user_buffer, byte_count, false)) {
        return -static_cast<int>(SAVANXP_EIO);
    }
    drain_event_queue();
    return 0;
}

void vs_stop() {
    reclaim_tx();
    drain_event_queue();
    stop_and_release_stream(g_playback);
    reclaim_tx();  // el device devuelve los buffers en vuelo tras STOP/RELEASE
    if (g_tx_drops != 0) {
        console::printf("virtio-sound: stop con %u periodos descartados\n", static_cast<unsigned>(g_tx_drops));
    }
    reset_tx_ring();
}

bool vs_capture_ready() {
    return g_ready && g_capture.selected;
}

bool vs_capture_configure() {
    if (!g_ready || !g_capture.selected) {
        return false;
    }
    drain_stale_rx();
    drain_event_queue();
    if (!ensure_stream_ready(g_capture)) {
        return false;
    }
    if (!g_rx_primed) {
        prime_rx_slots();
        g_rx_primed = true;
    }
    return true;
}

// A diferencia de vs_submit_period (fire-and-forget: descarta si el ring esta
// lleno), un read() tiene que devolver audio de verdad o un error -- no hay
// "silencio aceptable" del lado captura. wait_for_used_element bloquea
// (polling acotado) hasta que el device complete un periodo.
int vs_capture_read_period(uint64_t user_buffer, uint32_t byte_count) {
    if (byte_count == 0 || byte_count > kAudioPeriodBytes) {
        return -static_cast<int>(SAVANXP_EINVAL);
    }

    virtio_pci::UsedElement element = {};
    if (!wait_for_used_element(g_rx_queue, element)) {
        return -static_cast<int>(SAVANXP_EIO);
    }

    const uint16_t slot = static_cast<uint16_t>(element.id / kRxDescriptorsPerSlot);
    if (slot >= kRxSlots) {
        return -static_cast<int>(SAVANXP_EIO);
    }

    // element.len es cuanto lleno realmente el device (<= kAudioPeriodBytes).
    // Si el caller pidio menos que un periodo entero (ultimo chunk parcial de
    // un read() que no es multiplo de period_bytes), el resto capturado en
    // ese periodo se descarta: no hay donde bufferearlo hasta el proximo
    // read() sin sumar una cola aparte, y en la practica audiotest siempre
    // pide multiplos de period_bytes.
    const uint32_t captured = element.len < kAudioPeriodBytes ? element.len : kAudioPeriodBytes;
    const uint32_t usable = captured < byte_count ? captured : byte_count;
    const void* payload = virtio_pci::queue_extra(g_rx_queue, rx_slot_data_offset(slot));
    if (usable != 0 && !process::copy_to_user(user_buffer, payload, usable)) {
        return -static_cast<int>(SAVANXP_EINVAL);
    }

    return post_rx_slot(slot) ? 0 : -static_cast<int>(SAVANXP_EIO);
}

void vs_capture_stop() {
    drain_event_queue();
    stop_and_release_stream(g_capture);
    drain_stale_rx();
    g_rx_primed = false;
}

const audio::Backend g_backend = {
    .ready = vs_ready,
    .get_info = vs_get_info,
    .configure = vs_configure,
    .submit_period = vs_submit_period,
    .stop = vs_stop,
    .capture_ready = vs_capture_ready,
    .capture_configure = vs_capture_configure,
    .capture_read_period = vs_capture_read_period,
    .capture_stop = vs_capture_stop,
};

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
    g_playback = {};
    g_capture = {};
    g_last_status_code = 0;
    g_ready = false;
    g_event_buffer_armed = false;
    g_tx_usable_slots = 0;
    reset_tx_ring();
    reset_stream_state(g_playback);
    reset_stream_state(g_capture);

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
        !virtio_pci::setup_queue(g_device, kVirtioSoundTxQueue, kTxQueueLimit, kTxQueueExtraBytes, 16, g_tx_queue) ||
        !virtio_pci::setup_queue(g_device, kVirtioSoundRxQueue, kRxQueueLimit, kRxQueueExtraBytes, 16, g_rx_queue)) {
        fail_device("failed to setup queues");
        return;
    }

    // Cuantos slots de reproduccion caben en la cola que negocio el device (cada
    // uno usa 3 descriptores). En QEMU la TX suele ser grande y quedan los 8.
    g_tx_usable_slots = kTxSlots;
    if (g_tx_queue.size / kTxDescriptorsPerSlot < g_tx_usable_slots) {
        g_tx_usable_slots = static_cast<uint16_t>(g_tx_queue.size / kTxDescriptorsPerSlot);
    }

    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::device_status(g_device) | virtio_pci::kStatusDriverOk));
    g_ready = true;

    if (device_cfg()->streams == 0) {
        fail_device("device exposes no PCM streams");
        return;
    }
    if (!select_stream(kVirtioSndDirectionOutput, g_playback)) {
        fail_device("failed to query PCM stream info");
        return;
    }
    if (!g_playback.selected) {
        // El dispositivo responde pero ningun stream de salida ofrece el formato
        // fijo del ABI (S16 / 48 kHz / estereo). Antes esto quedaba mudo sin
        // traza; ahora se registra para no confundirlo con "no hay hardware".
        fail_device("no compatible output stream (S16/48kHz/stereo)");
        return;
    }
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
    if (!queue_event_buffer()) {
        fail_device("failed to arm event queue");
        return;
    }

    // Captura opcional: el perfil sin -Virtio-record (streams=1) no ofrece
    // ningun stream de entrada y eso es normal, no un fallo del device -- solo
    // deja /dev/audio0 sin soporte de lectura. Un error real de query aca
    // (comunicacion con el device rota) tampoco tira abajo la salida, que ya
    // quedo funcionando: se registra y se sigue con capture_ready() en false.
    if (!select_stream(kVirtioSndDirectionInput, g_capture)) {
        console::write_line("virtio-sound: failed to query PCM stream info for capture");
    }

    console::printf(
        "virtio-sound: ready pci=%x:%x.%u stream=%u pcm=%uHz/%uch/%ubit%s\n",
        static_cast<unsigned>(g_device.pci_device.bus),
        static_cast<unsigned>(g_device.pci_device.slot),
        static_cast<unsigned>(g_device.pci_device.function),
        static_cast<unsigned>(g_playback.stream_id),
        static_cast<unsigned>(g_audio_info.sample_rate_hz),
        static_cast<unsigned>(g_audio_info.channels),
        static_cast<unsigned>(g_audio_info.bits_per_sample),
        g_capture.selected ? " capture=on" : ""
    );
}

bool ready() {
    return g_ready && g_playback.selected;
}

const audio::Backend& backend() {
    return g_backend;
}

namespace {
// Prioridad alta: espejo de virtio-gpu frente al framebuffer plano.
constexpr int kDriverPriority = 100;

bool driver_probe() {
    initialize();
    return ready();
}

const audio::Driver kDriver = {
    "virtio-sound",
    kDriverPriority,
    &driver_probe,
    &backend,
};
} // namespace

const audio::Driver& driver() { return kDriver; }

} // namespace virtio_sound
