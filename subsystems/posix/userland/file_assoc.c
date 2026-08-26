#include "file_assoc.h"

#include "savanxp/sxe.h"

#include <dirent.h>
#include <stdio.h>

static struct file_assoc_entry g_entries[FILE_ASSOC_MAX_ENTRIES];
static int g_entry_count = 0;
/* Cuantos ejecutables se abrieron en el ultimo escaneo. Lo reporta el smoke:
 * es la magnitud que hay que mirar antes de decidir si hace falta una cache. */
static int g_last_scan_examined = 0;

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

/*
 * Normaliza una extension a ".ext" en minusculas. Acepta que venga con o sin
 * punto: un assoc.ini escrito a mano es probable que tenga las dos formas, y
 * rechazar una de las dos seria una trampa sin ningun beneficio.
 */
static int normalize_extension(const char *source, char *out, size_t capacity)
{
    size_t index = 0;

    if (source == 0 || out == 0 || capacity < 3u)
    {
        return 0;
    }
    if (source[0] == '\0' || (source[0] == '.' && source[1] == '\0'))
    {
        return 0;
    }

    out[index] = '.';
    index += 1u;
    if (source[0] == '.')
    {
        source += 1;
    }
    while (source[0] != '\0' && index + 1u < capacity)
    {
        out[index] = lower_char(source[0]);
        index += 1u;
        source += 1;
    }
    out[index] = '\0';
    return 1;
}

static int find_entry(const char *extension)
{
    int index;

    for (index = 0; index < g_entry_count; ++index)
    {
        if (strcmp(g_entries[index].extension, extension) == 0)
        {
            return index;
        }
    }
    return -1;
}

/*
 * Agrega o pisa una asociacion. La politica del usuario gana siempre: una
 * entrada de politica sobreescribe una del escaneo, pero nunca al reves. Sin
 * esto, el orden en que se cargan las dos fuentes decidiria la precedencia, que
 * es justo lo que no debe pasar.
 */
static int set_entry(const char *extension, const char *program, int from_policy)
{
    char normalized[FILE_ASSOC_EXT_CAPACITY];
    int index;

    if (!normalize_extension(extension, normalized, sizeof(normalized)))
    {
        return 0;
    }
    if (program == 0 || program[0] != '/')
    {
        /* Un programa sin path absoluto no se puede lanzar: se descarta en vez
         * de guardarse roto. */
        return 0;
    }

    index = find_entry(normalized);
    if (index >= 0)
    {
        /*
         * Lo que ya esta registrado SOLO lo pisa la politica del usuario.
         * Entre dos binarios que declaran la misma extension gana el primero
         * del escaneo -- y eso importa de verdad, no es teorico: /disk/bin es
         * una copia de /bin, asi que cada programa aparece dos veces y sin
         * esta regla la segunda pasada reescribia todas las asociaciones a la
         * ruta de /disk/bin.
         */
        if (!from_policy)
        {
            return 0;
        }
        copy_field(g_entries[index].program, sizeof(g_entries[index].program), program);
        g_entries[index].from_policy = from_policy;
        return 1;
    }

    if (g_entry_count >= FILE_ASSOC_MAX_ENTRIES)
    {
        return 0;
    }
    copy_field(g_entries[g_entry_count].extension, sizeof(g_entries[g_entry_count].extension), normalized);
    copy_field(g_entries[g_entry_count].program, sizeof(g_entries[g_entry_count].program), program);
    g_entries[g_entry_count].from_policy = from_policy;
    g_entry_count += 1;
    return 1;
}

const char *file_assoc_extension_of(const char *file_path, char *out, size_t capacity)
{
    const char *dot = 0;
    const char *cursor = file_path;

    if (file_path == 0 || out == 0 || capacity == 0)
    {
        return 0;
    }
    /* Solo el ultimo punto del ULTIMO componente: "/disk/v1.2/README" no tiene
     * extension, aunque haya un punto en el camino. */
    while (cursor[0] != '\0')
    {
        if (cursor[0] == '/')
        {
            dot = 0;
        }
        else if (cursor[0] == '.')
        {
            dot = cursor;
        }
        cursor += 1;
    }
    if (dot == 0 || dot[1] == '\0')
    {
        out[0] = '\0';
        return 0;
    }
    if (!normalize_extension(dot, out, capacity))
    {
        out[0] = '\0';
        return 0;
    }
    return out;
}

