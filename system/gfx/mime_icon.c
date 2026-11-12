#include "sxgfx/mime_icon.h"

#include <stdio.h>
#include <string.h>

#include "savanxp/sxe.h"

/* --- state ---------------------------------------------------------------- */

static struct mime_icon_entry g_entries[MIME_ICON_MAX_ENTRIES];
static int g_entry_count = 0;

/* The reserved keys: they are not extensions, so they stay out of g_entries.
 * Empty means "no rule", which is not the same as "a rule whose icon is
 * missing". */
static char g_icon_folder[MIME_ICON_NAME_CAPACITY];
static char g_icon_program[MIME_ICON_NAME_CAPACITY];
static char g_icon_default[MIME_ICON_NAME_CAPACITY];

static struct mime_icon_cache_slot g_small_slots[MIME_ICON_CACHE_ENTRIES];
static uint32_t g_small_pixels[MIME_ICON_CACHE_ENTRIES * MIME_ICON_SMALL_SIZE * MIME_ICON_SMALL_SIZE];
static struct mime_icon_cache g_small_cache = {
    MIME_ICON_SMALL_SIZE, 0, g_small_slots, g_small_pixels};

static mime_icon_reader g_reader = 0;

static int g_selftest_failures = 0;
static void (*g_selftest_report)(const char *label) = 0;

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

/* A name's extension, with the dot and in lower case. 0 if it has none. */
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
        /* index > 0 leaves dotfiles out: ".bashrc" has no extension
         * ".bashrc", it has no extension at all. */
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

/* --- policy --------------------------------------------------------------- */

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

/* The last line wins: assigning an extension again replaces it instead of
 * adding a duplicate, which is what makes appending lines to the file a way
 * to edit it. */
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

        /* The first '=': a key holds none, a value may. */
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
        /* Any key that is not reserved has to be an extension, and an
         * extension starts with a dot. A bare key is a typo, and swallowing
         * it silently would hide the mistake. */
        if (key[0] != '.' || key[1] == '\0')
        {
            continue;
        }
        copy_lower(extension, sizeof(extension), key);
        added += upsert(extension, value);
    }

    return added;
}

void mime_icon_set_reader(mime_icon_reader reader)
{
    g_reader = reader;
}

int mime_icon_load_file(const char *path)
{
    static char file_buffer[MIME_ICON_POLICY_MAX_BYTES];
    long total = g_reader != 0 && path != 0 ? g_reader(path, file_buffer, sizeof(file_buffer) - 1u) : -1;

    if (total <= 0)
    {
        /* No file means no icons and no error either: the list is drawn as
         * it was before this layer existed. */
        (void)mime_icon_parse_policy(0, 0);
        return 0;
    }
    return mime_icon_parse_policy(file_buffer, (size_t)total);
}

/* --- resolution ----------------------------------------------------------- */

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

/* --- pixels --------------------------------------------------------------- */

static struct mime_icon_cache_slot *cache_find(struct mime_icon_cache *cache, const char *icon_name)
{
    int index = 0;

    for (index = 0; index < cache->count; ++index)
    {
        if (strcmp(cache->slots[index].name, icon_name) == 0)
        {
            return &cache->slots[index];
        }
    }
    return 0;
}

/* Copies the blob's image of the cache's size into the slot. 0 if the blob
 * does not carry that exact size: scaling at runtime is exactly what an
 * .sxicon with both sizes exists to avoid, so an incomplete blob is a build
 * error, not something to fix by interpolating. */
static int fill_slot(
    struct mime_icon_cache *cache, int slot_index, const struct sxe_icons *icons)
{
    struct mime_icon_cache_slot *slot = &cache->slots[slot_index];
    uint32_t *destination = cache->pixels + (size_t)slot_index * (size_t)cache->size * (size_t)cache->size;
    const struct sxe_icon_entry *entry = sxe_icons_best(icons, (uint32_t)cache->size);
    const uint32_t *pixels = 0;
    size_t count = (size_t)cache->size * (size_t)cache->size;

