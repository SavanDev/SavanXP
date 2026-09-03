#include "kernel/partition.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/string.hpp"

namespace {

// Mas baja que ata (100) y que ramdisk (10): probe_all ordena por prioridad
// descendente, asi que este enumerate corre cuando ya no queda disco fisico por
// aparecer y puede leer las tablas de todos.
constexpr int kDriverPriority = -1000;

// 4 primarias por disco alcanzan para el disco del instalador (ESP + raiz) y
// para el pendrive tipico. Si se llena, las de mas quedan sin registrar y el
// resto del sistema sigue andando.
constexpr size_t kMaxSlices = 8;

constexpr size_t kMbrTableOffset = 446;
constexpr size_t kMbrEntrySize = 16;
constexpr size_t kMbrEntryCount = 4;
constexpr uint8_t kMbrTypeEmpty = 0x00;
constexpr uint8_t kMbrTypeExtendedChs = 0x05;
constexpr uint8_t kMbrTypeExtendedLba = 0x0f;
constexpr uint8_t kMbrTypeExtendedLinux = 0x85;
constexpr uint8_t kMbrTypeGptProtective = 0xee;
constexpr uint8_t kMbrTypeEsp = 0xef;

constexpr uint32_t kGptHeaderLba = 1;
constexpr size_t kGptMaxEntries = 128;

// C12A7328-F81F-11D2-BA4B-00A0C93EC93B tal como queda en disco: los tres
// primeros campos van little-endian y los ultimos 8 bytes tal cual.
const uint8_t kEspTypeGuid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};

struct Slice {
    bool present;
    size_t device_index;
    size_t parent_index;
    uint32_t start_lba;
    uint32_t sector_count;
    uint8_t mbr_type;
    bool esp;
};

Slice g_slices[kMaxSlices] = {};
size_t g_slice_count = 0;

// block::DeviceInfo guarda el name por puntero, no lo copia: el buffer tiene
// que sobrevivir al enumerate.
char g_names[kMaxSlices][24] = {};

uint16_t load_le16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
}

uint32_t load_le32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t load_le64(const uint8_t* data) {
    return static_cast<uint64_t>(load_le32(data)) |
        (static_cast<uint64_t>(load_le32(data + 4)) << 32);
}

bool is_extended(uint8_t type) {
    return type == kMbrTypeExtendedChs || type == kMbrTypeExtendedLba ||
        type == kMbrTypeExtendedLinux;
}

// "<padre>p<n>", con n en base 1 como en sda1. Se arma a mano porque el kernel
// no tiene snprintf y el nombre es puro cosmetico para sysinfo.
void build_name(char* out, size_t capacity, const char* parent, size_t ordinal) {
    size_t cursor = 0;
    if (parent != nullptr) {
        while (parent[cursor] != 0 && cursor + 4 < capacity) {
            out[cursor] = parent[cursor];
            ++cursor;
        }
    }
    out[cursor++] = 'p';
    if (ordinal >= 10) {
        out[cursor++] = static_cast<char>('0' + ((ordinal / 10) % 10));
    }
    out[cursor++] = static_cast<char>('0' + (ordinal % 10));
    out[cursor] = 0;
}

bool read_op(void* context, uint32_t lba, uint32_t sector_count, void* buffer) {
    const Slice& slice = *static_cast<Slice*>(context);
    // El rango contra sector_count ya lo valido block::read con las dimensiones
    // de la particion: aca solo queda correr el origen.
    return block::read(slice.parent_index, slice.start_lba + lba, sector_count, buffer);
}

bool write_op(void* context, uint32_t lba, uint32_t sector_count, const void* buffer) {
    const Slice& slice = *static_cast<Slice*>(context);
    return block::write(slice.parent_index, slice.start_lba + lba, sector_count, buffer);
}

const block::DeviceOps kOps = {
    &read_op,
    &write_op,
};

// Recorta la particion contra el tamano real del padre antes de registrarla:
// una tabla mentirosa (imagen truncada, disco clonado a uno mas chico) no tiene
// que poder generar lecturas fuera del device.
bool add_slice(
    size_t parent_index,
    const char* parent_name,
    bool parent_writable,
    uint32_t parent_sectors,
    uint32_t start_lba,
    uint32_t sector_count,
    uint8_t mbr_type,
    bool esp,
    size_t ordinal)
{
    if (g_slice_count >= kMaxSlices || sector_count == 0 || start_lba >= parent_sectors) {
        return false;
    }
    const uint32_t available = parent_sectors - start_lba;
    if (sector_count > available) {
        sector_count = available;
    }

    Slice& slice = g_slices[g_slice_count];
    slice.present = true;
    slice.parent_index = parent_index;
    slice.start_lba = start_lba;
    slice.sector_count = sector_count;
    slice.mbr_type = mbr_type;
    slice.esp = esp;
    slice.device_index = block::device_count();

    build_name(g_names[g_slice_count], sizeof(g_names[g_slice_count]), parent_name, ordinal);
    if (!block::register_device(kOps, &slice, sector_count, parent_writable, g_names[g_slice_count])) {
        slice.present = false;
        return false;
    }
    ++g_slice_count;
    return true;
}

