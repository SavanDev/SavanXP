/*
 * svfs_core.c -- Implementacion del core portable de SVFS2.
 *
 * Porta fielmente la logica del instalador host-side historico
 * (tools/UserAppCommon.ps1): allocacion de inodos secuencial, allocacion de
 * bloques first-fit contigua, modelo de extent unico por inodo (cada grow
 * reubica a una sola corrida contigua) y directorios como arrays empaquetados
 * de svfs_dir_entry. Mantener esa equivalencia es lo que hace que las imagenes
 * que produce este core sean montables por el kernel igual que las de antes.
 */
#include "svfs_core.h"

#include <string.h>

/* Buffer de preservacion para grow de inodos con datos previos. En el flujo de
 * build solo los directorios crecen con contenido existente (los archivos se
 * escriben una sola vez desde vacio), y un directorio nunca supera
 * SVFS_MAX_RECORDS entradas, asi que este tope alcanza sin heap. */
#define SVFS_PRESERVE_CAPACITY (SVFS_MAX_RECORDS * SVFS_DIR_ENTRY_SIZE)

static uint32_t sectors_for(uint32_t bytes) {
    return (bytes + (SVFS_SECTOR_SIZE - 1u)) / SVFS_SECTOR_SIZE;
}

static struct svfs_inode* inode_at(struct svfs_ctx* ctx, uint32_t inode_id) {
    if (inode_id == 0 || inode_id > SVFS_MAX_INODES) {
        return NULL;
    }
    return &ctx->inodes[inode_id - 1];
}

static uint32_t inode_capacity_bytes(const struct svfs_inode* inode) {
    return svfs_inode_capacity_sectors(inode) * SVFS_SECTOR_SIZE;
}

/* --- I/O de datos por extents (via callbacks) ---------------------------- */

static int read_inode_bytes(struct svfs_ctx* ctx, const struct svfs_inode* inode,
                            uint8_t* dst, uint32_t dst_capacity, uint32_t* out_len) {
    uint32_t want = inode->size;
    if (want > dst_capacity) {
        want = dst_capacity;
    }
    uint32_t pos = 0;
    for (uint32_t e = 0; e < inode->extent_count && pos < want; ++e) {
        const struct svfs_extent* extent = &inode->extents[e];
        for (uint32_t s = 0; s < extent->sector_count && pos < want; ++s) {
            uint8_t sector[SVFS_SECTOR_SIZE];
            if (ctx->read(ctx->cookie, extent->start_lba + s, 1, sector) != 0) {
                return SVFS_ERR_IO;
            }
            uint32_t n = want - pos;
            if (n > SVFS_SECTOR_SIZE) {
                n = SVFS_SECTOR_SIZE;
            }
            memcpy(dst + pos, sector, n);
            pos += n;
        }
    }
    if (out_len) {
        *out_len = pos;
    }
    return SVFS_OK;
}

/* Escribe `len` bytes en los extents del inodo, zero-rellenando el resto de
 * cada extent, y fija inode->size = len (los extents no cambian). */
static int write_inode_bytes(struct svfs_ctx* ctx, uint32_t inode_id,
                             const uint8_t* data, uint32_t len) {
    struct svfs_inode* inode = inode_at(ctx, inode_id);
    if (inode == NULL) {
        return SVFS_ERR_INVALID;
    }
    if (len > inode_capacity_bytes(inode)) {
        return SVFS_ERR_NO_SPACE;
    }
    uint32_t pos = 0;
    for (uint32_t e = 0; e < inode->extent_count && e < SVFS_MAX_EXTENTS; ++e) {
        const struct svfs_extent* extent = &inode->extents[e];
        for (uint32_t s = 0; s < extent->sector_count; ++s) {
            uint8_t sector[SVFS_SECTOR_SIZE];
            memset(sector, 0, sizeof(sector));
            if (pos < len) {
                uint32_t n = len - pos;
                if (n > SVFS_SECTOR_SIZE) {
                    n = SVFS_SECTOR_SIZE;
                }
                memcpy(sector, data + pos, n);
                pos += n;
            }
            if (ctx->write(ctx->cookie, extent->start_lba + s, 1, sector) != 0) {
                return SVFS_ERR_IO;
            }
        }
    }
    inode->size = len;
    return SVFS_OK;
}

/* --- Allocacion ---------------------------------------------------------- */

