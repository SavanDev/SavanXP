/*
 * svfs_volume_test.cpp -- Test de host de la maquina de estados de montaje de
 * SVFS2 (kernel/svfs.cpp), corriendo el driver REAL contra backends de mentira
 * (tests/host/svfs_host_stubs.cpp) y una imagen en memoria construida con el
 * core compartido (libsvfs).
 *
 * El caso que motivo el harness: un volumen cuyo journal quedo pendiente y cuya
 * recuperacion NO se pudo persistir (device de solo lectura) tiene que quedar
 * montado en read_only y rechazar toda escritura. attach() lo promovia a
 * mounted de forma incondicional, asi que las dos mitades del driver no
 * coincidian: mount_record() ya habia publicado los vnodes con writable=false,
 * pero svfs::writable() devolvia true y las mutaciones pasaban el guard.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/svfs.hpp"
#include "kernel/vfs.hpp"
#include "svfs/svfs_format.h"
#include "svfs_core.h"
#include "svfs_host_stubs.hpp"

namespace {

constexpr uint32_t kTotalSectors = 1024;
constexpr size_t kImageBytes = static_cast<size_t>(kTotalSectors) * SVFS_SECTOR_SIZE;

constexpr const char* kFilePath = "/disk/docs/hello.txt";
constexpr const char* kDirPath = "/disk/docs";
constexpr const char* kNewPath = "/disk/docs/nuevo.txt";
constexpr const char* kFileContents = "svfs2 read-only mount test\n";

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

// --- Construccion de la imagen ----------------------------------------------

struct ImageCookie {
    uint8_t* base;
    uint32_t sector_count;
};

int image_read(void* cookie, uint32_t lba, uint32_t count, void* buffer) {
    ImageCookie* image = static_cast<ImageCookie*>(cookie);
    if (static_cast<uint64_t>(lba) + count > image->sector_count) {
        return SVFS_ERR_IO;
    }
    memcpy(buffer, image->base + static_cast<size_t>(lba) * SVFS_SECTOR_SIZE,
           static_cast<size_t>(count) * SVFS_SECTOR_SIZE);
    return SVFS_OK;
}

int image_write(void* cookie, uint32_t lba, uint32_t count, const void* buffer) {
    ImageCookie* image = static_cast<ImageCookie*>(cookie);
    if (static_cast<uint64_t>(lba) + count > image->sector_count) {
        return SVFS_ERR_IO;
    }
    memcpy(image->base + static_cast<size_t>(lba) * SVFS_SECTOR_SIZE, buffer,
           static_cast<size_t>(count) * SVFS_SECTOR_SIZE);
    return SVFS_OK;
}

uint8_t* sector_at(uint8_t* image, uint32_t lba) {
    return image + static_cast<size_t>(lba) * SVFS_SECTOR_SIZE;
}

// Imagen SVFS2 valida con un directorio y un archivo, construida con el core
// compartido (el mismo que arma build/disk.img).
bool build_clean_image(uint8_t* image) {
    memset(image, 0, kImageBytes);

    ImageCookie cookie = {image, kTotalSectors};
    svfs_ctx* ctx = static_cast<svfs_ctx*>(calloc(1, sizeof(svfs_ctx)));
    if (ctx == nullptr) {
        return false;
    }
    svfs_ctx_init(ctx, &cookie, &image_read, &image_write);

    const bool ok = svfs_format(ctx, kTotalSectors) == SVFS_OK &&
        svfs_mkdir_p(ctx, "docs") == SVFS_OK &&
        svfs_write_file(ctx, "docs/hello.txt", kFileContents,
                        static_cast<uint32_t>(strlen(kFileContents))) == SVFS_OK &&
        svfs_flush(ctx) == SVFS_OK;

    free(ctx);
    return ok;
}

// Deja la imagen como la dejaria un corte de luz en medio de un commit: el
// journal con una transaccion pendiente (payload = la metadata que hay que
// replicar en su lugar definitivo) y los superblocks sin el flag de limpio.
// Al montarla, recover_journal() tiene que ESCRIBIR para completar el replay:
// si el device no lo deja, la recuperacion queda a medias y el volumen cae en
// read_only.
void make_journal_pending(uint8_t* image) {
    svfs_superblock superblock = {};
    memcpy(&superblock, sector_at(image, SVFS_PRIMARY_SB_LBA), sizeof(superblock));

    // El payload del journal es una copia de [block bitmap|inode bitmap|tabla
    // de inodos], que en el layout on-disk son sectores contiguos.
    memcpy(sector_at(image, SVFS_JOURNAL_LBA + 1), sector_at(image, SVFS_BLOCK_BITMAP_LBA),
           static_cast<size_t>(SVFS_JOURNAL_METADATA_SECTORS) * SVFS_SECTOR_SIZE);

    svfs_journal_header header = {};
    memcpy(header.magic, svfs_journal_magic, sizeof(header.magic));
    header.sequence = superblock.sequence + 1u;
    header.pending = 1;
    header.metadata_sectors = SVFS_JOURNAL_METADATA_SECTORS;
    header.checksum = svfs_journal_checksum(&header);
    memcpy(sector_at(image, SVFS_JOURNAL_LBA), &header, sizeof(header));

    superblock.flags &= ~static_cast<uint32_t>(SVFS_FLAG_CLEAN);
    superblock.checksum = svfs_superblock_checksum(&superblock);
    memcpy(sector_at(image, SVFS_PRIMARY_SB_LBA), &superblock, sizeof(superblock));
    memcpy(sector_at(image, SVFS_SECONDARY_SB_LBA), &superblock, sizeof(superblock));
}

// --- Casos ------------------------------------------------------------------

// Un volumen con el journal pendiente sobre un device de solo lectura: la
// recuperacion no se puede persistir, asi que el volumen queda read_only y
// tiene que seguir read_only DESPUES de attach().
void case_unrecoverable_journal_stays_read_only(uint8_t* image, uint8_t* pristine) {
    printf("caso: journal pendiente sobre device de solo lectura\n");

    if (!check(build_clean_image(image), "imagen SVFS2 construida")) {
        return;
    }
    make_journal_pending(image);
    memcpy(pristine, image, kImageBytes);

    hoststub::reset_vfs();
    hoststub::attach_device(image, kTotalSectors, /*writable=*/false);
    svfs::initialize();

    const svfs::VolumeId volume = svfs::probe(0, svfs::kRootMountPoint);
    if (!check(volume != svfs::kInvalidVolume, "probe reconoce el volumen")) {
        return;
    }
    check(svfs::status(volume) == svfs::MountStatus::read_only,
          "probe deja el volumen en read_only (la recuperacion no se pudo persistir)");
    check(hoststub::rejected_writes() > 0, "el device rechazo las escrituras de la recuperacion");

    check(svfs::attach(volume), "attach publica el arbol");
    check(svfs::status(volume) == svfs::MountStatus::read_only,
          "attach NO promueve el volumen a mounted");
    check(!svfs::writable(volume), "svfs::writable() es false");
    check(svfs::mounted(volume), "el volumen igual cuenta como montado");

    // La otra mitad del driver: los vnodes que publico mount_record().
    vfs::Vnode* node = hoststub::find_node(kFilePath);
    if (!check(node != nullptr, "el archivo quedo publicado en el vfs")) {
        return;
    }
    check(!node->writable, "el vnode del archivo quedo con writable=false");

    svfs::FileRecord* record = svfs::file_from_vnode(*node);
    if (!check(record != nullptr, "file_from_vnode encuentra el record")) {
        return;
    }

    // Leer si tiene que andar: read_only es montado, no inaccesible.
    char buffer[64] = {};
    check(svfs::read_file(*record, 0, buffer, strlen(kFileContents)) &&
              memcmp(buffer, kFileContents, strlen(kFileContents)) == 0,
          "read_file sigue funcionando");

    // Y ninguna mutacion puede pasar el guard.
    size_t written = 12345;
    check(!svfs::write_file(*record, 0, "x", 1, false, written) && written == 0,
          "write_file rechazado");
    check(!svfs::truncate_file(*record, 0), "truncate_file rechazado");
    check(!svfs::unlink_file(*record), "unlink_file rechazado");
    check(svfs::create_file(kNewPath) == nullptr, "create_file rechazado");
    check(svfs::create_directory("/disk/otro") == nullptr, "create_directory rechazado");
    check(!svfs::rename_path(kFilePath, "/disk/docs/renombrado.txt"), "rename_path rechazado");

    svfs::FileRecord* dir_record = nullptr;
    vfs::Vnode* dir_node = hoststub::find_node(kDirPath);
    if (dir_node != nullptr) {
        dir_record = svfs::file_from_vnode(*dir_node);
    }
    if (dir_record != nullptr) {
        check(!svfs::remove_directory(*dir_record), "remove_directory rechazado");
    }

    check(memcmp(image, pristine, kImageBytes) == 0, "la imagen quedo intacta byte a byte");

    hoststub::detach_device();
}

