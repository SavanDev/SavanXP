/*
 * svfs_cli.c -- Tool de host para crear/poblar imagenes SVFS2.
 *
 * Reemplaza el byte-poking en PowerShell de tools/UserAppCommon.ps1 por el core
 * portable (libsvfs) sobre un backend de archivo. Es agnostico del OS: no
 * recorre directorios (eso lo hace el driver, que conoce el filesystem del
 * host); recibe un manifiesto con las operaciones ya resueltas y aplica todo en
 * una sola pasada sobre la imagen.
 *
 * Uso:
 *   svfs-cli create <imagen> <total_sectores>
 *       Crea una imagen nueva, formateada y vacia (solo la raiz).
 *   svfs-cli apply  <imagen> <manifiesto>
 *       Monta una imagen existente (preserva su contenido) y aplica el
 *       manifiesto. Cada linea, separada por TAB:
 *           mkdir <ruta_relativa>
 *           file  <ruta_relativa>\t<archivo_host>
 *       Las rutas son relativas a la raiz de SVFS (sin prefijo /disk).
 */
#define _FILE_OFFSET_BITS 64
#define _CRT_SECURE_NO_WARNINGS 1 /* fopen/strtol: UCRT los marca "inseguros" */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "svfs_core.h"

/* --- Backend de bloque sobre un FILE* ------------------------------------ */

static int seek_sector(FILE* file, uint32_t lba) {
#if defined(_WIN32)
    return _fseeki64(file, (long long)lba * SVFS_SECTOR_SIZE, SEEK_SET);
#else
    return fseeko(file, (off_t)lba * SVFS_SECTOR_SIZE, SEEK_SET);
#endif
}

static int file_read(void* cookie, uint32_t lba, uint32_t count, void* buffer) {
    FILE* file = (FILE*)cookie;
    if (seek_sector(file, lba) != 0) {
        return -1;
    }
    size_t want = (size_t)count * SVFS_SECTOR_SIZE;
    return fread(buffer, 1, want, file) == want ? 0 : -1;
}

static int file_write(void* cookie, uint32_t lba, uint32_t count, const void* buffer) {
    FILE* file = (FILE*)cookie;
    if (seek_sector(file, lba) != 0) {
        return -1;
    }
    size_t want = (size_t)count * SVFS_SECTOR_SIZE;
    return fwrite(buffer, 1, want, file) == want ? 0 : -1;
}

/* --- Utilidades ---------------------------------------------------------- */

static const char* svfs_strerror(int rc) {
    switch (rc) {
        case SVFS_OK: return "ok";
        case SVFS_ERR_IO: return "error de I/O";
        case SVFS_ERR_NO_SPACE: return "sin espacio contiguo";
        case SVFS_ERR_NO_INODES: return "sin inodos libres";
        case SVFS_ERR_INVALID: return "argumento/ruta invalida";
        case SVFS_ERR_EXISTS: return "colision de tipo (archivo vs directorio)";
        case SVFS_ERR_NOT_FOUND: return "ruta inexistente";
        case SVFS_ERR_TOO_LONG: return "nombre/ruta demasiado largo";
        default: return "error desconocido";
    }
}

/* Crea (o trunca) una imagen de total_sectors * 512 bytes rellena de ceros. */
static int create_zeroed_image(const char* path, uint32_t total_sectors) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "svfs-cli: no se pudo crear '%s'.\n", path);
        return 1;
    }
    static uint8_t zeros[SVFS_SECTOR_SIZE * 64];
    memset(zeros, 0, sizeof(zeros));
    uint32_t remaining = total_sectors;
    const uint32_t chunk_sectors = sizeof(zeros) / SVFS_SECTOR_SIZE;
    while (remaining > 0) {
        uint32_t n = remaining < chunk_sectors ? remaining : chunk_sectors;
        if (fwrite(zeros, 1, (size_t)n * SVFS_SECTOR_SIZE, file) != (size_t)n * SVFS_SECTOR_SIZE) {
            fprintf(stderr, "svfs-cli: fallo al escribir ceros en '%s'.\n", path);
            fclose(file);
            return 1;
        }
        remaining -= n;
    }
    fclose(file);
    return 0;
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

