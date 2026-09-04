/*
 * sxfs_core.c -- Implementacion del core portable de SxFS.
 *
 * Porta fielmente la logica del instalador host-side historico
 * (tools/UserAppCommon.ps1): allocacion de inodos secuencial, allocacion de
 * bloques first-fit contigua, modelo de extent unico por inodo (cada grow
 * reubica a una sola corrida contigua) y directorios como arrays empaquetados
 * de sxfs_dir_entry. Mantener esa equivalencia es lo que hace que las imagenes
 * que produce este core sean montables por el kernel igual que las de antes.
 */
#include "sxfs_core.h"

#include <string.h>

static uint32_t sectors_for(uint32_t bytes) {
    return (bytes + (SXFS_SECTOR_SIZE - 1u)) / SXFS_SECTOR_SIZE;
}

static struct sxfs_inode* inode_at(struct sxfs_ctx* ctx, uint32_t inode_id) {
    if (inode_id == 0 || inode_id > SXFS_MAX_INODES) {
        return NULL;
    }
    return &ctx->inodes[inode_id - 1];
}

static uint32_t inode_capacity_bytes(const struct sxfs_inode* inode) {
    return sxfs_inode_capacity_sectors(inode) * SXFS_SECTOR_SIZE;
}

/* --- I/O de datos por extents (via callbacks) ---------------------------- */

static int read_inode_bytes(struct sxfs_ctx* ctx, const struct sxfs_inode* inode,
                            uint8_t* dst, uint32_t dst_capacity, uint32_t* out_len) {
    uint32_t want = inode->size;
    if (want > dst_capacity) {
        want = dst_capacity;
    }
    uint32_t pos = 0;
    for (uint32_t e = 0; e < inode->extent_count && pos < want; ++e) {
        const struct sxfs_extent* extent = &inode->extents[e];
        for (uint32_t s = 0; s < extent->sector_count && pos < want; ++s) {
            uint8_t sector[SXFS_SECTOR_SIZE];
            if (ctx->read(ctx->cookie, extent->start_lba + s, 1, sector) != 0) {
                return SXFS_ERR_IO;
            }
            uint32_t n = want - pos;
            if (n > SXFS_SECTOR_SIZE) {
                n = SXFS_SECTOR_SIZE;
            }
            memcpy(dst + pos, sector, n);
            pos += n;
        }
    }
    if (out_len) {
        *out_len = pos;
    }
    return SXFS_OK;
}

/* Escribe `len` bytes en los extents del inodo, zero-rellenando el resto de
 * cada extent, y fija inode->size = len (los extents no cambian). */
static int write_inode_bytes(struct sxfs_ctx* ctx, uint32_t inode_id,
                             const uint8_t* data, uint32_t len) {
    struct sxfs_inode* inode = inode_at(ctx, inode_id);
    if (inode == NULL) {
        return SXFS_ERR_INVALID;
    }
    if (len > inode_capacity_bytes(inode)) {
        return SXFS_ERR_NO_SPACE;
    }
    uint32_t pos = 0;
    for (uint32_t e = 0; e < inode->extent_count && e < SXFS_MAX_EXTENTS; ++e) {
        const struct sxfs_extent* extent = &inode->extents[e];
        for (uint32_t s = 0; s < extent->sector_count; ++s) {
            uint8_t sector[SXFS_SECTOR_SIZE];
            memset(sector, 0, sizeof(sector));
            if (pos < len) {
                uint32_t n = len - pos;
                if (n > SXFS_SECTOR_SIZE) {
                    n = SXFS_SECTOR_SIZE;
                }
                memcpy(sector, data + pos, n);
                pos += n;
            }
            if (ctx->write(ctx->cookie, extent->start_lba + s, 1, sector) != 0) {
                return SXFS_ERR_IO;
            }
        }
    }
    inode->size = len;
    return SXFS_OK;
}

/* --- Allocacion ---------------------------------------------------------- */

