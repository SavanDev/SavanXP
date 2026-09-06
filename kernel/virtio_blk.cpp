#include "kernel/virtio_blk.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/string.hpp"
#include "kernel/virtio_pci.hpp"

namespace {

constexpr uint16_t kVirtioBlkModernDevice = 0x1042u;
constexpr uint16_t kVirtioBlkSubsystemDevice = 2u;
constexpr uint16_t kRequestQueueIndex = 0;
constexpr uint16_t kRequestQueueDescriptorLimit = 8;

// VIRTIO_BLK_F_RO (bit 5): el device es de solo lectura.
// VIRTIO_BLK_F_FLUSH (bit 9): soporta VIRTIO_BLK_T_FLUSH. Sin este feature no
// hay forma de pedirle al backend que baje a disco lo que ya confirmo, asi que
// solo se pide si el device lo anuncia: negotiate_features rechaza el paquete
// entero si alguno de los bits pedidos no esta en device_feature.
constexpr uint32_t kFeatureRoBit = 1u << 5;
constexpr uint32_t kFeatureFlushBit = 1u << 9;

constexpr uint32_t kReqTypeIn = 0;    // lectura: el device escribe el buffer de datos.
constexpr uint32_t kReqTypeOut = 1;   // escritura: el driver escribe, el device lee.
constexpr uint32_t kReqTypeFlush = 4; // sin descriptor de datos.
constexpr uint8_t kStatusOk = 0;
constexpr uint8_t kStatusSentinel = 0xffu;

// Tope de sectores por request virtio. Los rangos mas grandes (bitmaps/inode
// table de SxFS) se parten aca en varios requests sincronicos en vez de
// fallar directo, a diferencia de ata::rw_sectors con su tope duro de 255
// sectores por comando PIO.
constexpr uint32_t kMaxSectorsPerRequest = 128;
constexpr size_t kDataBufferBytes = static_cast<size_t>(kMaxSectorsPerRequest) * block::kSectorSize;

// Buffers redondos dentro de la region "extra" de la cola, mismo estilo que
// los slots de virtio_gpu (kRequestBufferBytes/kResponseBufferBytes): el
// header real son 16 bytes y el status 1, el resto es margen.
constexpr size_t kHeaderBufferBytes = 32;
constexpr size_t kStatusBufferBytes = 32;
constexpr size_t kHeaderOffset = 0;
constexpr size_t kDataOffset = kHeaderBufferBytes;
constexpr size_t kStatusOffset = kDataOffset + kDataBufferBytes;
constexpr size_t kExtraBytes = kStatusOffset + kStatusBufferBytes;

// Vueltas de espera activa por request antes de darlo por perdido. El resto
// del stack de storage (ata::poll_status) tambien es polling puro: todavia no
// hay interrupciones de storage en este kernel.
constexpr uint32_t kMaxWaitSpins = 200000000u;

struct [[gnu::packed]] VirtioBlkConfig {
    uint64_t capacity; // sectores de 512 bytes, siempre, se negocie o no blk_size.
};

struct [[gnu::packed]] VirtioBlkReqHeader {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

virtio_pci::Device g_device = {};
virtio_pci::Queue g_request_queue = {};
bool g_ready = false;
bool g_flush_supported = false;

volatile VirtioBlkConfig* device_cfg() {
    return reinterpret_cast<volatile VirtioBlkConfig*>(virtio_pci::device_cfg_base(g_device));
}

VirtioBlkReqHeader* request_header() {
    return reinterpret_cast<VirtioBlkReqHeader*>(virtio_pci::queue_extra(g_request_queue, kHeaderOffset));
}

uint64_t request_header_physical() {
    return virtio_pci::queue_extra_physical(g_request_queue, kHeaderOffset);
}

uint8_t* data_buffer() {
    return virtio_pci::queue_extra(g_request_queue, kDataOffset);
}

uint64_t data_buffer_physical() {
    return virtio_pci::queue_extra_physical(g_request_queue, kDataOffset);
}

volatile uint8_t* status_byte() {
    return reinterpret_cast<volatile uint8_t*>(virtio_pci::queue_extra(g_request_queue, kStatusOffset));
}

uint64_t status_byte_physical() {
    return virtio_pci::queue_extra_physical(g_request_queue, kStatusOffset);
}

// Un unico request sincronico en vuelo por vez: arma la cadena de
// descriptores 0 (header, out) -> 1 (datos, opcional) -> 2 (status, in),
// notifica y hace polling del used ring hasta que el device la despacha.
// data_bytes == 0 se usa para FLUSH: la cadena queda header -> status.
bool submit_and_wait(uint32_t type, uint64_t sector, void* data, uint32_t data_bytes, bool device_writes_data) {
    if (!g_ready || !g_request_queue.enabled) {
        return false;
    }

    VirtioBlkReqHeader* header = request_header();
    header->type = type;
    header->reserved = 0;
    header->sector = sector;
    *status_byte() = kStatusSentinel;

    if (data_bytes != 0 && !device_writes_data) {
        memcpy(data_buffer(), data, data_bytes);
    }

    virtio_pci::Descriptor* descriptors = virtio_pci::queue_descriptors(g_request_queue);
    uint16_t status_index = 1;
    descriptors[0] = {
        .addr = request_header_physical(),
        .len = sizeof(VirtioBlkReqHeader),
        .flags = virtio_pci::kDescriptorFlagNext,
        .next = 1,
    };
    if (data_bytes != 0) {
        status_index = 2;
        const uint16_t data_flags = static_cast<uint16_t>(
            virtio_pci::kDescriptorFlagNext | (device_writes_data ? virtio_pci::kDescriptorFlagWrite : 0));
        descriptors[1] = {
            .addr = data_buffer_physical(),
            .len = data_bytes,
            .flags = data_flags,
            .next = 2,
        };
    }
    descriptors[status_index] = {
        .addr = status_byte_physical(),
        .len = 1,
        .flags = virtio_pci::kDescriptorFlagWrite,
        .next = 0,
    };

    virtio_pci::memory_barrier();
    if (!virtio_pci::submit_descriptor_head(g_request_queue, 0)) {
        return false;
    }
    virtio_pci::memory_barrier();
    virtio_pci::notify_queue(g_device, g_request_queue);

    const volatile virtio_pci::UsedHeader* used = virtio_pci::queue_used_header(g_request_queue);
    uint32_t spins = kMaxWaitSpins;
    while (g_request_queue.last_used_index == used->idx) {
        if (--spins == 0) {
            return false;
        }
    }
    virtio_pci::memory_barrier();
    g_request_queue.last_used_index = static_cast<uint16_t>(g_request_queue.last_used_index + 1);

    if (*status_byte() != kStatusOk) {
        return false;
    }

    if (data_bytes != 0 && device_writes_data) {
        memcpy(data, data_buffer(), data_bytes);
    }
    return true;
}

bool flush() {
    if (!g_flush_supported) {
        return true;
    }
    return submit_and_wait(kReqTypeFlush, 0, nullptr, 0, false);
}

// El rango de LBA ya lo valido block::read/write; aca solo se parte en chunks
// de kMaxSectorsPerRequest, cada uno un request virtio sincronico distinto.
bool rw_sectors(void* context, uint32_t lba, uint32_t sector_count, void* buffer, bool write) {
    (void)context;
    auto* bytes = static_cast<uint8_t*>(buffer);

    while (sector_count != 0) {
        const uint32_t chunk = sector_count > kMaxSectorsPerRequest ? kMaxSectorsPerRequest : sector_count;
        const uint32_t chunk_bytes = chunk * block::kSectorSize;
        if (!submit_and_wait(
                write ? kReqTypeOut : kReqTypeIn,
                lba,
                bytes,
                chunk_bytes,
                /*device_writes_data=*/!write)) {
            return false;
        }

        lba += chunk;
        sector_count -= chunk;
        bytes += chunk_bytes;
    }

    // Mismo motivo que kCommandCacheFlush en ata::rw_sectors: sin esto una
    // escritura "exitosa" puede quedar solo en la write-cache del backend de
    // QEMU y perderse ante un corte, corrompiendo el journal de SxFS.
    return !write || flush();
}

bool read_op(void* context, uint32_t lba, uint32_t sector_count, void* buffer) {
    return rw_sectors(context, lba, sector_count, buffer, false);
}

bool write_op(void* context, uint32_t lba, uint32_t sector_count, const void* buffer) {
    return rw_sectors(context, lba, sector_count, const_cast<void*>(buffer), true);
}

const block::DeviceOps kOps = {
    &read_op,
    &write_op,
};

void fail_device(const char* reason) {
    virtio_pci::fail_device(g_device);
    if (reason != nullptr) {
        console::printf("virtio-blk: %s\n", reason);
    }
    g_ready = false;
}

void enumerate() {
    memset(&g_device, 0, sizeof(g_device));
    memset(&g_request_queue, 0, sizeof(g_request_queue));
    g_ready = false;
    g_flush_supported = false;

    pci::DeviceInfo pci_device = {};
    if (!pci::ready() || !virtio_pci::find_modern_device(kVirtioBlkModernDevice, kVirtioBlkSubsystemDevice, pci_device)) {
        return;
    }

    if (!virtio_pci::initialize_device(pci_device, true, g_device)) {
        console::write_line("virtio-blk: missing required MMIO capabilities");
        return;
    }

    virtio_pci::set_device_status(g_device, 0);
    virtio_pci::memory_barrier();
    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::kStatusAcknowledge | virtio_pci::kStatusDriver));

