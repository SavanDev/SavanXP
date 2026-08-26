#include "progman_registry.h"

#include "savanxp/sxe.h"

/* Las apps de diagnostico se compilan solo si el build las pide, igual que en
 * windowd_menu.c: -NoTestApps las saca del rootfs y de los defaults a la vez. */
#ifndef DESKTOP_INCLUDE_TEST_APPS
#define DESKTOP_INCLUDE_TEST_APPS 1
#endif

/* Grupo al que van los items declarados antes de cualquier [group]. */
#define PROGMAN_IMPLICIT_GROUP_NAME "Programs"

/* current_group cuando se agoto la capacidad de grupos: los items que caen bajo
 * el se descartan en vez de reasignarse a un grupo equivocado. */
#define PROGMAN_GROUP_NONE (-1)
#define PROGMAN_GROUP_DROPPED (-2)

static struct progman_group g_groups[PROGMAN_MAX_GROUPS];
static struct progman_item g_items[PROGMAN_MAX_ITEMS];
static int g_group_count = 0;
static int g_item_count = 0;
static int g_source = PROGMAN_REGISTRY_SOURCE_DEFAULTS;

/*
 * Pool de iconos traidos de los .sxicon de los binarios.
 *
 * Cuesta PROGMAN_MAX_ITEMS * 32 * 32 * 4 = 192 KiB de BSS, y es una eleccion
 * consciente: sin malloc hay que reservar el peor caso, y 192 KiB al lado de
 * los 8 MiB de arena que el proceso ya tiene mapeados es ruido. La alternativa
 * -- releer el .sxicon cuando hay que pintar -- pondria I/O de disco adentro
 * del ciclo de repintado, que es exactamente donde no va.
 *
 * Se llena una sola vez, en progman_registry_apply_sxe(), DESPUES del pruning:
 * el pruning mueve items de indice y dejaria los slots apuntando al item
 * equivocado.
 */
#define PROGMAN_ICON_MAX_EXTENT 32u
#define PROGMAN_ICON_SLOT_PIXELS (PROGMAN_ICON_MAX_EXTENT * PROGMAN_ICON_MAX_EXTENT)

static uint32_t g_icon_pixels[PROGMAN_MAX_ITEMS][PROGMAN_ICON_SLOT_PIXELS];
static struct desktop_embedded_bitmap g_icon_bitmaps[PROGMAN_MAX_ITEMS];

struct progman_default_item
{
    const char *group;
    const char *name;
    const char *path;
    const char *description;
    uint32_t icon_id;
    uint32_t launch_flags;
};

/*
 * Defaults horneados: siembran una instalacion fresca y son la red de seguridad
 * si /disk/progman.ini falta o esta corrupto. Duplican transitoriamente el
 * contenido de k_menu_items (windowd_menu.c); esa tabla se retira en A2.4 y este
 * pasa a ser el unico catalogo.
 */
