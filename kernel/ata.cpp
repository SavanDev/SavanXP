#include "kernel/ata.hpp"

#include "kernel/console.hpp"

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
// Intentos por comando PIO antes de rendirse. Tres es lo justo para un fallo
// transitorio sin convertir un fallo real en una espera larga: el peor caso es
// 3 veces el coste de un comando, y el caso normal ni lo nota.
constexpr uint32_t kMaxChunkAttempts = 3;
// Presupuesto de spins por espera de estado. Es un timeout vestido de otra
// cosa: mide trabajo emulado, no tiempo, asi que su valor en reloj de pared
// depende de la velocidad del host. Con el host tranquilo no se nota; con una
// pausa larga, si.
constexpr uint32_t kPollSpinBudget = 1000000;
// Reset por software (ATA/ATAPI): SRST con nIEN limpio resetea los dos
// dispositivos del canal; con nIEN puesto, solo el seleccionado.
constexpr uint8_t kControlSrst = 0x04;
constexpr uint8_t kControlSrstNien = 0x06;
// Rondas de reintento por comando: la primera sin reset, la segunda tras un
// reset por software. Tres intentos en cada una; el reset solo cuando los tres
// fallan, porque resetear es un martillo: tira el estado del dispositivo.
constexpr uint32_t kMaxChunkRounds = 2;

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

/*
 * Por que fallo un sondeo. El sondeo tiene dos presupuestos ACOTADOS de spins
 * y agotarlos se traduce en un fallo de E/S duro que sube hasta el usuario,
 * porque no hay reintento en toda la cadena (block::write -> write_op ->
 * rw_sectors -> rw_chunk). Eso convierte "el host va lento" en "una copia de
 * 390 KB se pierde tras cinco minutos".
 *
 * Estos contadores separan las dos causas que de otro modo se confunden:
 *
 *   bsy=N   el dispositivo no solto BSY a tiempo
 *   drq=N   BSY se solto pero DRQ no llego
 *   err=N   el dispositivo puso ERR/DFQ: el fallo es del emulado
 *
 * Los dos primeros son el mismo diagnostico -- un timeout disfrazado de
 * presupuesto de spins, cuyo verdugito depende de la velocidad del host.
 */
uint32_t g_poll_failures[3] = {0, 0, 0};
uint32_t g_poll_failures_reported = 0;
uint32_t g_poll_chunk_retries = 0;
uint32_t g_poll_device_resets = 0;
// Cuantos spins se gastaron en el peor agotamiento de BSY, y cuantos en el
// peor caso que SI funciono. Esta medicion decide el arreglo, y no necesita un
// fallo paraExistir: en una operacion larga hay miles de esperas que si
// funcionan, y su maximo dice cuanto margen le queda al presupuesto.
//
//   agotamiento cercano al presupuesto -> el dispositivo se quedo en BSY y
//     hace falta RESET (SRST).
//   maximo de exito muy por debajo del presupuesto -> el presupuesto es
//     corto y basta con subirlo.
uint32_t g_poll_bsy_worst_spins = 0;
uint32_t g_poll_bsy_max_ok_spins = 0;
uint32_t g_poll_drq_max_ok_spins = 0;
// Se informa cada vez que un maximo dobla, para no inundar el log y para que
// una carrera larga deje la curva a la vista sin depender de ningun fallo.
uint32_t g_poll_reported_threshold = 0;

