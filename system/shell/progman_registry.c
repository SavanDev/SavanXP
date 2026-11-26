#include "shell/progman_registry.h"

#include "savanxp/sxe.h"
#include "shell/shell_io.h"

#include <stdio.h>
#include <string.h>

/* The diagnostic apps are built only when the build asks for them, as in
 * windowd_menu.c: --no-test-apps takes them out of the rootfs and the defaults
 * at once. */
#ifndef DESKTOP_INCLUDE_TEST_APPS
#define DESKTOP_INCLUDE_TEST_APPS 1
#endif

/* The group items declared before any [group] go to. */
#define PROGMAN_IMPLICIT_GROUP_NAME "Programs"

/* current_group once the group capacity ran out: the items falling under it
 * are dropped instead of being reassigned to the wrong group. */
#define PROGMAN_GROUP_NONE (-1)
#define PROGMAN_GROUP_DROPPED (-2)

static struct progman_group g_groups[PROGMAN_MAX_GROUPS];
static struct progman_item g_items[PROGMAN_MAX_ITEMS];
static int g_group_count = 0;
static int g_item_count = 0;
static int g_source = PROGMAN_REGISTRY_SOURCE_DEFAULTS;

/*
 * Pools of icons taken from the binaries' .sxicon sections: the large one
 * (up to 32x32, for the Program Manager grid) and, beside it, the 16x16
 * drawing when the binary carries one (for the Start menu). The small icon is
 * drawn separately from the large one (docs/desktop/ICON_STYLE.md), so
 * shrinking the 32x32 would lose exactly that work.
 *
 * They cost PROGMAN_MAX_ITEMS * (32 * 32 + 16 * 16) * 4 = 240 KiB of BSS, a
 * deliberate choice: without malloc the worst case has to be reserved, and
 * 240 KiB next to the 8 MiB arena the process already maps is noise. The
 * alternative -- rereading the .sxicon when painting -- would put disk I/O
 * inside the repaint loop, which is exactly where it must not go.
 *
 * They are filled when a binary's resources are read: by the scan for the
 * items it adds, by progman_registry_apply_sxe() for the rest. A slot is not
 * tied to an item's index -- each item holds the first one free when it got
 * its icon -- so pruning and sorting can move items freely.
 */
#define PROGMAN_ICON_MAX_EXTENT 32u
#define PROGMAN_ICON_SLOT_PIXELS (PROGMAN_ICON_MAX_EXTENT * PROGMAN_ICON_MAX_EXTENT)
#define PROGMAN_ICON_SMALL_EXTENT 16u
#define PROGMAN_ICON_SMALL_PIXELS (PROGMAN_ICON_SMALL_EXTENT * PROGMAN_ICON_SMALL_EXTENT)

static uint32_t g_icon_pixels[PROGMAN_MAX_ITEMS][PROGMAN_ICON_SLOT_PIXELS];
static struct desktop_embedded_bitmap g_icon_bitmaps[PROGMAN_MAX_ITEMS];
static uint32_t g_icon_small_pixels[PROGMAN_MAX_ITEMS][PROGMAN_ICON_SMALL_PIXELS];
static struct desktop_embedded_bitmap g_icon_small_bitmaps[PROGMAN_MAX_ITEMS];

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
 * Baked-in defaults: they seed a fresh installation and are the safety net if
 * /progman.ini is missing or corrupt. They temporarily duplicate the contents
 * of k_menu_items (windowd_menu.c); that table is retired in A2.4 and this one
 * becomes the only catalog.
 */
/*
 * The generic icon in EVERY row: none needs a baked-in id of its own. Each
 * program's real one travels in its .sxicon (all are stamped since phase 2),
 * and this is the safety net for the corner case where progman.ini is
 * missing or corrupt AND the binary cannot be read either.
 */