    if (entry == 0 || entry->width != (uint32_t)cache->size || entry->height != (uint32_t)cache->size)
    {
        return 0;
    }
    pixels = sxe_icons_pixels(icons, entry);
    if (pixels == 0)
    {
        return 0;
    }

    memcpy(destination, pixels, count * sizeof(uint32_t));
    slot->bitmap.pixels = destination;
    slot->bitmap.info.width = (uint32_t)cache->size;
    slot->bitmap.info.height = (uint32_t)cache->size;
    slot->bitmap.info.pitch = (uint32_t)cache->size * (uint32_t)sizeof(uint32_t);
    slot->bitmap.info.bpp = 32u;
    slot->bitmap.format = SX_PIXEL_FORMAT_BGRA8888;
    return 1;
}

const struct sx_bitmap *mime_icon_cache_load(struct mime_icon_cache *cache, const char *icon_name)
{
    /* One scratch, reused: parsing the blob needs the whole file in memory,
     * but only while it is copied into the slot. */
    static _Alignas(4) uint8_t scratch[MIME_ICON_SCRATCH_BYTES];
    struct mime_icon_cache_slot *slot = 0;
    struct sxe_icons icons;
    char path[MIME_ICON_NAME_CAPACITY + 48];
    long length;
    int slot_index;

    if (icon_name == 0 || icon_name[0] == '\0')
    {
        return 0;
    }
    slot = cache_find(cache, icon_name);
    if (slot != 0)
    {
        return slot->valid ? &slot->bitmap : 0;
    }
    if (cache->count >= MIME_ICON_CACHE_ENTRIES)
    {
        /* No eviction: the catalogue fits in the cache by design. If one day
         * it does not, the fix is to grow it, not to start evicting in the
         * middle of a repaint. */
        return 0;
    }

    slot_index = cache->count;
    slot = &cache->slots[slot_index];
    cache->count += 1;
    copy_field(slot->name, sizeof(slot->name), icon_name);
    slot->valid = 0;

    snprintf(path, sizeof(path), "%s/%s.sxicon", MIME_ICON_DIRECTORY, icon_name);
    length = g_reader != 0 ? g_reader(path, scratch, sizeof(scratch)) : -1;
    if (length <= 0 || sxe_icons_open(scratch, (size_t)length, &icons) != SXE_OK)
    {
        return 0;
    }
    slot->valid = fill_slot(cache, slot_index, &icons);
    return slot->valid ? &slot->bitmap : 0;
}

const struct sx_bitmap *mime_icon_small(const char *icon_name)
{
    return mime_icon_cache_load(&g_small_cache, icon_name);
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
        if (g_selftest_report != 0)
        {
            g_selftest_report(label);
        }
        g_selftest_failures += 1;
    }
}

static void selftest_parse(void)
{
    static const char k_policy[] =
        "# a comment\n"
        "; another comment\n"
        "\n"
        "folder=folder\n"
        "program=application-x-executable\n"
        "default=text-x-generic\n"
        "  .TXT  =  text-x-generic  \n"
        ".png=image-x-generic\n"
        "no-dot=should-be-ignored\n"
        ".no-value=\n"
        "line without equals\n"
        ".png=image-x-generic\n";

    const char *name = 0;

    expect(mime_icon_parse_policy(k_policy, sizeof(k_policy) - 1u) == 2,
        "parse: two new extensions");
    expect(mime_icon_count() == 2, "parse: reserved keys and junk are not counted");

    /* The key is lower-cased and its spaces trimmed. */
    name = mime_icon_name_for("notes.TXT", 0, 0);
    expect(name != 0 && strcmp(name, "text-x-generic") == 0, "parse: upper-case extension");
    name = mime_icon_name_for("capture.png", 0, 0);
    expect(name != 0 && strcmp(name, "image-x-generic") == 0, "parse: plain extension");

    /* A key without a dot is a typo and is not registered as an extension. */
    expect(mime_icon_name_for("file.no-dot", 0, 0) != 0, "parse: a typo falls to the default");
    name = mime_icon_name_for("file.unknown", 0, 0);
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

    /* A directory wins over everything, even with an extension. */
    name = mime_icon_name_for("folder.txt", 1, 0);
    expect(name != 0 && strcmp(name, "folder") == 0, "precedence: a directory wins");

    /* Launchable wins over the extension: a program without an extension
     * still shows the program icon. */
    name = mime_icon_name_for("doom", 0, 1);
    expect(name != 0 && strcmp(name, "application-x-executable") == 0,
        "precedence: launchable without an extension");
    name = mime_icon_name_for("notes.txt", 0, 1);
    expect(name != 0 && strcmp(name, "application-x-executable") == 0,
        "precedence: launchable wins over the extension");

    /* A dotfile has no extension: it falls to the default. */
    name = mime_icon_name_for(".profile", 0, 0);
    expect(name != 0 && strcmp(name, "text-x-generic") == 0, "precedence: a dotfile has no extension");

    /* The last line wins instead of adding a duplicate. */
    {
        static const char k_override[] = ".txt=text-x-generic\n.txt=x-office-document\n";

        expect(mime_icon_parse_policy(k_override, sizeof(k_override) - 1u) == 1,
            "precedence: assigning again does not duplicate");
        name = mime_icon_name_for("notes.txt", 0, 0);
        expect(name != 0 && strcmp(name, "x-office-document") == 0, "precedence: the last line wins");
    }
}