/* --- politica del usuario ------------------------------------------------- */

int file_assoc_parse_policy(const char *text, size_t length)
{
    static char buffer[FILE_ASSOC_POLICY_MAX_BYTES];
    size_t copy_length = 0;
    size_t offset = 0;
    int added = 0;

    g_entry_count = 0;
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
        size_t scan = 0;

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

        for (scan = 0; line[scan] != '\0'; ++scan)
        {
            if (line[scan] == '=')
            {
                separator = &line[scan];
                break;
            }
        }
        if (separator == 0)
        {
            continue;
        }
        *separator = '\0';
        if (set_entry(trim(line), trim(separator + 1), 1))
        {
            added += 1;
        }
    }
    return added;
}

int file_assoc_load_policy(void)
{
    static char file_buffer[FILE_ASSOC_POLICY_MAX_BYTES];
    int fd = (int)open_mode(FILE_ASSOC_POLICY_PATH, SAVANXP_OPEN_READ);
    long total = 0;

    g_entry_count = 0;
    if (fd < 0)
    {
        /* Sin politica no hay error: el escaneo solo decide todo. */
        return 0;
    }
    while (total < (long)(sizeof(file_buffer) - 1u))
    {
        long got = read(fd, file_buffer + total, sizeof(file_buffer) - 1u - (size_t)total);
        if (got <= 0)
        {
            break;
        }
        total += got;
    }
    (void)close(fd);
    if (total <= 0)
    {
        return 0;
    }
    return file_assoc_parse_policy(file_buffer, (size_t)total);
}

/* --- escaneo de capacidades ----------------------------------------------- */

static int default_path_exists(const char *path)
{
    long fd = open_mode(path, SAVANXP_OPEN_READ);

    if (fd < 0)
    {
        return 0;
    }
    (void)close((int)fd);
    return 1;
}

/* Lee las extensiones que declara un binario y las registra. */
static int scan_program(const char *path, void *meta_buffer, size_t meta_capacity)
{
    struct sxe_meta meta;
    char extension[FILE_ASSOC_EXT_CAPACITY];
    int added = 0;
    int index = 0;
    int count = 0;

    if (sxe_load_meta(path, meta_buffer, meta_capacity, &meta) != SXE_OK)
    {
        return 0;
    }
    count = sxe_meta_list_count(&meta, SXE_TAG_EXT_OPEN);
    for (index = 0; index < count; ++index)
    {
        if (sxe_meta_list_at(&meta, SXE_TAG_EXT_OPEN, index, extension, sizeof(extension)) == 0u)
        {
            continue;
        }
        if (set_entry(extension, path, 0))
        {
            added += 1;
        }
    }
    return added;
}

static int scan_directory(const char *directory, file_assoc_path_exists_fn exists, void *meta_buffer, size_t meta_capacity)
{
    char full_path[FILE_ASSOC_PATH_CAPACITY];
    DIR *handle = opendir(directory);
    struct dirent *entry = 0;
    int added = 0;

    if (handle == 0)
    {
        return 0;
    }
    while ((entry = readdir(handle)) != 0)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }
        {
            int written = snprintf(full_path, sizeof(full_path), "%s/%s", directory, entry->d_name);
            /* Un path truncado apuntaria a OTRO archivo, no al que se quiso:
             * se descarta en vez de abrirlo. */
            if (written < 0 || (size_t)written >= sizeof(full_path))
            {
                continue;
            }
        }
        if (exists != 0 && !exists(full_path))
        {
            continue;
        }
        g_last_scan_examined += 1;
        added += scan_program(full_path, meta_buffer, meta_capacity);
    }
    closedir(handle);
    return added;
}

int file_assoc_scan_programs(file_assoc_path_exists_fn exists)
{
    /* Estatico y no en el stack, igual que el resto de los scratch de userland:
     * son 4 KiB reusados por cada binario del escaneo. */
    _Alignas(4) static uint8_t meta_buffer[SXE_META_MAX_BYTES];
    int added = 0;

    g_last_scan_examined = 0;
    if (exists == 0)
    {
        exists = default_path_exists;
    }
    added += scan_directory(FILE_ASSOC_SCAN_DIR_PRIMARY, exists, meta_buffer, sizeof(meta_buffer));
    added += scan_directory(FILE_ASSOC_SCAN_DIR_SECONDARY, exists, meta_buffer, sizeof(meta_buffer));
    return added;
}