static const struct progman_default_item k_default_items[] = {
    /* Categories and order, THE SAME ones the manifests declare.
     *
     * For a long time this table was the only place with the list of programs,
     * and it fell behind: it said Main/Games/Diagnostics while every .sxres said
     * Accessories/System/Games/Diagnostics. The catalog scan rebuilt it from the
     * manifests and everything worked, so the disagreement did not show -- it
     * only showed when reading both, which is how it was found -- and in
     * practice the manifest is what governs. This table is plan B for when there
     * are no binaries to scan, and a plan B that does not match plan A is a
     * second answer to the same question.
     *
     * The ORDER of the groups matches too, and for a reason that is not
     * cosmetic: the scan sorts them alphabetically and the launcher's tabs are
     * the group number. With Main first and System missing, "the fourth tab"
     * that tools/shoot_session.py talks about did not exist. Here it is the
     * same: it reads top to bottom.
     */
    {"Accessories", "Calculator", "/bin/calc", "Standard calculator", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Accessories", "Command Prompt", "/system/console.sxe", "Windows-style console running the shell", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Accessories", "Files", "/bin/filesapp", "Browse the disk and preview files", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    /* The player is ALWAYS an entry when the port is built, because
     * /bin/mediaplayer exists then. Without the port the pruning drops it --
     * like Doom, which also lives on the volume -- and no launcher is needed to
     * explain its absence: the player itself explains it, from main, with
     * ldso_missing(). */
    {"Accessories", "Media Player", "/bin/mediaplayer", "Play video and audio files", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Accessories", "Notepad", "/bin/notepad", "Edit text files", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Accessories", "Paint", "/bin/paint", "Draw and edit bitmap pictures", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Accessories", "Web Viewer", "/system/webview.sxe", "Browse local and HTTP pages", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
#if DESKTOP_INCLUDE_TEST_APPS
    {"Diagnostics", "Gears", "/bin/gears", "3D rendering test", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN},
    {"Diagnostics", "Gfx Demo", "/bin/gfxdemo", "2D rendering test", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN},
    {"Diagnostics", "Key Test", "/bin/keytest", "Keyboard diagnostics", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Diagnostics", "Mouse Test", "/bin/mousetest", "Mouse diagnostics", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Diagnostics", "Widgets", "/bin/widgetsdemo", "sxgui control gallery", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
#endif
    {"Games", "Doom", "/bin/doomgeneric", "Classic FPS test port", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN},
    {"Games", "Minesweeper", "/bin/mines", "Clear the minefield without stepping on a mine", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Settings", "Add/Remove Programs", "/bin/appwiz", "Uninstall programs installed outside the system image", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"Settings", "System Properties", "/bin/aboutapp", "Version, hardware and installation", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"System", "Program Manager", "/bin/progman", "Program launcher", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
    {"System", "Task Manager", "/system/taskmgr.sxe", "Processes, performance and network", DESKTOP_ICON_DESKTOP, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE},
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

/* Trims spaces at the start and the end, changing the line buffer. */
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

/*
 * Resolves the value of "icon=" to a PATH whose .sxicon to borrow, or 0 when
 * there is none -- then the item keeps the generic icon (the default
 * icon_id), the same outcome as a path that later cannot be read.
 *
 * Two valid forms:
 *   - An absolute path ("icon=/bin/notepad"): used as it is, without checking
 *     here that it exists -- apply_sxe does that when it tries to read it, and
 *     failing there is indistinguishable from any other unreadable binary.
 *   - A legacy alias from before this change: short names that USED TO pick
 *     from a catalog baked in by id. They now resolve to the PROGRAM that
 *     draws that icon today (its own .sxicon, stamped at build time), not to
 *     a hard-coded pixel array. "desktop" never had an alias -- it was
 *     literally the generic one -- and still has none.
 */
static const char *icon_reference_path(const char *value)
{
    if (value == 0 || value[0] == '\0')
    {
        return 0;
    }
    if (value[0] == '/')
    {
        return value;
    }
    if (strcmp(value, "shell") == 0)
    {
        return "/system/console.sxe";
    }
    if (strcmp(value, "notepad") == 0)
    {
        return "/bin/notepad";
    }
    if (strcmp(value, "gfxdemo") == 0)
    {
        return "/bin/gfxdemo";
    }
    if (strcmp(value, "gears") == 0)
    {
        return "/bin/gears";
    }
    if (strcmp(value, "keytest") == 0)
    {
        return "/bin/keytest";
    }
    if (strcmp(value, "mousetest") == 0)
    {
        return "/bin/mousetest";
    }
    /* "desktop", "doom" (it left the set: see docs/userland/SXE_FORMAT.md), or
     * any name this reader does not recognise. */
    return 0;
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

/* An item only goes in with an absolute path: an entry without a path (or
 * with a relative one) cannot be started, so it is dropped instead of being
 * shown broken. */
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
            offset += 1u; /* skip the '\n' */
        }
        buffer[end] = '\0';

        line = trim(&buffer[start]);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
        {
            continue;
        }

        if (line[0] == '[')
        {
            /* A new section: close the item that was open. */
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
                    /* Items before the first [group]: implicit group. */
                    current_group = append_group(PROGMAN_IMPLICIT_GROUP_NAME);
                }
                begin_item(&pending, current_group);
                has_pending = 1;
            }
            /* Unknown sections are ignored (forward compatibility). */
            continue;
        }

        /* key=value */
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
                 * Each key present sets its override bit: it is what later
                 * tells apply_sxe that the user chose that field and it must
                 * not be overwritten with what the binary declares.
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
                    /* A resolved path or alias is kept so apply_sxe reads
                     * the borrowed .sxicon; with neither, the field stays
                     * empty (begin_item left it so) and the item settles for
                     * the generic icon_id. */
                    copy_field(pending.icon_borrow_path, sizeof(pending.icon_borrow_path), icon_reference_path(value));
                    pending.overrides |= PROGMAN_OVERRIDE_ICON;
                }
                else if (strcmp(key, "flags") == 0)
                {
                    pending.launch_flags = launch_flags_from_text(value);
                    pending.overrides |= PROGMAN_OVERRIDE_FLAGS;
                }
                /* Unknown keys are ignored. */
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

int progman_registry_load_file(void)
{
    static char file_buffer[PROGMAN_REGISTRY_MAX_BYTES];
    char path[256];
    long total = shell_io_settings_path(PROGMAN_REGISTRY_NAME, 0, path, sizeof(path)) == 0
        ? shell_io_read_file(path, file_buffer, sizeof(file_buffer))
        : -1;

    if (total > 0 && progman_registry_parse(file_buffer, (size_t)total) > 0)
    {
        return g_item_count;
    }
    /* No file, an unreadable one, or one without a single valid item: an
     * EMPTY registry. What fills the menu -- the scan, and only if that finds
     * nothing either, the defaults -- is the caller's decision. */
    reset_registry();
    g_source = PROGMAN_REGISTRY_SOURCE_DEFAULTS;
    return 0;
}

/*
 * A SUN program is <name>.sxe on disk (docs/userland/SXE_FORMAT.md). A
 * hand-written progman.ini from before the rename still says /bin/console:
 * when that path is gone and /system/console.sxe exists, the item follows the
 * program instead of leaving the menu. Returns 1 if the path was rewritten.
 */
static int adopt_sxe_name(char *path, progman_path_exists_fn exists)
{
    char candidate[PROGMAN_PATH_CAPACITY];
    size_t length = strlen(path);

    /* Older catalogs can still point at a shipped SUN program in /bin. */
    const char *old_name = 0;
    if (strncmp(path, "/bin/", 5u) == 0)
        old_name = path + 5u;
    else if (strncmp(path, "/disk/bin/", 10u) == 0)
        old_name = path + 10u;
    if (old_name != 0)
    {
        int written = snprintf(candidate, sizeof(candidate), "/system/%s%s", old_name,
            length >= 4u && strcmp(path + length - 4u, ".sxe") == 0 ? "" : ".sxe");
        if (written > 0 && (size_t)written < sizeof(candidate) && exists(candidate))
        {
            strcpy(path, candidate);
            return 1;
        }
    }
    if (length >= 4u && strcmp(path + length - 4u, ".sxe") == 0)
    {
        return 0;
    }
    if (length + 5u > sizeof(candidate))
    {
        return 0;
    }
    memcpy(candidate, path, length);
    memcpy(candidate + length, ".sxe", 5u);
    if (!exists(candidate))
    {
        return 0;
    }
    memcpy(path, candidate, length + 5u);
    return 1;
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

    /* 1) Compact the surviving items, counting them again by group. */
    for (group_index = 0; group_index < g_group_count; ++group_index)
    {
        g_groups[group_index].item_count = 0;
    }

    for (read_index = 0; read_index < g_item_count; ++read_index)
    {
        if (!exists(g_items[read_index].path) && !adopt_sxe_name(g_items[read_index].path, exists))
        {
            dropped += 1;
            continue;
        }

        if (g_items[read_index].icon_borrow_path[0] != '\0' &&
            !exists(g_items[read_index].icon_borrow_path))
        {
            (void)adopt_sxe_name(g_items[read_index].icon_borrow_path, exists);
        }
        g_items[write_index] = g_items[read_index];
        g_groups[g_items[write_index].group_index].item_count += 1;
        write_index += 1;
    }
    g_item_count = write_index;

    /* 2) Compact away the groups left empty, noting the remapping. */
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

    /* 3) Point the items at the new indices. No surviving item can point at a
     * dropped group: a group is dropped precisely because it ran out of
     * items. */
    for (read_index = 0; read_index < g_item_count; ++read_index)
    {
        g_items[read_index].group_index = group_remap[g_items[read_index].group_index];
    }

    return dropped;
}

/* --- SXE resources ------------------------------------------------------- */

/*
 * Copies the best icon in the blob into the item's slot. Only square images
 * that fit the slot are accepted: growing the pool for a giant icon would cost
 * memory in EVERY slot, and the grid draws at 32 anyway.
 *
 * An exact 16x16 image, if the blob has one, also goes into the small slot.
 * Only an exact one: a smaller size derived here would be the large icon
 * shrunk, which is what the Start menu already does when there is none.
 */
static int adopt_icon(struct progman_item *item, int slot, const struct sxe_icons *icons)
{
    const struct sxe_icon_entry *entry = sxe_icons_best(icons, PROGMAN_ICON_MAX_EXTENT);
    const struct sxe_icon_entry *small = sxe_icons_best(icons, PROGMAN_ICON_SMALL_EXTENT);
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

    g_icon_small_bitmaps[slot].pixels = 0;
    if (small != 0 && small->width == PROGMAN_ICON_SMALL_EXTENT && small->height == PROGMAN_ICON_SMALL_EXTENT)
    {
        pixels = sxe_icons_pixels(icons, small);
        if (pixels != 0)
        {
            memcpy(g_icon_small_pixels[slot], pixels, PROGMAN_ICON_SMALL_PIXELS * sizeof(uint32_t));
            g_icon_small_bitmaps[slot].width = PROGMAN_ICON_SMALL_EXTENT;
            g_icon_small_bitmaps[slot].height = PROGMAN_ICON_SMALL_EXTENT;
            g_icon_small_bitmaps[slot].pixels = g_icon_small_pixels[slot];
        }
    }
    item->icon_slot = slot;
    return 1;
}

/*
 * Scratch for reading resources, shared by the scan and apply_sxe (they never
 * run at the same time). Static and not on the stack: this libc runs with a
 * bounded stack. The icon scratch is sized to the FORMAT's limit, not to what
 * the manifests emit today: sizing it to "16 + 32 and done" would leave a
 * binary that also carries 48x48 without an icon -- a silent downgrade, for a
 * perfectly valid blob.
 */
_Alignas(4) static uint8_t g_meta_scratch[SXE_META_MAX_BYTES];
_Alignas(4) static uint8_t g_icon_scratch[SXE_ICON_MAX_BYTES];

/* First icon slot no item holds. There are as many slots as items, so one is
 * always free for an item that has none. */
static int take_icon_slot(void)
{
    int used[PROGMAN_MAX_ITEMS];
    int index;

    memset(used, 0, sizeof(used));
    for (index = 0; index < g_item_count; ++index)
    {
        int slot = g_items[index].icon_slot;

        if (slot >= 0 && slot < PROGMAN_MAX_ITEMS)
        {
            used[slot] = 1;
        }
    }
    for (index = 0; index < PROGMAN_MAX_ITEMS; ++index)
    {
        if (!used[index])
        {
            return index;
        }
    }
    return PROGMAN_ICON_SLOT_NONE;
}

/* The .sxmeta fields the .ini did not override. Returns 1 if any was taken. */
static int apply_meta_to_item(struct progman_item *item, const struct sxe_meta *meta)
{
    /* The size of the largest field copied out of the blob. */
    char text[PROGMAN_DESC_CAPACITY];
    uint32_t value = 0;
    int touched = 0;

    /*
     * Into the temporary buffer and not straight into the field:
     * sxe_meta_string empties the destination BEFORE looking for the tag, so
     * writing into item->name would erase the default when the binary
     * declares no name.
     */
    if ((item->overrides & PROGMAN_OVERRIDE_NAME) == 0 &&
        sxe_meta_string(meta, SXE_TAG_NAME, text, sizeof(text)) > 0u)
    {
        copy_field(item->name, sizeof(item->name), text);
        touched = 1;
    }
    if ((item->overrides & PROGMAN_OVERRIDE_DESCRIPTION) == 0 &&
        sxe_meta_string(meta, SXE_TAG_DESCRIPTION, text, sizeof(text)) > 0u)
    {
        copy_field(item->description, sizeof(item->description), text);
        touched = 1;
    }
    if ((item->overrides & PROGMAN_OVERRIDE_FLAGS) == 0 &&
        sxe_meta_u32(meta, SXE_TAG_LAUNCH_FLAGS, &value))
    {
        item->launch_flags = value;
        touched = 1;
    }
    return touched;
}

/* Takes the icon from the open `file` into a free slot. Returns 1 if it did. */
static int apply_icon_from_file(struct progman_item *item, struct sxe_file *file)
{
    struct sxe_icons icons;
    int slot;

    if (sxe_file_load_icons(file, g_icon_scratch, sizeof(g_icon_scratch), &icons) != SXE_OK)
    {
        return 0;
    }
    slot = take_icon_slot();
    return slot != PROGMAN_ICON_SLOT_NONE && adopt_icon(item, slot, &icons);
}

static int apply_sxe_to_item(struct progman_item *item)
{
    struct sxe_file file;
    struct sxe_meta meta;
    int own_file = sxe_file_open(&file, item->path) == SXE_OK;
    int touched = 0;

    if (own_file && sxe_file_load_meta(&file, g_meta_scratch, sizeof(g_meta_scratch), &meta) == SXE_OK)
    {
        touched |= apply_meta_to_item(item, &meta);
    }

    /*
     * Without an override the icon comes from the binary itself, read from
     * the file already open. With an override there is only something to
     * read if icon= resolved to a path (its own or borrowed from another
     * program) -- a name without an alias leaves icon_borrow_path empty on
     * purpose, and then there is nothing to try: the item keeps the generic
     * icon_id.
     */
    if ((item->overrides & PROGMAN_OVERRIDE_ICON) == 0)
    {
        if (own_file)
        {
            touched |= apply_icon_from_file(item, &file);
        }
    }
    else if (item->icon_borrow_path[0] != '\0')
    {
        struct sxe_file borrowed;

        if (sxe_file_open(&borrowed, item->icon_borrow_path) == SXE_OK)
        {
            touched |= apply_icon_from_file(item, &borrowed);
            sxe_file_close(&borrowed);
        }
    }

    sxe_file_close(&file);
    return touched;
}

int progman_registry_apply_sxe(void)
{
    int index;
    int touched = 0;

    /* Release the slots of the items about to be read again first, so they
     * can be reused; the ones the scan resolved keep theirs. */
    for (index = 0; index < g_item_count; ++index)
    {
        if (g_items[index].sxe_state == PROGMAN_SXE_PENDING)
        {
            g_items[index].icon_slot = PROGMAN_ICON_SLOT_NONE;
        }
    }
    for (index = 0; index < g_item_count; ++index)
    {
        struct progman_item *item = &g_items[index];

        if (item->sxe_state == PROGMAN_SXE_DONE_TOUCHED)
        {
            touched += 1;
        }
        else if (item->sxe_state == PROGMAN_SXE_PENDING && apply_sxe_to_item(item))
        {
            touched += 1;
        }
    }
    return touched;
}

/* --- scan of installed programs ------------------------------------------ */

static int g_last_scan_examined = 0;

static int default_path_exists(const char *path)
{
    struct shell_io_info info;

    if (path == 0 || path[0] == '\0')
    {
        return 0;
    }
    return shell_io_query(path, &info) == 0;
}

static const char *path_basename(const char *path)
{
    const char *base = path;

    if (path == 0)
    {
        return "";
    }
    while (*path != '\0')
    {
        if (*path == '/')
        {
            base = path + 1;
        }
        path += 1;
    }
    return base;
}

/*
 * Whether the registry already has an item pointing at a binary with this
 * basename.
 *
 * The tie-break is by BASENAME and not by full path because the same program
 * can be named by two paths: a /progman.ini written before the disk became the
 * root says /disk/bin/doomgeneric, and the scan finds /bin/doomgeneric.
 * Comparing paths would list it twice.
 */
static int item_with_basename_exists(const char *basename)
{
    int index;

    for (index = 0; index < g_item_count; ++index)
    {
        if (strcmp(path_basename(g_items[index].path), basename) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/*
 * Adds a candidate if its .sxmeta declares a category. Returns 1 if it did.
 *
 * The binary is opened once: the .sxmeta that decides whether it enters is
 * also what fills its presentation, and the .sxicon comes from the same open
 * file. The item enters without overrides, so the result is exactly what
 * apply_sxe would produce, and it is marked done so apply_sxe does not open
 * it again. The name in particular has to be set here and not later: the
 * final alphabetical order has to be the one the user sees ("Command
 * Prompt"), not the file's ("console").
 */
static int scan_candidate(const char *path)
{
    struct sxe_file file;
    struct sxe_meta meta;
    struct progman_item item;
    struct progman_item *added = 0;
    char category[PROGMAN_NAME_CAPACITY];
    int group_index;

    if (sxe_file_open(&file, path) != SXE_OK)
    {
        return 0;
    }
    if (sxe_file_load_meta(&file, g_meta_scratch, sizeof(g_meta_scratch), &meta) != SXE_OK ||
        sxe_meta_string(&meta, SXE_TAG_CATEGORY, category, sizeof(category)) == 0u ||
        item_with_basename_exists(path_basename(path)))
    {
        /* Without a category it did not ask to be in the menu. It is still
         * launchable from the shell, from filesapp and from any .ini item
         * that points at it by hand. */
        sxe_file_close(&file);
        return 0;
    }

    group_index = find_group(category);
    if (group_index == PROGMAN_GROUP_NONE)
    {
        group_index = append_group(category);
    }
    if (group_index < 0)
    {
        /* No room for one more group. The candidate is dropped rather than
         * put in a group that is not its own. */
        sxe_file_close(&file);
        return 0;
    }

    begin_item(&item, group_index);
    copy_field(item.path, sizeof(item.path), path);
    {
        /* The default when the binary declares no NAME: the program's name
         * rather than its file name, so a SUN program listed as console.sxe
         * is shown as console. */
        size_t length;

        copy_field(item.name, sizeof(item.name), path_basename(path));
        length = strlen(item.name);
        if (length > 4u && strcmp(item.name + length - 4u, ".sxe") == 0)
        {
            item.name[length - 4u] = '\0';
        }
    }
    if (!append_item(&item))
    {
        sxe_file_close(&file);
        return 0;
    }

    added = &g_items[g_item_count - 1];
    {
        int touched = apply_meta_to_item(added, &meta);

        touched |= apply_icon_from_file(added, &file);
        added->sxe_state = touched ? PROGMAN_SXE_DONE_TOUCHED : PROGMAN_SXE_DONE;
    }
    sxe_file_close(&file);
    return 1;
}

struct scan_context
{
    const char *directory;
    progman_path_exists_fn exists;
    int added;
};

static void scan_entry(const char *name, void *opaque)
{
    struct scan_context *context = (struct scan_context *)opaque;
    char full_path[PROGMAN_PATH_CAPACITY];
    int written;

    if (name[0] == '.')
    {
        return;
    }
    written = snprintf(full_path, sizeof(full_path), "%s/%s", context->directory, name);
    /* A truncated path would point at ANOTHER file, not the one meant: it is
     * dropped instead of opened. */
    if (written < 0 || (size_t)written >= sizeof(full_path))
    {
        return;
    }
    if (!context->exists(full_path))
    {
        return;
    }
    g_last_scan_examined += 1;
    context->added += scan_candidate(full_path);
}

static int scan_directory(const char *directory, progman_path_exists_fn exists)
{
    struct scan_context context = {directory, exists, 0};

    (void)shell_io_list(directory, scan_entry, &context);
    return context.added;
}

/*
 * Sorts alphabetically the groups the scan CREATED, leaving the ones from the
 * .ini in front and in their order -- the user chose that order.
 *
 * Without this the order of the tabs is the listing's, that is, the image's:
 * deterministic, but arbitrary for anyone looking for a group in a list. The
 * items keep their group_index, so moving a group means pointing them again,
 * as the pruning does.
 */
static void sort_scanned_groups(int first)
{
    static struct progman_group sorted[PROGMAN_MAX_GROUPS];
    int remap[PROGMAN_MAX_GROUPS];
    int order[PROGMAN_MAX_GROUPS];
    int count = g_group_count - first;
    int index;

    if (count < 2)
    {
        return;
    }
    for (index = 0; index < count; ++index)
    {
        order[index] = first + index;
    }
    for (index = 1; index < count; ++index)
    {
        int pivot = order[index];
        int hole = index;

        while (hole > 0 && strcmp(g_groups[order[hole - 1]].name, g_groups[pivot].name) > 0)
        {
            order[hole] = order[hole - 1];
            hole -= 1;
        }
        order[hole] = pivot;
    }

    for (index = 0; index < count; ++index)
    {
        sorted[index] = g_groups[order[index]];
        remap[order[index]] = first + index;
    }
    for (index = 0; index < count; ++index)
    {
        g_groups[first + index] = sorted[index];
    }
    for (index = 0; index < g_item_count; ++index)
    {
        if (g_items[index].group_index >= first)
        {
            g_items[index].group_index = remap[g_items[index].group_index];
        }
    }
}

/* Sorts by name the stretch the scan added, leaving what was already there
 * alone. The items keep their group_index, so moving one does not change its
 * group: sorting the whole stretch is enough for each group to be sorted
 * inside. */
static void sort_scanned_items(int first)
{
    /* Static and not on the stack: struct progman_item is over 400 bytes and
     * this libc runs with a bounded stack, the same rule as the rest of this
     * file's scratch buffers. */
    static struct progman_item pivot;
    int index;

    for (index = first + 1; index < g_item_count; ++index)
    {
        int hole = index;

        pivot = g_items[index];
        while (hole > first && strcmp(g_items[hole - 1].name, pivot.name) > 0)
        {
            g_items[hole] = g_items[hole - 1];
            hole -= 1;
        }
        g_items[hole] = pivot;
    }
}

int progman_registry_scan_programs(progman_path_exists_fn exists)
{
    int first = g_item_count;
    int first_group = g_group_count;
    int added = 0;

    g_last_scan_examined = 0;
    if (exists == 0)
    {
        exists = default_path_exists;
    }
    added += scan_directory(PROGMAN_SCAN_DIR, exists);
    added += scan_directory("/system", exists);
    sort_scanned_groups(first_group);
    sort_scanned_items(first);

    /* The scan only claims the source when it built the catalog itself: with
     * a valid .ini in front, the arrangement is still the user's. */
    if (first == 0 && added > 0)
    {
        g_source = PROGMAN_REGISTRY_SOURCE_SCAN;
    }
    return added;
}

/* --- disk signature -------------------------------------------------------- */

static uint32_t signature_mix(uint32_t hash, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t index;

    for (index = 0; index < length; ++index)
    {
        hash ^= bytes[index];
        hash *= 16777619u; /* FNV-1a */
    }
    return hash;
}

/* A path's name, whether it exists, its size and its mtime. */
static uint32_t signature_mix_path(uint32_t hash, const char *path)
{
    struct shell_io_info info;
    uint8_t present;

    hash = signature_mix(hash, path, strlen(path) + 1u);
    memset(&info, 0, sizeof(info));
    present = shell_io_query(path, &info) == 0 ? 1u : 0u;
    hash = signature_mix(hash, &present, sizeof(present));
    if (present)
    {
        hash = signature_mix(hash, &info.size, sizeof(info.size));
        hash = signature_mix(hash, &info.mtime, sizeof(info.mtime));
    }
    return hash;
}

struct signature_context
{
    const char *directory;
    uint32_t hash;
};

static void signature_entry(const char *name, void *opaque)
{
    struct signature_context *context = (struct signature_context *)opaque;
    char path[PROGMAN_PATH_CAPACITY];
    int written;

    if (name[0] == '.')
    {
        return;
    }
    written = snprintf(path, sizeof(path), "%s/%s", context->directory, name);
    if (written < 0 || (size_t)written >= sizeof(path))
    {
        return;
    }
    context->hash = signature_mix_path(context->hash, path);
}

uint32_t progman_registry_disk_signature(void)
{
    char path[PROGMAN_PATH_CAPACITY];
    uint32_t hash = 2166136261u;
    int index;

    if (shell_io_settings_path(PROGMAN_REGISTRY_NAME, 0, path, sizeof(path)) == 0)
    {
        hash = signature_mix_path(hash, path);
    }

    /* What both program directories contain. */
    const char *scan_dirs[] = {PROGMAN_SCAN_DIR, "/system"};
    for (int dir = 0; dir < 2; ++dir)
    {
        struct signature_context context = {scan_dirs[dir], hash};

        (void)shell_io_list(scan_dirs[dir], signature_entry, &context);
        hash = context.hash;
    }

    /* What the current catalog points at outside the scanned directory:
     * items the .ini placed elsewhere and borrowed icons. */
    for (index = 0; index < g_item_count; ++index)
    {
        hash = signature_mix_path(hash, g_items[index].path);
        if (g_items[index].icon_borrow_path[0] != '\0')
        {
            hash = signature_mix_path(hash, g_items[index].icon_borrow_path);
        }
    }
    return hash;
}

int progman_registry_scan_examined(void)
{
    return g_last_scan_examined;
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

const struct desktop_embedded_bitmap *progman_item_icon_small(const struct progman_item *item)
{
    if (item == 0 || item->icon_slot < 0 || item->icon_slot >= PROGMAN_MAX_ITEMS)
    {
        return 0;
    }
    if (g_icon_small_bitmaps[item->icon_slot].pixels == 0)
    {
        return 0;
    }
    return &g_icon_small_bitmaps[item->icon_slot];
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
    expect(progman_item_count() > 0, "defaults without items");
    expect(progman_group_count() > 0, "defaults without groups");
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
    expect(paths_ok, "defaults with an invalid path");
    expect(groups_ok, "defaults with an invalid group_index");
    /* Out of range returns NULL instead of garbage. */
    expect(progman_item_at(-1) == 0 && progman_item_at(progman_item_count()) == 0, "defaults bounds item");
    expect(progman_group_at(-1) == 0 && progman_group_at(progman_group_count()) == 0, "defaults bounds group");
}

static void selftest_well_formed(void)
{
    static const char kText[] =
        "# comment\n"
        "; another comment\n"
        "\n"
        "[group]\n"
        "name=Main\n"
        "\n"
        "[item]\n"
        "name=Shell\n"
        "path=/system/console.sxe\n"
        "desc=Terminal\n"
        "icon=shell\n"
        "\n"
        "[item]\n"
        "name = Doom\n"
        "path =  /bin/doomgeneric  \n"
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
    expect(group != 0 && strcmp(group->name, "Main") == 0, "group 0 name");
    expect(group != 0 && group->item_count == 2, "group 0 item_count");
    group = progman_group_at(1);
    expect(group != 0 && strcmp(group->name, "Diagnostics") == 0, "group 1 name");
    expect(group != 0 && group->item_count == 1, "group 1 item_count");

    /* This function is PURE parsing and does not call apply_sxe: what can be
     * checked of icon= here is which PATH it resolved to (icon_borrow_path),
     * not the final bitmap -- selftest_sxe covers that, against real
     * binaries. */
    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->name, "Shell") == 0, "item 0 name");
    expect(item != 0 && strcmp(item->path, "/system/console.sxe") == 0, "item 0 path");
    expect(item != 0 && strcmp(item->description, "Terminal") == 0, "item 0 desc");
    expect(item != 0 && strcmp(item->icon_borrow_path, "/system/console.sxe") == 0, "item 0 icon= legacy alias resolves to a path");
    expect(item != 0 && item->launch_flags == SAVANXP_DESKTOP_LAUNCH_FLAG_NONE, "item 0 flags");

    /* Spaces around key and value trimmed. */
    item = progman_item_at(1);
    expect(item != 0 && strcmp(item->name, "Doom") == 0, "item 1 name trim");
    expect(item != 0 && strcmp(item->path, "/bin/doomgeneric") == 0, "item 1 path trim");
    /* "doom" has no alias any more (it left the baked-in set): an old
     * progman.ini with icon=doom has to degrade cleanly to "no borrowed
     * path", neither breaking the parse nor landing on anything else. */
    expect(item != 0 && item->icon_borrow_path[0] == '\0', "item 1 icon=doom without an alias, without a path");
    expect(item != 0 && item->launch_flags == SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN, "item 1 flag fullscreen");

    /* Indexed by group. */
    item = progman_group_item_at(1, 0);
    expect(item != 0 && strcmp(item->name, "Keys") == 0, "group_item_at");
    expect(progman_group_item_at(1, 1) == 0, "group_item_at out of range");
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
        "future=something\r\n"
        "icon=nonexistent\r\n";
    const struct progman_item *item = 0;

    expect(progman_registry_parse(kText, sizeof(kText) - 1u) == 1, "crlf parse count");
    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->name, "Files") == 0, "crlf name");
    expect(item != 0 && strcmp(item->path, "/bin/filesapp") == 0, "crlf path");
    /* A name without an alias: no path to borrow, it falls to the generic one instead of breaking. */
    expect(item != 0 && item->icon_borrow_path[0] == '\0', "icon= without an alias keeps no path");
    expect(item != 0 && item->icon_id == DESKTOP_ICON_DESKTOP, "unknown icon -> generic");
}

static void selftest_invalid_items(void)
{
    static const char kText[] =
        "[group]\n"
        "name=Main\n"
        "[item]\n"
        "name=NoPath\n"
        "desc=has no path\n"
        "[item]\n"
        "name=Relative\n"
        "path=bin/relative\n"
        "[item]\n"
        "name=Valid\n"
        "path=/bin/valid\n";
    const struct progman_item *item = 0;

    /* Only the third can be started. */
    expect(progman_registry_parse(kText, sizeof(kText) - 1u) == 1, "invalid items dropped");
    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->name, "Valid") == 0, "the valid item survives");
}