static const struct progman_default_item k_default_items[] = {
    {"Main", "Shell", "/bin/shellapp", "Terminal and builtins", DESKTOP_ICON_SHELL, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Main", "Files", "/bin/filesapp", "Browse /disk and preview files", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Main", "Notepad", "/bin/notepad", "Edit text files", DESKTOP_ICON_NOTEPAD, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Main", "About", "/bin/aboutapp", "System overview and help", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Games", "Doom", "/disk/bin/doomgeneric", "Classic FPS test port", DESKTOP_ICON_DOOM, SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN},
#if DESKTOP_INCLUDE_TEST_APPS
    {"Diagnostics", "Widgets", "/bin/widgetsdemo", "sxgui control gallery", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Diagnostics", "Gfx Demo", "/bin/gfxdemo", "2D rendering test", DESKTOP_ICON_GFX_DEMO, SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN},
    {"Diagnostics", "Key Test", "/bin/keytest", "Keyboard diagnostics", DESKTOP_ICON_KEY_TEST, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Diagnostics", "Mouse Test", "/bin/mousetest", "Mouse diagnostics", DESKTOP_ICON_MOUSE_TEST, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
#endif
};

static int default_item_count(void)
{
    return (int)(sizeof(k_default_items) / sizeof(k_default_items[0]));
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

/* Recorta espacios al principio y al final, mutando el buffer de linea. */
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

static int text_contains_word(const char *text, const char *word)
{
    size_t word_length;

    if (text == 0 || word == 0)
    {
        return 0;
    }
    word_length = strlen(word);
    if (word_length == 0)
    {
        return 0;
    }
    while (*text != '\0')
    {
        if (strncmp(text, word, word_length) == 0)
        {
            return 1;
        }
        text += 1;
    }
    return 0;
}

static uint32_t icon_id_from_name(const char *name)
{
    if (name != 0)
    {
        if (strcmp(name, "shell") == 0)
        {
            return DESKTOP_ICON_SHELL;
        }
        if (strcmp(name, "doom") == 0)
        {
            return DESKTOP_ICON_DOOM;
        }
        if (strcmp(name, "gfxdemo") == 0)
        {
            return DESKTOP_ICON_GFX_DEMO;
        }
        if (strcmp(name, "keytest") == 0)
        {
            return DESKTOP_ICON_KEY_TEST;
        }
        if (strcmp(name, "mousetest") == 0)
        {
            return DESKTOP_ICON_MOUSE_TEST;
        }
        if (strcmp(name, "notepad") == 0)
        {
            return DESKTOP_ICON_NOTEPAD;
        }
    }
    /* Cubre "desktop" y cualquier nombre desconocido. */
    return DESKTOP_ICON_DESKTOP;
}

static uint32_t launch_flags_from_text(const char *text)
{
    uint32_t flags = SAVANXP_DESKTOP_LAUNCH_FLAG_NONE;

    if (text_contains_word(text, "fullscreen"))
    {
        flags |= SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN;
    }
    return flags;
}

static void reset_registry(void)
{
    g_group_count = 0;
    g_item_count = 0;
}

static int append_group(const char *name)
{
    struct progman_group *group = 0;

    if (g_group_count >= PROGMAN_MAX_GROUPS)
    {
        return PROGMAN_GROUP_DROPPED;
    }
    group = &g_groups[g_group_count];
    copy_field(group->name, sizeof(group->name), name);
    group->item_count = 0;
    g_group_count += 1;
    return g_group_count - 1;
}

static int find_group(const char *name)
{
    int index;

    for (index = 0; index < g_group_count; ++index)
    {
        if (strcmp(g_groups[index].name, name) == 0)
        {
            return index;
        }
    }
    return PROGMAN_GROUP_NONE;
}

/* Un item solo entra si tiene un path absoluto: una entrada sin path (o con uno
 * relativo) no se puede lanzar, asi que se descarta en vez de mostrarse rota. */
static int append_item(const struct progman_item *item)
{
    if (item == 0 || g_item_count >= PROGMAN_MAX_ITEMS)
    {
        return 0;
    }
    if (item->path[0] != '/')
    {
        return 0;
    }
    if (item->group_index < 0 || item->group_index >= g_group_count)
    {
        return 0;
    }
    g_items[g_item_count] = *item;
    g_groups[item->group_index].item_count += 1;
    g_item_count += 1;
    return 1;
}

static void begin_item(struct progman_item *pending, int group_index)
{
    memset(pending, 0, sizeof(*pending));
    pending->icon_id = DESKTOP_ICON_DESKTOP;
    pending->launch_flags = SAVANXP_DESKTOP_LAUNCH_FLAG_NONE;
    pending->overrides = PROGMAN_OVERRIDE_NONE;
    pending->icon_slot = PROGMAN_ICON_SLOT_NONE;
    pending->group_index = group_index;
}

void progman_registry_load_defaults(void)
{
    int index;

    reset_registry();
    for (index = 0; index < default_item_count(); ++index)
    {
        const struct progman_default_item *source = &k_default_items[index];
        struct progman_item item;
        int group_index = find_group(source->group);

        if (group_index == PROGMAN_GROUP_NONE)
        {
            group_index = append_group(source->group);
        }
        if (group_index < 0)
        {
            continue;
        }

        begin_item(&item, group_index);
        copy_field(item.name, sizeof(item.name), source->name);
        copy_field(item.path, sizeof(item.path), source->path);
        copy_field(item.description, sizeof(item.description), source->description);
        item.icon_id = source->icon_id;
        item.launch_flags = source->launch_flags;
        (void)append_item(&item);
    }
    g_source = PROGMAN_REGISTRY_SOURCE_DEFAULTS;
}

int progman_registry_parse(const char *text, size_t length)
{
    static char buffer[PROGMAN_REGISTRY_MAX_BYTES];
    struct progman_item pending;
    size_t copy_length = 0;
    size_t offset = 0;
    int current_group = PROGMAN_GROUP_NONE;
    int has_pending = 0;

    reset_registry();
    if (text == 0 || length == 0)
    {
        return 0;
    }

    copy_length = length < (sizeof(buffer) - 1u) ? length : (sizeof(buffer) - 1u);
    memcpy(buffer, text, copy_length);
    buffer[copy_length] = '\0';
    begin_item(&pending, PROGMAN_GROUP_NONE);

    while (offset < copy_length)
    {
        size_t start = offset;
        size_t end = offset;
        char *line = 0;
        char *separator = 0;
        size_t scan = 0;

        while (offset < copy_length && buffer[offset] != '\n')
        {
            offset += 1u;
        }
        end = offset;
        if (offset < copy_length)
        {
            offset += 1u; /* saltear el '\n' */
        }
        buffer[end] = '\0';

        line = trim(&buffer[start]);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
        {
            continue;
        }

        if (line[0] == '[')
        {
            /* Cambio de seccion: cerrar el item que estuviera abierto. */
            if (has_pending)
            {
                (void)append_item(&pending);
                has_pending = 0;
            }
            if (strcmp(line, "[group]") == 0)
            {
                current_group = append_group("");
            }
            else if (strcmp(line, "[item]") == 0)
            {
                if (current_group == PROGMAN_GROUP_NONE)
                {
                    /* Items antes del primer [group]: grupo implicito. */
                    current_group = append_group(PROGMAN_IMPLICIT_GROUP_NAME);
                }
                begin_item(&pending, current_group);
                has_pending = 1;
            }
            /* Secciones desconocidas: se ignoran (compat hacia adelante). */
            continue;
        }

        /* clave=valor */
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
        {
            char *key = trim(line);
            char *value = trim(separator + 1);

            if (has_pending)
            {
                /*
                 * Cada clave presente marca su bit de override: es lo que
                 * despues le dice a apply_sxe que ese campo lo eligio el
                 * usuario y no debe pisarse con lo que declare el binario.
                 */
                if (strcmp(key, "name") == 0)
                {
                    copy_field(pending.name, sizeof(pending.name), value);
                    pending.overrides |= PROGMAN_OVERRIDE_NAME;
                }
                else if (strcmp(key, "path") == 0)
                {
                    copy_field(pending.path, sizeof(pending.path), value);
                }
                else if (strcmp(key, "desc") == 0)
                {
                    copy_field(pending.description, sizeof(pending.description), value);
                    pending.overrides |= PROGMAN_OVERRIDE_DESCRIPTION;
                }
                else if (strcmp(key, "icon") == 0)
                {
                    pending.icon_id = icon_id_from_name(value);
                    pending.overrides |= PROGMAN_OVERRIDE_ICON;
                }
                else if (strcmp(key, "flags") == 0)
                {
                    pending.launch_flags = launch_flags_from_text(value);
                    pending.overrides |= PROGMAN_OVERRIDE_FLAGS;
                }
                /* Claves desconocidas: ignoradas. */
            }
            else if (current_group >= 0 && strcmp(key, "name") == 0)
            {
                copy_field(g_groups[current_group].name, sizeof(g_groups[current_group].name), value);
            }
        }
    }

    if (has_pending)
    {
        (void)append_item(&pending);
    }

    g_source = PROGMAN_REGISTRY_SOURCE_FILE;
    return g_item_count;
}

void progman_registry_load(void)
{
    static char file_buffer[PROGMAN_REGISTRY_MAX_BYTES];
    int fd = (int)open_mode(PROGMAN_REGISTRY_PATH, SAVANXP_OPEN_READ);
    size_t total = 0;

    if (fd >= 0)
    {
        for (;;)
        {
            long result = read(fd, file_buffer + total, sizeof(file_buffer) - total);
            if (result <= 0)
            {
                break;
            }
            total += (size_t)result;
            if (total >= sizeof(file_buffer))
            {
                break;
            }
        }
        (void)close(fd);
    }

    if (total != 0 && progman_registry_parse(file_buffer, total) > 0)
    {
        return;
    }
    /* Sin archivo, ilegible, o sin ningun item valido: defaults horneados. El
     * launcher nunca queda vacio. */
    progman_registry_load_defaults();
}

int progman_registry_prune_missing(progman_path_exists_fn exists)
{
    int group_remap[PROGMAN_MAX_GROUPS];
    int read_index;
    int write_index = 0;
    int group_index;
    int surviving_groups = 0;
    int dropped = 0;

    if (exists == 0)
    {
        return 0;
    }

    /* 1) Compactar los items que sobreviven, recontando por grupo. */
    for (group_index = 0; group_index < g_group_count; ++group_index)
    {
        g_groups[group_index].item_count = 0;
    }

    for (read_index = 0; read_index < g_item_count; ++read_index)
    {
        if (!exists(g_items[read_index].path))
        {
            dropped += 1;
            continue;
        }

        g_items[write_index] = g_items[read_index];
        g_groups[g_items[write_index].group_index].item_count += 1;
        write_index += 1;
    }
    g_item_count = write_index;

    /* 2) Compactar los grupos que quedaron vacios, anotando el remapeo. */
    for (group_index = 0; group_index < g_group_count; ++group_index)
    {
        if (g_groups[group_index].item_count == 0)
        {
            group_remap[group_index] = PROGMAN_GROUP_NONE;
            continue;
        }

        group_remap[group_index] = surviving_groups;
        if (surviving_groups != group_index)
        {
            g_groups[surviving_groups] = g_groups[group_index];
        }
        surviving_groups += 1;
    }
    g_group_count = surviving_groups;

    /* 3) Reapuntar los items a los indices nuevos. Ningun item sobreviviente
     * puede apuntar a un grupo descartado: un grupo se descarta justamente
     * porque se quedo sin items. */
    for (read_index = 0; read_index < g_item_count; ++read_index)
    {
        g_items[read_index].group_index = group_remap[g_items[read_index].group_index];
    }

    return dropped;
}

/* --- recursos SXE --------------------------------------------------------- */

/*
 * Copia el mejor icono del blob al slot del item. Solo acepta imagenes
 * cuadradas que entren en el slot: agrandar el pool para un icono gigante
 * costaria memoria en TODOS los slots, y la grilla dibuja a 32 igual.
 */
static int adopt_icon(struct progman_item *item, int slot, const struct sxe_icons *icons)
{
    const struct sxe_icon_entry *entry = sxe_icons_best(icons, PROGMAN_ICON_MAX_EXTENT);
    const uint32_t *pixels = 0;

    if (entry == 0 || entry->width != entry->height || entry->width > PROGMAN_ICON_MAX_EXTENT)
    {
        return 0;
    }
    pixels = sxe_icons_pixels(icons, entry);
    if (pixels == 0)
    {
        return 0;
    }

    memcpy(g_icon_pixels[slot], pixels, (size_t)entry->width * entry->height * sizeof(uint32_t));
    g_icon_bitmaps[slot].width = entry->width;
    g_icon_bitmaps[slot].height = entry->height;
    g_icon_bitmaps[slot].pixels = g_icon_pixels[slot];
    item->icon_slot = slot;
    return 1;
}

static int apply_sxe_to_item(struct progman_item *item, int slot, void *meta_buffer, size_t meta_capacity, void *icon_buffer, size_t icon_capacity)
{
    struct sxe_meta meta;
    struct sxe_icons icons;
    /* Del tamano del campo mas grande que se copia desde el blob. */
    char text[PROGMAN_DESC_CAPACITY];
    uint32_t value = 0;
    int touched = 0;

    if (sxe_load_meta(item->path, meta_buffer, meta_capacity, &meta) == SXE_OK)
    {
        /*
         * Al buffer temporal y no directo al campo: sxe_meta_string vacia el
         * destino ANTES de buscar el tag, asi que escribir en item->name
         * borraria el default cuando el binario no declara nombre.
         */
        if ((item->overrides & PROGMAN_OVERRIDE_NAME) == 0 &&
            sxe_meta_string(&meta, SXE_TAG_NAME, text, sizeof(text)) > 0u)
        {
            copy_field(item->name, sizeof(item->name), text);
            touched = 1;
        }
        if ((item->overrides & PROGMAN_OVERRIDE_DESCRIPTION) == 0 &&
            sxe_meta_string(&meta, SXE_TAG_DESCRIPTION, text, sizeof(text)) > 0u)
        {
            copy_field(item->description, sizeof(item->description), text);
            touched = 1;
        }
        if ((item->overrides & PROGMAN_OVERRIDE_FLAGS) == 0 &&
            sxe_meta_u32(&meta, SXE_TAG_LAUNCH_FLAGS, &value))
        {
            item->launch_flags = value;
            touched = 1;
        }
    }

    if ((item->overrides & PROGMAN_OVERRIDE_ICON) == 0 &&
        sxe_load_icons(item->path, icon_buffer, icon_capacity, &icons) == SXE_OK &&
        adopt_icon(item, slot, &icons))
    {
        touched = 1;
    }

    return touched;
}

int progman_registry_apply_sxe(void)
{
    /*
     * Estaticos y no en el stack: son 12 KiB de scratch y esta libc corre con
     * un stack acotado. Es el mismo criterio que el buffer de parseo de
     * progman_registry_parse().
     */
    _Alignas(4) static uint8_t meta_buffer[SXE_META_MAX_BYTES];
    /*
     * El scratch de iconos va al TOPE DEL FORMATO y no al tamano que hoy
     * emiten los manifiestos. Ajustarlo a "16 + 32 y listo" haria que un
     * binario que ademas trae 48x48 se pase de capacidad y se quede sin icono
     * -- degradado silencioso, y por un blob perfectamente valido. Son 64 KiB
     * de scratch compartido por todos los items, una sola vez.
     */
    _Alignas(4) static uint8_t icon_buffer[SXE_ICON_MAX_BYTES];
    int index;
    int touched = 0;

    for (index = 0; index < g_item_count; ++index)
    {
        g_items[index].icon_slot = PROGMAN_ICON_SLOT_NONE;
    }
    for (index = 0; index < g_item_count; ++index)
    {
        if (apply_sxe_to_item(&g_items[index], index, meta_buffer, sizeof(meta_buffer), icon_buffer, sizeof(icon_buffer)))
        {
            touched += 1;
        }
    }
    return touched;
}

const struct desktop_embedded_bitmap *progman_item_icon(const struct progman_item *item)
{
    if (item == 0 || item->icon_slot < 0 || item->icon_slot >= PROGMAN_MAX_ITEMS)
    {
        return 0;
    }
    if (g_icon_bitmaps[item->icon_slot].pixels == 0)
    {
        return 0;
    }
    return &g_icon_bitmaps[item->icon_slot];
}

int progman_registry_source(void)
{
    return g_source;
}

int progman_group_count(void)
{
    return g_group_count;
}

const struct progman_group *progman_group_at(int index)
{
    if (index < 0 || index >= g_group_count)
    {
        return 0;
    }
    return &g_groups[index];
}

int progman_item_count(void)
{
    return g_item_count;
}

const struct progman_item *progman_item_at(int index)
{
    if (index < 0 || index >= g_item_count)
    {
        return 0;
    }
    return &g_items[index];
}

const struct progman_item *progman_group_item_at(int group_index, int item_index)
{
    int index;
    int seen = 0;

    if (group_index < 0 || group_index >= g_group_count || item_index < 0)
    {
        return 0;
    }
    for (index = 0; index < g_item_count; ++index)
    {
        if (g_items[index].group_index != group_index)
        {
            continue;
        }
        if (seen == item_index)
        {
            return &g_items[index];
        }
        seen += 1;
    }
    return 0;
}

/* --- selftest ----------------------------------------------------------- */

static int g_selftest_failures = 0;

static void expect(int condition, const char *label)
{
    if (!condition)
    {
        printf("PROGMAN SMOKE FAIL %s\n", label);
        g_selftest_failures += 1;
    }
}

static size_t append_text(char *destination, size_t capacity, size_t offset, const char *text)
{
    size_t index = 0;

    while (text != 0 && text[index] != '\0' && offset + 1u < capacity)
    {
        destination[offset] = text[index];
        offset += 1u;
        index += 1u;
    }
    destination[offset] = '\0';
    return offset;
}

static void selftest_defaults(void)
{
    int index;
    int paths_ok = 1;
    int groups_ok = 1;

    progman_registry_load_defaults();
    expect(progman_item_count() > 0, "defaults sin items");
    expect(progman_group_count() > 0, "defaults sin grupos");
    expect(progman_registry_source() == PROGMAN_REGISTRY_SOURCE_DEFAULTS, "defaults source");

    for (index = 0; index < progman_item_count(); ++index)
    {
        const struct progman_item *item = progman_item_at(index);
        if (item == 0 || item->path[0] != '/')
        {
            paths_ok = 0;
        }
        if (item == 0 || item->group_index < 0 || item->group_index >= progman_group_count())
        {
            groups_ok = 0;
        }
    }
    expect(paths_ok, "defaults con path invalido");
    expect(groups_ok, "defaults con group_index invalido");
    /* Fuera de rango devuelve NULL en vez de basura. */
    expect(progman_item_at(-1) == 0 && progman_item_at(progman_item_count()) == 0, "defaults bounds item");
    expect(progman_group_at(-1) == 0 && progman_group_at(progman_group_count()) == 0, "defaults bounds group");
}

static void selftest_well_formed(void)
{
    static const char kText[] =
        "# comentario\n"
        "; otro comentario\n"
        "\n"
        "[group]\n"
        "name=Main\n"
        "\n"
        "[item]\n"
        "name=Shell\n"
        "path=/bin/shellapp\n"
        "desc=Terminal\n"
        "icon=shell\n"
        "\n"
        "[item]\n"
        "name = Doom\n"
        "path =  /disk/bin/doomgeneric  \n"
        "icon=doom\n"
        "flags=fullscreen\n"
        "\n"
        "[group]\n"
        "name=Diagnostics\n"
        "\n"
        "[item]\n"
        "name=Keys\n"
        "path=/bin/keytest\n"
        "icon=keytest\n";
    const struct progman_item *item = 0;
    const struct progman_group *group = 0;

    expect(progman_registry_parse(kText, sizeof(kText) - 1u) == 3, "parse count");
    expect(progman_group_count() == 2, "parse group count");
    expect(progman_registry_source() == PROGMAN_REGISTRY_SOURCE_FILE, "parse source");

    group = progman_group_at(0);
    expect(group != 0 && strcmp(group->name, "Main") == 0, "grupo 0 nombre");
    expect(group != 0 && group->item_count == 2, "grupo 0 item_count");
    group = progman_group_at(1);
    expect(group != 0 && strcmp(group->name, "Diagnostics") == 0, "grupo 1 nombre");
    expect(group != 0 && group->item_count == 1, "grupo 1 item_count");

    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->name, "Shell") == 0, "item 0 nombre");
    expect(item != 0 && strcmp(item->path, "/bin/shellapp") == 0, "item 0 path");
    expect(item != 0 && strcmp(item->description, "Terminal") == 0, "item 0 desc");
    expect(item != 0 && item->icon_id == DESKTOP_ICON_SHELL, "item 0 icono");
    expect(item != 0 && item->launch_flags == SAVANXP_DESKTOP_LAUNCH_FLAG_NONE, "item 0 flags");

    /* Espacios alrededor de clave y valor recortados. */
    item = progman_item_at(1);
    expect(item != 0 && strcmp(item->name, "Doom") == 0, "item 1 nombre trim");
    expect(item != 0 && strcmp(item->path, "/disk/bin/doomgeneric") == 0, "item 1 path trim");
    expect(item != 0 && item->icon_id == DESKTOP_ICON_DOOM, "item 1 icono");
    expect(item != 0 && item->launch_flags == SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN, "item 1 flag fullscreen");

    /* Indexado por grupo. */
    item = progman_group_item_at(1, 0);
    expect(item != 0 && strcmp(item->name, "Keys") == 0, "group_item_at");
    expect(progman_group_item_at(1, 1) == 0, "group_item_at fuera de rango");
}

static void selftest_crlf_and_unknown(void)
{
    static const char kText[] =
        "[group]\r\n"
        "name=Main\r\n"
        "[unknown-section]\r\n"
        "ignored=yes\r\n"
        "[item]\r\n"
        "name=Files\r\n"
        "path=/bin/filesapp\r\n"
        "futuro=algo\r\n"
        "icon=inexistente\r\n";
    const struct progman_item *item = 0;

    expect(progman_registry_parse(kText, sizeof(kText) - 1u) == 1, "crlf parse count");
    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->name, "Files") == 0, "crlf nombre");
    expect(item != 0 && strcmp(item->path, "/bin/filesapp") == 0, "crlf path");
    /* Icono desconocido cae al generico en vez de romper. */
    expect(item != 0 && item->icon_id == DESKTOP_ICON_DESKTOP, "icono desconocido -> generico");
}

