#include "mime_icon.h"

#include "savanxp/sxe.h"

#include <stdio.h>

/* --- estado --------------------------------------------------------------- */

static struct mime_icon_entry g_entries[MIME_ICON_MAX_ENTRIES];
static int g_entry_count = 0;

/* Reservadas: no son extensiones, asi que no entran en g_entries. Vacias
 * significa "no hay regla", que es distinto de "hay regla y el icono falta". */
static char g_icon_folder[MIME_ICON_NAME_CAPACITY];
static char g_icon_program[MIME_ICON_NAME_CAPACITY];
static char g_icon_default[MIME_ICON_NAME_CAPACITY];

struct mime_icon_cache_slot
{
    char name[MIME_ICON_NAME_CAPACITY];
    /* 0 = se intento y no salio. El fallo se cachea igual que el exito: sin
     * eso, un icono que no esta se reintenta en cada repintado de la lista. */
    int valid;
    struct sx_bitmap bitmap;
    uint32_t pixels[MIME_ICON_SMALL_SIZE * MIME_ICON_SMALL_SIZE];
};

static struct mime_icon_cache_slot g_cache[MIME_ICON_CACHE_ENTRIES];
static int g_cache_count = 0;

static int g_selftest_failures = 0;

/* --- helpers -------------------------------------------------------------- */