static void selftest_implicit_group(void)
{
    static const char kText[] =
        "[item]\n"
        "name=Orphan\n"
        "path=/bin/orphan\n";
    const struct progman_group *group = 0;

    expect(progman_registry_parse(kText, sizeof(kText) - 1u) == 1, "item without a group");
    expect(progman_group_count() == 1, "implicit group created");
    group = progman_group_at(0);
    expect(group != 0 && strcmp(group->name, PROGMAN_IMPLICIT_GROUP_NAME) == 0, "implicit group name");
}

static void selftest_truncation(void)
{
    static char text[512];
    static const char kLongName[] =
        "AVeryLongNameThatExceedsTheNameFieldCapacityAndMustBeTruncated";
    const struct progman_item *item = 0;
    size_t offset = 0;

    offset = append_text(text, sizeof(text), offset, "[group]\nname=Main\n[item]\nname=");
    offset = append_text(text, sizeof(text), offset, kLongName);
    offset = append_text(text, sizeof(text), offset, "\npath=/bin/x\n");

    expect(progman_registry_parse(text, offset) == 1, "truncation parse");
    item = progman_item_at(0);
    /* Truncates within the capacity and stays NUL-terminated. */
    expect(item != 0 && strlen(item->name) == PROGMAN_NAME_CAPACITY - 1u, "name truncated to the capacity");
    expect(item != 0 && strncmp(item->name, kLongName, PROGMAN_NAME_CAPACITY - 1u) == 0, "name prefix kept");
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

    /* It stops at the capacity instead of overflowing the arrays. */
    expect(progman_registry_parse(text, offset) == PROGMAN_MAX_ITEMS, "clamp to PROGMAN_MAX_ITEMS");
    expect(progman_item_count() == PROGMAN_MAX_ITEMS, "item_count clamped");
}