static uint32_t allocate_inode(struct sxfs_ctx* ctx) {
    for (uint32_t id = 1; id <= SXFS_MAX_INODES; ++id) {
        if (!sxfs_bitmap_test(ctx->inode_bitmap, id - 1)) {
            sxfs_bitmap_set(ctx->inode_bitmap, id - 1, 1);
            return id;
        }
    }
    return 0;
}

/* First-fit sobre la region de datos: delega en el helper del formato
 * compartido (misma logica que usa el driver del kernel). */
static uint32_t find_free_run(struct sxfs_ctx* ctx, uint32_t required) {
    return sxfs_find_free_run(ctx->block_bitmap, SXFS_DATA_LBA, ctx->total_sectors, required);
}

static void set_extent_bits(struct sxfs_ctx* ctx, uint32_t start_lba,
                            uint32_t sector_count, int value) {
    for (uint32_t s = 0; s < sector_count; ++s) {
        sxfs_bitmap_set(ctx->block_bitmap, start_lba + s, value);
    }
}

/* Marca (value=1) o libera (value=0) los bits de todos los extents del inodo. */
static void set_inode_extent_bits(struct sxfs_ctx* ctx, const struct sxfs_inode* inode, int value) {
    for (uint32_t e = 0; e < inode->extent_count && e < SXFS_MAX_EXTENTS; ++e) {
        set_extent_bits(ctx, inode->extents[e].start_lba, inode->extents[e].sector_count, value);
    }
}

/* Copia los primeros `sectors` sectores logicos del inodo a la corrida que
 * empieza en dst_lba, de a un sector (sin buffer del tamano del archivo: mover
 * un extent no debe tener tope de tamano). El caller garantiza que la corrida
 * destino es disjunta de los extents viejos o empieza en/antes del primero, asi
 * que copiar hacia adelante nunca pisa un sector fuente sin leer. */
static int copy_inode_sectors(struct sxfs_ctx* ctx, const struct sxfs_inode* inode,
                              uint32_t dst_lba, uint32_t sectors) {
    uint32_t copied = 0;
    for (uint32_t e = 0; e < inode->extent_count && e < SXFS_MAX_EXTENTS && copied < sectors; ++e) {
        const struct sxfs_extent* extent = &inode->extents[e];
        for (uint32_t s = 0; s < extent->sector_count && copied < sectors; ++s, ++copied) {
            uint8_t sector[SXFS_SECTOR_SIZE];
            if (ctx->read(ctx->cookie, extent->start_lba + s, 1, sector) != 0) {
                return SXFS_ERR_IO;
            }
            if (ctx->write(ctx->cookie, dst_lba + copied, 1, sector) != 0) {
                return SXFS_ERR_IO;
            }
        }
    }
    return SXFS_OK;
}

/* Asegura que el inodo tenga capacidad para `required_size` bytes. Si hace
 * falta crecer, reubica el contenido existente a una unica corrida contigua
 * nueva (modelo de extent unico), preservando los bytes previos. */