static void selftest_invalid_items(void)
{
    static const char kText[] =
        "[group]\n"
        "name=Main\n"
        "[item]\n"
        "name=SinPath\n"
        "desc=no tiene path\n"
        "[item]\n"
        "name=Relativo\n"
        "path=bin/relativo\n"
        "[item]\n"
        "name=Valido\n"
        "path=/bin/valido\n";
    const struct progman_item *item = 0;

    /* Solo el tercero es lanzable. */
    expect(progman_registry_parse(kText, sizeof(kText) - 1u) == 1, "items invalidos descartados");
    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->name, "Valido") == 0, "sobrevive el item valido");
}

static void selftest_implicit_group(void)
{
    static const char kText[] =
        "[item]\n"
        "name=Huerfano\n"
        "path=/bin/huerfano\n";
    const struct progman_group *group = 0;

    expect(progman_registry_parse(kText, sizeof(kText) - 1u) == 1, "item sin grupo");
    expect(progman_group_count() == 1, "grupo implicito creado");
    group = progman_group_at(0);
    expect(group != 0 && strcmp(group->name, PROGMAN_IMPLICIT_GROUP_NAME) == 0, "nombre del grupo implicito");
}

static void selftest_truncation(void)
{
    static char text[512];
    static const char kLongName[] =
        "NombreLarguisimoQueSuperaLaCapacidadDelCampoNombreYDebeTruncarse";
    const struct progman_item *item = 0;
    size_t offset = 0;

    offset = append_text(text, sizeof(text), offset, "[group]\nname=Main\n[item]\nname=");
    offset = append_text(text, sizeof(text), offset, kLongName);
    offset = append_text(text, sizeof(text), offset, "\npath=/bin/x\n");

    expect(progman_registry_parse(text, offset) == 1, "truncado parse");
    item = progman_item_at(0);
    /* Trunca dentro de capacidad y sigue NUL-terminado. */
    expect(item != 0 && strlen(item->name) == PROGMAN_NAME_CAPACITY - 1u, "nombre truncado a capacidad");
    expect(item != 0 && strncmp(item->name, kLongName, PROGMAN_NAME_CAPACITY - 1u) == 0, "prefijo del nombre preservado");
}