static void selftest_empty(void)
{
    expect(progman_registry_parse(0, 0) == 0, "null input");
    expect(progman_registry_parse("", 0) == 0, "empty input");
    expect(progman_registry_parse("# comments only\n", 16) == 0, "comments only");
    /* After an empty input the registry is empty, and the defaults fill it
     * again: the last-resort path, when neither the .ini nor the scan give
     * anything. */
    expect(progman_item_count() == 0, "empty registry after an empty parse");
    progman_registry_load_defaults();
    expect(progman_item_count() > 0, "defaults fill it again");
}

/* A fake predicate: only the paths in this list "exist", so the pruning is
 * exercised without depending on what is on the disk when the selftest runs. */
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
    expect(progman_group_count() == 3, "prune fixture 3 groups");

    /* /bin/a1 and /bin/b1 go: Alpha keeps one item and Beta, which loses the
     * only one it had, disappears altogether. */
    expect(progman_registry_prune_missing(fake_exists) == 2, "prune drops 2");
    expect(progman_item_count() == 2, "prune leaves 2 items");
    expect(progman_group_count() == 2, "prune drops the empty group");

    /* Gamma has to have moved down from index 2 to 1, and its item with it. */
    group = progman_group_at(0);
    expect(group != 0 && strcmp(group->name, "Alpha") == 0, "prune keeps Alpha at 0");
    group = progman_group_at(1);
    expect(group != 0 && strcmp(group->name, "Gamma") == 0, "prune moves Gamma to 1");

    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->path, "/bin/a2") == 0 && item->group_index == 0, "prune repoints A2");
    item = progman_item_at(1);
    expect(item != 0 && strcmp(item->path, "/bin/c1") == 0 && item->group_index == 1, "prune repoints C1");

    /* group_item_at has to stay consistent with the new indices. */
    item = progman_group_item_at(1, 0);
    expect(item != 0 && strcmp(item->path, "/bin/c1") == 0, "prune group_item_at after the remap");

    /* A null predicate touches nothing; running it twice is idempotent. */
    expect(progman_registry_prune_missing(0) == 0, "prune with a null predicate");
    expect(progman_registry_prune_missing(fake_exists) == 0, "prune idempotent");
    expect(progman_item_count() == 2 && progman_group_count() == 2, "prune idempotent keeps");
}