int file_assoc_scan_examined(void)
{
    return g_last_scan_examined;
}

int file_assoc_load(void)
{
    (void)file_assoc_load_policy();
    (void)file_assoc_scan_programs(0);
    return g_entry_count;
}

/* --- consulta ------------------------------------------------------------- */

const char *file_assoc_program_for_file(const char *file_path)
{
    char extension[FILE_ASSOC_EXT_CAPACITY];
    int index;

    if (file_assoc_extension_of(file_path, extension, sizeof(extension)) == 0)
    {
        return 0;
    }
    index = find_entry(extension);
    if (index < 0)
    {
        return 0;
    }
    return g_entries[index].program;
}

int file_assoc_count(void)
{
    return g_entry_count;
}

const struct file_assoc_entry *file_assoc_at(int index)
{
    if (index < 0 || index >= g_entry_count)
    {
        return 0;
    }
    return &g_entries[index];
}

/* --- selftest ------------------------------------------------------------- */

static int g_selftest_failures = 0;

static void expect(int condition, const char *label)
{
    if (!condition)
    {
        printf("FILESAPP SMOKE FAIL %s\n", label);
        g_selftest_failures += 1;
    }
}

static void selftest_extension_of(void)
{
    char extension[FILE_ASSOC_EXT_CAPACITY];

    expect(file_assoc_extension_of("/disk/notas.txt", extension, sizeof(extension)) != 0 &&
        strcmp(extension, ".txt") == 0, "extension simple");
    /* Mayusculas normalizadas: un .TXT tiene que resolver como un .txt. */
    expect(file_assoc_extension_of("/disk/NOTAS.TXT", extension, sizeof(extension)) != 0 &&
        strcmp(extension, ".txt") == 0, "extension en mayusculas");
    /* Solo cuenta el ultimo componente. */
    expect(file_assoc_extension_of("/disk/v1.2/README", extension, sizeof(extension)) == 0,
        "punto en un directorio no es extension");
    expect(file_assoc_extension_of("/bin/progman", extension, sizeof(extension)) == 0,
        "binario sin extension");
    expect(file_assoc_extension_of("/disk/archivo.", extension, sizeof(extension)) == 0,
        "punto final sin extension");
    expect(file_assoc_extension_of("/disk/.oculto", extension, sizeof(extension)) != 0 &&
        strcmp(extension, ".oculto") == 0, "archivo que empieza con punto");
    expect(file_assoc_extension_of(0, extension, sizeof(extension)) == 0, "path nulo");
    /* Extension mas larga que la capacidad: se trunca, no desborda. */
    expect(file_assoc_extension_of("/disk/x.extensionmuylargaquenoentra", extension, sizeof(extension)) != 0,
        "extension larga truncada");
    expect(strlen(extension) == sizeof(extension) - 1u, "truncado a la capacidad");
}

static void selftest_policy(void)
{
    static const char kText[] =
        "# comentario\n"
        "; otro comentario\n"
        ".txt=/bin/notepad\n"
        "  .md  =  /bin/notepad  \n"
        "cfg=/bin/notepad\n"          /* sin punto: se acepta igual */
        ".BAT=/bin/shellapp\n"        /* mayusculas: se normaliza */
        "sin_igual\n"                 /* linea invalida: se ignora */
        ".rel=relativo/programa\n"    /* path relativo: se descarta */
        ".vacio=\n";                  /* sin programa: se descarta */

    expect(file_assoc_parse_policy(kText, sizeof(kText) - 1) == 4, "politica: 4 entradas validas");
    expect(file_assoc_count() == 4, "politica: cuenta");
    expect(file_assoc_program_for_file("/disk/a.txt") != 0 &&
        strcmp(file_assoc_program_for_file("/disk/a.txt"), "/bin/notepad") == 0, "politica: .txt");
    expect(file_assoc_program_for_file("/disk/a.md") != 0 &&
        strcmp(file_assoc_program_for_file("/disk/a.md"), "/bin/notepad") == 0, "politica: espacios recortados");
    expect(file_assoc_program_for_file("/disk/a.cfg") != 0, "politica: extension sin punto");
    expect(file_assoc_program_for_file("/disk/a.bat") != 0 &&
        strcmp(file_assoc_program_for_file("/disk/a.bat"), "/bin/shellapp") == 0, "politica: normaliza mayusculas");
    expect(file_assoc_program_for_file("/disk/a.rel") == 0, "politica: descarta path relativo");
    expect(file_assoc_program_for_file("/disk/a.vacio") == 0, "politica: descarta programa vacio");
    expect(file_assoc_program_for_file("/disk/a.desconocida") == 0, "politica: extension no declarada");
    expect(file_assoc_program_for_file("/bin/progman") == 0, "politica: archivo sin extension");

    /* Fuera de rango devuelve 0 en vez de basura. */
    expect(file_assoc_at(-1) == 0 && file_assoc_at(file_assoc_count()) == 0, "politica: bounds");

    /* Texto vacio deja el registro vacio, no lo rompe. */
    expect(file_assoc_parse_policy("", 0) == 0 && file_assoc_count() == 0, "politica vacia");
}