static void selftest_limits(void)
{
    static char big[MIME_ICON_POLICY_MAX_BYTES];
    static const char k_long[] =
        ".txt=an-absurdly-long-icon-name-that-does-not-fit-in-the-capacity\n";
    const struct mime_icon_entry *entry = 0;
    size_t offset = 0;
    int index = 0;

    /* No policy: everything resolves to 0, which is "no icon", not a crash. */
    expect(mime_icon_parse_policy(0, 0) == 0, "limits: null text");
    expect(mime_icon_name_for("notes.txt", 0, 0) == 0, "limits: no policy means no icon");
    expect(mime_icon_name_for(0, 0, 0) == 0, "limits: null name");
    expect(mime_icon_small(0) == 0, "limits: null icon name");
    expect(mime_icon_small("") == 0, "limits: empty icon name");

    /* The name is truncated without overflowing. */
    (void)mime_icon_parse_policy(k_long, sizeof(k_long) - 1u);
    entry = mime_icon_at(0);
    expect(entry != 0, "limits: the truncated entry exists");
    expect(entry != 0 && strlen(entry->icon) == MIME_ICON_NAME_CAPACITY - 1u,
        "limits: the name is truncated to the capacity");

    /* More extensions than MIME_ICON_MAX_ENTRIES: cut, not overflowed. */
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
    expect(mime_icon_count() == MIME_ICON_MAX_ENTRIES, "limits: cut at the cap");
    expect(mime_icon_at(MIME_ICON_MAX_ENTRIES) == 0, "limits: index out of range");
}

int mime_icon_selftest(void (*report)(const char *label))
{
    /* The checks overwrite the policy; the loaded one is put back afterwards
     * so the rest of the process does not inherit the test's. */
    static struct mime_icon_entry saved_entries[MIME_ICON_MAX_ENTRIES];
    static char saved_folder[MIME_ICON_NAME_CAPACITY];
    static char saved_program[MIME_ICON_NAME_CAPACITY];
    static char saved_default[MIME_ICON_NAME_CAPACITY];
    const int saved_count = g_entry_count;

    memcpy(saved_entries, g_entries, sizeof(saved_entries));
    memcpy(saved_folder, g_icon_folder, sizeof(saved_folder));
    memcpy(saved_program, g_icon_program, sizeof(saved_program));
    memcpy(saved_default, g_icon_default, sizeof(saved_default));

    g_selftest_failures = 0;
    g_selftest_report = report;
    selftest_parse();
    selftest_precedence();
    selftest_limits();
    g_selftest_report = 0;

    memcpy(g_entries, saved_entries, sizeof(saved_entries));
    memcpy(g_icon_folder, saved_folder, sizeof(saved_folder));
    memcpy(g_icon_program, saved_program, sizeof(saved_program));
    memcpy(g_icon_default, saved_default, sizeof(saved_default));
    g_entry_count = saved_count;
    return g_selftest_failures;
}
