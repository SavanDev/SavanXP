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
    // Puede ser nullptr: el device queda como solo-lectura.
    bool (*write)(void* context, uint32_t lba, uint32_t sector_count, const void* buffer);
};

// Driver de almacenamiento. A diferencia de display y audio, aca NO se elige un
// driver: los devices de todos coexisten (los ATA y el ramdisk del LiveCD a la
// vez), asi que probe_all() corre TODOS los enumerate en vez de cortar en el
// primero que reclama el hardware.
struct Driver {
    const char* name;
    // Mayor enumera primero. Define el orden de los indices de device y con eso
    // a quien termina montando svfs: ATA antes que ramdisk hace que un disco
    // IDE persistente (dev) le gane a la imagen del LiveCD.
    int priority;
    // Registra con register_device() cada device que encuentre.
    void (*enumerate)();
};

constexpr size_t kMaxDrivers = 4;

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
bool read(size_t index, uint32_t lba, uint32_t sector_count, void* buffer);
bool write(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer);

} // namespace block