bool poll_status(const Slot& slot, bool require_drq) {
    uint8_t status = 0;
    uint32_t spins = kPollSpinBudget;
    do {
        status = inb(static_cast<uint16_t>(slot.io_base + 7));
    } while ((status & kStatusBsy) != 0 && --spins != 0);
    if (spins != 0 && spins < kPollSpinBudget) {
        const uint32_t spent = kPollSpinBudget - spins;
        if (spent > g_poll_bsy_max_ok_spins) {
            g_poll_bsy_max_ok_spins = spent;
        }
    }
    if (spins == 0) {
        const uint32_t spent = kPollSpinBudget - spins;
        if (spent > g_poll_bsy_worst_spins) {
            g_poll_bsy_worst_spins = spent;
        }
        ++g_poll_failures[0];
        return false;
    }

    if ((status & (kStatusErr | kStatusDfq)) != 0) {
        ++g_poll_failures[2];
        return false;
    }

    if (require_drq) {
        spins = kPollSpinBudget;
        while ((status & kStatusDrq) == 0 && --spins != 0) {
            status = inb(static_cast<uint16_t>(slot.io_base + 7));
            if ((status & (kStatusErr | kStatusDfq)) != 0) {
                ++g_poll_failures[2];
                return false;
            }
        }
        if (spins != 0 && spins < kPollSpinBudget) {
            const uint32_t spent = kPollSpinBudget - spins;
            if (spent > g_poll_drq_max_ok_spins) {
                g_poll_drq_max_ok_spins = spent;
            }
        }
        if ((status & kStatusDrq) == 0) {
            ++g_poll_failures[1];
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

// Una sola transferencia PIO de hasta kMaxSectorsPerCommand sectores, sin reintentos.
bool rw_chunk_once(Slot& slot, uint32_t lba, uint32_t sector_count, uint8_t* bytes, bool write) {
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

        /* 32 bits per data-port access, not 16.
         *
         * The data port is a 16-bit window on the sector, so the canonical
         * transfer is word by word, but the protocol allows dwords: the device
         * sees both halves in order. Half the port accesses for the same bytes.
         *
         * Measured with progman-smoke's 390 KB fixture under TCG: 365930 and
         * 401218 ms with 16 bits, 280703 and 278922 with 32. 26% less, not the
         * half it would be if the cost were the number of accesses: most of it
         * is a fixed cost per sector in QEMU's IDE model, which issues one
         * backend request per sector of a PIO data phase.
         *
         * Only DMA removes that, and isa-ide has no bus master: it would take
         * an AHCI driver (q35 has an ICH9 AHCI controller on board). See
         * docs/fs/SXFS_ROADMAP.md, "Where the remaining cost is". */
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

    // The data may now sit in the drive's write cache; flush_op commits it.
    // Waiting for BSY to clear is what lets the next command be issued.
    if (write && !poll_status(slot, false)) {
        return false;
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
/*
 * Reset por software del dispositivo (SRST).
 *
 * Es lo que pide la especificacion cuando un comando se queda colgado, y lo
 * que aqui hace falta: la medicion dio que la peor espera que FUNCIONA cuesta
 * 412349 spins (41% del presupuesto) mientras el fallo agotaba los 1000000
 * tres veces seguidas. Mas de 2,4 veces el peor caso legitimo no es impaciencia,
 * es el dispositivo wedged, y reemitir el comando a un dispositivo wedged no
 * lo desataca.
 *
 * La secuencia son dos escrituras, no una: la primera con nIEN limpio resetea
 * los dos dispositivos del canal, y la segunda con nIEN puesto solo al que esta
 * seleccionado, que es el que nos interesa no molestar.
 *
 * Limite que NO se resuelve aqui: sin cerrojo por dispositivo, un reset puede
 * pisar una transferencia en curso de otro core. El driver hoy no serializa
 * accesos y esta rebanada no introduce bloqueos; es el mismo supuesto que ya
 * tiene el resto del camino de E/S.
 */
void software_reset(Slot& slot) {
    ++g_poll_device_resets;
    outb(static_cast<uint16_t>(slot.control_base), kControlSrst);
    wait_400ns(slot);
    (void)poll_status(slot, false);

    select_drive(slot, 0);
    outb(static_cast<uint16_t>(slot.control_base), kControlSrstNien);
    wait_400ns(slot);
    (void)poll_status(slot, false);

    // Tras un reset el dispositivo vuelve a su estado de encendido y el
    // IDENTIFY cacheado deja de valer. Si esto falla, el dispositivo no
    // respondio ni al reset: se sigue adelante y el reintento que viene
    // decidira.
    if (!identify(slot)) {
        const size_t index = static_cast<size_t>(&slot - &g_slots[0]);
        console::printf(
            "ata: el reset no devolvio el dispositivo %s\n",
            index < kSlotCount ? kSlotNames[index] : "?"
        );
    }
}

/*
 * Reintentos por comando PIO.
 *
 * Una transferencia fallida no se puede reanudar: el comando quedo a medias y
 * el puerto de datos esta en un estado que solo se limpia reemitiendo el
 * comando entero. Asi que el reintento es de aqui arriba, no del bucle de
 * sectores, y es idempotente porque el buffer no cambia: los sectores ya
 * escritos se vuelven a escribir con el mismo dato.
 *
 * Antes no habia ninguno en toda la cadena (block::write -> write_op ->
 * rw_sectors -> rw_chunk -> poll_status), asi que un sondeo que se quedo sin
 * aire a mitad de una escritura de cinco minutos la tiraba entera. El sondeo
 * tiene presupuestos ACOTADOS de spins --un timeout vestido de otra cosa-- y
 * agotarlos produce un fallo de E/S duro que sube hasta userland.
 *
 * lecturas tambien reintentan: el mismo razonamiento, y el camino de lectura
 * comparte el sondeo. Lo que NO se hace aqui es el reset del controlador
 * (SRST) que pediria la especificacion tras un timeout: es otra pieza, con su
 * propio coste de temporizacion, y no hace falta para un fallo transitorio.
 *
 * El camino bueno no cambia: el reintento solo existe cuando algo fallo.
 */
bool rw_chunk(Slot& slot, uint32_t lba, uint32_t sector_count, uint8_t* bytes, bool write) {
    for (uint32_t round = 0; round < kMaxChunkRounds; ++round) {
        if (round != 0) {
            // Agotados los reintentos baratos. Aqui el dispositivo estaba
            // wedged, no lento, y reemitir no lo arregla: reset y otra vez.
            software_reset(slot);
        }
        for (uint32_t attempt = 1; ; ++attempt) {
            if (rw_chunk_once(slot, lba, sector_count, bytes, write)) {
                if (attempt > 1) {
                    ++g_poll_chunk_retries;
                }
                return true;
            }
            if (attempt >= kMaxChunkAttempts) {
                break;
            }
            ++g_poll_chunk_retries;
            // Un select_drive limpio es parte del reintento: sin el, un comando
            // a medias puede dejar el dispositivo apuntando a otro sector.
            io_wait();
        }
    }
    return false;
}

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

// FLUSH CACHE, issued by block:: at each ordering point. Only once the drive
// is idle: a command written while BSY is set is dropped. The flush used to be
// written straight after each command's last sector, while the drive was still
// busy with it, so QEMU discarded every one of them.
bool flush_op(void* context) {
    Slot& slot = *static_cast<Slot*>(context);
    select_drive(slot, 0);
    if (!poll_status(slot, false)) {
        return false;
    }
    outb(static_cast<uint16_t>(slot.io_base + 7), kCommandCacheFlush);
    return poll_status(slot, false);
}

const block::DeviceOps kOps = {
    &read_op,
    &write_op,
    &flush_op,
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

// La curva de esperas que SI funcionan. Sin esto, un fallo raro no dice si el
// presupuesto era corto o si el dispositivo se wedgeo.
void report_poll_headroom() {
    const uint32_t worst = g_poll_bsy_max_ok_spins > g_poll_drq_max_ok_spins
        ? g_poll_bsy_max_ok_spins
        : g_poll_drq_max_ok_spins;
    if (worst < g_poll_reported_threshold * 2) {
        return;
    }
    g_poll_reported_threshold = worst == 0 ? 1 : worst;
    console::printf(
        "ata: espera maxima ok bsy=%u drq=%u de %u spins\n",
        static_cast<unsigned>(g_poll_bsy_max_ok_spins),
        static_cast<unsigned>(g_poll_drq_max_ok_spins),
        static_cast<unsigned>(kPollSpinBudget)
    );
}

// Se informa desde el fallo que llega hasta userland, una sola vez, para que el
// diagnostico aparezca junto al sintoma y no dentro de la copia.
void report_poll_failures() {
    const uint32_t total = g_poll_failures[0] + g_poll_failures[1] + g_poll_failures[2];
    if (total == 0 || g_poll_failures_reported != 0) {
        return;
    }
    g_poll_failures_reported = 1;
    console::printf(
        "ata: sondeo fallido bsy=%u drq=%u err=%u reintentos=%u resets=%u\n",
        static_cast<unsigned>(g_poll_failures[0]),
        static_cast<unsigned>(g_poll_failures[1]),
        static_cast<unsigned>(g_poll_failures[2]),
        static_cast<unsigned>(g_poll_chunk_retries),
        static_cast<unsigned>(g_poll_device_resets)
    );
}


const block::Driver& driver() { return kDriver; }

} // namespace ata