static int ensure_capacity(struct sxfs_ctx* ctx, uint32_t inode_id, uint32_t required_size) {
    struct sxfs_inode* inode = inode_at(ctx, inode_id);
    if (inode == NULL) {
        return SXFS_ERR_INVALID;
    }
    if (required_size <= inode_capacity_bytes(inode)) {
        return SXFS_OK;
    }

    const uint32_t required_sectors = sectors_for(required_size);
    const uint32_t old_sectors = sxfs_inode_capacity_sectors(inode);

    /* Con un solo extent (lo unico que este core produce) los bloques propios se
     * liberan ANTES de buscar, para que un archivo que crece pueda reusar su
     * propia corrida --extendiendola, o absorbiendo el hueco que la precede-- en
     * vez de mudarse siempre a sectores virgenes dejando el suyo como agujero.
     * Sin esto cada rebuild que agranda binarios fragmenta la imagen preservada
     * entre builds hasta que no queda corrida contigua con espacio libre de
     * sobra. Con mas de un extent (imagen ajena) se busca primero: eso garantiza
     * que la corrida nueva no solape ninguna vieja. */
    const int reuse_own_blocks = (inode->extent_count <= 1);
    if (reuse_own_blocks) {
        set_inode_extent_bits(ctx, inode, 0);
    }

    const uint32_t start = find_free_run(ctx, required_sectors);
    if (start == 0) {
        if (reuse_own_blocks) {
            set_inode_extent_bits(ctx, inode, 1); /* deja el bitmap como estaba */
        }
        return SXFS_ERR_NO_SPACE;
    }

    /* find_free_run devuelve la primera corrida libre suficientemente larga: o es
     * disjunta del extent viejo, o lo contiene empezando en/antes de su inicio
     * (los sectores previos al inicio de la corrida estan ocupados, y el extent
     * viejo entero quedo libre). Nunca empieza adentro del extent viejo, asi que
     * la copia hacia adelante es segura incluso solapando. */
    int rc = copy_inode_sectors(ctx, inode, start, old_sectors);
    if (rc != SXFS_OK) {
        return rc;
    }

    if (!reuse_own_blocks) {
        set_inode_extent_bits(ctx, inode, 0);
    }
    set_extent_bits(ctx, start, required_sectors, 1);

    inode->extent_count = 1;
    inode->extents[0].start_lba = start;
    inode->extents[0].sector_count = required_sectors;
    for (uint32_t e = 1; e < SXFS_MAX_EXTENTS; ++e) {
        inode->extents[e].start_lba = 0;
        inode->extents[e].sector_count = 0;
    }

    /* size se mantiene en el valor previo (el contenido copiado); el caller lo
     * actualiza al escribir el contenido nuevo con write_inode_bytes, que zero-
     * rellena los sectores restantes de la corrida nueva. */
    return SXFS_OK;
}

/* --- Directorios y resolucion de rutas ----------------------------------- */

static int read_dir_entries(struct sxfs_ctx* ctx, uint32_t dir_id,
                            struct sxfs_dir_entry* entries, uint32_t* out_count) {
    struct sxfs_inode* inode = inode_at(ctx, dir_id);
    if (inode == NULL) {
        return SXFS_ERR_INVALID;
    }
    uint32_t count = inode->size / SXFS_DIR_ENTRY_SIZE;
    if (count > SXFS_MAX_RECORDS) {
        count = SXFS_MAX_RECORDS;
    }
    uint32_t got = 0;
    int rc = read_inode_bytes(ctx, inode, (uint8_t*)entries,
                              count * SXFS_DIR_ENTRY_SIZE, &got);
    if (rc != SXFS_OK) {
        return rc;
    }
    *out_count = got / SXFS_DIR_ENTRY_SIZE;
    return SXFS_OK;
}

static int name_matches(const struct sxfs_dir_entry* entry, const char* name, size_t name_len) {
    return entry->inode_id != 0 && entry->name_length == name_len &&
        memcmp(entry->name, name, name_len) == 0;
}

/* Resuelve una ruta relativa (separada por '/', sin barras iniciales) a un
 * inode id. La cadena vacia resuelve a la raiz. */