static void selftest_precedence(void)
{
    static const char kText[] = ".txt=/bin/shellapp\n";
    const struct file_assoc_entry *entry = 0;

    /*
     * La politica gana sobre lo que declaren los binarios. notepad declara
     * .txt en su manifiesto, asi que sin este orden el escaneo se lo llevaria.
     */
    expect(file_assoc_parse_policy(kText, sizeof(kText) - 1) == 1, "precedencia: fixture");
    (void)file_assoc_scan_programs(0);
    expect(file_assoc_program_for_file("/disk/a.txt") != 0 &&
        strcmp(file_assoc_program_for_file("/disk/a.txt"), "/bin/shellapp") == 0,
        "precedencia: la politica le gana al binario");

    entry = file_assoc_at(0);
    expect(entry != 0 && entry->from_policy == 1, "precedencia: marcada como politica");
}

static int deny_all(const char *path)
{
    (void)path;
    return 0;
}

static void selftest_scan(void)
{
    const char *program = 0;

    /*
     * Escaneo contra los binarios REALES de la imagen. notepad declara
     * ext_open=.txt,.ini,.cfg,.md en su manifiesto: si el estampado o el
     * lector se rompieran, esto se cae. Es el unico check que ata la
     * declaracion del .sxres con la resolucion en vivo.
     */
    expect(file_assoc_parse_policy("", 0) == 0, "escaneo: sin politica previa");
    expect(file_assoc_scan_programs(0) > 0, "escaneo: alguna extension declarada");

    program = file_assoc_program_for_file("/disk/notas.txt");
    expect(program != 0 && strcmp(program, "/bin/notepad") == 0, "escaneo: .txt lo toma notepad");
    program = file_assoc_program_for_file("/disk/config.ini");
    expect(program != 0 && strcmp(program, "/bin/notepad") == 0, "escaneo: .ini lo toma notepad");
    program = file_assoc_program_for_file("/disk/algo.md");
    expect(program != 0 && strcmp(program, "/bin/notepad") == 0, "escaneo: .md lo toma notepad");

    /* Una extension que nadie declara sigue sin duenio. */
    expect(file_assoc_program_for_file("/disk/algo.xyz") == 0, "escaneo: extension sin duenio");

    /* El escaneo tiene que ser idempotente: correrlo dos veces no puede
     * duplicar entradas ni cambiar a quien resuelve. */
    {
        int before = file_assoc_count();

        (void)file_assoc_scan_programs(0);
        expect(file_assoc_count() == before, "escaneo: idempotente");
        program = file_assoc_program_for_file("/disk/notas.txt");
        expect(program != 0 && strcmp(program, "/bin/notepad") == 0, "escaneo: idempotente conserva");
    }

    /* Con un predicado que niega todo, no se abre ningun binario y no queda
     * ninguna asociacion: prueba que la inyeccion corta de verdad el escaneo. */
    expect(file_assoc_parse_policy("", 0) == 0, "escaneo: reset");
    expect(file_assoc_scan_programs(deny_all) == 0, "escaneo: predicado que niega todo");
    expect(file_assoc_count() == 0, "escaneo: sin entradas con predicado falso");
    expect(file_assoc_scan_examined() == 0, "escaneo: no abrio ningun binario");
}

int file_assoc_selftest(void)
{
    g_selftest_failures = 0;

    selftest_extension_of();
    selftest_policy();
    selftest_scan();
    selftest_precedence();

    return g_selftest_failures;
}
