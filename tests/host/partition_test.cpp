/*
 * partition_test.cpp -- Test de host del driver de particiones
 * (kernel/partition.cpp), corriendo el driver REAL contra un block:: de
 * mentira y tablas MBR/GPT armadas en memoria.
 *
 * Por que en el host y no en un smoke de QEMU: el kernel no expone las tablas
 * de particiones a userland, y build/disk.img es un SxFS crudo sin tabla, asi
 * que un arranque solo puede verificar el caso "no hay nada que rebanar". Los
 * casos que importan -- una GPT valida, una tabla que miente el tamano, una
 * particion que se sale del disco -- se arman aca en dos lineas.
 *
 * El block:: de mentira vive en este mismo TU (y no en sxfs_host_stubs.cpp) a
 * proposito: el del test de SxFS es de un solo device y sin register_device,
 * mientras que aca el registro de devices ES lo que esta bajo prueba, porque
 * partition:: se cuelga de el para publicar cada rebanada.
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel/block.hpp"
#include "kernel/partition.hpp"
#include "kernel/string.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

bool check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("  FAIL %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
    return ok;
}

// --- block:: de mentira -----------------------------------------------------
// Dos clases de device conviven, igual que en el kernel: los "crudos" que
// respalda una imagen en memoria (el disco fisico) y los que registra
// partition:: con sus propias DeviceOps (las rebanadas). La validacion de rango
// y del flag de solo-lectura la hace el core de block:: en el kernel, asi que
// se replica aca: sin eso el test no podria comprobar que una lectura fuera de
// la particion se rechaza.

constexpr size_t kMaxDevices = 16;

struct Device {
    bool present;
    bool writable;
    const char* name;
    uint32_t sector_count;
    // Device crudo: bytes propios. Rebanada: ops + context del driver.
    uint8_t* image;
    const block::DeviceOps* ops;
    void* context;
};

Device g_devices[kMaxDevices] = {};
size_t g_device_count = 0;

void reset_devices() {
    for (size_t index = 0; index < g_device_count; ++index) {
        free(g_devices[index].image);
    }
    memset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;
}

// Disco fisico en blanco. Devuelve su indice de device.
size_t add_disk(uint32_t sector_count, const char* name, bool writable) {
    Device& device = g_devices[g_device_count];
    device.present = true;
    device.writable = writable;
    device.name = name;
    device.sector_count = sector_count;
    device.image = static_cast<uint8_t*>(calloc(sector_count, block::kSectorSize));
    return g_device_count++;
}

uint8_t* disk_bytes(size_t index) {
    return g_devices[index].image;
}

bool range_ok(size_t index, uint32_t lba, uint32_t sector_count) {
    if (index >= g_device_count || !g_devices[index].present || sector_count == 0) {
        return false;
    }
    return static_cast<uint64_t>(lba) + sector_count <= g_devices[index].sector_count;
}

size_t byte_offset(uint32_t lba) {
    return static_cast<size_t>(lba) * block::kSectorSize;
}

size_t byte_length(uint32_t sector_count) {
    return static_cast<size_t>(sector_count) * block::kSectorSize;
}

// --- Armado de tablas -------------------------------------------------------

void put32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

void put64(uint8_t* out, uint64_t value) {
    put32(out, static_cast<uint32_t>(value));
    put32(out + 4, static_cast<uint32_t>(value >> 32));
}

constexpr size_t kMbrTableOffset = 446;
constexpr size_t kMbrEntrySize = 16;

// GUID de EFI System Partition tal como queda en disco.
const uint8_t kEspTypeGuid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};
// Basic data partition, para la segunda entrada.
const uint8_t kDataTypeGuid[16] = {
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
    0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
};

void write_boot_signature(uint8_t* image) {
    image[510] = 0x55;
    image[511] = 0xaa;
}

uint8_t* mbr_entry(uint8_t* image, size_t index) {
    return image + kMbrTableOffset + (index * kMbrEntrySize);
}

// GPT con dos particiones: una ESP de 16 MiB en el LBA 2048 y el resto de
// datos. Con `protective`, ademas el MBR 0xEE que pone cualquier herramienta
// real; sin el, un hibrido, que tambien tiene que montar.
void build_gpt(size_t device, bool protective) {
    uint8_t* image = disk_bytes(device);
    const uint32_t sectors = g_devices[device].sector_count;
    write_boot_signature(image);

    if (protective) {
        uint8_t* entry = mbr_entry(image, 0);
        entry[4] = 0xee;
        put32(entry + 8, 1);
        put32(entry + 12, sectors - 1);
    }

    uint8_t* header = image + block::kSectorSize;
    memcpy(header, "EFI PART", 8);
    put64(header + 72, 2);    // LBA del array de entries
    put32(header + 80, 128);  // cantidad de entries
    put32(header + 84, 128);  // tamano de cada entry

    uint8_t* first = image + 2 * block::kSectorSize;
    memcpy(first, kEspTypeGuid, sizeof(kEspTypeGuid));
    first[16] = 0x11; // GUID unico != 0, o la entry cuenta como vacia
    put64(first + 32, 2048);
    put64(first + 40, 2048 + 32768 - 1);

    uint8_t* second = first + 128;
    memcpy(second, kDataTypeGuid, sizeof(kDataTypeGuid));
    second[16] = 0x22;
    put64(second + 32, 2048 + 32768);
    put64(second + 40, sectors - 34); // ultimo LBA usable, antes de la GPT de respaldo
}

// MBR clasico: una FAT32, una extendida (que hay que saltear), una ESP por tipo
// 0xEF, y una ultima que miente el tamano y tiene que quedar recortada.
void build_mbr(size_t device) {
    uint8_t* image = disk_bytes(device);
    write_boot_signature(image);

    uint8_t* first = mbr_entry(image, 0);
    first[4] = 0x0c; // FAT32 LBA
    put32(first + 8, 2048);
    put32(first + 12, 16384);

    uint8_t* extended = mbr_entry(image, 1);
    extended[4] = 0x05;
    put32(extended + 8, 2048 + 16384);
    put32(extended + 12, 1024);

    uint8_t* esp = mbr_entry(image, 2);
    esp[4] = 0xef;
    put32(esp + 8, 2048 + 17408);
    put32(esp + 12, 4096);

    uint8_t* oversized = mbr_entry(image, 3);
    oversized[4] = 0x83;
    put32(oversized + 8, 2048 + 21504);
    put32(oversized + 12, 0xffffffffu);
}

void enumerate_partitions() {
    partition::driver().enumerate();
}

// --- Casos ------------------------------------------------------------------

// Una GPT valida se rebana en devices propios, con la ESP marcada y el nombre
// derivado del padre.
void case_gpt_publishes_partitions() {
    printf("caso: GPT con MBR protectivo\n");
    reset_devices();
    const size_t disk = add_disk(131072, "ata0", /*writable=*/true); // 64 MiB
    build_gpt(disk, /*protective=*/true);
    enumerate_partitions();

    check(g_device_count == 3, "el disco mas sus dos particiones");

    partition::Info info = {};
    check(!partition::info(disk, info), "el disco entero no es una particion");
    if (check(partition::info(1, info), "la primera rebanada es una particion")) {
        check(info.parent_index == disk, "apunta a su disco padre");
        check(info.start_lba == 2048 && info.sector_count == 32768, "offset y tamano de la ESP");
        check(info.esp, "reconocida como ESP por su type GUID");
        check(info.mbr_type == 0, "sin tipo MBR: viene de una GPT");
    }
    if (check(partition::info(2, info), "la segunda rebanada es una particion")) {
        check(info.start_lba == 34816, "arranca donde termina la ESP");
        check(!info.esp, "la de datos no es ESP");
    }

    block::DeviceInfo device = {};
    check(block::device_info(1, device) && strcmp(device.name, "ata0p1") == 0, "nombre ata0p1");
    check(block::device_info(2, device) && strcmp(device.name, "ata0p2") == 0, "nombre ata0p2");
}