static int resolve_path(struct sxfs_ctx* ctx, const char* relpath,
                        uint32_t* out_inode, uint16_t* out_type) {
    uint32_t current = SXFS_ROOT_INODE;
    uint16_t type = SXFS_INODE_DIRECTORY;

    const char* p = relpath;
    while (*p != '\0') {
        const char* slash = p;
        while (*slash != '\0' && *slash != '/') {
            ++slash;
        }
        size_t comp_len = (size_t)(slash - p);
        if (comp_len == 0) {
            return SXFS_ERR_INVALID;
        }

        struct sxfs_inode* dir = inode_at(ctx, current);
        if (dir == NULL || dir->type != SXFS_INODE_DIRECTORY) {
            return SXFS_ERR_NOT_FOUND;
        }

        struct sxfs_dir_entry entries[SXFS_MAX_RECORDS];
        uint32_t count = 0;
        int rc = read_dir_entries(ctx, current, entries, &count);
        if (rc != SXFS_OK) {
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
            return SXFS_ERR_NOT_FOUND;
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
    return SXFS_OK;
}

/* Agrega una entrada (child) al directorio parent, haciendolo crecer si hace
 * falta. */
static int add_dir_entry(struct sxfs_ctx* ctx, uint32_t parent_id, uint32_t child_id,
                         uint16_t type, const char* name, size_t name_len) {
    struct sxfs_dir_entry entries[SXFS_MAX_RECORDS];
    uint32_t count = 0;
    int rc = read_dir_entries(ctx, parent_id, entries, &count);
    if (rc != SXFS_OK) {
        return rc;
    }
    if (count >= SXFS_MAX_RECORDS) {
        return SXFS_ERR_NO_SPACE;
    }

    struct sxfs_dir_entry* slot = &entries[count];
    memset(slot, 0, sizeof(*slot));
    slot->inode_id = child_id;
    slot->type = type;
    slot->name_length = (uint16_t)name_len;
    memcpy(slot->name, name, name_len);
    count += 1;

    rc = ensure_capacity(ctx, parent_id, count * SXFS_DIR_ENTRY_SIZE);
    if (rc != SXFS_OK) {
        return rc;
    }
    return write_inode_bytes(ctx, parent_id, (const uint8_t*)entries, count * SXFS_DIR_ENTRY_SIZE);
}

static void init_inode(struct sxfs_ctx* ctx, uint32_t inode_id, uint16_t type) {
    struct sxfs_inode* inode = inode_at(ctx, inode_id);
    memset(inode, 0, sizeof(*inode));
    inode->inode_id = inode_id;
    inode->type = type;
    inode->link_count = 1;
}

/* Crea un unico componente `name` (len) dentro de parent_id, del tipo dado, y
 * devuelve su inode id. */
static int create_child(struct sxfs_ctx* ctx, uint32_t parent_id, const char* name,
                        size_t name_len, uint16_t type, uint32_t* out_inode) {
    if (name_len == 0 || name_len > SXFS_INODE_NAME_CAPACITY - 1) {
        return SXFS_ERR_TOO_LONG;
    }
    uint32_t child = allocate_inode(ctx);
    if (child == 0) {
        return SXFS_ERR_NO_INODES;
    }
    init_inode(ctx, child, type);
    int rc = add_dir_entry(ctx, parent_id, child, type, name, name_len);
    if (rc != SXFS_OK) {
        return rc;
    }
    if (out_inode) {
        *out_inode = child;
    }
    return SXFS_OK;
}

/* --- API publica --------------------------------------------------------- */

void sxfs_ctx_init(struct sxfs_ctx* ctx, void* cookie, sxfs_read_fn read, sxfs_write_fn write) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->cookie = cookie;
    ctx->read = read;
    ctx->write = write;
}

int sxfs_format(struct sxfs_ctx* ctx, uint32_t total_sectors) {
    if (total_sectors <= SXFS_DATA_LBA) {
        return SXFS_ERR_INVALID;
    }
    ctx->total_sectors = total_sectors;
    memset(ctx->block_bitmap, 0, sizeof(ctx->block_bitmap));
    memset(ctx->inode_bitmap, 0, sizeof(ctx->inode_bitmap));
    memset(ctx->inodes, 0, sizeof(ctx->inodes));

    struct sxfs_superblock* sb = &ctx->superblock;
    memset(sb, 0, sizeof(*sb));
    memcpy(sb->magic, sxfs_superblock_magic, sizeof(sb->magic));
    sb->version = SXFS_VERSION;
    sb->total_sectors = total_sectors;
    sb->journal_lba = SXFS_JOURNAL_LBA;
    sb->journal_sectors = SXFS_JOURNAL_SECTORS;
    sb->block_bitmap_lba = SXFS_BLOCK_BITMAP_LBA;
    sb->block_bitmap_sectors = SXFS_BLOCK_BITMAP_SECTORS;
    sb->inode_bitmap_lba = SXFS_INODE_BITMAP_LBA;
    sb->inode_bitmap_sectors = SXFS_INODE_BITMAP_SECTORS;
    sb->inode_table_lba = SXFS_INODE_TABLE_LBA;
    sb->inode_table_sectors = SXFS_INODE_TABLE_SECTORS;
    sb->data_lba = SXFS_DATA_LBA;
    sb->max_inodes = SXFS_MAX_INODES;
    sb->root_inode = SXFS_ROOT_INODE;

    /* La region de metadata (todo lo anterior a los datos) queda ocupada. */
    for (uint32_t sector = 0; sector < SXFS_DATA_LBA; ++sector) {
        sxfs_bitmap_set(ctx->block_bitmap, sector, 1);
    }
    /* Inodo raiz: id 1, directorio vacio. */
    sxfs_bitmap_set(ctx->inode_bitmap, SXFS_ROOT_INODE - 1, 1);
    init_inode(ctx, SXFS_ROOT_INODE, SXFS_INODE_DIRECTORY);
    return SXFS_OK;
}

int sxfs_open(struct sxfs_ctx* ctx) {
    struct sxfs_superblock primary;
    struct sxfs_superblock secondary;
    /* sxfs_superblock_valid (formato compartido) valida layout + checksum. */
    int have_primary = (ctx->read(ctx->cookie, SXFS_PRIMARY_SB_LBA, 1, &primary) == 0) &&
        sxfs_superblock_valid(&primary);
    int have_secondary = (ctx->read(ctx->cookie, SXFS_SECONDARY_SB_LBA, 1, &secondary) == 0) &&
        sxfs_superblock_valid(&secondary);

    const struct sxfs_superblock* chosen = NULL;
    if (have_primary && have_secondary) {
        chosen = (secondary.sequence > primary.sequence) ? &secondary : &primary;
    } else if (have_primary) {
        chosen = &primary;
    } else if (have_secondary) {
        chosen = &secondary;
    } else {
        return SXFS_ERR_INVALID;
    }

    ctx->superblock = *chosen;
    ctx->total_sectors = chosen->total_sectors;
    if (ctx->read(ctx->cookie, SXFS_BLOCK_BITMAP_LBA, SXFS_BLOCK_BITMAP_SECTORS, ctx->block_bitmap) != 0 ||
        ctx->read(ctx->cookie, SXFS_INODE_BITMAP_LBA, SXFS_INODE_BITMAP_SECTORS, ctx->inode_bitmap) != 0 ||
        ctx->read(ctx->cookie, SXFS_INODE_TABLE_LBA, SXFS_INODE_TABLE_SECTORS, ctx->inodes) != 0) {
        return SXFS_ERR_IO;
    }
    return SXFS_OK;
}

int sxfs_flush(struct sxfs_ctx* ctx) {
    struct sxfs_superblock* sb = &ctx->superblock;
    sb->sequence = 1;
    sb->flags = SXFS_FLAG_CLEAN;
    sb->checksum = 0;
    sb->checksum = sxfs_checksum(sb, sizeof(*sb));

    if (ctx->write(ctx->cookie, SXFS_PRIMARY_SB_LBA, 1, sb) != 0 ||
        ctx->write(ctx->cookie, SXFS_SECONDARY_SB_LBA, 1, sb) != 0) {
        return SXFS_ERR_IO;
    }
    if (ctx->write(ctx->cookie, SXFS_BLOCK_BITMAP_LBA, SXFS_BLOCK_BITMAP_SECTORS, ctx->block_bitmap) != 0) {
        return SXFS_ERR_IO;
    }
    if (ctx->write(ctx->cookie, SXFS_INODE_BITMAP_LBA, SXFS_INODE_BITMAP_SECTORS, ctx->inode_bitmap) != 0) {
        return SXFS_ERR_IO;
    }
    if (ctx->write(ctx->cookie, SXFS_INODE_TABLE_LBA, SXFS_INODE_TABLE_SECTORS, ctx->inodes) != 0) {
        return SXFS_ERR_IO;
    }
    return SXFS_OK;
}

int sxfs_mkdir_p(struct sxfs_ctx* ctx, const char* relpath) {
    if (relpath == NULL) {
        return SXFS_ERR_INVALID;
    }
    /* Recorre componentes de a uno, creando los que falten. */
    const char* p = relpath;
    uint32_t parent = SXFS_ROOT_INODE;
    while (*p != '\0') {
        const char* slash = p;
        while (*slash != '\0' && *slash != '/') {
            ++slash;
        }
        size_t comp_len = (size_t)(slash - p);
        if (comp_len == 0) {
            return SXFS_ERR_INVALID;
        }
        if (comp_len > SXFS_INODE_NAME_CAPACITY - 1) {
            return SXFS_ERR_TOO_LONG;
        }

        /* Busca el componente en el directorio padre actual. */
        struct sxfs_dir_entry entries[SXFS_MAX_RECORDS];
        uint32_t count = 0;
        int rc = read_dir_entries(ctx, parent, entries, &count);
        if (rc != SXFS_OK) {
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
            if (found_type != SXFS_INODE_DIRECTORY) {
                return SXFS_ERR_EXISTS;
            }
            parent = found;
        } else {
            uint32_t child = 0;
            rc = create_child(ctx, parent, p, comp_len, SXFS_INODE_DIRECTORY, &child);
            if (rc != SXFS_OK) {
                return rc;
            }
            parent = child;
        }

        p = (*slash == '/') ? slash + 1 : slash;
    }
    return SXFS_OK;
}

/* Un directorio esta vacio si no le queda ninguna entrada viva. */
static int dir_is_empty(struct sxfs_ctx* ctx, uint32_t dir_id, int* out_empty) {
    struct sxfs_dir_entry entries[SXFS_MAX_RECORDS];
    uint32_t count = 0;
    int rc = read_dir_entries(ctx, dir_id, entries, &count);
    if (rc != SXFS_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (entries[i].inode_id != 0) {
            *out_empty = 0;
            return SXFS_OK;
        }
    }
    *out_empty = 1;
    return SXFS_OK;
}

int sxfs_remove(struct sxfs_ctx* ctx, const char* relpath) {
    if (relpath == NULL || *relpath == '\0') {
        /* La raiz no se borra. */
        return SXFS_ERR_INVALID;
    }

    /* Separa parent/leaf en la ultima '/', igual que sxfs_write_file. */
    const char* leaf = relpath;
    for (const char* c = relpath; *c != '\0'; ++c) {
        if (*c == '/') {
            leaf = c + 1;
        }
    }
    size_t leaf_len = strlen(leaf);
    if (leaf_len == 0) {
        return SXFS_ERR_INVALID;
    }

    uint32_t parent = SXFS_ROOT_INODE;
    if (leaf != relpath) {
        size_t parent_len = (size_t)(leaf - relpath - 1);
        char parent_path[256];
        if (parent_len >= sizeof(parent_path)) {
            return SXFS_ERR_TOO_LONG;
        }
        memcpy(parent_path, relpath, parent_len);
        parent_path[parent_len] = '\0';

        uint16_t ptype = 0;
        int prc = resolve_path(ctx, parent_path, &parent, &ptype);
        if (prc != SXFS_OK) {
            return prc;
        }
        if (ptype != SXFS_INODE_DIRECTORY) {
            return SXFS_ERR_NOT_FOUND;
        }
    }

    struct sxfs_dir_entry entries[SXFS_MAX_RECORDS];
    uint32_t count = 0;
    int rc = read_dir_entries(ctx, parent, entries, &count);
    if (rc != SXFS_OK) {
        return rc;
    }

    uint32_t slot = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (name_matches(&entries[i], leaf, leaf_len)) {
            slot = i;
            break;
        }
    }
    if (slot == count) {
        return SXFS_ERR_NOT_FOUND;
    }

    const uint32_t victim_id = entries[slot].inode_id;
    struct sxfs_inode* victim = inode_at(ctx, victim_id);
    if (victim == NULL) {
        return SXFS_ERR_INVALID;
    }

    if (victim->type == SXFS_INODE_DIRECTORY) {
        int empty = 0;
        rc = dir_is_empty(ctx, victim_id, &empty);
        if (rc != SXFS_OK) {
            return rc;
        }
        if (!empty) {
            return SXFS_ERR_EXISTS;
        }
    }

    /* Saca la entrada compactando el array y reescribe el directorio con un
     * registro menos. write_inode_bytes cerea el resto de los sectores del
     * extent, asi que la entrada sobrante no queda de fantasma. La capacidad
     * del directorio no se achica: igual que en el resto del core, los extents
     * solo crecen. */
    for (uint32_t i = slot + 1; i < count; ++i) {
        entries[i - 1] = entries[i];
    }
    count -= 1;
    rc = write_inode_bytes(ctx, parent, (const uint8_t*)entries, count * SXFS_DIR_ENTRY_SIZE);
    if (rc != SXFS_OK) {
        return rc;
    }

    /* Recien con el padre ya reescrito se liberan los recursos: si el rewrite
     * hubiera fallado, el inodo seguiria referenciado y no habria ni fuga de
     * bloques ni una entrada apuntando a un inodo liberado. */
    set_inode_extent_bits(ctx, victim, 0);
    sxfs_bitmap_set(ctx->inode_bitmap, victim_id - 1, 0);
    memset(victim, 0, sizeof(*victim));
    return SXFS_OK;
}