// Control: la MISMA imagen sobre un device escribible. Ahi la recuperacion si
// se persiste, el volumen queda mounted y las escrituras entran. Sin este caso,
// el de arriba pasaria igual si el driver rechazara todo por cualquier motivo.
void case_recovered_journal_is_writable(uint8_t* image) {
    printf("caso: mismo journal pendiente sobre device escribible\n");

    if (!check(build_clean_image(image), "imagen SVFS2 construida")) {
        return;
    }
    make_journal_pending(image);

    hoststub::reset_vfs();
    hoststub::attach_device(image, kTotalSectors, /*writable=*/true);
    svfs::initialize();

    const svfs::VolumeId volume = svfs::probe(0, svfs::kRootMountPoint);
    if (!check(volume != svfs::kInvalidVolume, "probe reconoce el volumen")) {
        return;
    }
    check(svfs::status(volume) == svfs::MountStatus::mounted,
          "la recuperacion se persistio: el volumen queda mounted");
    check(svfs::attach(volume), "attach publica el arbol");
    check(svfs::writable(volume), "svfs::writable() es true");

    vfs::Vnode* node = hoststub::find_node(kFilePath);
    if (!check(node != nullptr, "el archivo quedo publicado en el vfs")) {
        return;
    }
    check(node->writable, "el vnode del archivo quedo con writable=true");

    svfs::FileRecord* created = svfs::create_file(kNewPath);
    if (!check(created != nullptr, "create_file aceptado")) {
        return;
    }
    size_t written = 0;
    check(svfs::write_file(*created, 0, "hola", 4, true, written) && written == 4,
          "write_file aceptado");
    check(svfs::unlink_file(*created), "unlink_file aceptado");

    hoststub::detach_device();
}

