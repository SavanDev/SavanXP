#include "kernel/ata.hpp"

#include <stddef.h>
#include <stdint.h>

namespace {

constexpr uint16_t kPrimaryIoBase = 0x1f0;
constexpr uint16_t kPrimaryControlBase = 0x3f6;
constexpr uint16_t kSecondaryIoBase = 0x170;
constexpr uint16_t kSecondaryControlBase = 0x376;
constexpr size_t kSlotCount = 4;

constexpr uint8_t kStatusErr = 0x01;
constexpr uint8_t kStatusDrq = 0x08;
constexpr uint8_t kStatusDfq = 0x20;
constexpr uint8_t kStatusBsy = 0x80;

constexpr uint8_t kCommandIdentify = 0xec;
constexpr uint8_t kCommandReadSectors = 0x20;
constexpr uint8_t kCommandWriteSectors = 0x30;
constexpr uint8_t kCommandCacheFlush = 0xe7;

// Tope de sectores por comando del PIO de 28 bits. Es del protocolo ATA, no del
// contrato de block::, asi que la particion en pedazos vive aca: el registro de
// conteo de sectores es de 8 bits, asi que un comando no puede pedir mas de 256
// (o sea, 255 sin ambiguedad con el 0 que significa 256).
constexpr uint32_t kMaxSectorsPerCommand = 255;

// Prioridad alta: los ATA enumeran antes que el ramdisk, asi un disco IDE
// persistente (dev) le gana a la imagen del LiveCD cuando sxfs elige que montar.
constexpr int kDriverPriority = 100;

struct Slot {
    uint16_t io_base;
    uint16_t control_base;
    uint8_t drive_select;
    uint32_t sector_count;
};

Slot g_slots[kSlotCount] = {
    {kPrimaryIoBase, kPrimaryControlBase, 0x00, 0},
    {kPrimaryIoBase, kPrimaryControlBase, 0x10, 0},
    {kSecondaryIoBase, kSecondaryControlBase, 0x00, 0},
    {kSecondaryIoBase, kSecondaryControlBase, 0x10, 0},
};

const char* const kSlotNames[kSlotCount] = {"ata0", "ata1", "ata2", "ata3"};

void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint16_t inw(uint16_t port) {
    uint16_t value = 0;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

uint32_t inl(uint16_t port) {
    uint32_t value = 0;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void outl(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

void io_wait() {
    outb(0x80, 0);
}

void wait_400ns(const Slot& slot) {
    (void)inb(slot.control_base);
    (void)inb(slot.control_base);
    (void)inb(slot.control_base);
    (void)inb(slot.control_base);
}

void select_drive(const Slot& slot, uint32_t lba) {
    outb(
        static_cast<uint16_t>(slot.io_base + 6),
        static_cast<uint8_t>(0xe0u | slot.drive_select | ((lba >> 24) & 0x0f))
    );
    wait_400ns(slot);
}

bool poll_status(const Slot& slot, bool require_drq) {
    uint8_t status = 0;
    uint32_t spins = 1000000;
    do {
        status = inb(static_cast<uint16_t>(slot.io_base + 7));
    } while ((status & kStatusBsy) != 0 && --spins != 0);
    if (spins == 0) {
        return false;
    }

    if ((status & (kStatusErr | kStatusDfq)) != 0) {
        return false;
    }

    if (require_drq) {
        spins = 1000000;
        while ((status & kStatusDrq) == 0 && --spins != 0) {
            status = inb(static_cast<uint16_t>(slot.io_base + 7));
            if ((status & (kStatusErr | kStatusDfq)) != 0) {
                return false;
            }
        }
        if ((status & kStatusDrq) == 0) {
            return false;
        }
    }

    return true;
}

bool identify(Slot& slot) {
    slot.sector_count = 0;

    select_drive(slot, 0);
    outb(static_cast<uint16_t>(slot.control_base), 0);
    outb(static_cast<uint16_t>(slot.io_base + 2), 0);
    outb(static_cast<uint16_t>(slot.io_base + 3), 0);
    outb(static_cast<uint16_t>(slot.io_base + 4), 0);
    outb(static_cast<uint16_t>(slot.io_base + 5), 0);
    outb(static_cast<uint16_t>(slot.io_base + 7), kCommandIdentify);

    const uint8_t initial_status = inb(static_cast<uint16_t>(slot.io_base + 7));
    if (initial_status == 0) {
        return false;
    }

    uint8_t lba_mid = inb(static_cast<uint16_t>(slot.io_base + 4));
    uint8_t lba_high = inb(static_cast<uint16_t>(slot.io_base + 5));
    if (lba_mid != 0 || lba_high != 0) {
        return false;
    }

    if (!poll_status(slot, true)) {
        return false;
    }

    uint16_t identify_data[256] = {};
    for (size_t index = 0; index < 256; ++index) {
        identify_data[index] = inw(slot.io_base);
    }

    const uint32_t sector_count =
        static_cast<uint32_t>(identify_data[60]) |
        (static_cast<uint32_t>(identify_data[61]) << 16);
    if (sector_count == 0) {
        return false;
    }

    slot.sector_count = sector_count;
    return true;
}

// Una sola transferancia PIO de hasta kMaxSectorsPerCommand sectores.
bool rw_chunk(Slot& slot, uint32_t lba, uint32_t sector_count, uint8_t* bytes, bool write) {
    select_drive(slot, lba);
    outb(static_cast<uint16_t>(slot.io_base + 1), 0);
    outb(static_cast<uint16_t>(slot.io_base + 2), static_cast<uint8_t>(sector_count));
    outb(static_cast<uint16_t>(slot.io_base + 3), static_cast<uint8_t>(lba & 0xff));
    outb(static_cast<uint16_t>(slot.io_base + 4), static_cast<uint8_t>((lba >> 8) & 0xff));
    outb(static_cast<uint16_t>(slot.io_base + 5), static_cast<uint8_t>((lba >> 16) & 0xff));
    outb(static_cast<uint16_t>(slot.io_base + 7), write ? kCommandWriteSectors : kCommandReadSectors);

    for (uint32_t sector = 0; sector < sector_count; ++sector) {
        if (!poll_status(slot, true)) {
            return false;
        }

        /* 32 bits por acceso al puerto de datos, no 16.
         *
         * El puerto de datos es una ventana de 16 bits sobre el sector, asi que la
         * forma canonica es ir de palabra en palabra. Se puede de doble en doble, y el
         * protocolo lo permite: el dispositivo ve dos mitades del mismo dword y las
         * entrega en orden. Menos la mitad de las salidas a puerto para los mismos
         * bytes.
         *
         * Medido con la fixture de progman-smoke, que escribe 390 KB y por lo tanto
         * pasa por ~16.000 sectores: 365930 y 401218 ms con 16 bits, 280703 y 278922
         * con 32. Un 26% menos, reproducible en dos muestras de cada.
         *
         * Y 26% es mucho menos que la mitad, que es lo que haria falta si el coste
         * estuviese en el numero de escrituras. No esta: esta en algo FIJO por sector,
         * del lado del dispositivo emulado, que cobra una peticion al medio de
         * almacenamiento por sector. Medido aparte: quitar el cache flush --143 por
         * este mismo fichero-- no cambio nada (379989 ms), y tampoco es el numero de
         * escrituras, porque duplicarlo solo dio el 26%.
         *
         * Lo que si lo quita es DMA de bus master, que entrega los sectores en una
         * sola peticion. isa-ide lo ofrece y el kernel no lo usa. Es la pieza que
         * falta, y no se escribe aqui porque es otra cosa: una tabla de descriptores
         * en memoria del kernel, barreras, y esperar a que el dispositivo la recorra. */
        if (write) {
            for (size_t dword = 0; dword < (block::kSectorSize / sizeof(uint32_t)); ++dword) {
                const size_t byte_index = static_cast<size_t>(sector) * block::kSectorSize + dword * sizeof(uint32_t);
                const uint32_t value =
                    static_cast<uint32_t>(bytes[byte_index]) |
                    (static_cast<uint32_t>(bytes[byte_index + 1]) << 8) |
                    (static_cast<uint32_t>(bytes[byte_index + 2]) << 16) |
                    (static_cast<uint32_t>(bytes[byte_index + 3]) << 24);
                outl(slot.io_base, value);
            }
        } else {
            for (size_t dword = 0; dword < (block::kSectorSize / sizeof(uint32_t)); ++dword) {
                const uint32_t value = inl(slot.io_base);
                const size_t byte_index = static_cast<size_t>(sector) * block::kSectorSize + dword * sizeof(uint32_t);
                bytes[byte_index] = static_cast<uint8_t>(value & 0xff);
                bytes[byte_index + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
                bytes[byte_index + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
                bytes[byte_index + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
            }
        }
    }

    if (write) {
        outb(static_cast<uint16_t>(slot.io_base + 7), kCommandCacheFlush);
        if (!poll_status(slot, false)) {
            return false;
        }
    }

    return true;
}

// El rango de LBA ya lo valido block::read/write; aca solo queda el tope por
// comando del protocolo, y se resuelve troceando en vez de fallar. Antes
// rw_sectors rechazaba cualquier peticion de mas de 255 sectores, y eso era
// inofensivo solo porque la metadata de SxFS cabia en 97. Con la tabla de inodos
// de 1024 sectores y el bitmap de bloques de 512, los commits de metadata piden
// 1537 sectores de una: habrian fallado los cuatro, y con ellos el montaje, la
// recuperacion del journal y toda mutacion, en la maquina base por defecto. Es el
// mismo troceo que ya hace virtio-blk, con la diferencia de que hay que volver
// a emitir select_drive con el LBA adelantado, porque el registro LBA es de 28
// bits y no puede addressing mas alla de 128 GiB por si solo.
bool rw_sectors(Slot& slot, uint32_t lba, uint32_t sector_count, void* buffer, bool write) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    while (sector_count != 0) {
        const uint32_t chunk = sector_count > kMaxSectorsPerCommand ? kMaxSectorsPerCommand : sector_count;
        if (!rw_chunk(slot, lba, chunk, bytes, write)) {
            return false;
        }
        lba += chunk;
        bytes += static_cast<size_t>(chunk) * block::kSectorSize;
        sector_count -= chunk;
    }
    return true;
}

bool read_op(void* context, uint32_t lba, uint32_t sector_count, void* buffer) {
    return rw_sectors(*static_cast<Slot*>(context), lba, sector_count, buffer, false);
}

bool write_op(void* context, uint32_t lba, uint32_t sector_count, const void* buffer) {
    return rw_sectors(*static_cast<Slot*>(context), lba, sector_count, const_cast<void*>(buffer), true);
}

const block::DeviceOps kOps = {
    &read_op,
    &write_op,
};

void enumerate() {
    for (size_t index = 0; index < kSlotCount; ++index) {
        if (identify(g_slots[index])) {
            (void)block::register_device(
                kOps, &g_slots[index], g_slots[index].sector_count, /*writable=*/true, kSlotNames[index]);
        }
        io_wait();
    }
}

const block::Driver kDriver = {
    "ata",
    kDriverPriority,
    &enumerate,
};

} // namespace

namespace ata {

const block::Driver& driver() { return kDriver; }

} // namespace ata
