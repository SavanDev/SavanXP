/*
 * Lector de recursos SXE. Ver savanxp/sxe.h para la API y docs/SXE_FORMAT.md
 * para el diseno.
 */

#include "savanxp/libc.h"
#include "savanxp/sxe.h"

/*
 * El valor de SXE_ICON_FORMAT_BGRA8888 esta elegido para coincidir con
 * SX_PIXEL_FORMAT_BGRA8888: es lo que permite pasarle los pixeles de un
 * .sxicon a un struct sx_bitmap sin traducir nada. Si alguien renumera los
 * formatos de gfx2d, esto tiene que romper el build en vez de degradarse a
 * colores dados vuelta en tiempo de ejecucion.
 */
_Static_assert((int)SXE_ICON_FORMAT_BGRA8888 == (int)SX_PIXEL_FORMAT_BGRA8888,
    "SXE_ICON_FORMAT_BGRA8888 debe seguir siendo SX_PIXEL_FORMAT_BGRA8888");

/* --- Helpers ------------------------------------------------------------- */

static uint16_t sxe_load_u16(const uint8_t* bytes)
{
    uint16_t value = 0;

    memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint32_t sxe_load_u32(const uint8_t* bytes)
{
    uint32_t value = 0;

    memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint64_t sxe_load_u64(const uint8_t* bytes)
{
    uint64_t value = 0;

    memcpy(&value, bytes, sizeof(value));
    return value;
}

static int sxe_bytes_equal(const void* left, const void* right, size_t count)
{
    const uint8_t* a = (const uint8_t*)left;
    const uint8_t* b = (const uint8_t*)right;
    size_t index = 0;

    for (index = 0; index < count; ++index)
    {
        if (a[index] != b[index])
        {
            return 0;
        }
    }
    return 1;
}

/* Redondeo a multiplo de SXE_RECORD_ALIGNMENT, saturando en vez de dar la
 * vuelta: un length cerca de UINT32_MAX no debe producir un padded chiquito. */
static uint32_t sxe_pad4(uint32_t value)
{
    if (value > 0xfffffffcu)
    {
        return 0xffffffffu;
    }
    return (value + (SXE_RECORD_ALIGNMENT - 1u)) & ~(SXE_RECORD_ALIGNMENT - 1u);
}

static int sxe_is_aligned4(const void* pointer)
{
    return ((uintptr_t)pointer & 3u) == 0u;
}

const char* sxe_result_string(int result)
{
    switch (result)
    {
    case SXE_OK:
        return "ok";
    case SXE_ABSENT:
        return "sin recursos";
    case SXE_BAD_MAGIC:
        return "magia invalida";
    case SXE_BAD_VERSION:
        return "version no soportada";
    case SXE_TRUNCATED:
        return "blob truncado";
    case SXE_TOO_LARGE:
        return "blob demasiado grande";
    case SXE_MALFORMED:
        return "blob malformado";
    case SXE_REQUIRED_UNKNOWN:
        return "registro requerido desconocido";
    default:
        return "resultado desconocido";
    }
}

int sxe_tag_is_known(uint16_t tag)
{
    switch (tag)
    {
    case SXE_TAG_NAME:
    case SXE_TAG_DESCRIPTION:
    case SXE_TAG_VERSION:
    case SXE_TAG_VERSION_STRING:
    case SXE_TAG_VENDOR:
    case SXE_TAG_COPYRIGHT:
    case SXE_TAG_BUILD_ID:
    case SXE_TAG_ACCENT:
    case SXE_TAG_LAUNCH_FLAGS:
    case SXE_TAG_INTERPRETER:
    case SXE_TAG_SUBSYSTEM:
    case SXE_TAG_MIME_OPEN:
    case SXE_TAG_EXT_OPEN:
        return 1;
    default:
        return 0;
    }
}

/* --- .sxmeta ------------------------------------------------------------- */

int sxe_meta_open(const void* blob, size_t length, struct sxe_meta* out)
{
    const uint8_t* bytes = (const uint8_t*)blob;
    struct sxe_meta_header header;
    uint32_t offset = 0;
    uint32_t seen = 0;

    if (out != 0)
    {
        memset(out, 0, sizeof(*out));
    }
    if (out == 0)
    {
        return SXE_MALFORMED;
    }
    if (bytes == 0 || length == 0)
    {
        return SXE_ABSENT;
    }
    /* Los registros se recorren en sitio y algunos payloads son enteros de 4
     * bytes: un buffer desalineado se rechaza fuerte en vez de leerse mal. */
    if (!sxe_is_aligned4(bytes))
    {
        return SXE_MALFORMED;
    }
    if (length < sizeof(header))
    {
        return SXE_TRUNCATED;
    }

    memcpy(&header, bytes, sizeof(header));
    if (header.magic[0] != SXE_META_MAGIC0 || header.magic[1] != SXE_META_MAGIC1 ||
        header.magic[2] != SXE_META_MAGIC2 || header.magic[3] != SXE_META_MAGIC3)
    {
        return SXE_BAD_MAGIC;
    }
    if (header.version == 0u || header.version > SXE_META_VERSION)
    {
        return SXE_BAD_VERSION;
    }
    if (header.header_bytes < sizeof(header) || (header.header_bytes % SXE_RECORD_ALIGNMENT) != 0u)
    {
        return SXE_MALFORMED;
    }
    if (header.blob_bytes > SXE_META_MAX_BYTES)
    {
        return SXE_TOO_LARGE;
    }
    if (header.blob_bytes > length)
    {
        return SXE_TRUNCATED;
    }
    if (header.blob_bytes < header.header_bytes || (header.blob_bytes % SXE_RECORD_ALIGNMENT) != 0u)
    {
        return SXE_MALFORMED;
    }

    /*
     * Walk completo por adelantado. Es lo que le permite a los accessors ser
     * bucles pelados: si esto pasa, ya no hay forma de que se salgan del blob.
     */
    offset = header.header_bytes;
    while (offset < header.blob_bytes)
    {
        struct sxe_record record;
        uint32_t payload_offset = 0;
        uint32_t padded = 0;

        if (header.blob_bytes - offset < sizeof(record))
        {
            return SXE_TRUNCATED;
        }
        memcpy(&record, bytes + offset, sizeof(record));
        payload_offset = offset + (uint32_t)sizeof(record);

        if (header.blob_bytes - payload_offset < record.length)
        {
            return SXE_TRUNCATED;
        }
        if ((record.flags & SXE_RECORD_REQUIRED) != 0u && !sxe_tag_is_known(record.tag))
        {
            return SXE_REQUIRED_UNKNOWN;
        }

        /* El padding tiene que estar presente, tambien en el ultimo registro:
         * por eso blob_bytes es siempre multiplo de 4 y el walk cierra justo. */
        padded = sxe_pad4(record.length);
        if (padded < record.length || header.blob_bytes - payload_offset < padded)
        {
            return SXE_TRUNCATED;
        }

        offset = payload_offset + padded;
        seen += 1u;
    }

    if (offset != header.blob_bytes)
    {
        return SXE_MALFORMED;
    }
    if (seen != header.record_count)
    {
        return SXE_MALFORMED;
    }

    out->records = bytes + header.header_bytes;
    out->records_bytes = header.blob_bytes - header.header_bytes;
    out->record_count = header.record_count;
    out->version = header.version;
    return SXE_OK;
}

int sxe_meta_find(const struct sxe_meta* meta, uint16_t tag, const void** payload, uint32_t* length)
{
    uint32_t offset = 0;

    if (meta == 0 || meta->records == 0)
    {
        return 0;
    }

    while (offset + sizeof(struct sxe_record) <= meta->records_bytes)
    {
        struct sxe_record record;

        memcpy(&record, meta->records + offset, sizeof(record));
        offset += (uint32_t)sizeof(record);

        if (record.tag == tag)
        {
            if (payload != 0)
            {
                *payload = meta->records + offset;
            }
            if (length != 0)
            {
                *length = record.length;
            }
            return 1;
        }
        offset += sxe_pad4(record.length);
    }
    return 0;
}

size_t sxe_meta_string(const struct sxe_meta* meta, uint16_t tag, char* out, size_t capacity)
{
    const void* payload = 0;
    uint32_t length = 0;
    const char* text = 0;
    size_t copied = 0;

    if (out == 0 || capacity == 0)
    {
        return 0;
    }
    out[0] = '\0';
    if (!sxe_meta_find(meta, tag, &payload, &length))
    {
        return 0;
    }

    text = (const char*)payload;
    while (copied < length && copied + 1u < capacity && text[copied] != '\0')
    {
        out[copied] = text[copied];
        copied += 1u;
    }
    out[copied] = '\0';
    return copied;
}

int sxe_meta_u32(const struct sxe_meta* meta, uint16_t tag, uint32_t* out)
{
    const void* payload = 0;
    uint32_t length = 0;

    if (out == 0 || !sxe_meta_find(meta, tag, &payload, &length) || length != sizeof(uint32_t))
    {
        return 0;
    }
    *out = sxe_load_u32((const uint8_t*)payload);
    return 1;
}

int sxe_meta_u8(const struct sxe_meta* meta, uint16_t tag, uint8_t* out)
{
    const void* payload = 0;
    uint32_t length = 0;

    if (out == 0 || !sxe_meta_find(meta, tag, &payload, &length) || length != sizeof(uint8_t))
    {
        return 0;
    }
    *out = *(const uint8_t*)payload;
    return 1;
}

int sxe_meta_version(const struct sxe_meta* meta, uint16_t out[SXE_VERSION_COMPONENTS])
{
    const void* payload = 0;
    uint32_t length = 0;
    uint32_t index = 0;

    if (out == 0 || !sxe_meta_find(meta, SXE_TAG_VERSION, &payload, &length) || length != SXE_VERSION_BYTES)
    {
        return 0;
    }
    for (index = 0; index < SXE_VERSION_COMPONENTS; ++index)
    {
        out[index] = sxe_load_u16((const uint8_t*)payload + (index * sizeof(uint16_t)));
    }
    return 1;
}

/* Recorre una lista separada por NUL. Devuelve el inicio de la entrada
 * `index`-esima no vacia y su largo, o 0 si no llega. */
static const char* sxe_list_entry(const struct sxe_meta* meta, uint16_t tag, int index, size_t* out_length)
{
    const void* payload = 0;
    uint32_t length = 0;
    const char* text = 0;
    uint32_t cursor = 0;
    int seen = 0;

    if (index < 0 || !sxe_meta_find(meta, tag, &payload, &length))
    {
        return 0;
    }
    text = (const char*)payload;

    while (cursor < length)
    {
        uint32_t start = cursor;

        while (cursor < length && text[cursor] != '\0')
        {
            cursor += 1u;
        }
        if (cursor > start)
        {
            if (seen == index)
            {
                if (out_length != 0)
                {
                    *out_length = (size_t)(cursor - start);
                }
                return text + start;
            }
            seen += 1;
        }
        cursor += 1u; /* saltea el NUL separador */
    }
    return 0;
}

int sxe_meta_list_count(const struct sxe_meta* meta, uint16_t tag)
{
    int count = 0;

    while (sxe_list_entry(meta, tag, count, 0) != 0)
    {
        count += 1;
    }
    return count;
}

size_t sxe_meta_list_at(const struct sxe_meta* meta, uint16_t tag, int index, char* out, size_t capacity)
{
    const char* entry = 0;
    size_t length = 0;
    size_t copied = 0;

    if (out == 0 || capacity == 0)
    {
        return 0;
    }
    out[0] = '\0';

    entry = sxe_list_entry(meta, tag, index, &length);
    if (entry == 0)
    {
        return 0;
    }
    while (copied < length && copied + 1u < capacity)
    {
        out[copied] = entry[copied];
        copied += 1u;
    }
    out[copied] = '\0';
    return copied;
}

/* --- .sxicon ------------------------------------------------------------- */

int sxe_icons_open(const void* blob, size_t length, struct sxe_icons* out)
{
    const uint8_t* bytes = (const uint8_t*)blob;
    struct sxe_icon_header header;
    uint32_t entries_bytes = 0;
    uint32_t entries_end = 0;
    uint32_t index = 0;

    if (out == 0)
    {
        return SXE_MALFORMED;
    }
    memset(out, 0, sizeof(*out));

    if (bytes == 0 || length == 0)
    {
        return SXE_ABSENT;
    }
    /* Se devuelven punteros a struct sxe_icon_entry y a pixeles uint32 que
     * apuntan adentro del buffer: sin esto serian accesos desalineados. */
    if (!sxe_is_aligned4(bytes))
    {
        return SXE_MALFORMED;
    }
    if (length < sizeof(header))
    {
        return SXE_TRUNCATED;
    }

    memcpy(&header, bytes, sizeof(header));
    if (header.magic[0] != SXE_ICON_MAGIC0 || header.magic[1] != SXE_ICON_MAGIC1 ||
        header.magic[2] != SXE_ICON_MAGIC2 || header.magic[3] != SXE_ICON_MAGIC3)
    {
        return SXE_BAD_MAGIC;
    }
    if (header.version == 0u || header.version > SXE_ICON_VERSION)
    {
        return SXE_BAD_VERSION;
    }
    if (header.header_bytes < sizeof(header) || (header.header_bytes % SXE_RECORD_ALIGNMENT) != 0u)
    {
        return SXE_MALFORMED;
    }
    if (header.blob_bytes > SXE_ICON_MAX_BYTES)
    {
        return SXE_TOO_LARGE;
    }
    if (header.blob_bytes > length)
    {
        return SXE_TRUNCATED;
    }
    if (header.blob_bytes < header.header_bytes)
    {
        return SXE_MALFORMED;
    }
    if (header.image_count > SXE_ICON_MAX_IMAGES)
    {
        return SXE_MALFORMED;
    }

    entries_bytes = header.image_count * (uint32_t)sizeof(struct sxe_icon_entry);
    if (header.blob_bytes - header.header_bytes < entries_bytes)
    {
        return SXE_TRUNCATED;
    }
    entries_end = header.header_bytes + entries_bytes;

    for (index = 0; index < header.image_count; ++index)
    {
        struct sxe_icon_entry entry;
        uint32_t expected = 0;

        memcpy(&entry, bytes + header.header_bytes + (index * sizeof(entry)), sizeof(entry));

        if (entry.format != SXE_ICON_FORMAT_BGRA8888)
        {
            return SXE_MALFORMED;
        }
        if (entry.width == 0u || entry.height == 0u)
        {
            return SXE_MALFORMED;
        }
        /* width y height son uint16, asi que el producto por 4 entra comodo en
         * uint32 y no puede dar la vuelta. */
        expected = (uint32_t)entry.width * (uint32_t)entry.height * SXE_ICON_BYTES_PER_PIXEL;
        if (entry.length != expected)
        {
            return SXE_MALFORMED;
        }
        if ((entry.offset % SXE_RECORD_ALIGNMENT) != 0u)
        {
            return SXE_MALFORMED;
        }
        if (entry.offset < entries_end)
        {
            return SXE_MALFORMED;
        }
        if (entry.offset > header.blob_bytes || header.blob_bytes - entry.offset < entry.length)
        {
            return SXE_TRUNCATED;
        }
    }

    out->blob = bytes;
    out->entries = (const struct sxe_icon_entry*)(const void*)(bytes + header.header_bytes);
    out->blob_bytes = header.blob_bytes;
    out->image_count = header.image_count;
    out->version = header.version;
    return SXE_OK;
}

const struct sxe_icon_entry* sxe_icons_best(const struct sxe_icons* icons, uint32_t wanted_size)
{
    const struct sxe_icon_entry* smallest_fit = 0;
    const struct sxe_icon_entry* largest = 0;
    uint32_t index = 0;

    if (icons == 0 || icons->entries == 0 || icons->image_count == 0u)
    {
        return 0;
    }

    for (index = 0; index < icons->image_count; ++index)
    {
        const struct sxe_icon_entry* entry = &icons->entries[index];
        uint32_t size = entry->width;

        if (size == wanted_size)
        {
            return entry;
        }
        /* Agrandar se ve peor que achicar: se prefiere el menor que alcance. */
        if (size > wanted_size && (smallest_fit == 0 || size < smallest_fit->width))
        {
            smallest_fit = entry;
        }
        if (largest == 0 || size > largest->width)
        {
            largest = entry;
        }
    }

    return smallest_fit != 0 ? smallest_fit : largest;
}

const uint32_t* sxe_icons_pixels(const struct sxe_icons* icons, const struct sxe_icon_entry* entry)
{
    if (icons == 0 || icons->blob == 0 || entry == 0)
    {
        return 0;
    }
    return (const uint32_t*)(const void*)(icons->blob + entry->offset);
}

/* --- Capa de disco: extraccion de secciones ELF -------------------------- */

#define SXE_ELF_HEADER_BYTES 64u
#define SXE_ELF_SHDR_BYTES 64u
/* Acota el recorrido de la tabla de secciones. Un binario de userland de este
 * sistema tiene decenas, no cientos. */
#define SXE_ELF_MAX_SECTIONS 256u
/* Solo hace falta para comparar ".sxmeta"/".sxicon": no se carga .shstrtab
 * entero, se lee de a un nombre. */
#define SXE_SECTION_NAME_CAPACITY 32u

/* Offsets dentro del ELF64 header. */
#define SXE_ELF_OFF_CLASS 4u
#define SXE_ELF_OFF_DATA 5u
#define SXE_ELF_OFF_SHOFF 40u
#define SXE_ELF_OFF_SHENTSIZE 58u
#define SXE_ELF_OFF_SHNUM 60u
#define SXE_ELF_OFF_SHSTRNDX 62u

/* Offsets dentro de un section header ELF64. */
#define SXE_SHDR_OFF_NAME 0u
#define SXE_SHDR_OFF_OFFSET 24u
#define SXE_SHDR_OFF_SIZE 32u

static int sxe_read_exact(int fd, void* buffer, size_t count)
{
    uint8_t* cursor = (uint8_t*)buffer;
    size_t done = 0;

    while (done < count)
    {
        long got = read(fd, cursor + done, count - done);

        if (got <= 0)
        {
            return 0;
        }
        done += (size_t)got;
    }
    return 1;
}

static int sxe_read_at(int fd, uint64_t offset, void* buffer, size_t count)
{
    if (offset > (uint64_t)0x7fffffffffffffffULL)
    {
        return 0;
    }
    if (seek(fd, (long)offset, SAVANXP_SEEK_SET) < 0)
    {
        return 0;
    }
    return sxe_read_exact(fd, buffer, count);
}

/* Compara el nombre de la seccion `name_offset` de .shstrtab contra `name`,
 * sin cargar la tabla de strings entera. */
static int sxe_section_name_matches(int fd, uint64_t strtab_offset, uint64_t strtab_size, uint32_t name_offset, const char* name)
{
    char buffer[SXE_SECTION_NAME_CAPACITY];
    size_t name_length = strlen(name);
    size_t needed = name_length + 1u;

    if (needed > sizeof(buffer))
    {
        return 0;
    }
    if (name_offset >= strtab_size || strtab_size - name_offset < needed)
    {
        return 0;
    }
    if (!sxe_read_at(fd, strtab_offset + name_offset, buffer, needed))
    {
        return 0;
    }
    /* El byte de mas tiene que ser el terminador: sin esto ".sxmetaX" pasaria
     * como ".sxmeta". */
    return sxe_bytes_equal(buffer, name, name_length) && buffer[name_length] == '\0';
}

int sxe_read_section(const char* path, const char* section, void* buffer, size_t capacity, size_t* out_length)
{
    uint8_t header[SXE_ELF_HEADER_BYTES];
    uint8_t shdr[SXE_ELF_SHDR_BYTES];
    long fd = -1;
    uint64_t section_header_offset = 0;
    uint64_t strtab_offset = 0;
    uint64_t strtab_size = 0;
    uint32_t section_count = 0;
    uint32_t name_index = 0;
    uint32_t index = 0;
    int result = SXE_ABSENT;

    if (out_length != 0)
    {
        *out_length = 0;
    }
    if (path == 0 || section == 0 || buffer == 0 || capacity == 0)
    {
        return SXE_ABSENT;
    }

    fd = open(path);
    if (fd < 0)
    {
        return SXE_ABSENT;
    }

    if (!sxe_read_exact((int)fd, header, sizeof(header)))
    {
        (void)close((int)fd);
        return SXE_ABSENT;
    }
    /* Un archivo que no es ELF64 little-endian no tiene secciones que buscar:
     * para el llamador es exactamente lo mismo que no tener recursos. */
    if (header[0] != 0x7fu || header[1] != 'E' || header[2] != 'L' || header[3] != 'F' ||
        header[SXE_ELF_OFF_CLASS] != 2u || header[SXE_ELF_OFF_DATA] != 1u)
    {
        (void)close((int)fd);
        return SXE_ABSENT;
    }

    section_header_offset = sxe_load_u64(header + SXE_ELF_OFF_SHOFF);
    section_count = sxe_load_u16(header + SXE_ELF_OFF_SHNUM);
    name_index = sxe_load_u16(header + SXE_ELF_OFF_SHSTRNDX);

    if (section_header_offset == 0u || section_count == 0u || section_count > SXE_ELF_MAX_SECTIONS ||
        sxe_load_u16(header + SXE_ELF_OFF_SHENTSIZE) != SXE_ELF_SHDR_BYTES || name_index >= section_count)
    {
        (void)close((int)fd);
        return SXE_ABSENT;
    }

    if (!sxe_read_at((int)fd, section_header_offset + ((uint64_t)name_index * SXE_ELF_SHDR_BYTES), shdr, sizeof(shdr)))
    {
        (void)close((int)fd);
        return SXE_ABSENT;
    }
    strtab_offset = sxe_load_u64(shdr + SXE_SHDR_OFF_OFFSET);
    strtab_size = sxe_load_u64(shdr + SXE_SHDR_OFF_SIZE);
    if (strtab_size == 0u)
    {
        (void)close((int)fd);
        return SXE_ABSENT;
    }

    /* La seccion 0 es SHT_NULL por definicion: se saltea. */
    for (index = 1u; index < section_count; ++index)
    {
        uint64_t data_offset = 0;
        uint64_t data_size = 0;

        if (!sxe_read_at((int)fd, section_header_offset + ((uint64_t)index * SXE_ELF_SHDR_BYTES), shdr, sizeof(shdr)))
        {
            break;
        }
        if (!sxe_section_name_matches((int)fd, strtab_offset, strtab_size, sxe_load_u32(shdr + SXE_SHDR_OFF_NAME), section))
        {
            continue;
        }

        data_offset = sxe_load_u64(shdr + SXE_SHDR_OFF_OFFSET);
        data_size = sxe_load_u64(shdr + SXE_SHDR_OFF_SIZE);
        if (data_size == 0u)
        {
            result = SXE_ABSENT;
            break;
        }
        if (data_size > capacity)
        {
            result = SXE_TOO_LARGE;
            break;
        }
        if (!sxe_read_at((int)fd, data_offset, buffer, (size_t)data_size))
        {
            result = SXE_TRUNCATED;
            break;
        }
        if (out_length != 0)
        {
            *out_length = (size_t)data_size;
        }
        result = SXE_OK;
        break;
    }

    (void)close((int)fd);
    return result;
}

int sxe_load_meta(const char* path, void* buffer, size_t capacity, struct sxe_meta* out)
{
    size_t length = 0;
    int result = sxe_read_section(path, SXE_SECTION_META, buffer, capacity, &length);

    if (out != 0)
    {
        memset(out, 0, sizeof(*out));
    }
    if (result != SXE_OK)
    {
        return result;
    }
    return sxe_meta_open(buffer, length, out);
}

int sxe_load_icons(const char* path, void* buffer, size_t capacity, struct sxe_icons* out)
{
    size_t length = 0;
    int result = sxe_read_section(path, SXE_SECTION_ICON, buffer, capacity, &length);

    if (out != 0)
    {
        memset(out, 0, sizeof(*out));
    }
    if (result != SXE_OK)
    {
        return result;
    }
    return sxe_icons_open(buffer, length, out);
}

int sxe_path_has_extension(const char* path)
{
    size_t path_length = 0;
    size_t extension_length = strlen(SXE_FILE_EXTENSION);

    if (path == 0)
    {
        return 0;
    }
    path_length = strlen(path);
    if (path_length <= extension_length)
    {
        return 0;
    }
    return sxe_bytes_equal(path + (path_length - extension_length), SXE_FILE_EXTENSION, extension_length);
}

/* --- selftest ------------------------------------------------------------ */

static int g_sxe_selftest_failures = 0;

static void expect(int condition, const char* label)
{
    if (!condition)
    {
        printf("SXE SMOKE FAIL %s\n", label);
        g_sxe_selftest_failures += 1;
    }
}

/*
 * Constructor de blobs solo para los tests. El generador de verdad es
 * host-side (fase 2); esto existe para poder fabricar casos degradados --
 * truncados, cuentas mentirosas, tags desconocidos -- que ningun generador
 * correcto produciria nunca.
 *
 * El buffer es del llamador y va en el stack: nada de esto debe costar BSS en
 * los binarios que solo usan el lector.
 */
struct sxe_builder {
    uint8_t* bytes;
    uint32_t capacity;
    uint32_t length;
    uint32_t records;
};

static void builder_begin(struct sxe_builder* builder, void* storage, uint32_t capacity, int is_icon)
{
    builder->bytes = (uint8_t*)storage;
    builder->capacity = capacity;
    builder->length = 0;
    builder->records = 0;

    memset(builder->bytes, 0, capacity);
    builder->bytes[0] = 'S';
    builder->bytes[1] = 'X';
    builder->bytes[2] = is_icon ? 'I' : 'M';
    builder->bytes[3] = is_icon ? 'C' : 'E';
    builder->bytes[4] = 1u; /* version */
    builder->bytes[6] = 16u; /* header_bytes */
    builder->length = 16u;
}

static void builder_append(struct sxe_builder* builder, const void* data, uint32_t length)
{
    if (builder->length + length > builder->capacity)
    {
        return;
    }
    memcpy(builder->bytes + builder->length, data, length);
    builder->length += length;
}

static void builder_record(struct sxe_builder* builder, uint16_t tag, uint16_t flags, const void* payload, uint32_t length)
{
    struct sxe_record record;
    uint32_t padded = sxe_pad4(length);
    uint32_t padding = padded - length;

    record.tag = tag;
    record.flags = flags;
    record.length = length;
    builder_append(builder, &record, (uint32_t)sizeof(record));
    if (length != 0u)
    {
        builder_append(builder, payload, length);
    }
    while (padding > 0u)
    {
        uint8_t zero = 0;

        builder_append(builder, &zero, 1u);
        padding -= 1u;
    }
    builder->records += 1u;
}

static void builder_text(struct sxe_builder* builder, uint16_t tag, const char* text)
{
    builder_record(builder, tag, SXE_RECORD_NONE, text, (uint32_t)strlen(text));
}

static void builder_u32(struct sxe_builder* builder, uint16_t tag, uint32_t value)
{
    builder_record(builder, tag, SXE_RECORD_NONE, &value, (uint32_t)sizeof(value));
}

static void builder_finish(struct sxe_builder* builder)
{
    uint32_t blob_bytes = builder->length;
    uint32_t record_count = builder->records;

    memcpy(builder->bytes + 8, &blob_bytes, sizeof(blob_bytes));
    memcpy(builder->bytes + 12, &record_count, sizeof(record_count));
}

static void selftest_well_formed(void)
{
    _Alignas(4) uint8_t storage[512];
    struct sxe_builder builder;
    struct sxe_meta meta;
    char text[64];
    uint16_t version[SXE_VERSION_COMPONENTS];
    uint16_t source_version[SXE_VERSION_COMPONENTS] = {1u, 4u, 2u, 77u};
    uint32_t value = 0;
    uint8_t small = 0;

    builder_begin(&builder, storage, (uint32_t)sizeof(storage), 0);
    builder_text(&builder, SXE_TAG_NAME, "Bloc de notas");
    builder_text(&builder, SXE_TAG_DESCRIPTION, "Editor de texto plano");
    builder_record(&builder, SXE_TAG_VERSION, SXE_RECORD_NONE, source_version, SXE_VERSION_BYTES);
    builder_text(&builder, SXE_TAG_VERSION_STRING, "1.4.2-rc1");
    builder_u32(&builder, SXE_TAG_ACCENT, 0x003c5f9cu);
    builder_u32(&builder, SXE_TAG_LAUNCH_FLAGS, SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN);
    builder_text(&builder, SXE_TAG_INTERPRETER, "/bin/hlvm");
    builder_record(&builder, SXE_TAG_SUBSYSTEM, SXE_RECORD_NONE, "\x53", 1u);
    builder_record(&builder, SXE_TAG_MIME_OPEN, SXE_RECORD_NONE, "text/plain\0text/markdown", 24u);
    builder_finish(&builder);

    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_OK, "blob bien formado");
    expect(meta.record_count == 9u, "record_count");

    expect(sxe_meta_string(&meta, SXE_TAG_NAME, text, sizeof(text)) == 13u, "largo de NAME");
    expect(strcmp(text, "Bloc de notas") == 0, "valor de NAME");
    expect(sxe_meta_string(&meta, SXE_TAG_VERSION_STRING, text, sizeof(text)) == 9u, "VERSION_STRING");
    expect(strcmp(text, "1.4.2-rc1") == 0, "valor de VERSION_STRING");

    expect(sxe_meta_version(&meta, version) == 1, "VERSION presente");
    expect(version[0] == 1u && version[1] == 4u && version[2] == 2u && version[3] == 77u, "valor de VERSION");

    expect(sxe_meta_u32(&meta, SXE_TAG_ACCENT, &value) == 1 && value == 0x003c5f9cu, "ACCENT");
    expect(sxe_meta_u32(&meta, SXE_TAG_LAUNCH_FLAGS, &value) == 1 &&
        value == SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN, "LAUNCH_FLAGS");
    expect(sxe_meta_u8(&meta, SXE_TAG_SUBSYSTEM, &small) == 1 && small == 0x53u, "SUBSYSTEM");
    expect(sxe_meta_string(&meta, SXE_TAG_INTERPRETER, text, sizeof(text)) == 9u &&
        strcmp(text, "/bin/hlvm") == 0, "INTERPRETER");

    /* Tags ausentes: no estan, y no rompen nada. */
    expect(sxe_meta_string(&meta, SXE_TAG_VENDOR, text, sizeof(text)) == 0 && text[0] == '\0', "VENDOR ausente");
    expect(sxe_meta_u32(&meta, SXE_TAG_ACCENT + 0x40u, &value) == 0, "u32 de tag ausente");

    /* Un tipo pedido con el payload de otro tamano se trata como ausente. */
    expect(sxe_meta_u32(&meta, SXE_TAG_SUBSYSTEM, &value) == 0, "u32 sobre payload de 1 byte");
    expect(sxe_meta_u8(&meta, SXE_TAG_ACCENT, &small) == 0, "u8 sobre payload de 4 bytes");

    /* Listas separadas por NUL. */
    expect(sxe_meta_list_count(&meta, SXE_TAG_MIME_OPEN) == 2, "cuenta de MIME_OPEN");
    expect(sxe_meta_list_at(&meta, SXE_TAG_MIME_OPEN, 0, text, sizeof(text)) == 10u &&
        strcmp(text, "text/plain") == 0, "MIME_OPEN[0]");
    expect(sxe_meta_list_at(&meta, SXE_TAG_MIME_OPEN, 1, text, sizeof(text)) == 13u &&
        strcmp(text, "text/markdown") == 0, "MIME_OPEN[1]");
    expect(sxe_meta_list_at(&meta, SXE_TAG_MIME_OPEN, 2, text, sizeof(text)) == 0 && text[0] == '\0', "MIME_OPEN fuera de rango");
    expect(sxe_meta_list_at(&meta, SXE_TAG_MIME_OPEN, -1, text, sizeof(text)) == 0, "MIME_OPEN indice negativo");
    expect(sxe_meta_list_count(&meta, SXE_TAG_EXT_OPEN) == 0, "lista ausente cuenta 0");
}

static void selftest_truncation(void)
{
    _Alignas(4) uint8_t storage[256];
    struct sxe_builder builder;
    struct sxe_meta meta;
    char text[8];

    builder_begin(&builder, storage, (uint32_t)sizeof(storage), 0);
    builder_text(&builder, SXE_TAG_NAME, "Administrador de tareas");
    /* Payload con NUL embebido: el accessor de texto se corta ahi. */
    builder_record(&builder, SXE_TAG_VENDOR, SXE_RECORD_NONE, "Savan\0Dev", 9u);
    builder_finish(&builder);

    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_OK, "truncation fixture");
    /* capacity 8 => 7 bytes utiles + NUL. */
    expect(sxe_meta_string(&meta, SXE_TAG_NAME, text, sizeof(text)) == 7u, "truncado a capacity - 1");
    expect(strcmp(text, "Adminis") == 0, "contenido truncado");
    expect(sxe_meta_string(&meta, SXE_TAG_VENDOR, text, sizeof(text)) == 5u &&
        strcmp(text, "Savan") == 0, "corte en NUL embebido");
    /* capacity 0 no escribe nada ni revienta. */
    expect(sxe_meta_string(&meta, SXE_TAG_NAME, text, 0) == 0, "capacity cero");
}