    // Se piden RO/FLUSH solo si el device los anuncia: negotiate_features
    // rechaza el paquete entero si algun bit pedido no esta en device_feature.
    volatile virtio_pci::CommonCfg* cfg = virtio_pci::common_cfg(g_device);
    cfg->device_feature_select = 0;
    virtio_pci::memory_barrier();
    const uint32_t device_features_word0 = cfg->device_feature;
    const bool read_only = (device_features_word0 & kFeatureRoBit) != 0;
    const uint32_t requested_word0 = device_features_word0 & (kFeatureRoBit | kFeatureFlushBit);

    if (!virtio_pci::negotiate_features(g_device, requested_word0, virtio_pci::kFeatureVersion1Bit)) {
        fail_device("feature negotiation failed");
        return;
    }
    g_flush_supported = (requested_word0 & kFeatureFlushBit) != 0;

    const uint64_t capacity_sectors = device_cfg()->capacity;
    if (capacity_sectors == 0 || capacity_sectors > 0xffffffffu) {
        fail_device("unsupported capacity");
        return;
    }

    if (!virtio_pci::setup_queue(g_device, kRequestQueueIndex, kRequestQueueDescriptorLimit, kExtraBytes, 16, g_request_queue)) {
        fail_device("failed to initialize request queue");
        return;
    }