static void selftest_capacity(void)
{
    static char text[PROGMAN_REGISTRY_MAX_BYTES];
    size_t offset = 0;
    int index;

    offset = append_text(text, sizeof(text), offset, "[group]\nname=Main\n");
    for (index = 0; index < PROGMAN_MAX_ITEMS + 10; ++index)
    {
        offset = append_text(text, sizeof(text), offset, "[item]\nname=X\npath=/bin/x\n");
    }

    /* Se corta en la capacidad en vez de desbordar los arrays. */
    expect(progman_registry_parse(text, offset) == PROGMAN_MAX_ITEMS, "clamp a PROGMAN_MAX_ITEMS");
    expect(progman_item_count() == PROGMAN_MAX_ITEMS, "item_count clampeado");
}

static void selftest_empty(void)
{
    expect(progman_registry_parse(0, 0) == 0, "entrada nula");
    expect(progman_registry_parse("", 0) == 0, "entrada vacia");
    expect(progman_registry_parse("# solo comentarios\n", 19) == 0, "solo comentarios");
    /* Tras una entrada vacia el registro queda vacio, y los defaults lo repueblan:
     * es el camino que toma progman_registry_load() cuando el archivo no sirve. */
    expect(progman_item_count() == 0, "registro vacio tras parse vacio");
    progman_registry_load_defaults();
    expect(progman_item_count() > 0, "defaults repueblan");
}

