/*
 * sxfs_cli.c -- the host tool that creates and fills SxFS images.
 *
 * The host CLI runs the portable core (libsxfs) over a file backend. `apply`
 * takes a manifest with the operations already resolved; `extract` uses the
 * core's read-only walk for the safe compaction.
 *
 * Usage:
 *   sxfs-cli info    <image>
 *       Prints the sector count of a valid image.
 *   sxfs-cli extract <image> <destination_directory> [<ownership_manifest>]
 *       Extracts the tree into host files for a safe compaction. With a third
 *       argument it also writes there one `owner` line per path, so `apply`
 *       can put the ownership back on the rebuilt image.
 *   sxfs-cli create <image> <total_sectors>
 *       Creates a new image, formatted and empty (only the root).
 *   sxfs-cli apply  <image> <manifest>
 *       Opens an existing image (keeping its contents) and applies the
 *       manifest. One operation per line, fields separated by TAB:
 *           mkdir <relative_path>
 *           file  <relative_path>\t<host_file>
 *           owner <relative_path>\t<uid>\t<gid>\t<octal_mode>
 *       `owner` records ownership on a path that already exists; its mode
 *       carries the type bits. Paths are relative to the SxFS root, which is
 *       the guest's root.
 *   sxfs-cli rm     <image> <path> [<path>...]
 *       Removes files or empty directories. It is apply's counterpart, which
 *       is additive and never deletes: without it, the only way to take
 *       something out of an image is to rebuild it from scratch. Same path
 *       convention, tolerating a leading '/'.
 *
 * apply and rm stamp an image written before ownership existed
 * (sxfs_stamp_ownership) before changing it.
 *
 * Exit codes: 0 ok, 1 error, 2 bad usage, 3 the allocator ran out of a
 * contiguous run (see SXFS_CLI_EXIT_NO_SPACE).
 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L /* fseeko/off_t: glibc los oculta bajo -std=c11 sin esto */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SXFS_MKDIR(path) mkdir(path, 0755)

#include "sxfs_core.h"
#include "sxfs/sxfs_ownership.h"

/* Codigo de salida propio para SXFS_ERR_NO_SPACE: el volumen tiene espacio libre
 * pero no una corrida contigua del tamano pedido. Se distingue del error
 * generico (1) para que el build pueda compactar la imagen y reintentar en vez
 * de abortar; el sincronizador de build reintenta tras compactar. */
#define SXFS_CLI_EXIT_NO_SPACE 3

/* --- Backend de bloque sobre un FILE* ------------------------------------ */

static int seek_sector(FILE* file, uint32_t lba) {
    return fseeko(file, (off_t)lba * SXFS_SECTOR_SIZE, SEEK_SET);
}

static int file_read(void* cookie, uint32_t lba, uint32_t count, void* buffer) {
    FILE* file = (FILE*)cookie;
    if (seek_sector(file, lba) != 0) {
        return -1;
    }
    size_t want = (size_t)count * SXFS_SECTOR_SIZE;
    return fread(buffer, 1, want, file) == want ? 0 : -1;
}

static int file_write(void* cookie, uint32_t lba, uint32_t count, const void* buffer) {
    FILE* file = (FILE*)cookie;
    if (seek_sector(file, lba) != 0) {
        return -1;
    }
    size_t want = (size_t)count * SXFS_SECTOR_SIZE;
    return fwrite(buffer, 1, want, file) == want ? 0 : -1;
}

/* --- Utilidades ---------------------------------------------------------- */

static const char* sxfs_strerror(int rc) {
    switch (rc) {
        case SXFS_OK: return "ok";
        case SXFS_ERR_IO: return "error de I/O";
        case SXFS_ERR_NO_SPACE: return "sin espacio contiguo";
        case SXFS_ERR_NO_INODES: return "sin inodos libres";
        case SXFS_ERR_INVALID: return "argumento/ruta invalida";
        case SXFS_ERR_EXISTS: return "colision de tipo (archivo vs directorio)";
        case SXFS_ERR_NOT_FOUND: return "ruta inexistente";
        case SXFS_ERR_TOO_LONG: return "nombre/ruta demasiado largo";
        default: return "error desconocido";
    }
}

