#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/fs.hpp"

namespace vfs {
struct Vnode;
}

namespace svfs {

// Id de un volumen SVFS2 montado. Antes habia uno solo y era implicito; ahora
// el instalador necesita el origen (el SVFS2 del LiveCD) y el destino (la
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

// Reconoce el SVFS2 del device y carga su metadata, sin publicar vnodes.
VolumeId probe(size_t device_index, const char* mount_point);
// Publica el arbol del volumen bajo su mount point.
bool attach(VolumeId volume);
// El volumen montado en kRootMountPoint, o kInvalidVolume.
VolumeId root();
// true si la ruta cae bajo el mount point de algun volumen SVFS2 montado.
bool owns_path(const char* path);

MountStatus status(VolumeId volume);
bool mounted(VolumeId volume);
bool writable(VolumeId volume);
size_t file_count(VolumeId volume);
uint64_t total_bytes(VolumeId volume);
uint64_t used_bytes(VolumeId volume);
uint64_t free_bytes(VolumeId volume);
bool sync(VolumeId volume);

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

} // namespace svfs