static int fake_exists_renamed(const char *path)
{
    return strcmp(path, "/bin/old.sxe") == 0 || strcmp(path, "/system/moved.sxe") == 0;
}

/* An item written before the .sxe rename follows the program to its new name
 * instead of leaving the menu; a name that exists under neither still goes. */
static void selftest_prune_follows_sxe_name(void)
{
    static const char kText[] =
        "[group]\n"
        "name=Accessories\n"
        "[item]\n"
        "name=Old\n"
        "path=/bin/old\n"
        "[item]\n"
        "name=Gone\n"
        "path=/bin/gone\n";
    const struct progman_item *item;

    expect(progman_registry_parse(kText, sizeof(kText) - 1) == 2, "sxe rename fixture 2 items");
    expect(progman_registry_prune_missing(fake_exists_renamed) == 1, "sxe rename drops only the missing one");
    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->path, "/bin/old.sxe") == 0, "sxe rename follows the program");

    static const char moved[] =
        "[group]\nname=Accessories\n"
        "[item]\nname=Moved\npath=/bin/moved.sxe\nicon=/disk/bin/moved.sxe\n"
        "[item]\nname=Legacy\npath=/bin/moved\n";
    expect(progman_registry_parse(moved, sizeof(moved) - 1) == 2, "system relocation fixture");
    expect(progman_registry_prune_missing(fake_exists_renamed) == 0, "system relocation retains both items");
    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->path, "/system/moved.sxe") == 0,
        "system relocation follows the SUN program");
    expect(item != 0 && strcmp(item->icon_borrow_path, "/system/moved.sxe") == 0,
        "system relocation follows its borrowed icon");
    item = progman_item_at(1);
    expect(item != 0 && strcmp(item->path, "/system/moved.sxe") == 0,
        "system relocation follows the extensionless legacy name");
}