static uint32_t allocate_inode(struct svfs_ctx* ctx) {
    for (uint32_t id = 1; id <= SVFS_MAX_INODES; ++id) {
        if (!svfs_bitmap_test(ctx->inode_bitmap, id - 1)) {
            svfs_bitmap_set(ctx->inode_bitmap, id - 1, 1);
            return id;
        }
    }
    return 0;
}

/* First-fit sobre la region de datos: delega en el helper del formato
 * compartido (misma logica que usa el driver del kernel). */
static uint32_t find_free_run(struct svfs_ctx* ctx, uint32_t required) {
    return svfs_find_free_run(ctx->block_bitmap, SVFS_DATA_LBA, ctx->total_sectors, required);
}

static void set_extent_bits(struct svfs_ctx* ctx, uint32_t start_lba,
                            uint32_t sector_count, int value) {
    for (uint32_t s = 0; s < sector_count; ++s) {
        svfs_bitmap_set(ctx->block_bitmap, start_lba + s, value);
    }
}

/* Asegura que el inodo tenga capacidad para `required_size` bytes. Si hace
 * falta crecer, reubica el contenido existente a una unica corrida contigua
 * nueva (modelo de extent unico), preservando los bytes previos. */
static int ensure_capacity(struct svfs_ctx* ctx, uint32_t inode_id, uint32_t required_size) {
    struct svfs_inode* inode = inode_at(ctx, inode_id);
    if (inode == NULL) {
        return SVFS_ERR_INVALID;
    }
    if (required_size <= inode_capacity_bytes(inode)) {
        return SVFS_OK;
    }

    uint32_t required_sectors = sectors_for(required_size);

    uint8_t preserve[SVFS_PRESERVE_CAPACITY];
    if (inode->size > sizeof(preserve)) {
        return SVFS_ERR_NO_SPACE;
    }
    uint32_t preserved = 0;
    int rc = read_inode_bytes(ctx, inode, preserve, sizeof(preserve), &preserved);
    if (rc != SVFS_OK) {
        return rc;
    }

    uint32_t start = find_free_run(ctx, required_sectors);
    if (start == 0) {
        return SVFS_ERR_NO_SPACE;
    }

    for (uint32_t e = 0; e < inode->extent_count && e < SVFS_MAX_EXTENTS; ++e) {
        set_extent_bits(ctx, inode->extents[e].start_lba, inode->extents[e].sector_count, 0);
    }
    set_extent_bits(ctx, start, required_sectors, 1);

    inode->extent_count = 1;
    inode->extents[0].start_lba = start;
    inode->extents[0].sector_count = required_sectors;
    for (uint32_t e = 1; e < SVFS_MAX_EXTENTS; ++e) {
        inode->extents[e].start_lba = 0;
        inode->extents[e].sector_count = 0;
    }

    /* size se mantiene en el valor previo; write_inode_bytes lo re-fija al
     * preservado, y el caller lo actualiza al escribir el contenido nuevo. */
    return write_inode_bytes(ctx, inode_id, preserve, preserved);
}

/* --- Directorios y resolucion de rutas ----------------------------------- */

static int read_dir_entries(struct svfs_ctx* ctx, uint32_t dir_id,
                            struct svfs_dir_entry* entries, uint32_t* out_count) {
    struct svfs_inode* inode = inode_at(ctx, dir_id);
    if (inode == NULL) {
        return SVFS_ERR_INVALID;
    }
    uint32_t count = inode->size / SVFS_DIR_ENTRY_SIZE;
    if (count > SVFS_MAX_RECORDS) {
        count = SVFS_MAX_RECORDS;
    }
    uint32_t got = 0;
    int rc = read_inode_bytes(ctx, inode, (uint8_t*)entries,
                              count * SVFS_DIR_ENTRY_SIZE, &got);
    if (rc != SVFS_OK) {
        return rc;
    }
    *out_count = got / SVFS_DIR_ENTRY_SIZE;
    return SVFS_OK;
}

static int name_matches(const struct svfs_dir_entry* entry, const char* name, size_t name_len) {
    return entry->inode_id != 0 && entry->name_length == name_len &&
        memcmp(entry->name, name, name_len) == 0;
}

/* Resuelve una ruta relativa (separada por '/', sin barras iniciales) a un
 * inode id. La cadena vacia resuelve a la raiz. */
