#pragma once

#include <stddef.h>
#include <stdint.h>

namespace block {

constexpr uint32_t kSectorSize = 512;

struct DeviceInfo {
    bool present;
    uint32_t sector_count;
    bool writable;
    const char* name;
};

// Operaciones dependientes del medio. Cada driver llena una instancia estatica
// y la entrega junto a cada device que registra; block:: solo despacha, igual
// que display::Backend para la GPU. El context es opaco para el core: cada
// driver guarda ahi lo suyo (el slot ATA, la base del ramdisk). La validacion
// comun (indice, rango de LBA, solo-lectura) la hace el core antes de llamar,
// asi que los drivers se quedan solo con lo especifico de su protocolo.
struct DeviceOps {
    bool (*read)(void* context, uint32_t lba, uint32_t sector_count, void* buffer);
    // May be nullptr: the device is then read-only. Returns once the device
    // has the data, which may still be in its volatile write cache.
    bool (*write)(void* context, uint32_t lba, uint32_t sector_count, const void* buffer);
    // Commits the device's write cache to the medium. nullptr when there is
    // no cache to commit (a ramdisk).
    bool (*flush)(void* context);
    // Physical disks set this: block:: then queues their writes in memory and
    // writes them back in the background (see write_cached). A device that
    // forwards to another (a partition) or is memory (a ramdisk) leaves it
    // false and is written through.
    bool deferred_writes;
};

// Driver de almacenamiento. A diferencia de display y audio, aca NO se elige un
// driver: los devices de todos coexisten (los ATA y el ramdisk del LiveCD a la
// vez), asi que probe_all() corre TODOS los enumerate en vez de cortar en el
// primero que reclama el hardware.
struct Driver {
    const char* name;
    // Mayor enumera primero. Define el orden de los indices de device y con eso
    // a quien termina montando sxfs: ATA antes que ramdisk hace que un disco
    // IDE persistente (dev) le gane a la imagen del LiveCD.
    int priority;
    // Registra con register_device() cada device que encuentre.
    void (*enumerate)();
    // Optional diagnostics. Filesystems call block::report_health(false) as
    // writes go by and (true) when one failed; a driver with something to say
    // about its devices (ATA's polling margins) prints it, at most once per
    // condition.
    void (*report_health)(bool failed);
};

constexpr size_t kMaxDrivers = 8;

// false si la tabla esta llena o el driver viene incompleto.
bool register_driver(const Driver& driver);
// Corre los enumerate de todos los drivers registrados, por prioridad
// descendente. Devuelve cuantos devices quedaron registrados.
size_t probe_all();
// La llaman los drivers desde su enumerate(). El core no interpreta el context.
bool register_device(
    const DeviceOps& ops,
    void* context,
    uint32_t sector_count,
    bool writable,
    const char* name);

bool ready();
size_t device_count();
bool device_info(size_t index, DeviceInfo& info);
// Sees every write issued so far, including those still queued.
bool read(size_t index, uint32_t lba, uint32_t sector_count, void* buffer);
// Durable write: on the medium when it returns (write_cached + flush + sync).
bool write(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer);
// Write that may land in any order relative to other writes until the next
// flush(). On a device with deferred_writes it is copied to the writeback
// queue and returns at once; the queue reaches the disk in the background, in
// order, with a device flush at every ordering point. See
// docs/kernel/BLOCK_WRITEBACK.md.
bool write_cached(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer);
// Ordering point: every write issued before it reaches the medium before any
// write issued after it. It does not wait: durability is sync()'s job.
bool flush(size_t index);
// Everything issued so far is on the medium when it returns. Drains the whole
// writeback queue, whatever device `index` is.
bool sync(size_t index);
bool sync_all();
void report_health(bool failed);

// Background writeback. writeback_step() moves one bounded piece of the queue
// to its disk (up to 64 KiB of adjacent writes, or one device flush) and says
// whether it did anything; the idle task calls it instead of halting.
// writeback_tick() feeds the clock and, if queued data has waited too long
// because no CPU went idle, does a step itself.
bool writeback_step();
void writeback_tick(uint64_t now_ms);
bool writeback_pending();

struct WritebackStats {
    uint64_t queued_bytes;      // waiting in the queue now
    uint64_t peak_queued_bytes;
    uint64_t steps;
    uint64_t device_writes;
    uint64_t device_flushes;
    uint64_t forced_drains;     // writes that had to wait for room in the queue
    uint32_t failures;
};
WritebackStats writeback_stats();

} // namespace block