/* Traduce un codigo del core al codigo de salida del proceso. */
static int exit_code_for(int rc) {
    return rc == SXFS_ERR_NO_SPACE ? SXFS_CLI_EXIT_NO_SPACE : 1;
}

/* Crea una imagen de total_sectors * 512 bytes, toda ella ceros.
 *
 * Los ceros NO se escriben: el archivo se deja sparse con ftruncate, y el
 * filesystem lo ve igual porque un hueco lee como cero. La diferencia no es
 * cosmetics: escribir de verdad un volumen de 1 GiB costaria 1 GiB en disco y,
 * peor, cada build copiaria la imagen entera. copy_candidate preserva los
 * huecos, pero no puede preservar lo que nunca fue un hueco. Con el volumen
 * default de 1 GiB eso son 1 GiB de escritura por build en vez de los ~40 MiB
 * que el volumen ocupa de verdad. */
static int create_zeroed_image(const char* path, uint32_t total_sectors) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "sxfs-cli: no se pudo crear '%s'.\n", path);
        return 1;
    }
    const off_t bytes = (off_t)total_sectors * SXFS_SECTOR_SIZE;
    int rc = 0;
    if (ftruncate(fd, bytes) != 0) {
        fprintf(stderr, "sxfs-cli: no se pudo dimensionar '%s' a %lld bytes: %s.\n",
                path, (long long)bytes, strerror(errno));
        rc = 1;
    }
    if (close(fd) != 0 && rc == 0) {
        fprintf(stderr, "sxfs-cli: no se pudo cerrar '%s'.\n", path);
        rc = 1;
    }
    if (rc != 0) {
        unlink(path);
    }
    return rc;
}

/* Lee un archivo del host completo en memoria (malloc). El caller hace free. */
static uint8_t* read_host_file(const char* path, uint32_t* out_size) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    uint8_t* data = (uint8_t*)malloc(size > 0 ? (size_t)size : 1);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }
    if (size > 0 && fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (uint32_t)size;
    return data;
}

/* Recorta los terminadores de línea del manifiesto. */
static void chomp(char* line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

#define SXFS_HOST_SEPARATOR '/'
#define SXFS_IS_SEPARATOR(value) ((value) == '/')

static int make_one_directory(const char* path) {
    if (path[0] == '\0') {
        return 0;
    }
    if (SXFS_MKDIR(path) == 0) {
        return 0;
    }
    if (errno != EEXIST) {
        return -1;
    }
    struct stat info;
    if (stat(path, &info) != 0 || (info.st_mode & 0170000u) != 0040000u) {
        return -1;
    }
    return 0;
}

static int ensure_directory_tree(const char* path) {
    char buffer[4096];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(buffer)) {
        return -1;
    }
    memcpy(buffer, path, length + 1);
    while (length > 1 && SXFS_IS_SEPARATOR(buffer[length - 1])) {
        buffer[--length] = '\0';
    }

    size_t start = 0;
    if (SXFS_IS_SEPARATOR(buffer[0])) {
        start = 1;
    }
    for (size_t index = start; index < length; ++index) {
        if (!SXFS_IS_SEPARATOR(buffer[index])) {
            continue;
        }
        char saved = buffer[index];
        buffer[index] = '\0';
        if (index > start && make_one_directory(buffer) != 0) {
            return -1;
        }
        buffer[index] = saved;
    }
    return make_one_directory(buffer);
}

static int ensure_parent_directory(const char* path) {
    char buffer[4096];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(buffer)) {
        return -1;
    }
    memcpy(buffer, path, length + 1);
    size_t separator = length;
    while (separator > 0 && !SXFS_IS_SEPARATOR(buffer[separator - 1])) {
        --separator;
    }
    if (separator == 0) {
        return 0;
    }
    if (separator == 1 && buffer[0] == '/') {
        return 0;
    }
    buffer[separator - 1] = '\0';
    return ensure_directory_tree(buffer);
}