static int resolve_path(struct svfs_ctx* ctx, const char* relpath,
                        uint32_t* out_inode, uint16_t* out_type) {
    uint32_t current = SVFS_ROOT_INODE;
    uint16_t type = SVFS_INODE_DIRECTORY;

    const char* p = relpath;
    while (*p != '\0') {
        const char* slash = p;
        while (*slash != '\0' && *slash != '/') {
            ++slash;
        }
        size_t comp_len = (size_t)(slash - p);
        if (comp_len == 0) {
            return SVFS_ERR_INVALID;
        }

        struct svfs_inode* dir = inode_at(ctx, current);
        if (dir == NULL || dir->type != SVFS_INODE_DIRECTORY) {
            return SVFS_ERR_NOT_FOUND;
        }

        struct svfs_dir_entry entries[SVFS_MAX_RECORDS];
        uint32_t count = 0;
        int rc = read_dir_entries(ctx, current, entries, &count);
        if (rc != SVFS_OK) {
            return rc;
        }

        uint32_t found = 0;
        uint16_t found_type = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (name_matches(&entries[i], p, comp_len)) {
                found = entries[i].inode_id;
                found_type = entries[i].type;
                break;
            }
        }
        if (found == 0) {
            return SVFS_ERR_NOT_FOUND;
        }
        current = found;
        type = found_type;

        p = (*slash == '/') ? slash + 1 : slash;
    }

    if (out_inode) {
        *out_inode = current;
    }
    if (out_type) {
        *out_type = type;
    }
    return SVFS_OK;
}

/* Agrega una entrada (child) al directorio parent, haciendolo crecer si hace
 * falta. */
static int add_dir_entry(struct svfs_ctx* ctx, uint32_t parent_id, uint32_t child_id,
                         uint16_t type, const char* name, size_t name_len) {
    struct svfs_dir_entry entries[SVFS_MAX_RECORDS];
    uint32_t count = 0;
    int rc = read_dir_entries(ctx, parent_id, entries, &count);
    if (rc != SVFS_OK) {
        return rc;
    }
    if (count >= SVFS_MAX_RECORDS) {
        return SVFS_ERR_NO_SPACE;
    }

    struct svfs_dir_entry* slot = &entries[count];
    memset(slot, 0, sizeof(*slot));
    slot->inode_id = child_id;
    slot->type = type;
    slot->name_length = (uint16_t)name_len;
    memcpy(slot->name, name, name_len);
    count += 1;

    rc = ensure_capacity(ctx, parent_id, count * SVFS_DIR_ENTRY_SIZE);
    if (rc != SVFS_OK) {
        return rc;
    }
    return write_inode_bytes(ctx, parent_id, (const uint8_t*)entries, count * SVFS_DIR_ENTRY_SIZE);
}

static void init_inode(struct svfs_ctx* ctx, uint32_t inode_id, uint16_t type) {
    struct svfs_inode* inode = inode_at(ctx, inode_id);
    memset(inode, 0, sizeof(*inode));
    inode->inode_id = inode_id;
    inode->type = type;
    inode->link_count = 1;
}

/* Crea un unico componente `name` (len) dentro de parent_id, del tipo dado, y
 * devuelve su inode id. */
static int create_child(struct svfs_ctx* ctx, uint32_t parent_id, const char* name,
                        size_t name_len, uint16_t type, uint32_t* out_inode) {
    if (name_len == 0 || name_len > SVFS_INODE_NAME_CAPACITY - 1) {
        return SVFS_ERR_TOO_LONG;
    }
    uint32_t child = allocate_inode(ctx);
    if (child == 0) {
        return SVFS_ERR_NO_INODES;
    }
    init_inode(ctx, child, type);
    int rc = add_dir_entry(ctx, parent_id, child, type, name, name_len);
    if (rc != SVFS_OK) {
        return rc;
    }
    if (out_inode) {
        *out_inode = child;
    }
    return SVFS_OK;
}

/* --- API publica --------------------------------------------------------- */

void svfs_ctx_init(struct svfs_ctx* ctx, void* cookie, svfs_read_fn read, svfs_write_fn write) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->cookie = cookie;
    ctx->read = read;
    ctx->write = write;
}

