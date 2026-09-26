#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/fs.hpp"

namespace vfs {
struct Vnode;
}

namespace sxfs {

// Id de un volumen SxFS montado. Antes habia uno solo y era implicito; ahora
// el instalador necesita el origen (el SxFS del LiveCD) y el destino (la
// particion recien formateada) montados al mismo tiempo.
using VolumeId = size_t;
constexpr VolumeId kInvalidVolume = static_cast<VolumeId>(-1);
constexpr size_t kMountPointCapacity = 32;

// Donde monta el disco del sistema. Lo comparte con vfs::, que rutea por
// prefijo, y con el shell.
constexpr const char* kRootMountPoint = "/disk";

enum class MountStatus : uint8_t {
    unavailable = 0,
    mounted = 1,
    read_only = 2,
};

struct FileRecord {
    bool in_use;
    bool directory;
    // Volumen del que salio. Los FileRecord viajan solos por la API publica
    // (read_file, unlink_file, ...), asi que sin esto no habria como volver al
    // volumen al que pertenecen.
    uint16_t volume;
    uint32_t inode_id;
    uint32_t parent_inode_id;
    char name[64];
    char path[256];
    uint32_t size;
    vfs::Vnode* vnode;
};

// Vacia la tabla de volumenes. No monta nada: de eso se encarga fs::.
void initialize();

// Driver para el registro fs::. probe/attach salen de aca.
const fs::Driver& driver();

// Reconoce el SxFS del device y carga su metadata, sin publicar vnodes.
VolumeId probe(size_t device_index, const char* mount_point);
// Publica el arbol del volumen bajo su mount point.
bool attach(VolumeId volume);
// El volumen montado en kRootMountPoint, o kInvalidVolume.
VolumeId root();
// true si la ruta cae bajo el mount point de algun volumen SxFS montado.
bool owns_path(const char* path);

MountStatus status(VolumeId volume);
bool mounted(VolumeId volume);
bool writable(VolumeId volume);
size_t file_count(VolumeId volume);
uint64_t total_bytes(VolumeId volume);
uint64_t used_bytes(VolumeId volume);
uint64_t free_bytes(VolumeId volume);
bool sync(VolumeId volume);

// Informe de consistencia de solo lectura, el que responde "que cree el disco
// que tiene encima". Es deliberadamente un informe y no una reparacion: no
// escribe ni un byte, se puede correr con el volumen montado, y por lo tanto no
// puede empeorar un sistema de archivos que ya esta roto. La reparacion es un
// camino aparte y necesita el volumen sin montar.
//
// El montaje ya rechaza el superblock invalido, los extents fuera de rango, los
// extents que se pisan dentro de un inodo, el `size` mayor que la capacidad, un
// sector reclamado por un inodo que el bitmap da por libre, un sector reclamado
// por dos inodos, y un inodo marcado asignado que no es del tipo que le
// corresponde. Eso NO alcanza para la pregunta que hace este informe, y las
// dos direcciones que faltan son las que no cubre nadie:
//
//   - un bloque marcado ocupado en el bitmap que ningun inodo reclama (una
//     fuga) solo se ve como espacio que `df` no puede explicar;
//   - un inodo asignado, valido y con sectores propios que ningun directorio
//     alcanza (un huerfano) sobrevive a todos los chequeos del montaje.
//
// `double_claimed_blocks` y `lost_blocks` estan en cero en un volumen sano y se
// reportan por completitud: son la misma comprobacion que hace el montaje, vista
// desde el informe.
struct CheckReport {
    // Geometria, tal cual esta en el superblock.
    uint32_t total_sectors;
    uint32_t data_lba;
    uint32_t sequence;
    uint8_t clean_shutdown;   // el superblock tiene SXFS_FLAG_CLEAN
    uint8_t journal_valid;    // el header del journal se pudo leer y validar
    uint16_t reserved0;
    uint32_t journal_pending; // una transaccion quedo a medias

    // Poblacion del volumen.
    uint32_t inodes_allocated;
    uint32_t files;
    uint32_t directories;

    // Contabilidad de la region de datos.
    uint32_t data_sectors;         // data_lba .. total_sectors
    uint32_t data_used_sectors;    // marcados ocupados en el bitmap
    uint32_t data_claimed_sectors; // reclamados por algun inodo

    // Hallazgos. Todos en cero en un volumen sano; ver clean().
    uint32_t leaked_blocks;        // ocupado en el bitmap, sin dueno
    uint32_t lost_blocks;          // reclamado por un inodo, libre en el bitmap
    uint32_t double_claimed_blocks;// dos inodos sobre el mismo sector
    uint32_t metadata_unmarked;    // [0, data_lba) que el bitmap da por libre
    uint32_t orphan_inodes;        // asignados e inalcanzables desde la raiz
    uint32_t alias_inodes;         // dos entradas de directorio por un inodo
    uint32_t duplicate_names;      // un nombre repetido en el mismo directorio
    uint32_t bad_dir_entries;      // entrada que no pasa read_dir_entry
    uint32_t unreadable_dirs;      // directorio ilegible o mas profundo del limite

    // true si y solo si no hay ningun hallazgo. La geometria, la poblacion y la
    // contabilidad NO son hallazgos: son datos.
    //
    // `clean_shutdown` NO es un hallazgo: el superblock se marca sucio cuando el
    // montaje completo necesita escribir, y eso es lo normal en cada arranque.
    // El journal tampoco: clear_journal() lo deja a cero, y un header vacio no
    // tiene la magia que sxfs_journal_valid() exige, asi que journal_valid == 0
    // es el estado de un volumen sano. Solo cuenta el journal con magia presente
    // que no valida, que si es dano.
    bool clean() const {
        return leaked_blocks == 0 && lost_blocks == 0 && double_claimed_blocks == 0 &&
            metadata_unmarked == 0 && orphan_inodes == 0 && alias_inodes == 0 &&
            duplicate_names == 0 && bad_dir_entries == 0 && unreadable_dirs == 0 &&
            !(journal_valid == 0 && journal_pending != 0);
    }
};

// Llena `report` con la foto de consistencia del volumen. No escribe en disco y
// no cambia el volumen. Devuelve false si el volumen no esta montado o no tiene
// metadata cargada.
bool check(VolumeId volume, CheckReport& report);

// Operaciones sobre archivos: el volumen sale del propio record o de la ruta.
bool read_file(FileRecord& file, size_t offset, void* buffer, size_t count);
bool write_file(FileRecord& file, size_t offset, const void* buffer, size_t count, bool truncate, size_t& written);
bool truncate_file(FileRecord& file, size_t size);
FileRecord* create_file(const char* path);
FileRecord* create_directory(const char* path);
bool remove_directory(FileRecord& file);
bool rename_path(const char* old_path, const char* new_path);
bool unlink_file(FileRecord& file);
FileRecord* file_from_vnode(vfs::Vnode& node);
void refresh_vnode(vfs::Vnode& node);

} // namespace sxfs