int sxfs_write_file(struct sxfs_ctx* ctx, const char* relpath, const void* data, uint32_t size) {
    if (relpath == NULL || *relpath == '\0') {
        return SXFS_ERR_INVALID;
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
        return SXFS_ERR_INVALID;
    }
    if (leaf_len > SXFS_INODE_NAME_CAPACITY - 1) {
        return SXFS_ERR_TOO_LONG;
    }

    uint32_t parent = SXFS_ROOT_INODE;
    if (leaf != relpath) {
        size_t parent_len = (size_t)(leaf - relpath - 1);
        char parent_path[256];
        if (parent_len >= sizeof(parent_path)) {
            return SXFS_ERR_TOO_LONG;
        }
        memcpy(parent_path, relpath, parent_len);
        parent_path[parent_len] = '\0';

        int rc = sxfs_mkdir_p(ctx, parent_path);
        if (rc != SXFS_OK) {
            return rc;
        }
        uint16_t ptype = 0;
        rc = resolve_path(ctx, parent_path, &parent, &ptype);
        if (rc != SXFS_OK) {
            return rc;
        }
        if (ptype != SXFS_INODE_DIRECTORY) {
            return SXFS_ERR_EXISTS;
        }
    }

    /* Busca el leaf; si existe como dir, error; si no existe, crealo. */
    uint32_t file_inode = 0;
    uint16_t ftype = 0;
    int rc = resolve_path(ctx, relpath, &file_inode, &ftype);
    if (rc == SXFS_OK) {
        if (ftype == SXFS_INODE_DIRECTORY) {
            return SXFS_ERR_EXISTS;
        }
    } else if (rc == SXFS_ERR_NOT_FOUND) {
        rc = create_child(ctx, parent, leaf, leaf_len, SXFS_INODE_FILE, &file_inode);
        if (rc != SXFS_OK) {
            return rc;
        }
    } else {
        return rc;
    }

    rc = ensure_capacity(ctx, file_inode, size);
    if (rc != SXFS_OK) {
        return rc;
    }
    return write_inode_bytes(ctx, file_inode, (const uint8_t*)data, size);
}