    virtio_pci::set_device_status(g_device, static_cast<uint8_t>(virtio_pci::device_status(g_device) | virtio_pci::kStatusDriverOk));
    g_ready = true;

    (void)block::register_device(kOps, nullptr, static_cast<uint32_t>(capacity_sectors), !read_only, "vblk0");

    console::printf(
        "virtio-blk: ready pci=%x:%x.%u capacity=%u sectors flush=%s\n",
        static_cast<unsigned>(g_device.pci_device.bus),
        static_cast<unsigned>(g_device.pci_device.slot),
        static_cast<unsigned>(g_device.pci_device.function),
        static_cast<unsigned>(capacity_sectors),
        g_flush_supported ? "yes" : "no"
    );
}

// Mayor prioridad que ata:: (100): en una maquina -Virtio, isa-ide ni siquiera
// esta en la linea de comandos de QEMU, asi que en la practica nunca compiten
// por el mismo indice de device; esto solo fija el orden si algun dia
// coexistieran los dos frentes sobre discos distintos.
constexpr int kDriverPriority = 110;

const block::Driver kDriver = {
    "virtio-blk",
    kDriverPriority,
    &enumerate,
};

} // namespace

namespace virtio_blk {

const block::Driver& driver() { return kDriver; }

} // namespace virtio_blk
