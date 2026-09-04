#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fs {

// Registro de sistemas de archivos, mismo patron que block::, display:: y
// audio::. Aca los drivers NO compiten por el hardware: compiten por reconocer
// un volumen, asi que mount() corta en el primero que reclama el device (como
// display::bind_best) pero puede volver a correr sobre otro device y que gane
// otro driver. El que decide que se monta y donde es fs::, no el driver.

// Id de volumen interno de cada driver. fs:: no lo interpreta: lo guarda y se
// lo devuelve al driver que lo emitio.
using VolumeId = size_t;
constexpr VolumeId kInvalidVolume = static_cast<VolumeId>(-1);

constexpr size_t kInvalidMount = static_cast<size_t>(-1);
constexpr size_t kMaxDrivers = 4;
// Raiz + destino del instalador + un medio removible. El tope real de volumenes
// lo pone cada driver (sxfs:: tiene el suyo, y es mas chico).
constexpr size_t kMaxMounts = 4;
constexpr size_t kMountPointCapacity = 32;

struct Driver {
    const char* name;
    // Mayor prueba primero. Importa cuando dos formatos pueden confundirse
    // (una FAT32 con basura donde SxFS pone su superblock, por ejemplo).
    int priority;
    // Reconoce el device y carga su metadata, sin tocar el vfs. Devolver
    // kInvalidVolume no es un error: significa "el formato no es mio" y fs::
    // sigue con el driver siguiente.
    VolumeId (*probe)(size_t device_index, const char* mount_point);
    // Publica el arbol del volumen bajo su mount point. Va separado de probe
    // porque en el arranque la metadata se lee antes de que vfs:: este listo
    // para recibir vnodes.
    bool (*attach)(VolumeId volume);
    // Puede ser nullptr: el volumen queda montado hasta el reset.
    void (*unmount)(VolumeId volume);
};

struct MountInfo {
    const char* driver_name;
    VolumeId volume;
    size_t device_index;
    const char* mount_point;
    bool attached;
};

// Vacia la tabla de montajes; los drivers registrados quedan. NO desmonta:
// es el reset de arranque, hermano de block::probe_all() y sxfs::initialize().
void initialize();

bool register_driver(const Driver& driver);

// Prueba los drivers sobre un device, por prioridad descendente, y corta en el
// primero que lo reclame. Devuelve el indice de montaje o kInvalidMount.
size_t mount(size_t device_index, const char* mount_point);
// Igual, pero recorriendo todos los block devices en orden: gana el primero que
// algun driver reclame. Es el camino del arranque.
size_t mount_any(const char* mount_point);

bool attach(size_t mount_index);
void unmount(size_t mount_index);

size_t mount_count();
bool mount_info(size_t mount_index, MountInfo& out);
// Indice del volumen montado exactamente en mount_point, o kInvalidMount.
size_t find(const char* mount_point);

} // namespace fs