// El caso que separa de verdad los dos comportamientos: device ESCRIBIBLE, pero
// la escritura de la recuperacion se topa con un error de I/O transitorio. El
// volumen queda read_only con un device que despues acepta escrituras, asi que
// lo unico que frena las mutaciones es la politica del driver. Con la promocion
// incondicional en attach(), create_file/write_file entraban y mutaban un
// volumen cuya metadata on-disk nunca se reconcilio.
void case_transient_io_failure_rejects_writes(uint8_t* image, uint8_t* pristine) {
    printf("caso: fallo de I/O transitorio durante la recuperacion\n");

    if (!check(build_clean_image(image), "imagen SVFS2 construida")) {
        return;
    }
    make_journal_pending(image);
    memcpy(pristine, image, kImageBytes);

    hoststub::reset_vfs();
    hoststub::attach_device(image, kTotalSectors, /*writable=*/true);
    // Solo la primera escritura falla: alcanza para cortar el replay del
    // journal, y a partir de ahi el device vuelve a aceptar escrituras.
    hoststub::fail_next_writes(1);
    svfs::initialize();

    const svfs::VolumeId volume = svfs::probe(0, svfs::kRootMountPoint);
    if (!check(volume != svfs::kInvalidVolume, "probe reconoce el volumen")) {
        return;
    }
    check(svfs::status(volume) == svfs::MountStatus::read_only,
          "el replay fallado deja el volumen en read_only");
    check(svfs::attach(volume), "attach publica el arbol");
    check(svfs::status(volume) == svfs::MountStatus::read_only,
          "attach NO promueve el volumen a mounted");
    check(!svfs::writable(volume), "svfs::writable() es false");

    // El device ya acepta escrituras: si la mutacion pasa el guard, llega al
    // disco. Que no llegue es exactamente lo que tiene que garantizar el status.
    check(svfs::create_file(kNewPath) == nullptr, "create_file rechazado por politica");

    vfs::Vnode* node = hoststub::find_node(kFilePath);
    if (check(node != nullptr, "el archivo quedo publicado en el vfs")) {
        check(!node->writable, "el vnode del archivo quedo con writable=false");
        svfs::FileRecord* record = svfs::file_from_vnode(*node);
        if (check(record != nullptr, "file_from_vnode encuentra el record")) {
            size_t written = 12345;
            check(!svfs::write_file(*record, 0, "x", 1, false, written) && written == 0,
                  "write_file rechazado por politica");
        }
    }

    check(memcmp(image, pristine, kImageBytes) == 0,
          "no se escribio nada sobre el volumen sin recuperar");

    hoststub::detach_device();
}

} // namespace

int main() {
    printf("SVFS VOLUME TEST START\n");

    uint8_t* image = static_cast<uint8_t*>(malloc(kImageBytes));
    uint8_t* pristine = static_cast<uint8_t*>(malloc(kImageBytes));
    if (image == nullptr || pristine == nullptr) {
        printf("SVFS VOLUME TEST FAIL: sin memoria para la imagen\n");
        return 1;
    }

    case_unrecoverable_journal_stays_read_only(image, pristine);
    case_transient_io_failure_rejects_writes(image, pristine);
    case_recovered_journal_is_writable(image);

    free(image);
    free(pristine);

    printf("%s (%d checks, %d fallas)\n",
           g_failures == 0 ? "SVFS VOLUME TEST PASS" : "SVFS VOLUME TEST FAIL",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