/* Compares two bitmaps by content, not by pointer: it is what it takes to
 * prove that a BORROWED icon really is the referenced program's and not
 * anything else left in the slot. */
static int bitmaps_equal(const struct desktop_embedded_bitmap *a, const struct desktop_embedded_bitmap *b)
{
    uint32_t index;
    uint32_t count;

    if (a == 0 || b == 0 || a->pixels == 0 || b->pixels == 0)
    {
        return 0;
    }
    if (a->width != b->width || a->height != b->height)
    {
        return 0;
    }
    count = a->width * a->height;
    for (index = 0; index < count; ++index)
    {
        if (a->pixels[index] != b->pixels[index])
        {
            return 0;
        }
    }
    return 1;
}

/*
 * Precedence .ini > .sxe > default (docs/userland/SXE_FORMAT.md, phase 3).
 *
 * It runs against the image's REAL binaries, the only way to test this: the
 * step being checked is precisely the one that opens the executable. With
 * default stamping EVERY binary has a .sxmeta -- init included, although it
 * never declared an init.sxres -- so the item below that uses /bin/init does
 * not test "no resources": it tests that an .ini declaring name AND
 * description beats the automatic stamp in both fields, and that what the
 * automatic stamp never provides (icon, flags) stays untouched.
 *
 * "icon=" no longer picks from a baked-in catalog: it points at a PROGRAM.
 * This fixture covers the three forms -- explicit path, legacy alias, and a
 * name without an alias -- against real binaries, because the step being
 * checked is precisely the one that opens the borrowed executable.
 */