static void selftest_unknown_tags(void)
{
    _Alignas(4) uint8_t storage[256];
    struct sxe_builder builder;
    struct sxe_meta meta;
    char text[32];

    /* Reservado del sistema, todavia sin significado: se ignora. */
    builder_begin(&builder, storage, (uint32_t)sizeof(storage), 0);
    builder_text(&builder, SXE_TAG_NAME, "Futuro");
    builder_record(&builder, 0x0500u, SXE_RECORD_NONE, "algo nuevo", 10u);
    builder_finish(&builder);
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_OK, "tag desconocido se ignora");
    expect(sxe_meta_string(&meta, SXE_TAG_NAME, text, sizeof(text)) == 6u, "NAME sobrevive a tag desconocido");
    expect(meta.record_count == 2u, "el registro desconocido igual se cuenta");

    /* Privado: tampoco lo conoce el lector, pero sin REQUIRED es inofensivo. */
    builder_begin(&builder, storage, (uint32_t)sizeof(storage), 0);
    builder_text(&builder, SXE_TAG_NAME, "Privado");
    builder_record(&builder, SXE_TAG_PRIVATE_FIRST, SXE_RECORD_NONE, "xx", 2u);
    builder_finish(&builder);
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_OK, "tag privado se ignora");

    /* Con REQUIRED, el blob entero deja de ser confiable. */
    builder_begin(&builder, storage, (uint32_t)sizeof(storage), 0);
    builder_text(&builder, SXE_TAG_NAME, "Roto");
    builder_record(&builder, 0x0500u, SXE_RECORD_REQUIRED, "importante", 10u);
    builder_finish(&builder);
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_REQUIRED_UNKNOWN, "REQUIRED desconocido rechaza");
    expect(meta.records == 0, "rechazo deja la vista en cero");

    /* Un tag privado marcado REQUIRED tambien: el sistema nunca los conoce. */
    builder_begin(&builder, storage, (uint32_t)sizeof(storage), 0);
    builder_record(&builder, SXE_TAG_PRIVATE_FIRST + 7u, SXE_RECORD_REQUIRED, "x", 1u);
    builder_finish(&builder);
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_REQUIRED_UNKNOWN, "privado REQUIRED rechaza");

    /* REQUIRED sobre un tag que SI se conoce es normal. */
    builder_begin(&builder, storage, (uint32_t)sizeof(storage), 0);
    builder_record(&builder, SXE_TAG_NAME, SXE_RECORD_REQUIRED, "Obligatorio", 11u);
    builder_finish(&builder);
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_OK, "REQUIRED conocido pasa");

    expect(sxe_tag_is_known(SXE_TAG_NAME) == 1, "NAME es conocido");
    expect(sxe_tag_is_known(0x0500u) == 0, "0x0500 no es conocido");
    expect(sxe_tag_is_known(SXE_TAG_PRIVATE_FIRST) == 0, "los privados nunca son conocidos");
}