static char lower_char(char value)
{
    if (value >= 'A' && value <= 'Z')
    {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

static void copy_field(char *destination, size_t capacity, const char *source)
{
    size_t index = 0;

    if (destination == 0 || capacity == 0)
    {
        return;
    }
    if (source != 0)
    {
        while (source[index] != '\0' && index + 1u < capacity)
        {
            destination[index] = source[index];
            index += 1u;
        }
    }
    destination[index] = '\0';
}

static void copy_lower(char *destination, size_t capacity, const char *source)
{
    size_t index = 0;

    if (destination == 0 || capacity == 0)
    {
        return;
    }
    if (source != 0)
    {
        while (source[index] != '\0' && index + 1u < capacity)
        {
            destination[index] = lower_char(source[index]);
            index += 1u;
        }
    }
    destination[index] = '\0';
}

static int is_space_char(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static char *trim(char *text)
{
    size_t length;

    if (text == 0)
    {
        return text;
    }
    while (*text != '\0' && is_space_char(*text))
    {
        text += 1;
    }
    length = strlen(text);
    while (length != 0 && is_space_char(text[length - 1u]))
    {
        text[length - 1u] = '\0';
        length -= 1u;
    }
    return text;
}

/* Extension de un nombre, con punto y en minusculas. 0 si no tiene. */
static const char *extension_of(const char *file_name, char *out, size_t capacity)
{
    const char *dot = 0;
    size_t index = 0;

    if (file_name == 0 || out == 0 || capacity == 0)
    {
        return 0;
    }
    for (index = 0; file_name[index] != '\0'; ++index)
    {
        /* index > 0 deja afuera los dotfiles: ".bashrc" no tiene extension
         * ".bashrc", no tiene extension y punto. */
        if (file_name[index] == '.' && index > 0 && file_name[index + 1] != '\0')
        {
            dot = &file_name[index];
        }
    }
    if (dot == 0)
    {
        return 0;
    }
    copy_lower(out, capacity, dot);
    return out;
}

/* --- politica ------------------------------------------------------------- */

static int assign_reserved(const char *key, const char *value)
{
    if (strcmp(key, "folder") == 0)
    {
        copy_field(g_icon_folder, sizeof(g_icon_folder), value);
        return 1;
    }
    if (strcmp(key, "program") == 0)
    {
        copy_field(g_icon_program, sizeof(g_icon_program), value);
        return 1;
    }
    if (strcmp(key, "default") == 0)
    {
        copy_field(g_icon_default, sizeof(g_icon_default), value);
        return 1;
    }
    return 0;
}

/* Ultima linea gana: reasignar una extension la pisa en vez de duplicarla, que
 * es lo que hace util editar el archivo agregando lineas al final. */
static int upsert(const char *extension, const char *icon)
{
    int index = 0;

    for (index = 0; index < g_entry_count; ++index)
    {
        if (strcmp(g_entries[index].extension, extension) == 0)
        {
            copy_field(g_entries[index].icon, sizeof(g_entries[index].icon), icon);
            return 0;
        }
    }
    if (g_entry_count >= MIME_ICON_MAX_ENTRIES)
    {
        return 0;
    }
    copy_field(g_entries[g_entry_count].extension, MIME_ICON_EXT_CAPACITY, extension);
    copy_field(g_entries[g_entry_count].icon, MIME_ICON_NAME_CAPACITY, icon);
    g_entry_count += 1;
    return 1;
}

int mime_icon_parse_policy(const char *text, size_t length)
{
    static char buffer[MIME_ICON_POLICY_MAX_BYTES];
    size_t copy_length = 0;
    size_t offset = 0;
    int added = 0;

    g_entry_count = 0;
    g_icon_folder[0] = '\0';
    g_icon_program[0] = '\0';
    g_icon_default[0] = '\0';

    if (text == 0 || length == 0)
    {
        return 0;
    }

    copy_length = length < (sizeof(buffer) - 1u) ? length : (sizeof(buffer) - 1u);
    memcpy(buffer, text, copy_length);
    buffer[copy_length] = '\0';

    while (offset < copy_length)
    {
        size_t start = offset;
        char *line = 0;
        char *separator = 0;
        char *key = 0;
        char *value = 0;
        char extension[MIME_ICON_EXT_CAPACITY];

        while (offset < copy_length && buffer[offset] != '\n')
        {
            offset += 1u;
        }
        if (offset < copy_length)
        {
            buffer[offset] = '\0';
            offset += 1u;
        }

        line = trim(&buffer[start]);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
        {
            continue;
        }

        /* Sin strchr: la libc de userland no lo trae, y file_assoc resuelve
         * lo mismo con un barrido a mano. */
        {
            size_t scan = 0;

            while (line[scan] != '\0')
            {
                if (line[scan] == '=')
                {
                    separator = &line[scan];
                    break;
                }
                scan += 1u;
            }
        }
        if (separator == 0)
        {
            continue;
        }
        *separator = '\0';
        key = trim(line);
        value = trim(separator + 1);
        if (key[0] == '\0' || value[0] == '\0')
        {
            continue;
        }

        if (assign_reserved(key, value))
        {
            continue;
        }
        /* Cualquier clave que no sea reservada tiene que ser una extension, y
         * una extension empieza con punto. Una clave suelta es un typo, y
         * tragarselo en silencio esconderia el error. */
        if (key[0] != '.' || key[1] == '\0')
        {
            continue;
        }
        copy_lower(extension, sizeof(extension), key);
        added += upsert(extension, value);
    }

    return added;
}

int mime_icon_load(void)
{
    static char file_buffer[MIME_ICON_POLICY_MAX_BYTES];
    int fd = (int)savanxp_open_mode(MIME_ICON_POLICY_PATH, SAVANXP_OPEN_READ);
    long total = 0;

    g_entry_count = 0;
    if (fd < 0)
    {
        /* Sin archivo no hay iconos y tampoco hay error: la lista se dibuja
         * como se dibujaba antes de que esta capa existiera. */
        return 0;
    }
    while (total < (long)(sizeof(file_buffer) - 1u))
    {
        long got = savanxp_read(fd, file_buffer + total, sizeof(file_buffer) - 1u - (size_t)total);

        if (got <= 0)
        {
            break;
        }
        total += got;
    }
    (void)savanxp_close(fd);
    if (total <= 0)
    {
        return 0;
    }
    return mime_icon_parse_policy(file_buffer, (size_t)total);
}

/* --- resolucion ----------------------------------------------------------- */

static const char *or_null(const char *text)
{
    return (text != 0 && text[0] != '\0') ? text : 0;
}

const char *mime_icon_name_for(const char *file_name, int is_dir, int launchable)
{
    char extension[MIME_ICON_EXT_CAPACITY];
    int index = 0;

    if (is_dir)
    {
        return or_null(g_icon_folder);
    }
    if (launchable)
    {
        return or_null(g_icon_program);
    }
    if (extension_of(file_name, extension, sizeof(extension)) != 0)
    {
        for (index = 0; index < g_entry_count; ++index)
        {
            if (strcmp(g_entries[index].extension, extension) == 0)
            {
                return or_null(g_entries[index].icon);
            }
        }
    }
    return or_null(g_icon_default);
}

/* --- pixeles -------------------------------------------------------------- */

static struct mime_icon_cache_slot *cache_find(const char *icon_name)
{
    int index = 0;

    for (index = 0; index < g_cache_count; ++index)
    {
        if (strcmp(g_cache[index].name, icon_name) == 0)
        {
            return &g_cache[index];
        }
    }
    return 0;
}

/* Copia el 16x16 del blob al slot. 0 si el blob no trae ese tamano exacto:
 * escalar en runtime es justamente lo que el .sxicon con los dos tamanos
 * existe para evitar, asi que un blob incompleto es un error del build, no
 * algo que se arregle interpolando. */
static int fill_slot(struct mime_icon_cache_slot *slot, const struct sxe_icons *icons)
{
    const struct sxe_icon_entry *entry = sxe_icons_best(icons, MIME_ICON_SMALL_SIZE);
    const uint32_t *pixels = 0;
    size_t count = (size_t)MIME_ICON_SMALL_SIZE * (size_t)MIME_ICON_SMALL_SIZE;

    if (entry == 0 || entry->width != MIME_ICON_SMALL_SIZE || entry->height != MIME_ICON_SMALL_SIZE)
    {
        return 0;
    }
    pixels = sxe_icons_pixels(icons, entry);
    if (pixels == 0)
    {
        return 0;
    }

    memcpy(slot->pixels, pixels, count * sizeof(uint32_t));
    slot->bitmap.pixels = slot->pixels;
    slot->bitmap.info.width = MIME_ICON_SMALL_SIZE;
    slot->bitmap.info.height = MIME_ICON_SMALL_SIZE;
    slot->bitmap.info.pitch = MIME_ICON_SMALL_SIZE * (uint32_t)sizeof(uint32_t);
    slot->bitmap.info.bpp = 32u;
    slot->bitmap.format = SX_PIXEL_FORMAT_BGRA8888;
    return 1;
}

const struct sx_bitmap *mime_icon_small(const char *icon_name)
{
    /* Uno solo y reusado: parsear el blob necesita el archivo entero en
     * memoria, pero solo mientras dura la copia al slot. */
    static _Alignas(4) uint8_t scratch[MIME_ICON_SCRATCH_BYTES];
    struct mime_icon_cache_slot *slot = 0;
    struct sxe_icons icons;
    char path[MIME_ICON_NAME_CAPACITY + 48];

    if (icon_name == 0 || icon_name[0] == '\0')
    {
        return 0;
    }
    slot = cache_find(icon_name);
    if (slot != 0)
    {
        return slot->valid ? &slot->bitmap : 0;
    }
    if (g_cache_count >= MIME_ICON_CACHE_ENTRIES)
    {
        /* Sin eviccion: el catalogo entra en la cache por diseno. Si algun dia
         * no entra, lo correcto es agrandarla, no empezar a desalojar en el
         * medio de un repintado. */
        return 0;
    }

    slot = &g_cache[g_cache_count];
    g_cache_count += 1;
    copy_field(slot->name, sizeof(slot->name), icon_name);
    slot->valid = 0;

    snprintf(path, sizeof(path), "%s/%s.sxicon", MIME_ICON_DIRECTORY, icon_name);
    if (sxe_load_icon_file(path, scratch, sizeof(scratch), &icons) != SXE_OK)
    {
        return 0;
    }
    slot->valid = fill_slot(slot, &icons);
    return slot->valid ? &slot->bitmap : 0;
}

const struct sx_bitmap *mime_icon_for_file(const char *file_name, int is_dir, int launchable)
{
    return mime_icon_small(mime_icon_name_for(file_name, is_dir, launchable));
}

int mime_icon_count(void)
{
    return g_entry_count;
}

const struct mime_icon_entry *mime_icon_at(int index)
{
    if (index < 0 || index >= g_entry_count)
    {
        return 0;
    }
    return &g_entries[index];
}

/* --- selftest ------------------------------------------------------------- */

static void expect(int condition, const char *label)
{
    if (!condition)
    {
        printf("FILESAPP SMOKE FAIL %s\n", label);
        g_selftest_failures += 1;
    }
}

static void selftest_parse(void)
{
    static const char k_policy[] =
        "# comentario\n"
        "; otro comentario\n"
        "\n"
        "folder=folder\n"
        "program=application-x-executable\n"
        "default=text-x-generic\n"
        "  .TXT  =  text-x-generic  \n"
        ".png=image-x-generic\n"
        "sin-punto=deberia-ignorarse\n"
        ".sin-valor=\n"
        "linea sin igual\n"
        ".png=image-x-generic\n";

    const char *name = 0;

    expect(mime_icon_parse_policy(k_policy, sizeof(k_policy) - 1u) == 2,
        "parse: dos extensiones nuevas");
    expect(mime_icon_count() == 2, "parse: no cuenta reservadas ni basura");

    /* La clave se normaliza a minusculas y se le sacan los espacios. */
    name = mime_icon_name_for("notas.TXT", 0, 0);
    expect(name != 0 && strcmp(name, "text-x-generic") == 0, "parse: extension en mayusculas");
    name = mime_icon_name_for("captura.png", 0, 0);
    expect(name != 0 && strcmp(name, "image-x-generic") == 0, "parse: extension simple");

    /* Una clave sin punto es un typo y no se registra como extension. */
    expect(mime_icon_name_for("archivo.sin-punto", 0, 0) != 0, "parse: typo cae al default");
    name = mime_icon_name_for("archivo.desconocido", 0, 0);
    expect(name != 0 && strcmp(name, "text-x-generic") == 0, "parse: default");
}

static void selftest_precedence(void)
{
    static const char k_policy[] =
        "folder=folder\n"
        "program=application-x-executable\n"
        "default=text-x-generic\n"
        ".sxe=application-x-executable\n"
        ".txt=text-x-generic\n";

    const char *name = 0;

    (void)mime_icon_parse_policy(k_policy, sizeof(k_policy) - 1u);

    /* Directorio gana sobre todo, incluso con extension. */
    name = mime_icon_name_for("carpeta.txt", 1, 0);
    expect(name != 0 && strcmp(name, "folder") == 0, "precedencia: directorio gana");

    /* Lanzable gana sobre la extension: un programa sin extension igual
     * muestra el icono de programa. */
    name = mime_icon_name_for("doom", 0, 1);
    expect(name != 0 && strcmp(name, "application-x-executable") == 0,
        "precedencia: lanzable sin extension");
    name = mime_icon_name_for("notas.txt", 0, 1);
    expect(name != 0 && strcmp(name, "application-x-executable") == 0,
        "precedencia: lanzable gana a la extension");

    /* Un dotfile no tiene extension: cae al default. */
    name = mime_icon_name_for(".perfil", 0, 0);
    expect(name != 0 && strcmp(name, "text-x-generic") == 0, "precedencia: dotfile sin extension");

    /* La ultima linea gana en vez de duplicar. */
    {
        static const char k_override[] = ".txt=text-x-generic\n.txt=x-office-document\n";

        expect(mime_icon_parse_policy(k_override, sizeof(k_override) - 1u) == 1,
            "precedencia: reasignar no duplica");
        name = mime_icon_name_for("notas.txt", 0, 0);
        expect(name != 0 && strcmp(name, "x-office-document") == 0, "precedencia: ultima linea gana");
    }
}

static void selftest_limits(void)
{
    static char big[MIME_ICON_POLICY_MAX_BYTES];
    static const char k_long[] =
        ".txt=un-nombre-de-icono-absurdamente-largo-que-no-entra-en-la-capacidad\n";
    const struct mime_icon_entry *entry = 0;
    size_t offset = 0;
    int index = 0;

    /* Sin politica: todo resuelve a 0, que es "sin icono" y no un crash. */
    expect(mime_icon_parse_policy(0, 0) == 0, "limites: texto nulo");
    expect(mime_icon_name_for("notas.txt", 0, 0) == 0, "limites: sin politica no hay icono");
    expect(mime_icon_name_for(0, 0, 0) == 0, "limites: nombre nulo");
    expect(mime_icon_small(0) == 0, "limites: nombre de icono nulo");
    expect(mime_icon_small("") == 0, "limites: nombre de icono vacio");

    /* El nombre se trunca sin desbordar. */
    (void)mime_icon_parse_policy(k_long, sizeof(k_long) - 1u);
    entry = mime_icon_at(0);
    expect(entry != 0, "limites: entrada truncada existe");
    expect(entry != 0 && strlen(entry->icon) == MIME_ICON_NAME_CAPACITY - 1u,
        "limites: nombre truncado a capacidad");

    /* Mas extensiones que MIME_ICON_MAX_ENTRIES: se corta, no se desborda. */
    for (index = 0; index < MIME_ICON_MAX_ENTRIES + 16; ++index)
    {
        int written = snprintf(big + offset, sizeof(big) - offset, ".e%d=text-x-generic\n", index);

        if (written <= 0 || (size_t)written >= sizeof(big) - offset)
        {
            break;
        }
        offset += (size_t)written;
    }
    (void)mime_icon_parse_policy(big, offset);
    expect(mime_icon_count() == MIME_ICON_MAX_ENTRIES, "limites: se corta en el tope");
    expect(mime_icon_at(MIME_ICON_MAX_ENTRIES) == 0, "limites: indice fuera de rango");
}

int mime_icon_selftest(void)
{
    g_selftest_failures = 0;

    selftest_parse();
    selftest_precedence();
    selftest_limits();

    /* El selftest deja la politica pisada; recargar la real para que el resto
     * del proceso no herede el estado de prueba. */
    (void)mime_icon_load();

    return g_selftest_failures;
}