static void selftest_sxe(void)
{
    static const char kText[] =
        "[group]\n"
        "name=Main\n"
        "[item]\n"
        "path=/bin/notepad\n"
        "[item]\n"
        "name=My Notes\n"
        "path=/bin/notepad\n"
        "icon=/bin/aboutapp\n"
        "[item]\n"
        "name=Legacy\n"
        "path=/bin/notepad\n"
        "icon=shell\n"
        "[item]\n"
        "name=Generic\n"
        "path=/bin/notepad\n"
        "icon=desktop\n"
        "[item]\n"
        "name=Startup\n"
        "desc=PID 1\n"
        "path=/bin/init\n"
        "[item]\n"
        "path=/system/console.sxe\n";
    const struct progman_item *item;
    const struct desktop_embedded_bitmap *icon;

    expect(progman_registry_parse(kText, sizeof(kText) - 1) == 6, "sxe fixture 6 items");

    /* Before applying, nobody has an icon of its own. */
    item = progman_item_at(0);
    expect(item != 0 && item->icon_slot == PROGMAN_ICON_SLOT_NONE, "no icon_slot before applying");
    expect(progman_item_icon(item) == 0, "no bitmap before applying");

    /* The override bits have to reflect what the .ini declared. */
    expect(item != 0 && item->overrides == PROGMAN_OVERRIDE_NONE, "item 0 without overrides");
    item = progman_item_at(1);
    expect(item != 0 && (item->overrides & PROGMAN_OVERRIDE_NAME) != 0, "item 1 name override");
    expect(item != 0 && (item->overrides & PROGMAN_OVERRIDE_ICON) != 0, "item 1 icon override");
    expect(item != 0 && (item->overrides & PROGMAN_OVERRIDE_DESCRIPTION) == 0, "item 1 without a desc override");
    expect(item != 0 && strcmp(item->icon_borrow_path, "/bin/aboutapp") == 0, "icon=/path is kept as it is");

    /* item0 (its own icon), item1 and item2 (borrow an icon), item3 (the
     * binary's description although the icon stays generic) and item5 (the
     * console, its own icon and name) make 5; item4 (init, no icon= and no
     * resources of its own to give) changes nothing. */
    expect(progman_registry_apply_sxe() == 5, "apply_sxe touches 5 items");

    /* Item 0: no overrides, everything comes from the binary. */
    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->name, "Notepad") == 0, "name from the .sxmeta");
    expect(item != 0 && strcmp(item->description, "Edit text files") == 0, "description from the .sxmeta");
    expect(item != 0 && item->icon_slot != PROGMAN_ICON_SLOT_NONE, "icon from the .sxicon");
    icon = progman_item_icon(item);
    expect(icon != 0 && icon->pixels != 0, "icon bitmap available");
    expect(icon != 0 && icon->width == 32u && icon->height == 32u, "32x32 icon");
    /* The Start menu's icon is the binary's own 16x16 drawing, not the 32x32. */
    icon = progman_item_icon_small(item);
    expect(icon != 0 && icon->pixels != 0 && icon->width == 16u && icon->height == 16u,
        "16x16 icon from the .sxicon");

    /*
     * Item 1: icon=/bin/aboutapp, an explicit PATH -- the new and main form.
     * aboutapp.sxres declares icon=desktop, so its real .sxicon is pixel for
     * pixel the baked-in generic one: comparing against
     * desktop_icon_large(DESKTOP_ICON_DESKTOP) proves the borrowing brought
     * the right binary, not just anything.
     */
    item = progman_item_at(1);
    expect(item != 0 && strcmp(item->name, "My Notes") == 0, "the .ini name beats the .sxmeta");
    expect(item != 0 && item->icon_slot != PROGMAN_ICON_SLOT_NONE, "icon=/path borrows a .sxicon");
    icon = progman_item_icon(item);
    expect(bitmaps_equal(icon, desktop_icon_large(DESKTOP_ICON_DESKTOP)),
        "the borrowed icon is aboutapp's (=generic, aboutapp declares icon=desktop)");
    /* What the .ini did NOT declare still comes from the binary this item
     * starts (notepad), not from the one lending it the icon (aboutapp). */
    expect(item != 0 && strcmp(item->description, "Edit text files") == 0, "desc without an override comes from the item's own binary");

    /*
     * Item 2: icon=shell, a LEGACY ALIAS from before icons came from the
     * binaries. It resolves to the terminal, /system/console.sxe today, and borrows
     * ITS real .sxicon. There is no baked DESKTOP_ICON_SHELL to compare with
     * (docs/userland/SXE_FORMAT.md): the cross-check is against item 5, which
     * launches /system/console.sxe WITHOUT any override and so adopts its own
     * .sxicon by the normal path -- two independent uses of the same
     * mechanism have to reach the same bitmap.
     */
    item = progman_item_at(2);
    expect(item != 0 && strcmp(item->icon_borrow_path, "/system/console.sxe") == 0, "legacy alias resolves to the program's path");
    expect(item != 0 && item->icon_slot != PROGMAN_ICON_SLOT_NONE, "a legacy alias lends the .sxicon too");
    icon = progman_item_icon(item);
    expect(bitmaps_equal(icon, progman_item_icon(progman_item_at(5))),
        "the icon borrowed by alias is the one the console adopts for itself");

    /*
     * Item 3: icon=desktop, a name WITHOUT an alias -- "desktop" never had
     * one, it was literally the generic icon. It stays at the default icon_id
     * and does NOT try to read anything, not even notepad's own .sxicon: the
     * user asked explicitly for the generic one, and that has to beat what
     * the binary brings.
     */
    item = progman_item_at(3);
    expect(item != 0 && item->icon_borrow_path[0] == '\0', "a name without an alias keeps no path");
    expect(item != 0 && item->icon_slot == PROGMAN_ICON_SLOT_NONE, "without an alias no .sxicon is borrowed");
    expect(item != 0 && item->icon_id == DESKTOP_ICON_DESKTOP, "without an alias it falls to the generic icon_id");
    expect(progman_item_icon(item) == 0, "without an alias there is no borrowed bitmap");
    /* The description does still come from the binary: the override was of
     * the icon only, not of the other fields. */
    expect(item != 0 && strcmp(item->description, "Edit text files") == 0, "desc of an item without a desc override");

    /*
     * Item 4: init, with ONLY the automatic stamp (name/version/subsystem/
     * build_id, no icon and no flags) and an .ini that covers name AND
     * description by hand. Nothing from the automatic stamp should show in
     * either of the two fields the .ini already declared.
     */
    item = progman_item_at(4);
    expect(item != 0 && strcmp(item->name, "Startup") == 0, "the .ini beats the automatic stamp on the name");
    expect(item != 0 && strcmp(item->description, "PID 1") == 0, "the .ini beats the automatic stamp on the desc");
    expect(item != 0 && item->icon_slot == PROGMAN_ICON_SLOT_NONE, "the automatic stamp never brings an icon");

    /* Item 5: /system/console.sxe without any override -- it adopts its own .sxicon
     * by the normal path (the one item 0 uses with notepad). It is what item
     * 2 is compared against. A SUN program's .sxe is stamped like any other,
     * so the launcher reads it the same way. */
    item = progman_item_at(5);
    expect(item != 0 && strcmp(item->name, "Command Prompt") == 0, "item 5 name from the console's .sxmeta");
    expect(item != 0 && item->icon_slot != PROGMAN_ICON_SLOT_NONE, "item 5 the console's own icon");
    icon = progman_item_icon(item);
    expect(icon != 0 && icon->width == 32u && icon->height == 32u, "item 5 32x32 icon");

    /* Applying twice must neither duplicate nor corrupt anything. */
    expect(progman_registry_apply_sxe() == 5, "apply_sxe is idempotent");
    item = progman_item_at(0);
    expect(item != 0 && strcmp(item->name, "Notepad") == 0, "idempotent keeps the name");