static void selftest_broken_headers(void)
{
    _Alignas(4) uint8_t storage[256];
    struct sxe_builder builder;
    struct sxe_meta meta;
    uint32_t value = 0;

    expect(sxe_meta_open(0, 0, &meta) == SXE_ABSENT, "blob nulo");
    expect(sxe_meta_open(storage, 0, &meta) == SXE_ABSENT, "length cero");
    expect(sxe_meta_open(storage, 4u, &meta) == SXE_TRUNCATED, "mas corto que el header");

    builder_begin(&builder, storage, (uint32_t)sizeof(storage), 0);
    builder_text(&builder, SXE_TAG_NAME, "X");
    builder_finish(&builder);

    /* Magia rota. */
    storage[2] = 'Z';
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_BAD_MAGIC, "magia invalida");
    storage[2] = 'M';

    /* Un .sxicon donde se esperaba .sxmeta cae por magia, no por otra cosa. */
    storage[2] = 'I';
    storage[3] = 'C';
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_BAD_MAGIC, "magia de iconos en meta");
    storage[2] = 'M';
    storage[3] = 'E';

    /* Version futura: se rechaza entero (ver sxe_format.h). */
    storage[4] = (uint8_t)(SXE_META_VERSION + 1u);
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_BAD_VERSION, "version futura");
    storage[4] = 0u;
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_BAD_VERSION, "version cero");
    storage[4] = (uint8_t)SXE_META_VERSION;

    /* header_bytes mas chico que el header real. */
    storage[6] = 8u;
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_MALFORMED, "header_bytes chico");
    /* header_bytes desalineado. */
    storage[6] = 18u;
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_MALFORMED, "header_bytes desalineado");
    storage[6] = 16u;

    /* blob_bytes por encima del tope del formato. */
    value = SXE_META_MAX_BYTES + 4u;
    memcpy(storage + 8, &value, sizeof(value));
    expect(sxe_meta_open(storage, sizeof(storage), &meta) == SXE_TOO_LARGE, "blob_bytes sobre el tope");

    /* blob_bytes mayor que lo que realmente hay. */
    value = builder.length + 4u;
    memcpy(storage + 8, &value, sizeof(value));
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_TRUNCATED, "blob_bytes mayor que length");

    /* blob_bytes desalineado. */
    value = builder.length - 1u;
    memcpy(storage + 8, &value, sizeof(value));
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_MALFORMED, "blob_bytes desalineado");

    /* record_count mentiroso. */
    value = builder.length;
    memcpy(storage + 8, &value, sizeof(value));
    value = builder.records + 3u;
    memcpy(storage + 12, &value, sizeof(value));
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_MALFORMED, "record_count mentiroso");
}