static int join_host_path(const char* root, const char* relative, char* output,
                          size_t capacity) {
    size_t root_length = strlen(root);
    size_t relative_length = strlen(relative);
    size_t separator = root_length != 0 && !SXFS_IS_SEPARATOR(root[root_length - 1]) ? 1u : 0u;
    if (root_length + separator + relative_length + 1u > capacity) {
        return -1;
    }
    memcpy(output, root, root_length);
    if (separator != 0) {
        output[root_length] = SXFS_HOST_SEPARATOR;
    }
    memcpy(output + root_length + separator, relative, relative_length);
    output[root_length + separator + relative_length] = '\0';
    return 0;
}

struct extract_context {
    const struct sxfs_ctx* image;
    const char* destination;
    uint32_t file_count;
    uint64_t byte_count;
    /* The `owner` manifest being written, or NULL. */
    FILE* ownership;
};

static int extract_file(struct extract_context* context, const char* host_path,
                        uint32_t inode_id) {
    uint32_t size = context->image->inodes[inode_id - 1].size;
    uint8_t* data = size == 0 ? NULL : (uint8_t*)malloc(size);
    if (size != 0 && data == NULL) {
        return SXFS_ERR_IO;
    }
    uint32_t read_size = 0;
    int rc = sxfs_read_inode(context->image, inode_id, data, size, &read_size);
    if (rc != SXFS_OK || read_size != size) {
        free(data);
        return rc == SXFS_OK ? SXFS_ERR_IO : rc;
    }
    FILE* output = fopen(host_path, "wb");
    if (output == NULL) {
        free(data);
        return SXFS_ERR_IO;
    }
    if (size != 0 && fwrite(data, 1, size, output) != size) {
        fclose(output);
        free(data);
        remove(host_path);
        return SXFS_ERR_IO;
    }
    free(data);
    if (fclose(output) != 0) {
        remove(host_path);
        return SXFS_ERR_IO;
    }
    context->file_count += 1;
    context->byte_count += size;
    return SXFS_OK;
}

static int extract_callback(void* cookie, const char* relative, uint32_t inode_id,
                            uint16_t type) {
    struct extract_context* context = (struct extract_context*)cookie;
    char host_path[4096];
    if (join_host_path(context->destination, relative, host_path, sizeof(host_path)) != 0) {
        return SXFS_ERR_TOO_LONG;
    }
    if (context->ownership != NULL) {
        uint32_t uid = 0;
        uint32_t gid = 0;
        uint16_t mode = 0;
        /* Mode 0 records nothing: the rebuilt image gives it the default. */
        if (sxfs_get_ownership(context->image, inode_id, &uid, &gid, &mode) == SXFS_OK && mode != 0 &&
            fprintf(context->ownership, "owner\t%s\t%u\t%u\t%o\n", relative, uid, gid, mode) < 0) {
            return SXFS_ERR_IO;
        }
    }
    if (type == SXFS_INODE_DIRECTORY) {
        return ensure_directory_tree(host_path) == 0 ? SXFS_OK : SXFS_ERR_IO;
    }
    if (ensure_parent_directory(host_path) != 0) {
        return SXFS_ERR_IO;
    }
    return extract_file(context, host_path, inode_id);
}

static int check_metadata(const struct sxfs_ctx* ctx);
static int validate_image(const struct sxfs_ctx* ctx);
static int validate_image_file(const char* image_path, const struct sxfs_ctx* ctx);

static int cmd_info(const char* image_path) {
    FILE* image = fopen(image_path, "rb");
    if (image == NULL) {
        fprintf(stderr, "sxfs-cli: no existe o no se pudo leer la imagen '%s'.\n", image_path);
        return 1;
    }
    struct sxfs_ctx ctx;
    sxfs_ctx_init(&ctx, image, file_read, file_write);
    int rc = sxfs_open(&ctx);
    if (rc == SXFS_OK) {
        rc = validate_image_file(image_path, &ctx);
    }
    fclose(image);
    if (rc != SXFS_OK) {
        fprintf(stderr, "sxfs-cli: '%s' no es una imagen SxFS valida: %s.\n",
            image_path, sxfs_strerror(rc));
        return 1;
    }
    printf("%u\n", ctx.total_sectors);
    return 0;
}