// Lo que hace util a la capa: la particion es un device con su LBA 0 propio.
void case_partition_offsets_reach_the_parent() {
    printf("caso: el LBA 0 de la particion cae en el offset del padre\n");
    reset_devices();
    const size_t disk = add_disk(131072, "ata0", /*writable=*/true);
    build_gpt(disk, /*protective=*/true);
    enumerate_partitions();

    uint8_t sector[block::kSectorSize];
    memset(sector, 0xa5, sizeof(sector));
    check(block::write(1, 0, 1, sector), "escritura al LBA 0 de la particion");

    uint8_t readback[block::kSectorSize] = {};
    check(block::read(disk, 2048, 1, readback) && readback[0] == 0xa5,
          "aterrizo en el LBA 2048 del disco");

    check(!block::read(1, 32768, 1, readback), "leer un sector despues del final se rechaza");
    check(block::read(1, 32767, 1, readback), "el ultimo sector de la particion si se lee");
}

// Sin el MBR 0xEE la GPT tiene que ganar igual: hay imagenes hibridas asi.
void case_gpt_without_protective_mbr() {
    printf("caso: GPT sin MBR protectivo\n");
    reset_devices();
    const size_t disk = add_disk(131072, "ata0", /*writable=*/true);
    build_gpt(disk, /*protective=*/false);
    enumerate_partitions();
    check(g_device_count == 3, "la GPT gana sin el 0xEE");
}