static void selftest_record_bounds(void)
{
    _Alignas(4) uint8_t storage[256];
    struct sxe_builder builder;
    struct sxe_meta meta;
    struct sxe_record record;
    uint32_t value = 0;

    builder_begin(&builder, storage, (uint32_t)sizeof(storage), 0);
    builder_text(&builder, SXE_TAG_NAME, "Recorte");
    builder_finish(&builder);

    /* Un registro que declara mas payload del que entra en el blob. */
    memcpy(&record, storage + 16, sizeof(record));
    record.length = builder.length;
    memcpy(storage + 16, &record, sizeof(record));
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_TRUNCATED, "payload fuera del blob");

    /* Un length que casi da la vuelta al redondear a 4. */
    record.length = 0xfffffffeu;
    memcpy(storage + 16, &record, sizeof(record));
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_TRUNCATED, "length cerca de UINT32_MAX");

    /* Cabecera de registro cortada: blob_bytes deja menos de 8 bytes sueltos. */
    builder_begin(&builder, storage, (uint32_t)sizeof(storage), 0);
    builder_text(&builder, SXE_TAG_NAME, "abcd");
    builder_finish(&builder);
    value = 16u + 4u;
    memcpy(storage + 8, &value, sizeof(value));
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_TRUNCATED, "header de registro cortado");

    /* Blob sin registros: valido, y todo consulta como ausente. */
    builder_begin(&builder, storage, (uint32_t)sizeof(storage), 0);
    builder_finish(&builder);
    expect(sxe_meta_open(storage, builder.length, &meta) == SXE_OK, "blob vacio es valido");
    expect(meta.record_count == 0u && meta.records_bytes == 0u, "blob vacio sin registros");
    expect(sxe_meta_find(&meta, SXE_TAG_NAME, 0, 0) == 0, "find sobre blob vacio");
}