// true si el device tiene una GPT valida, aunque no haya quedado ninguna
// particion registrada: el caller no debe caer al MBR en ese caso.
bool scan_gpt(size_t parent_index, const char* parent_name, bool parent_writable, uint32_t parent_sectors) {
    uint8_t sector[block::kSectorSize] = {};
    if (!block::read(parent_index, kGptHeaderLba, 1, sector)) {
        return false;
    }
    if (memcmp(sector, "EFI PART", 8) != 0) {
        return false;
    }

    const uint64_t entry_lba = load_le64(sector + 72);
    const uint32_t entry_count = load_le32(sector + 80);
    const uint32_t entry_size = load_le32(sector + 84);
    if (entry_lba == 0 || entry_lba >= parent_sectors || entry_size < 128 ||
        entry_size > block::kSectorSize || (block::kSectorSize % entry_size) != 0) {
        return false;
    }

    const uint32_t per_sector = block::kSectorSize / entry_size;
    const uint32_t total = entry_count < kGptMaxEntries ? entry_count : kGptMaxEntries;
    const uint8_t zero_guid[16] = {};

    for (uint32_t index = 0; index < total; index += per_sector) {
        const uint32_t lba = static_cast<uint32_t>(entry_lba) + (index / per_sector);
        if (lba >= parent_sectors || !block::read(parent_index, lba, 1, sector)) {
            break;
        }
        for (uint32_t slot = 0; slot < per_sector && (index + slot) < total; ++slot) {
            const uint8_t* entry = sector + (slot * entry_size);
            if (memcmp(entry, zero_guid, sizeof(zero_guid)) == 0) {
                continue;
            }
            const uint64_t first = load_le64(entry + 32);
            const uint64_t last = load_le64(entry + 40);
            // block:: direcciona en 32 bits (2 TiB con sectores de 512): una
            // particion que arranca mas alla se saltea entera en vez de
            // registrarse truncada y mentir sobre su contenido.
            if (last < first || first > 0xffffffffull || last > 0xffffffffull) {
                continue;
            }
            add_slice(
                parent_index,
                parent_name,
                parent_writable,
                parent_sectors,
                static_cast<uint32_t>(first),
                static_cast<uint32_t>(last - first + 1),
                /*mbr_type=*/0,
                memcmp(entry, kEspTypeGuid, sizeof(kEspTypeGuid)) == 0,
                index + slot + 1);
        }
    }
    return true;
}

void scan_mbr(
    size_t parent_index,
    const char* parent_name,
    bool parent_writable,
    uint32_t parent_sectors,
    const uint8_t* boot_sector)
{
    for (size_t index = 0; index < kMbrEntryCount; ++index) {
        const uint8_t* entry = boot_sector + kMbrTableOffset + (index * kMbrEntrySize);
        const uint8_t type = entry[4];
        // Las extendidas piden recorrer la cadena de EBRs. Todavia no hace
        // falta: el instalador escribe GPT y los pendrives de fabrica traen una
        // sola primaria.
        if (type == kMbrTypeEmpty || is_extended(type)) {
            continue;
        }
        add_slice(
            parent_index,
            parent_name,
            parent_writable,
            parent_sectors,
            load_le32(entry + 8),
            load_le32(entry + 12),
            type,
            type == kMbrTypeEsp,
            index + 1);
    }
}

bool has_protective_mbr(const uint8_t* boot_sector) {
    for (size_t entry = 0; entry < kMbrEntryCount; ++entry) {
        if (boot_sector[kMbrTableOffset + (entry * kMbrEntrySize) + 4] == kMbrTypeGptProtective) {
            return true;
        }
    }
    return false;
}

void enumerate() {
    memset(g_slices, 0, sizeof(g_slices));
    memset(g_names, 0, sizeof(g_names));
    g_slice_count = 0;

    // Snapshot antes del bucle: add_slice registra devices nuevos y el conteo
    // crece mientras iteramos. Congelarlo evita rebanar una particion.
    const size_t parents = block::device_count();
    for (size_t index = 0; index < parents; ++index) {
        block::DeviceInfo info = {};
        if (!block::device_info(index, info) || !info.present || info.sector_count < 2) {
            continue;
        }

        uint8_t boot_sector[block::kSectorSize] = {};
        if (!block::read(index, 0, 1, boot_sector)) {
            continue;
        }
        if (load_le16(boot_sector + 510) != 0xaa55) {
            continue;
        }

        // MBR protectivo: la tabla de 4 entries miente a proposito (una sola
        // particion 0xEE que tapa el disco) y la real es la GPT. Se intenta la
        // GPT igual sin el, porque hay imagenes hibridas sin 0xEE; el MBR solo
        // corre si no hay GPT valida.
        if (scan_gpt(index, info.name, info.writable, info.sector_count)) {
            continue;
        }
        if (has_protective_mbr(boot_sector)) {
            continue;
        }
        scan_mbr(index, info.name, info.writable, info.sector_count, boot_sector);
    }
}

const block::Driver kDriver = {
    "partition",
    kDriverPriority,
    &enumerate,
};

} // namespace

namespace partition {

const block::Driver& driver() { return kDriver; }

bool info(size_t device_index, Info& out) {
    for (size_t index = 0; index < g_slice_count; ++index) {
        const Slice& slice = g_slices[index];
        if (!slice.present || slice.device_index != device_index) {
            continue;
        }
        out.parent_index = slice.parent_index;
        out.start_lba = slice.start_lba;
        out.sector_count = slice.sector_count;
        out.mbr_type = slice.mbr_type;
        out.esp = slice.esp;
        return true;
    }
    return false;
}

} // namespace partition