int svfs_format(struct svfs_ctx* ctx, uint32_t total_sectors) {
    if (total_sectors <= SVFS_DATA_LBA) {
        return SVFS_ERR_INVALID;
    }
    ctx->total_sectors = total_sectors;
    memset(ctx->block_bitmap, 0, sizeof(ctx->block_bitmap));
    memset(ctx->inode_bitmap, 0, sizeof(ctx->inode_bitmap));
    memset(ctx->inodes, 0, sizeof(ctx->inodes));

    struct svfs_superblock* sb = &ctx->superblock;
    memset(sb, 0, sizeof(*sb));
    memcpy(sb->magic, svfs_superblock_magic, sizeof(sb->magic));
    sb->version = SVFS_VERSION;
    sb->total_sectors = total_sectors;
    sb->journal_lba = SVFS_JOURNAL_LBA;
    sb->journal_sectors = SVFS_JOURNAL_SECTORS;
    sb->block_bitmap_lba = SVFS_BLOCK_BITMAP_LBA;
    sb->block_bitmap_sectors = SVFS_BLOCK_BITMAP_SECTORS;
    sb->inode_bitmap_lba = SVFS_INODE_BITMAP_LBA;
    sb->inode_bitmap_sectors = SVFS_INODE_BITMAP_SECTORS;
    sb->inode_table_lba = SVFS_INODE_TABLE_LBA;
    sb->inode_table_sectors = SVFS_INODE_TABLE_SECTORS;
    sb->data_lba = SVFS_DATA_LBA;
    sb->max_inodes = SVFS_MAX_INODES;
    sb->root_inode = SVFS_ROOT_INODE;

    /* La region de metadata (todo lo anterior a los datos) queda ocupada. */
    for (uint32_t sector = 0; sector < SVFS_DATA_LBA; ++sector) {
        svfs_bitmap_set(ctx->block_bitmap, sector, 1);
    }
    /* Inodo raiz: id 1, directorio vacio. */
    svfs_bitmap_set(ctx->inode_bitmap, SVFS_ROOT_INODE - 1, 1);
    init_inode(ctx, SVFS_ROOT_INODE, SVFS_INODE_DIRECTORY);
    return SVFS_OK;
}

int svfs_open(struct svfs_ctx* ctx) {
    struct svfs_superblock primary;
    struct svfs_superblock secondary;
    /* svfs_superblock_valid (formato compartido) valida layout + checksum. */
    int have_primary = (ctx->read(ctx->cookie, SVFS_PRIMARY_SB_LBA, 1, &primary) == 0) &&
        svfs_superblock_valid(&primary);
    int have_secondary = (ctx->read(ctx->cookie, SVFS_SECONDARY_SB_LBA, 1, &secondary) == 0) &&
        svfs_superblock_valid(&secondary);

    const struct svfs_superblock* chosen = NULL;
    if (have_primary && have_secondary) {
        chosen = (secondary.sequence > primary.sequence) ? &secondary : &primary;
    } else if (have_primary) {
        chosen = &primary;
    } else if (have_secondary) {
        chosen = &secondary;
    } else {
        return SVFS_ERR_INVALID;
    }

    ctx->superblock = *chosen;
    ctx->total_sectors = chosen->total_sectors;
    if (ctx->read(ctx->cookie, SVFS_BLOCK_BITMAP_LBA, SVFS_BLOCK_BITMAP_SECTORS, ctx->block_bitmap) != 0 ||
        ctx->read(ctx->cookie, SVFS_INODE_BITMAP_LBA, SVFS_INODE_BITMAP_SECTORS, ctx->inode_bitmap) != 0 ||
        ctx->read(ctx->cookie, SVFS_INODE_TABLE_LBA, SVFS_INODE_TABLE_SECTORS, ctx->inodes) != 0) {
        return SVFS_ERR_IO;
    }
    return SVFS_OK;
}

int svfs_flush(struct svfs_ctx* ctx) {
    struct svfs_superblock* sb = &ctx->superblock;
    sb->sequence = 1;
    sb->flags = SVFS_FLAG_CLEAN;
    sb->checksum = 0;
    sb->checksum = svfs_checksum(sb, sizeof(*sb));

    if (ctx->write(ctx->cookie, SVFS_PRIMARY_SB_LBA, 1, sb) != 0 ||
        ctx->write(ctx->cookie, SVFS_SECONDARY_SB_LBA, 1, sb) != 0) {
        return SVFS_ERR_IO;
    }
    if (ctx->write(ctx->cookie, SVFS_BLOCK_BITMAP_LBA, SVFS_BLOCK_BITMAP_SECTORS, ctx->block_bitmap) != 0) {
        return SVFS_ERR_IO;
    }
    if (ctx->write(ctx->cookie, SVFS_INODE_BITMAP_LBA, SVFS_INODE_BITMAP_SECTORS, ctx->inode_bitmap) != 0) {
        return SVFS_ERR_IO;
    }
    if (ctx->write(ctx->cookie, SVFS_INODE_TABLE_LBA, SVFS_INODE_TABLE_SECTORS, ctx->inodes) != 0) {
        return SVFS_ERR_IO;
    }
    return SVFS_OK;
}