static int cmd_extract(const char* image_path, const char* destination, const char* ownership_path) {
    FILE* image = fopen(image_path, "rb");
    if (image == NULL) {
        fprintf(stderr, "sxfs-cli: no existe o no se pudo leer la imagen '%s'.\n", image_path);
        return 1;
    }
    struct sxfs_ctx ctx;
    sxfs_ctx_init(&ctx, image, file_read, file_write);
    int rc = sxfs_open(&ctx);
    if (rc == SXFS_OK) {
        rc = validate_image_file(image_path, &ctx);
    }
    if (rc == SXFS_OK && ensure_directory_tree(destination) != 0) {
        fprintf(stderr, "sxfs-cli: no se pudo crear el destino '%s'.\n", destination);
        rc = SXFS_ERR_IO;
    }
    struct extract_context context = { &ctx, destination, 0, 0, NULL };
    if (rc == SXFS_OK && ownership_path != NULL) {
        context.ownership = fopen(ownership_path, "w");
        if (context.ownership == NULL) {
            fprintf(stderr, "sxfs-cli: could not write '%s'.\n", ownership_path);
            rc = SXFS_ERR_IO;
        }
    }
    if (rc == SXFS_OK) {
        rc = sxfs_walk(&ctx, extract_callback, &context);
    }
    if (context.ownership != NULL && fclose(context.ownership) != 0 && rc == SXFS_OK) {
        rc = SXFS_ERR_IO;
    }
    fclose(image);
    if (rc != SXFS_OK) {
        fprintf(stderr, "sxfs-cli: no se pudo extraer la imagen: %s.\n", sxfs_strerror(rc));
        return 1;
    }
    printf("sxfs-cli: extraidas %u archivo(s), %llu bytes.\n", context.file_count,
        (unsigned long long)context.byte_count);
    return 0;
}

/* --- Subcomandos --------------------------------------------------------- */

static int cmd_create(const char* image_path, uint32_t total_sectors) {
    if (create_zeroed_image(image_path, total_sectors) != 0) {
        return 1;
    }
    FILE* file = fopen(image_path, "rb+");
    if (file == NULL) {
        fprintf(stderr, "sxfs-cli: no se pudo abrir '%s' para formatear.\n", image_path);
        return 1;
    }
    struct sxfs_ctx ctx;
    sxfs_ctx_init(&ctx, file, file_read, file_write);
    int rc = sxfs_format(&ctx, total_sectors);
    if (rc == SXFS_OK) {
        rc = sxfs_flush(&ctx);
    }
    fclose(file);
    if (rc != SXFS_OK) {
        fprintf(stderr, "sxfs-cli: fallo al formatear: %s.\n", sxfs_strerror(rc));
        return 1;
    }
    return 0;
}

static int check_metadata(const struct sxfs_ctx* ctx) {
    if (ctx->total_sectors <= SXFS_DATA_LBA || ctx->total_sectors > SXFS_MAX_TOTAL_SECTORS) {
        fprintf(stderr, "sxfs-cli: total_sectors=%u fuera del rango del bitmap.\n", ctx->total_sectors);
        return 1;
    }

    uint8_t claims[sizeof(ctx->block_bitmap)];
    memset(claims, 0, sizeof(claims));
    for (uint32_t inode_id = 1; inode_id <= SXFS_MAX_INODES; ++inode_id) {
        const struct sxfs_inode* inode = &ctx->inodes[inode_id - 1];
        const int allocated = sxfs_bitmap_test(ctx->inode_bitmap, inode_id - 1);
        const int used = inode->type != SXFS_INODE_UNUSED;
        if (allocated != used || (used && inode->inode_id != inode_id) ||
            inode->extent_count > SXFS_MAX_EXTENTS ||
            (inode->type != SXFS_INODE_UNUSED && inode->type != SXFS_INODE_FILE &&
             inode->type != SXFS_INODE_DIRECTORY)) {
            fprintf(stderr, "sxfs-cli: inodo %u inconsistente.\n", inode_id);
            return 1;
        }
        if (!used) {
            continue;
        }

        uint64_t capacity = 0;
        for (uint32_t index = 0; index < inode->extent_count; ++index) {
            const struct sxfs_extent* extent = &inode->extents[index];
            if (extent->sector_count == 0 || extent->start_lba < SXFS_DATA_LBA ||
                extent->start_lba >= ctx->total_sectors ||
                extent->sector_count > ctx->total_sectors - extent->start_lba) {
                fprintf(stderr, "sxfs-cli: extent invalido en inodo %u.\n", inode_id);
                return 1;
            }
            for (uint32_t sector = 0; sector < extent->sector_count; ++sector) {
                const uint32_t absolute = extent->start_lba + sector;
                if (!sxfs_bitmap_test(ctx->block_bitmap, absolute) ||
                    sxfs_bitmap_test(claims, absolute)) {
                    fprintf(stderr, "sxfs-cli: bloque %u asignado invalido en inodo %u.\n",
                        absolute, inode_id);
                    return 1;
                }
                sxfs_bitmap_set(claims, absolute, 1);
            }
            capacity += extent->sector_count;
        }
        if (inode->size > capacity * SXFS_SECTOR_SIZE) {
            fprintf(stderr, "sxfs-cli: inodo %u declara mas bytes que su capacidad.\n", inode_id);
            return 1;
        }
    }
    return 0;
}