#if DESKTOP_INCLUDE_TEST_APPS
    {
        /* gfxdemo declares launch_flags=fullscreen in its manifest: an item
         * that writes no flags= has to inherit it from the binary. */
        static const char kFlagsText[] =
            "[item]\n"
            "path=/bin/gfxdemo\n";

        expect(progman_registry_parse(kFlagsText, sizeof(kFlagsText) - 1) == 1, "flags fixture");
        (void)progman_registry_apply_sxe();
        item = progman_item_at(0);
        expect(item != 0 && item->launch_flags == SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN,
            "launch_flags from the .sxmeta");
    }
#endif

    /* A path that does not exist must neither break the step nor leave garbage. */
    {
        static const char kMissingText[] =
            "[item]\n"
            "name=Ghost\n"
            "path=/bin/__does_not_exist__\n";

        expect(progman_registry_parse(kMissingText, sizeof(kMissingText) - 1) == 1, "missing fixture");
        expect(progman_registry_apply_sxe() == 0, "a missing path touches nothing");
        item = progman_item_at(0);
        expect(item != 0 && strcmp(item->name, "Ghost") == 0, "a missing path keeps the name");
        expect(progman_item_icon(item) == 0, "a missing path has no icon");
    }
}

int progman_registry_selftest(void)
{
    g_selftest_failures = 0;

    selftest_defaults();
    selftest_prune();
    selftest_prune_follows_sxe_name();
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