static void selftest_alignment(void)
{
    _Alignas(4) uint8_t storage[256];
    struct sxe_builder builder;
    struct sxe_meta meta;
    struct sxe_icons icons;

    builder_begin(&builder, storage, (uint32_t)sizeof(storage), 0);
    builder_text(&builder, SXE_TAG_NAME, "Alineado");
    builder_finish(&builder);

    /* Correrlo un byte tiene que fallar fuerte, no leerse torcido. */
    expect(sxe_meta_open(storage + 1, builder.length, &meta) == SXE_MALFORMED, "meta desalineado");
    expect(sxe_icons_open(storage + 2, builder.length, &icons) == SXE_MALFORMED, "iconos desalineados");
}

static void selftest_icons(void)
{
    _Alignas(4) uint8_t storage[2048];
    struct sxe_icon_header header;
    struct sxe_icon_entry entries[2];
    struct sxe_icons icons;
    const struct sxe_icon_entry* chosen = 0;
    const uint32_t* pixels = 0;
    uint32_t entries_offset = (uint32_t)sizeof(header);
    uint32_t small_offset = entries_offset + (uint32_t)sizeof(entries);
    uint32_t large_offset = small_offset + (8u * 8u * 4u);
    uint32_t blob_bytes = large_offset + (16u * 16u * 4u);
    uint32_t index = 0;

    memset(storage, 0, sizeof(storage));
    header.magic[0] = SXE_ICON_MAGIC0;
    header.magic[1] = SXE_ICON_MAGIC1;
    header.magic[2] = SXE_ICON_MAGIC2;
    header.magic[3] = SXE_ICON_MAGIC3;
    header.version = SXE_ICON_VERSION;
    header.header_bytes = (uint16_t)sizeof(header);
    header.blob_bytes = blob_bytes;
    header.image_count = 2u;
    memcpy(storage, &header, sizeof(header));

    entries[0].width = 8u;
    entries[0].height = 8u;
    entries[0].format = SXE_ICON_FORMAT_BGRA8888;
    entries[0].offset = small_offset;
    entries[0].length = 8u * 8u * 4u;
    entries[1].width = 16u;
    entries[1].height = 16u;
    entries[1].format = SXE_ICON_FORMAT_BGRA8888;
    entries[1].offset = large_offset;
    entries[1].length = 16u * 16u * 4u;
    memcpy(storage + entries_offset, entries, sizeof(entries));

    /* Marca reconocible en el primer pixel de cada imagen. */
    for (index = 0; index < 4u; ++index)
    {
        storage[small_offset + index] = (uint8_t)(0x11u * (index + 1u));
        storage[large_offset + index] = (uint8_t)(0x22u * (index + 1u));
    }

    expect(sxe_icons_open(storage, blob_bytes, &icons) == SXE_OK, "iconos bien formados");
    expect(icons.image_count == 2u, "cuenta de imagenes");

    /* Exacto. */
    chosen = sxe_icons_best(&icons, 8u);
    expect(chosen != 0 && chosen->width == 8u, "best exacto 8");
    chosen = sxe_icons_best(&icons, 16u);
    expect(chosen != 0 && chosen->width == 16u, "best exacto 16");
    /* Sin exacto: el menor que alcance. */
    chosen = sxe_icons_best(&icons, 12u);
    expect(chosen != 0 && chosen->width == 16u, "best sube al que alcanza");
    chosen = sxe_icons_best(&icons, 4u);
    expect(chosen != 0 && chosen->width == 8u, "best con pedido chico");
    /* Mas grande que todo: el mayor disponible. */
    chosen = sxe_icons_best(&icons, 64u);
    expect(chosen != 0 && chosen->width == 16u, "best cae al mas grande");

    pixels = sxe_icons_pixels(&icons, sxe_icons_best(&icons, 16u));
    expect(pixels != 0 && pixels[0] == 0x88664422u, "pixeles del icono grande");
    pixels = sxe_icons_pixels(&icons, sxe_icons_best(&icons, 8u));
    expect(pixels != 0 && pixels[0] == 0x44332211u, "pixeles del icono chico");

    /* Formato desconocido. */
    entries[0].format = 7u;
    memcpy(storage + entries_offset, entries, sizeof(entries));
    expect(sxe_icons_open(storage, blob_bytes, &icons) == SXE_MALFORMED, "formato de icono invalido");
    entries[0].format = SXE_ICON_FORMAT_BGRA8888;

    /* length que no coincide con width * height * 4. */
    entries[0].length = 8u * 8u * 4u - 4u;
    memcpy(storage + entries_offset, entries, sizeof(entries));
    expect(sxe_icons_open(storage, blob_bytes, &icons) == SXE_MALFORMED, "length de icono incoherente");
    entries[0].length = 8u * 8u * 4u;

    /* Dimension cero. */
    entries[0].width = 0u;
    entries[0].length = 0u;
    memcpy(storage + entries_offset, entries, sizeof(entries));
    expect(sxe_icons_open(storage, blob_bytes, &icons) == SXE_MALFORMED, "icono de ancho cero");
    entries[0].width = 8u;
    entries[0].length = 8u * 8u * 4u;

    /* Offset pisando la tabla de entradas. */
    entries[0].offset = entries_offset;
    memcpy(storage + entries_offset, entries, sizeof(entries));
    expect(sxe_icons_open(storage, blob_bytes, &icons) == SXE_MALFORMED, "offset dentro de la tabla");

    /* Offset desalineado. */
    entries[0].offset = small_offset + 2u;
    memcpy(storage + entries_offset, entries, sizeof(entries));
    expect(sxe_icons_open(storage, blob_bytes, &icons) == SXE_MALFORMED, "offset de pixeles desalineado");

    /* Pixeles que se salen del blob. */
    entries[0].offset = blob_bytes - 8u;
    memcpy(storage + entries_offset, entries, sizeof(entries));
    expect(sxe_icons_open(storage, blob_bytes, &icons) == SXE_TRUNCATED, "pixeles fuera del blob");
    entries[0].offset = small_offset;
    memcpy(storage + entries_offset, entries, sizeof(entries));

    /* Mas imagenes que el tope. */
    header.image_count = SXE_ICON_MAX_IMAGES + 1u;
    memcpy(storage, &header, sizeof(header));
    expect(sxe_icons_open(storage, blob_bytes, &icons) == SXE_MALFORMED, "image_count sobre el tope");
    header.image_count = 2u;

    /* blob_bytes sobre el tope del formato. */
    header.blob_bytes = SXE_ICON_MAX_BYTES + 4u;
    memcpy(storage, &header, sizeof(header));
    expect(sxe_icons_open(storage, sizeof(storage), &icons) == SXE_TOO_LARGE, "iconos sobre el tope");
    header.blob_bytes = blob_bytes;

    /* Magia de meta donde se esperaban iconos. */
    header.magic[2] = 'M';
    header.magic[3] = 'E';
    memcpy(storage, &header, sizeof(header));
    expect(sxe_icons_open(storage, blob_bytes, &icons) == SXE_BAD_MAGIC, "magia de meta en iconos");

    /* Sin imagenes: valido, pero best no inventa nada. */
    memset(&icons, 0, sizeof(icons));
    expect(sxe_icons_best(&icons, 16u) == 0, "best sin imagenes");
    expect(sxe_icons_pixels(&icons, 0) == 0, "pixels sin entrada");
}