static int validate_noop(void* cookie, const char* relpath, uint32_t inode_id,
                        uint16_t type) {
    (void)cookie;
    (void)relpath;
    (void)inode_id;
    (void)type;
    return SXFS_OK;
}

static int validate_image(const struct sxfs_ctx* ctx) {
    int rc = check_metadata(ctx);
    if (rc != SXFS_OK) {
        return rc;
    }
    return sxfs_walk(ctx, validate_noop, NULL);
}

static int validate_image_file(const char* image_path, const struct sxfs_ctx* ctx) {
    int rc = validate_image(ctx);
    if (rc != SXFS_OK) {
        return rc;
    }
    /* El archivo puede ser MAS GRANDE que el filesystem, y eso es lo que el
     * kernel ya acepta: mount exige superblock.total_sectors <= sectores del
     * device, no igualdad (kernel/sxfs.cpp, load_filesystem_from_device). La
     * cola de mas queda fuera del sistema de archivos, sin tocar, y es lo que
     * permite agrandar la imagen sin que el guest se entere. Antes esto exigia
     * igualdad y rechazaba una imagen que el kernel monta sin pestanear.
     *
     * Lo que si es un error es lo contrario: un archivo mas corto que lo que el
     * superblock declara es una imagen truncada, y ahi si hay un sector que el
     * filesystem cree suyo y no existe. */
    struct stat info;
    const uint64_t declared = (uint64_t)ctx->total_sectors * SXFS_SECTOR_SIZE;
    if (stat(image_path, &info) != 0) {
        fprintf(stderr, "sxfs-cli: no se pudo consultar el tamano de '%s'.\n", image_path);
        return SXFS_ERR_INVALID;
    }
    if ((uint64_t)info.st_size < declared) {
        fprintf(stderr,
            "sxfs-cli: '%s' esta truncada: %llu bytes en disco, %llu declarados por el superblock.\n",
            image_path, (unsigned long long)info.st_size, (unsigned long long)declared);
        return SXFS_ERR_INVALID;
    }
    return SXFS_OK;
}

struct ownership_count {
    const struct sxfs_ctx* image;
    uint32_t unowned;
};

static int count_unowned(void* cookie, const char* relative, uint32_t inode_id, uint16_t type) {
    struct ownership_count* count = (struct ownership_count*)cookie;
    const struct sxfs_inode* inode = &count->image->inodes[inode_id - 1];
    (void)relative;
    (void)type;
    if (inode->mode == 0 || !sxfs_mode_matches_type(inode->mode, inode->type)) {
        count->unowned += 1;
    }
    return SXFS_OK;
}