/* Predicado falso: solo "existen" los paths de esta lista, asi el pruning se
 * ejercita sin depender de que haya en el disco al correr el selftest. */
static int fake_exists(const char *path)
{
    static const char *const kInstalled[] = {"/bin/a2", "/bin/c1"};
    int index;

    for (index = 0; index < (int)(sizeof(kInstalled) / sizeof(kInstalled[0])); ++index)
    {
        if (strcmp(path, kInstalled[index]) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static void selftest_prune(void)
{
    static const char kText[] =
        "[group]\n"
        "name=Alpha\n"
        "[item]\n"
        "name=A1\n"
        "path=/bin/a1\n"
        "[item]\n"
        "name=A2\n"
        "path=/bin/a2\n"
        "[group]\n"
        "name=Beta\n"
        "[item]\n"
        "name=B1\n"
        "path=/bin/b1\n"
        "[group]\n"
        "name=Gamma\n"
        "[item]\n"
        "name=C1\n"
        "path=/bin/c1\n";
    const struct progman_item *item;
    const struct progman_group *group;

    expect(progman_registry_parse(kText, sizeof(kText) - 1) == 4, "prune fixture 4 items");
    expect(progman_group_count() == 3, "prune fixture 3 grupos");

    /* Se caen /bin/a1 y /bin/b1: Alpha se queda con un item y Beta, que pierde
     * el unico que tenia, desaparece entero. */
    expect(progman_registry_prune_missing(fake_exists) == 2, "prune descarta 2");
    expect(progman_item_count() == 2, "prune deja 2 items");
    expect(progman_group_count() == 2, "prune descarta el grupo vacio");

    /* Gamma tiene que haber bajado del indice 2 al 1, y su item seguirlo. */
    group = progman_group_at(0);
    expect(group != 0 && strcmp(group->name, "Alpha") == 0, "prune conserva Alpha en 0");
    group = progman_group_at(1);
    expect(group != 0 && strcmp(group->name, "Gamma") == 0, "prune corre Gamma a 1");

    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->path, "/bin/a2") == 0 && item->group_index == 0, "prune reapunta A2");
    item = progman_item_at(1);
    expect(item != 0 && strcmp(item->path, "/bin/c1") == 0 && item->group_index == 1, "prune reapunta C1");

    /* group_item_at tiene que seguir coherente con los indices nuevos. */
    item = progman_group_item_at(1, 0);
    expect(item != 0 && strcmp(item->path, "/bin/c1") == 0, "prune group_item_at tras remapeo");

    /* Un predicado nulo no toca nada; correrlo dos veces es idempotente. */
    expect(progman_registry_prune_missing(0) == 0, "prune con predicado nulo");
    expect(progman_registry_prune_missing(fake_exists) == 0, "prune idempotente");
    expect(progman_item_count() == 2 && progman_group_count() == 2, "prune idempotente conserva");
}

/*
 * Precedencia .ini > .sxe > default (docs/SXE_FORMAT.md, fase 3).
 *
 * Corre contra los binarios REALES de la imagen, que es la unica forma de
 * probar esto: el paso que se valida es justamente el que abre el ejecutable.
 * notepad esta estampado; init no lo esta y nunca lo va a estar, asi que sirve
 * de control del camino "sin recursos".
 */
static void selftest_sxe(void)
{
    static const char kText[] =
        "[group]\n"
        "name=Main\n"
        "[item]\n"
        "path=/bin/notepad\n"
        "[item]\n"
        "name=Mi Bloc\n"
        "path=/bin/notepad\n"
        "icon=doom\n"
        "[item]\n"
        "name=Arranque\n"
        "desc=PID 1\n"
        "path=/bin/init\n";
    const struct progman_item *item;
    const struct desktop_embedded_bitmap *icon;

    expect(progman_registry_parse(kText, sizeof(kText) - 1) == 3, "sxe fixture 3 items");

    /* Antes de aplicar, nadie tiene icono propio. */
    item = progman_item_at(0);
    expect(item != 0 && item->icon_slot == PROGMAN_ICON_SLOT_NONE, "sin aplicar no hay icon_slot");
    expect(progman_item_icon(item) == 0, "sin aplicar no hay bitmap");

    /* Los bits de override tienen que reflejar lo que el .ini declaro. */
    expect(item != 0 && item->overrides == PROGMAN_OVERRIDE_NONE, "item 0 sin overrides");
    item = progman_item_at(1);
    expect(item != 0 && (item->overrides & PROGMAN_OVERRIDE_NAME) != 0, "item 1 override de nombre");
    expect(item != 0 && (item->overrides & PROGMAN_OVERRIDE_ICON) != 0, "item 1 override de icono");
    expect(item != 0 && (item->overrides & PROGMAN_OVERRIDE_DESCRIPTION) == 0, "item 1 sin override de desc");

    expect(progman_registry_apply_sxe() == 2, "apply_sxe toca 2 items");

    /* Item 0: sin overrides, todo sale del binario. */
    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->name, "Notepad") == 0, "nombre desde el .sxmeta");
    expect(item != 0 && strcmp(item->description, "Edit text files") == 0, "descripcion desde el .sxmeta");
    expect(item != 0 && item->icon_slot != PROGMAN_ICON_SLOT_NONE, "icono desde el .sxicon");
    icon = progman_item_icon(item);
    expect(icon != 0 && icon->pixels != 0, "bitmap del icono disponible");
    expect(icon != 0 && icon->width == 32u && icon->height == 32u, "icono de 32x32");

    /* Item 1: mismo binario, pero el .ini gano donde hablo. */
    item = progman_item_at(1);
    expect(item != 0 && strcmp(item->name, "Mi Bloc") == 0, "el nombre del .ini le gana al .sxmeta");
    expect(item != 0 && item->icon_id == DESKTOP_ICON_DOOM, "el icono del .ini sobrevive");
    expect(item != 0 && item->icon_slot == PROGMAN_ICON_SLOT_NONE, "icono overrideado no toma el .sxicon");
    expect(progman_item_icon(item) == 0, "icono overrideado cae al set horneado");
    /* Lo que el .ini NO declaro sigue viniendo del binario. */
    expect(item != 0 && strcmp(item->description, "Edit text files") == 0, "desc sin override viene del .sxmeta");

    /* Item 2: binario sin recursos, se queda con lo declarado. */
    item = progman_item_at(2);
    expect(item != 0 && strcmp(item->name, "Arranque") == 0, "binario sin .sxe conserva el nombre");
    expect(item != 0 && strcmp(item->description, "PID 1") == 0, "binario sin .sxe conserva la desc");
    expect(item != 0 && item->icon_slot == PROGMAN_ICON_SLOT_NONE, "binario sin .sxe no tiene icono propio");

    /* Aplicar dos veces no debe duplicar ni corromper nada. */
    expect(progman_registry_apply_sxe() == 2, "apply_sxe es idempotente");
    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->name, "Notepad") == 0, "idempotente conserva el nombre");