void case_mbr_table() {
    printf("caso: MBR clasico\n");
    reset_devices();
    const size_t disk = add_disk(65536, "ata0", /*writable=*/true); // 32 MiB
    build_mbr(disk);
    enumerate_partitions();

    check(g_device_count == 4, "tres primarias: la extendida se saltea");

    partition::Info info = {};
    if (check(partition::info(1, info), "primera primaria registrada")) {
        check(info.mbr_type == 0x0c && info.start_lba == 2048, "FAT32 en el LBA 2048");
    }
    if (check(partition::info(2, info), "segunda primaria registrada")) {
        check(info.mbr_type == 0xef && info.esp, "ESP reconocida por el tipo 0xEF");
    }
    if (check(partition::info(3, info), "tercera primaria registrada")) {
        // La tabla declara 0xffffffff sectores desde el 23552 sobre un disco de
        // 65536: sin el recorte, block:: aceptaria lecturas fuera del device.
        check(info.sector_count == 65536 - 23552, "recortada al tamano real del padre");
    }
}

void case_read_only_disk_yields_read_only_partitions() {
    printf("caso: disco de solo lectura\n");
    reset_devices();
    const size_t disk = add_disk(131072, "cd0", /*writable=*/false);
    build_gpt(disk, /*protective=*/true);
    enumerate_partitions();

    block::DeviceInfo device = {};
    check(block::device_info(1, device) && !device.writable, "la particion hereda el solo-lectura");

    uint8_t sector[block::kSectorSize] = {};
    check(!block::write(1, 0, 1, sector), "escribirle se rechaza");
}

// El disco de SavanXP hoy es un SxFS crudo sin tabla: no se toca.
void case_raw_disk_is_left_alone() {
    printf("caso: disco crudo sin tabla\n");
    reset_devices();
    add_disk(4096, "ata0", /*writable=*/true);
    enumerate_partitions();
    check(g_device_count == 1, "sin firma 0xAA55 no se rebana nada");
}

void case_bogus_entries_are_discarded() {
    printf("caso: entries MBR invalidas\n");
    reset_devices();
    const size_t disk = add_disk(4096, "ata0", /*writable=*/true);
    uint8_t* image = disk_bytes(disk);
    write_boot_signature(image);

    uint8_t* out_of_range = mbr_entry(image, 0);
    out_of_range[4] = 0x83;
    put32(out_of_range + 8, 999999); // arranca fuera del disco
    put32(out_of_range + 12, 100);

    uint8_t* empty = mbr_entry(image, 1);
    empty[4] = 0x83;
    put32(empty + 8, 100);
    put32(empty + 12, 0); // tamano cero

    enumerate_partitions();
    check(g_device_count == 1, "las entries fuera de rango o vacias se descartan");
}