int svfs_mkdir_p(struct svfs_ctx* ctx, const char* relpath) {
    if (relpath == NULL) {
        return SVFS_ERR_INVALID;
    }
    /* Recorre componentes de a uno, creando los que falten. */
    const char* p = relpath;
    uint32_t parent = SVFS_ROOT_INODE;
    while (*p != '\0') {
        const char* slash = p;
        while (*slash != '\0' && *slash != '/') {
            ++slash;
        }
        size_t comp_len = (size_t)(slash - p);
        if (comp_len == 0) {
            return SVFS_ERR_INVALID;
        }
        if (comp_len > SVFS_INODE_NAME_CAPACITY - 1) {
            return SVFS_ERR_TOO_LONG;
        }

        /* Busca el componente en el directorio padre actual. */
        struct svfs_dir_entry entries[SVFS_MAX_RECORDS];
        uint32_t count = 0;
        int rc = read_dir_entries(ctx, parent, entries, &count);
        if (rc != SVFS_OK) {
            return rc;
        }
        uint32_t found = 0;
        uint16_t found_type = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (name_matches(&entries[i], p, comp_len)) {
                found = entries[i].inode_id;
                found_type = entries[i].type;
                break;
            }
        }

        if (found != 0) {
            if (found_type != SVFS_INODE_DIRECTORY) {
                return SVFS_ERR_EXISTS;
            }
            parent = found;
        } else {
            uint32_t child = 0;
            rc = create_child(ctx, parent, p, comp_len, SVFS_INODE_DIRECTORY, &child);
            if (rc != SVFS_OK) {
                return rc;
            }
            parent = child;
        }

        p = (*slash == '/') ? slash + 1 : slash;
    }
    return SVFS_OK;
}

int svfs_write_file(struct svfs_ctx* ctx, const char* relpath, const void* data, uint32_t size) {
    if (relpath == NULL || *relpath == '\0') {
        return SVFS_ERR_INVALID;
    }

    /* Separa parent/leaf en la ultima '/'. */
    const char* leaf = relpath;
    for (const char* c = relpath; *c != '\0'; ++c) {
        if (*c == '/') {
            leaf = c + 1;
        }
    }
    size_t leaf_len = strlen(leaf);
    if (leaf_len == 0) {
        return SVFS_ERR_INVALID;
    }
    if (leaf_len > SVFS_INODE_NAME_CAPACITY - 1) {
        return SVFS_ERR_TOO_LONG;
    }

    uint32_t parent = SVFS_ROOT_INODE;
    if (leaf != relpath) {
        size_t parent_len = (size_t)(leaf - relpath - 1);
        char parent_path[256];
        if (parent_len >= sizeof(parent_path)) {
            return SVFS_ERR_TOO_LONG;
        }
        memcpy(parent_path, relpath, parent_len);
        parent_path[parent_len] = '\0';

        int rc = svfs_mkdir_p(ctx, parent_path);
        if (rc != SVFS_OK) {
            return rc;
        }
        uint16_t ptype = 0;
        rc = resolve_path(ctx, parent_path, &parent, &ptype);
        if (rc != SVFS_OK) {
            return rc;
        }
        if (ptype != SVFS_INODE_DIRECTORY) {
            return SVFS_ERR_EXISTS;
        }
    }

    /* Busca el leaf; si existe como dir, error; si no existe, crealo. */
    uint32_t file_inode = 0;
    uint16_t ftype = 0;
    int rc = resolve_path(ctx, relpath, &file_inode, &ftype);
    if (rc == SVFS_OK) {
        if (ftype == SVFS_INODE_DIRECTORY) {
            return SVFS_ERR_EXISTS;
        }
    } else if (rc == SVFS_ERR_NOT_FOUND) {
        rc = create_child(ctx, parent, leaf, leaf_len, SVFS_INODE_FILE, &file_inode);
        if (rc != SVFS_OK) {
            return rc;
        }
    } else {
        return rc;
    }

    rc = ensure_capacity(ctx, file_inode, size);
    if (rc != SVFS_OK) {
        return rc;
    }
    return write_inode_bytes(ctx, file_inode, (const uint8_t*)data, size);
}