static void selftest_extension(void)
{
    expect(sxe_path_has_extension("/disk/bin/notepad.sxe") == 1, "extension .sxe");
    expect(sxe_path_has_extension("/disk/bin/notepad") == 0, "sin extension");
    expect(sxe_path_has_extension("/disk/bin/notepad.elf") == 0, "extension .elf");
    expect(sxe_path_has_extension(".sxe") == 0, "solo la extension no alcanza");
    expect(sxe_path_has_extension("/a.sxe") == 1, "path corto con extension");
    expect(sxe_path_has_extension(0) == 0, "path nulo");
}

/*
 * Camino de disco. Todavia no hay ningun .sxe en la imagen (eso es la fase 2),
 * asi que lo que se puede validar de verdad es lo mas importante: que un
 * binario NORMAL, sin recursos, se resuelva limpio como "sin metadata" y no
 * como un error que impida lanzarlo.
 */
static void selftest_disk(void)
{
    _Alignas(4) uint8_t buffer[SXE_META_MAX_BYTES];
    struct sxe_meta meta;
    struct sxe_icons icons;
    size_t length = 1u;

    expect(sxe_load_meta("/bin/progman", buffer, sizeof(buffer), &meta) == SXE_ABSENT,
        "binario sin .sxmeta");
    expect(sxe_load_icons("/bin/progman", buffer, sizeof(buffer), &icons) == SXE_ABSENT,
        "binario sin .sxicon");
    expect(sxe_load_meta("/disk/bin/__no_instalado__", buffer, sizeof(buffer), &meta) == SXE_ABSENT,
        "path inexistente");

    /*
     * .text existe y no entra en 16 bytes. Que devuelva TOO_LARGE y no ABSENT
     * es justamente la prueba de que la busqueda por nombre ENCONTRO la
     * seccion: sin este check, un matcher que no acierta nunca se veria
     * exactamente igual que "este binario no trae recursos".
     */
    expect(sxe_read_section("/bin/progman", ".text", buffer, 16u, &length) == SXE_TOO_LARGE,
        ".text encontrado");
    expect(length == 0u, "TOO_LARGE no reporta largo");

    /* Prefijo de un nombre real: el comparador exige el NUL, asi que no matchea. */
    expect(sxe_read_section("/bin/progman", ".tex", buffer, sizeof(buffer), &length) == SXE_ABSENT,
        "prefijo de nombre no matchea");
    expect(sxe_read_section("/bin/progman", ".no_existe", buffer, sizeof(buffer), &length) == SXE_ABSENT,
        "seccion inexistente");

    /* Un archivo que no es ELF se resuelve como ausente, no como error. */
    expect(sxe_read_section("/disk/wallpaper.bmp", SXE_SECTION_META, buffer, sizeof(buffer), &length) == SXE_ABSENT,
        "archivo que no es ELF");
}

int sxe_selftest(void)
{
    g_sxe_selftest_failures = 0;

    selftest_well_formed();
    selftest_truncation();
    selftest_unknown_tags();
    selftest_broken_headers();
    selftest_record_bounds();
    selftest_alignment();
    selftest_icons();
    selftest_extension();
    selftest_disk();

    return g_sxe_selftest_failures;
}