// Varios discos a la vez: cada rebanada tiene que quedar atada a SU padre.
void case_two_disks_do_not_cross() {
    printf("caso: dos discos particionados\n");
    reset_devices();
    const size_t first = add_disk(131072, "ata0", /*writable=*/true);
    const size_t second = add_disk(65536, "ata1", /*writable=*/true);
    build_gpt(first, /*protective=*/true);
    build_mbr(second);
    enumerate_partitions();

    check(g_device_count == 2 + 2 + 3, "dos discos, dos rebanadas y tres rebanadas");

    partition::Info info = {};
    size_t from_first = 0;
    size_t from_second = 0;
    for (size_t index = 2; index < g_device_count; ++index) {
        if (!partition::info(index, info)) {
            continue;
        }
        if (info.parent_index == first) {
            ++from_first;
        } else if (info.parent_index == second) {
            ++from_second;
        }
    }
    check(from_first == 2 && from_second == 3, "cada rebanada apunta a su propio disco");

    block::DeviceInfo device = {};
    check(block::device_info(4, device) && strcmp(device.name, "ata1p1") == 0,
          "las del segundo disco se llaman ata1pN");
}

} // namespace

// --- block:: ----------------------------------------------------------------

namespace block {

bool register_device(const DeviceOps& ops, void* context, uint32_t sector_count, bool writable, const char* name) {
    if (ops.read == nullptr || sector_count == 0 || g_device_count >= kMaxDevices) {
        return false;
    }
    Device& device = g_devices[g_device_count];
    device.present = true;
    device.ops = &ops;
    device.context = context;
    device.sector_count = sector_count;
    device.writable = writable && ops.write != nullptr;
    device.name = name;
    ++g_device_count;
    return true;
}

size_t device_count() {
    return g_device_count;
}

bool device_info(size_t index, DeviceInfo& info) {
    if (index >= g_device_count || !g_devices[index].present) {
        return false;
    }
    info.present = true;
    info.sector_count = g_devices[index].sector_count;
    info.writable = g_devices[index].writable;
    info.name = g_devices[index].name;
    return true;
}

bool read(size_t index, uint32_t lba, uint32_t sector_count, void* buffer) {
    if (buffer == nullptr || !range_ok(index, lba, sector_count)) {
        return false;
    }
    Device& device = g_devices[index];
    if (device.ops != nullptr) {
        return device.ops->read(device.context, lba, sector_count, buffer);
    }
    memcpy(buffer, device.image + byte_offset(lba), byte_length(sector_count));
    return true;
}

bool write(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer) {
    if (buffer == nullptr || !range_ok(index, lba, sector_count)) {
        return false;
    }
    Device& device = g_devices[index];
    if (!device.writable) {
        return false;
    }
    if (device.ops != nullptr) {
        return device.ops->write(device.context, lba, sector_count, buffer);
    }
    memcpy(device.image + byte_offset(lba), buffer, byte_length(sector_count));
    return true;
}

bool ready() {
    return g_device_count != 0;
}

bool register_driver(const Driver&) {
    return true;
}

size_t probe_all() {
    return g_device_count;
}

} // namespace block

int main() {
    printf("PARTITION TEST START\n");

    case_gpt_publishes_partitions();
    case_partition_offsets_reach_the_parent();
    case_gpt_without_protective_mbr();
    case_mbr_table();
    case_read_only_disk_yields_read_only_partitions();
    case_raw_disk_is_left_alone();
    case_bogus_entries_are_discarded();
    case_two_disks_do_not_cross();

    reset_devices();

    printf("%s (%d checks, %d fallas)\n",
           g_failures == 0 ? "PARTITION TEST PASS" : "PARTITION TEST FAIL",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