/* Recorta el '\r' y '\n' finales (manifiestos generados en Windows). */
static void chomp(char* line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

/* --- Subcomandos --------------------------------------------------------- */

static int cmd_create(const char* image_path, uint32_t total_sectors) {
    if (create_zeroed_image(image_path, total_sectors) != 0) {
        return 1;
    }
    FILE* file = fopen(image_path, "rb+");
    if (file == NULL) {
        fprintf(stderr, "svfs-cli: no se pudo abrir '%s' para formatear.\n", image_path);
        return 1;
    }
    struct svfs_ctx ctx;
    svfs_ctx_init(&ctx, file, file_read, file_write);
    int rc = svfs_format(&ctx, total_sectors);
    if (rc == SVFS_OK) {
        rc = svfs_flush(&ctx);
    }
    fclose(file);
    if (rc != SVFS_OK) {
        fprintf(stderr, "svfs-cli: fallo al formatear: %s.\n", svfs_strerror(rc));
        return 1;
    }
    return 0;
}

static int cmd_apply(const char* image_path, const char* manifest_path) {
    FILE* image = fopen(image_path, "rb+");
    if (image == NULL) {
        fprintf(stderr, "svfs-cli: no existe la imagen '%s'.\n", image_path);
        return 1;
    }
    struct svfs_ctx ctx;
    svfs_ctx_init(&ctx, image, file_read, file_write);
    int rc = svfs_open(&ctx);
    if (rc != SVFS_OK) {
        fprintf(stderr, "svfs-cli: '%s' no es una imagen SVFS2 valida: %s.\n",
                image_path, svfs_strerror(rc));
        fclose(image);
        return 1;
    }

    FILE* manifest = (strcmp(manifest_path, "-") == 0) ? stdin : fopen(manifest_path, "r");
    if (manifest == NULL) {
        fprintf(stderr, "svfs-cli: no se pudo abrir el manifiesto '%s'.\n", manifest_path);
        fclose(image);
        return 1;
    }

    int status = 0;
    char line[1024];
    unsigned long lineno = 0;
    while (fgets(line, sizeof(line), manifest) != NULL) {
        lineno += 1;
        chomp(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char* tab = strchr(line, '\t');
        if (tab == NULL) {
            fprintf(stderr, "svfs-cli: linea %lu mal formada (falta TAB): %s\n", lineno, line);
            status = 1;
            break;
        }
        *tab = '\0';
        const char* op = line;
        char* rest = tab + 1;

        if (strcmp(op, "mkdir") == 0) {
            rc = svfs_mkdir_p(&ctx, rest);
            if (rc != SVFS_OK) {
                fprintf(stderr, "svfs-cli: mkdir '%s' fallo: %s.\n", rest, svfs_strerror(rc));
                status = 1;
                break;
            }
        } else if (strcmp(op, "file") == 0) {
            char* tab2 = strchr(rest, '\t');
            if (tab2 == NULL) {
                fprintf(stderr, "svfs-cli: linea %lu 'file' sin archivo host.\n", lineno);
                status = 1;
                break;
            }
            *tab2 = '\0';
            const char* relpath = rest;
            const char* host_path = tab2 + 1;
            uint32_t size = 0;
            uint8_t* data = read_host_file(host_path, &size);
            if (data == NULL) {
                fprintf(stderr, "svfs-cli: no se pudo leer '%s'.\n", host_path);
                status = 1;
                break;
            }
            rc = svfs_write_file(&ctx, relpath, data, size);
            free(data);
            if (rc != SVFS_OK) {
                fprintf(stderr, "svfs-cli: file '%s' fallo: %s.\n", relpath, svfs_strerror(rc));
                status = 1;
                break;
            }
        } else {
            fprintf(stderr, "svfs-cli: operacion desconocida '%s' en linea %lu.\n", op, lineno);
            status = 1;
            break;
        }
    }

    if (manifest != stdin) {
        fclose(manifest);
    }

    if (status == 0) {
        rc = svfs_flush(&ctx);
        if (rc != SVFS_OK) {
            fprintf(stderr, "svfs-cli: fallo al persistir: %s.\n", svfs_strerror(rc));
            status = 1;
        }
    }
    fclose(image);
    return status;
}

static void usage(void) {
    fprintf(stderr,
        "Uso:\n"
        "  svfs-cli create <imagen> <total_sectores>\n"
        "  svfs-cli apply  <imagen> <manifiesto>\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "create") == 0) {
        if (argc != 4) {
            usage();
            return 2;
        }
        long sectors = strtol(argv[3], NULL, 10);
        if (sectors <= (long)SVFS_DATA_LBA) {
            fprintf(stderr, "svfs-cli: total_sectores debe ser > %u.\n", (unsigned)SVFS_DATA_LBA);
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
    usage();
    return 2;
}