static int cmd_check(const char* image_path) {
    FILE* image = fopen(image_path, "rb");
    if (image == NULL) {
        fprintf(stderr, "sxfs-cli: no existe o no se pudo leer la imagen '%s'.\n", image_path);
        return 1;
    }

    struct sxfs_ctx ctx;
    sxfs_ctx_init(&ctx, image, file_read, file_write);
    int rc = sxfs_open(&ctx);
    if (rc != SXFS_OK) {
        fprintf(stderr, "sxfs-cli: '%s' no es una imagen SxFS valida: %s.\n",
            image_path, sxfs_strerror(rc));
        fclose(image);
        return 1;
    }
    if (validate_image_file(image_path, &ctx) != 0) {
        fclose(image);
        return 1;
    }
    /* Ownership is checked only where it is recorded: an image written before
     * it is valid, and gets stamped by the next apply or mount. The walk reads
     * directories, so the image is still open. */
    if ((ctx.superblock.features & SXFS_FEATURE_OWNERSHIP) != 0) {
        struct ownership_count count = { &ctx, 0 };
        const struct sxfs_inode* root = &ctx.inodes[SXFS_ROOT_INODE - 1];
        if (root->mode == 0 || !sxfs_mode_matches_type(root->mode, root->type)) {
            count.unowned += 1;
        }
        const int walked = sxfs_walk(&ctx, count_unowned, &count);
        if (walked != SXFS_OK) {
            fprintf(stderr, "sxfs-cli: could not walk '%s' to check ownership: %s.\n",
                image_path, sxfs_strerror(walked));
            fclose(image);
            return 1;
        }
        if (count.unowned != 0) {
            fprintf(stderr, "sxfs-cli: '%s' records ownership, but %u inode(s) have none.\n",
                image_path, count.unowned);
            fclose(image);
            return 1;
        }
    }
    fclose(image);

    printf("sxfs-cli: '%s' es una imagen SxFS valida (%u sectores, ownership %s).\n",
        image_path, ctx.total_sectors,
        (ctx.superblock.features & SXFS_FEATURE_OWNERSHIP) != 0 ? "recorded" : "not recorded");
    return 0;
}