#if DESKTOP_INCLUDE_TEST_APPS
    {
        /* gfxdemo declara launch_flags=fullscreen en su manifiesto: un item
         * que no escribe flags= tiene que heredarlo del binario. */
        static const char kFlagsText[] =
            "[item]\n"
            "path=/bin/gfxdemo\n";

        expect(progman_registry_parse(kFlagsText, sizeof(kFlagsText) - 1) == 1, "fixture de flags");
        (void)progman_registry_apply_sxe();
        item = progman_item_at(0);
        expect(item != 0 && item->launch_flags == SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN,
            "launch_flags desde el .sxmeta");
    }
#endif

    /* Un path que no existe no debe romper el paso ni dejar basura. */
    {
        static const char kMissingText[] =
            "[item]\n"
            "name=Fantasma\n"
            "path=/bin/__no_existe__\n";

        expect(progman_registry_parse(kMissingText, sizeof(kMissingText) - 1) == 1, "fixture inexistente");
        expect(progman_registry_apply_sxe() == 0, "path inexistente no toca nada");
        item = progman_item_at(0);
        expect(item != 0 && strcmp(item->name, "Fantasma") == 0, "path inexistente conserva el nombre");
        expect(progman_item_icon(item) == 0, "path inexistente sin icono");
    }
}

int progman_registry_selftest(void)
{
    g_selftest_failures = 0;

    selftest_defaults();
    selftest_prune();
    selftest_well_formed();
    selftest_crlf_and_unknown();
    selftest_invalid_items();
    selftest_implicit_group();
    selftest_truncation();
    selftest_capacity();
    selftest_empty();
    selftest_sxe();

    return g_selftest_failures;
}