static int cmd_apply(const char* image_path, const char* manifest_path) {
    FILE* image = fopen(image_path, "rb+");
    if (image == NULL) {
        fprintf(stderr, "sxfs-cli: no existe la imagen '%s'.\n", image_path);
        return 1;
    }
    struct sxfs_ctx ctx;
    sxfs_ctx_init(&ctx, image, file_read, file_write);
    int rc = sxfs_open(&ctx);
    if (rc != SXFS_OK) {
        fprintf(stderr, "sxfs-cli: '%s' no es una imagen SxFS valida: %s.\n",
                image_path, sxfs_strerror(rc));
        fclose(image);
        return 1;
    }
    if (validate_image_file(image_path, &ctx) != 0) {
        fclose(image);
        return 1;
    }
    rc = sxfs_stamp_ownership(&ctx);
    if (rc != SXFS_OK) {
        fprintf(stderr, "sxfs-cli: could not stamp ownership: %s.\n", sxfs_strerror(rc));
        fclose(image);
        return 1;
    }

    FILE* manifest = (strcmp(manifest_path, "-") == 0) ? stdin : fopen(manifest_path, "r");
    if (manifest == NULL) {
        fprintf(stderr, "sxfs-cli: no se pudo abrir el manifiesto '%s'.\n", manifest_path);
        fclose(image);
        return 1;
    }

    int status = 0;
    char line[4096];
    unsigned long lineno = 0;
    while (fgets(line, sizeof(line), manifest) != NULL) {
        lineno += 1;
        if (strchr(line, '\n') == NULL && !feof(manifest)) {
            int discard;
            while ((discard = fgetc(manifest)) != '\n' && discard != EOF) {
            }
            fprintf(stderr, "sxfs-cli: linea %lu supera el limite del manifiesto.\n", lineno);
            status = 1;
            break;
        }
        chomp(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char* tab = strchr(line, '\t');
        if (tab == NULL) {
            fprintf(stderr, "sxfs-cli: linea %lu mal formada (falta TAB): %s\n", lineno, line);
            status = 1;
            break;
        }
        *tab = '\0';
        const char* op = line;
        char* rest = tab + 1;

        if (strcmp(op, "mkdir") == 0) {
            if (!sxfs_path_valid(rest)) {
                fprintf(stderr, "sxfs-cli: linea %lu contiene una ruta invalida.\n", lineno);
                status = 1;
                break;
            }
            rc = sxfs_mkdir_p(&ctx, rest);
            if (rc != SXFS_OK) {
                fprintf(stderr, "sxfs-cli: mkdir '%s' fallo: %s.\n", rest, sxfs_strerror(rc));
                status = exit_code_for(rc);
                break;
            }
        } else if (strcmp(op, "file") == 0) {
            char* tab2 = strchr(rest, '\t');
            if (tab2 == NULL) {
                fprintf(stderr, "sxfs-cli: linea %lu 'file' sin archivo host.\n", lineno);
                status = 1;
                break;
            }
            *tab2 = '\0';
            const char* relpath = rest;
            const char* host_path = tab2 + 1;
            if (!sxfs_path_valid(relpath) || strchr(host_path, '\t') != NULL) {
                fprintf(stderr, "sxfs-cli: linea %lu contiene una ruta invalida.\n", lineno);
                status = 1;
                break;
            }
            uint32_t size = 0;
            uint8_t* data = read_host_file(host_path, &size);
            if (data == NULL) {
                fprintf(stderr, "sxfs-cli: no se pudo leer '%s'.\n", host_path);
                status = 1;
                break;
            }
            rc = sxfs_write_file(&ctx, relpath, data, size);
            free(data);
            if (rc != SXFS_OK) {
                fprintf(stderr, "sxfs-cli: file '%s' fallo: %s.\n", relpath, sxfs_strerror(rc));
                status = exit_code_for(rc);
                break;
            }
        } else if (strcmp(op, "owner") == 0) {
            char* fields[4] = {rest, NULL, NULL, NULL};
            for (int field = 1; field < 4; ++field) {
                char* next = strchr(fields[field - 1], '\t');
                if (next == NULL) {
                    break;
                }
                *next = '\0';
                fields[field] = next + 1;
            }
            char* end = NULL;
            unsigned long uid = 0;
            unsigned long gid = 0;
            unsigned long mode = 0;
            int parsed = fields[3] != NULL && strchr(fields[3], '\t') == NULL;
            if (parsed) {
                errno = 0;
                uid = strtoul(fields[1], &end, 10);
                parsed = errno == 0 && *fields[1] != '\0' && *end == '\0' && uid <= UINT32_MAX;
            }
            if (parsed) {
                gid = strtoul(fields[2], &end, 10);
                parsed = errno == 0 && *fields[2] != '\0' && *end == '\0' && gid <= UINT32_MAX;
            }
            if (parsed) {
                mode = strtoul(fields[3], &end, 8);
                parsed = errno == 0 && *fields[3] != '\0' && *end == '\0' && mode <= 0177777u;
            }
            if (!parsed) {
                fprintf(stderr, "sxfs-cli: line %lu: 'owner' takes <path> <uid> <gid> <octal mode>.\n", lineno);
                status = 1;
                break;
            }
            rc = sxfs_set_ownership(&ctx, fields[0], (uint32_t)uid, (uint32_t)gid, (uint16_t)mode);
            if (rc != SXFS_OK) {
                fprintf(stderr, "sxfs-cli: owner '%s' failed: %s.\n", fields[0], sxfs_strerror(rc));
                status = exit_code_for(rc);
                break;
            }
        } else {
            fprintf(stderr, "sxfs-cli: operacion desconocida '%s' en linea %lu.\n", op, lineno);
            status = 1;
            break;
        }
    }

    if (manifest != stdin) {
        fclose(manifest);
    }

    if (status == 0) {
        rc = sxfs_flush(&ctx);
        if (rc != SXFS_OK) {
            fprintf(stderr, "sxfs-cli: fallo al persistir: %s.\n", sxfs_strerror(rc));
            status = exit_code_for(rc);
        }
    }
    fclose(image);
    return status;
}

/* Removes one or more paths from the image. Paths follow the manifest
 * convention -- relative to the SxFS root, which is the guest's root -- and a
 * leading '/' is tolerated: "bin/x" and "/bin/x" are the same, and both are
 * the "/bin/x" the OS sees. With force, a path that does not exist is
 * not an error (rm -f), which is what retiring a file that may or may not be
 * there needs. */
static int cmd_rm(const char* image_path, char** paths, int path_count, int force) {
    FILE* image = fopen(image_path, "rb+");
    if (image == NULL) {
        fprintf(stderr, "sxfs-cli: no existe la imagen '%s'.\n", image_path);
        return 1;
    }

    struct sxfs_ctx ctx;
    sxfs_ctx_init(&ctx, image, file_read, file_write);
    int rc = sxfs_open(&ctx);
    if (rc != SXFS_OK) {
        fprintf(stderr, "sxfs-cli: '%s' no es una imagen SxFS valida: %s.\n",
                image_path, sxfs_strerror(rc));
        fclose(image);
        return 1;
    }
    rc = validate_image_file(image_path, &ctx);
    if (rc != SXFS_OK) {
        fclose(image);
        return 1;
    }
    rc = sxfs_stamp_ownership(&ctx);
    if (rc != SXFS_OK) {
        fprintf(stderr, "sxfs-cli: could not stamp ownership: %s.\n", sxfs_strerror(rc));
        fclose(image);
        return 1;
    }

    int status = 0;
    int removed = 0;
    for (int i = 0; i < path_count; ++i) {
        const char* relpath = paths[i];
        while (*relpath == '/') {
            ++relpath;
        }

        rc = sxfs_remove(&ctx, relpath);
        if (rc == SXFS_ERR_NOT_FOUND && force) {
            continue;
        }
        if (rc != SXFS_OK) {
            fprintf(stderr, "sxfs-cli: rm '%s': %s.\n", paths[i], sxfs_strerror(rc));
            status = exit_code_for(rc);
            continue;
        }
        removed += 1;
    }

    /* Se persiste lo que si se pudo borrar aunque alguna ruta haya fallado: el
     * core ya dejo la metadata en memoria consistente para esas. */
    if (removed > 0) {
        rc = sxfs_flush(&ctx);
        if (rc != SXFS_OK) {
            fprintf(stderr, "sxfs-cli: fallo el flush: %s.\n", sxfs_strerror(rc));
            status = exit_code_for(rc);
        }
    }

    fclose(image);
    return status;
}

static void usage(void) {
    fprintf(stderr,
        "Usage:\n"
        "  sxfs-cli check   <image>\n"
        "  sxfs-cli info    <image>\n"
        "  sxfs-cli extract <image> <destination_directory> [<ownership_manifest>]\n"
        "  sxfs-cli create  <image> <total_sectors>\n"
        "  sxfs-cli apply   <image> <manifest>\n"
        "  sxfs-cli rm [-f] <image> <path> [<path>...]\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "check") == 0) {
        if (argc != 3) {
            usage();
            return 2;
        }
        return cmd_check(argv[2]);
    }
    if (strcmp(argv[1], "info") == 0) {
        if (argc != 3) {
            usage();
            return 2;
        }
        return cmd_info(argv[2]);
    }
    if (strcmp(argv[1], "extract") == 0) {
        if (argc != 4 && argc != 5) {
            usage();
            return 2;
        }
        return cmd_extract(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
    }
    if (strcmp(argv[1], "create") == 0) {
        if (argc != 4) {
            usage();
            return 2;
        }
        long sectors = strtol(argv[3], NULL, 10);
        if (sectors <= (long)SXFS_DATA_LBA || sectors > (long)SXFS_MAX_TOTAL_SECTORS) {
            fprintf(stderr, "sxfs-cli: total_sectores debe estar entre %u y %u.\n",
                (unsigned)SXFS_DATA_LBA + 1u, (unsigned)SXFS_MAX_TOTAL_SECTORS);
            return 2;
        }
        return cmd_create(argv[2], (uint32_t)sectors);
    }
    if (strcmp(argv[1], "apply") == 0) {
        if (argc != 4) {
            usage();
            return 2;
        }
        return cmd_apply(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "rm") == 0) {
        const int force = argc > 2 && strcmp(argv[2], "-f") == 0;
        const int first = force ? 3 : 2;
        if (argc < first + 2) {
            usage();
            return 2;
        }
        return cmd_rm(argv[first], &argv[first + 1], argc - first - 1, force);
    }
    usage();
    return 2;
}
