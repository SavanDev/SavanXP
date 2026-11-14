/*
 * System Properties.
 *
 * The machine's identity card, in the shape of the period's "About this
 * Computer": the logo on the left and three blocks of text on the right --
 * which system this is, where it is installed and what it runs on -- plus a
 * second tab with the hardware inventory and a third with the licenses of
 * every third-party component installed (/system/licenses, see
 * docs/third-party/LICENSE_NOTICES.md).
 *
 * What this window does NOT show is live state: the memory in use now, the
 * processes, the CPU usage. That is Task Manager's (/system/taskmgr.sxe), and
 * having it in both places meant having two versions of the same number.
 * Here goes what does not change while the machine is on.
 */

#include "libc.h"
#include "savanxp/sxgui.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "brand_logo.h"
#include "shared/version.h"
#include "build_number.h"

static struct sxgui_app g_app;

/* ---- grilla ---------------------------------------------------------------
 *
 * Layout fijo, con la ventana dimensionada a mano (no por autosize): el
 * contenido de las pestanias lo pinta esta app, asi que el bounding box de los
 * widgets no sabe cuanto mide.
 *
 * La columna de texto arranca despues del logo. El paso entre lineas es el de
 * la fuente mas su aire; ABOUT_INDENT es la sangria de las lineas de un bloque
 * respecto de su encabezado, que es lo que agrupa sin necesidad de un marco. */
#define ABOUT_MARGIN SXGUI_CONTENT_MARGIN
#define ABOUT_ROW 18
#define ABOUT_BLOCK_GAP 12
#define ABOUT_INDENT 14
#define ABOUT_PAGE_PAD 16
#define ABOUT_LOGO_SIZE SX_BRAND_LOGO_W
#define ABOUT_LOGO_GAP 28
#define ABOUT_TEXT_LEFT (ABOUT_PAGE_PAD + ABOUT_LOGO_SIZE + ABOUT_LOGO_GAP)
/* El ancho lo fija la linea mas larga que se puede llegar a mostrar, que es la
 * de capacidades del procesador con las tres puestas. Una ventana de identidad
 * no tiene scroll: si el texto no entra, se recorta y el dato se pierde. */
#define ABOUT_TEXT_WIDTH 348
#define ABOUT_PAGE_WIDTH (ABOUT_TEXT_LEFT + ABOUT_TEXT_WIDTH + ABOUT_PAGE_PAD)

/* Alto de la pagina: el de la pestania que mas pide. La General son tres
 * encabezados con sus lineas mas las dos separaciones entre bloques; la
 * Hardware es una fila por dispositivo. Se calcula y no se clava para que
 * agregar una fila mueva la ventana en vez de recortarla en silencio. */
#define ABOUT_GENERAL_ROWS 14
#define ABOUT_HARDWARE_ROWS 15
#define ABOUT_GENERAL_HEIGHT (ABOUT_GENERAL_ROWS * ABOUT_ROW + ABOUT_BLOCK_GAP * 2)
#define ABOUT_HARDWARE_HEIGHT (ABOUT_HARDWARE_ROWS * ABOUT_ROW)
#define ABOUT_PAGE_HEIGHT                     \
    (ABOUT_PAGE_PAD * 2 +                     \
     (ABOUT_GENERAL_HEIGHT > ABOUT_HARDWARE_HEIGHT ? ABOUT_GENERAL_HEIGHT : ABOUT_HARDWARE_HEIGHT))

#define ABOUT_TAB_GENERAL 0
#define ABOUT_TAB_HARDWARE 1
#define ABOUT_TAB_LICENSES 2

static const char *const k_tab_labels[] = {"General", "Hardware", "Licenses"};
#define ABOUT_TAB_COUNT ((int)(sizeof(k_tab_labels) / sizeof(k_tab_labels[0])))

/* The Licenses page: the component list on top, its notice and license text
 * below. The list shows whole rows: sxgui's row is the text height plus 4. */
#define ABOUT_LICENSE_LIST_ROWS 5
#define ABOUT_LICENSE_LIST_HEIGHT \
    ((gfx_text_height() + 4) * ABOUT_LICENSE_LIST_ROWS + SXGUI_BORDER_SUNKEN * 2)

/* In focus order: TAB goes from the tabs to the page's controls, then OK. */
static struct sxgui_widget g_widgets[4];
#define ABOUT_TABS (&g_widgets[0])
#define ABOUT_LICENSE_LIST (&g_widgets[1])
#define ABOUT_LICENSE_TEXT (&g_widgets[2])
#define ABOUT_OK (&g_widgets[3])
#define ABOUT_WIDGET_COUNT ((int)(sizeof(g_widgets) / sizeof(g_widgets[0])))

/* ---- datos ----------------------------------------------------------------
 *
 * Se arman una sola vez, al arrancar. Son los datos que no cambian con la
 * maquina prendida: version, disco, procesador. El unico que se mueve -- el
 * espacio libre del disco -- se rearma con el boton Refresh. */

#define ABOUT_LINE_CAPACITY 96

struct about_block {
    const char *title;
    char lines[4][ABOUT_LINE_CAPACITY];
    int line_count;
};

static struct about_block g_system;
static struct about_block g_install;
static struct about_block g_computer;

static char g_hardware_labels[ABOUT_HARDWARE_ROWS][24];
static char g_hardware_values[ABOUT_HARDWARE_ROWS][ABOUT_LINE_CAPACITY];
static int g_hardware_count;

static struct sx_bitmap g_logo;

/* What the logo keeps for whoever double clicks it with Ctrl+Shift held, as
 * the About boxes of the period did. It replaces the General page until the
 * tab changes, F5 rebuilds the data or the same gesture is made again. */
static const struct about_block k_note_company = {
    "In good company:",
    {"ReactOS",
     "Another classic desktop rebuilt from scratch,",
     "one open source line at a time."},
    3,
};
static const struct about_block k_note_reply = {
    "From SavanXP:",
    {"Same road, a different engine.",
     "Hats off, and good luck to both of us."},
    2,
};
static int g_note_shown;

/* Pointer events carry no modifiers; key events do, with the state the
 * driver keeps, so the last one seen is the state to check a click against. */
static uint32_t g_modifiers;
static uint32_t g_last_buttons;
static unsigned long g_last_logo_click_ms;
static int g_logo_clicks;

static char *block_line(struct about_block *block)
{
    if (block->line_count >= (int)(sizeof(block->lines) / sizeof(block->lines[0])))
    {
        /* Reescribir la ultima antes que pisar memoria de al lado. Que una
         * linea se pierda es un bloque mal declarado, no un fallo del sistema. */
        return block->lines[block->line_count - 1];
    }
    return block->lines[block->line_count++];
}

static void add_hardware_row(const char *label, const char *format, ...)
{
    va_list args;

    if (g_hardware_count >= ABOUT_HARDWARE_ROWS)
    {
        return;
    }
    snprintf(g_hardware_labels[g_hardware_count], sizeof(g_hardware_labels[0]), "%s", label);
    va_start(args, format);
    vsnprintf(g_hardware_values[g_hardware_count], sizeof(g_hardware_values[0]), format, args);
    va_end(args);
    g_hardware_count += 1;
}

/* Bytes a la unidad que los hace legibles, con dos decimales arriba de 1 GB,
 * como el "2.00 GB of RAM" del original. Por debajo de eso los decimales no
 * aportan nada y se muestran MB enteros. */
static void format_memory(uint64_t bytes, char *out, size_t capacity)
{
    const uint64_t mib = bytes / (1024ull * 1024ull);

    if (mib >= 1024ull)
    {
        const uint64_t hundredths = (mib * 100ull) / 1024ull;

        snprintf(out, capacity, "%llu.%02llu GB",
                 (unsigned long long)(hundredths / 100ull),
                 (unsigned long long)(hundredths % 100ull));
        return;
    }
    snprintf(out, capacity, "%llu MB", (unsigned long long)mib);
}

static const char *timer_backend_name(uint32_t backend)
{
    switch (backend)
    {
    case SAVANXP_TIMER_LOCAL_APIC:
        return "local APIC";
    case SAVANXP_TIMER_PIT:
        return "PIT";
    default:
        return "none";
    }
}

static const char *present_absent(uint8_t value)
{
    return value != 0 ? "present" : "not detected";
}

static void build_system_block(void)
{
    g_system.title = "System:";
    g_system.line_count = 0;
    snprintf(block_line(&g_system), ABOUT_LINE_CAPACITY, "%s", SAVANXP_SYSTEM_NAME);
    snprintf(block_line(&g_system), ABOUT_LINE_CAPACITY, "%s", SAVANXP_EDITION);
    snprintf(block_line(&g_system), ABOUT_LINE_CAPACITY, "Version %s",
             SAVANXP_FULL_VERSION_NUMBER);
    snprintf(block_line(&g_system), ABOUT_LINE_CAPACITY, "SDK %d.%d",
             SAVANXP_SDK_VERSION_MAJOR, SAVANXP_SDK_VERSION_MINOR);
}

/*
 * El bloque del medio es donde el original decia "Registered to:". SavanXP no
 * tiene cuentas de usuario, asi que no hay nadie a quien atribuirlo: en vez de
 * inventar un nombre, ese lugar lo ocupa de donde arranco y donde vive el
 * sistema, que es la otra mitad de la identidad de una instalacion.
 */
static void build_install_block(const struct savanxp_system_info *info)
{
    g_install.title = "Installed on:";
    g_install.line_count = 0;

    if (info->sxfs_mounted != 0)
    {
        snprintf(block_line(&g_install), ABOUT_LINE_CAPACITY, "SxFS volume mounted at /");
        snprintf(block_line(&g_install), ABOUT_LINE_CAPACITY, "%llu of %llu MiB used, %u files",
                 (unsigned long long)(info->sxfs_used_bytes / (1024ull * 1024ull)),
                 (unsigned long long)(info->sxfs_total_bytes / (1024ull * 1024ull)),
                 (unsigned)info->sxfs_file_count);
    }
    else
    {
        snprintf(block_line(&g_install), ABOUT_LINE_CAPACITY, "No persistent volume mounted");
        snprintf(block_line(&g_install), ABOUT_LINE_CAPACITY, "Running from the boot image only");
    }

    snprintf(block_line(&g_install), ABOUT_LINE_CAPACITY, "%s firmware, %s %s",
             info->firmware[0] != '\0' ? info->firmware : "unknown",
             info->bootloader_name[0] != '\0' ? info->bootloader_name : "unknown bootloader",
             info->bootloader_version);
}

static void build_computer_block(const struct savanxp_system_info *info)
{
    char memory[32];
    char *speed_line;

    g_computer.title = "Computer:";
    g_computer.line_count = 0;

    snprintf(block_line(&g_computer), ABOUT_LINE_CAPACITY, "%s",
             info->cpu_brand[0] != '\0'
                 ? info->cpu_brand
                 : (info->cpu_vendor[0] != '\0' ? info->cpu_vendor : "Unidentified processor"));

    /* Velocidad y memoria comparten linea, como en el original. Sin calibrado
     * de TSC no hay velocidad que decir y la linea queda solo con la RAM: un
     * "0.00 GHz" seria peor que no decir nada. */
    format_memory(info->memory_usable_bytes, memory, sizeof(memory));
    speed_line = block_line(&g_computer);
    if (info->cpu_khz != 0)
    {
        snprintf(speed_line, ABOUT_LINE_CAPACITY, "%u.%02u GHz, %s of RAM",
                 (unsigned)(info->cpu_khz / 1000000u),
                 (unsigned)((info->cpu_khz / 10000u) % 100u),
                 memory);
    }
    else
    {
        snprintf(speed_line, ABOUT_LINE_CAPACITY, "%s of RAM", memory);
    }

    /* La linea de capacidades: el equivalente del "Physical Address Extension"
     * que el original ponia al final del bloque. */
    {
        char features[ABOUT_LINE_CAPACITY];
        size_t used = 0;
        int count = 0;
        static const struct {
            uint32_t flag;
            const char *name;
        } k_features[] = {
            {SAVANXP_CPU_FEATURE_LONG_MODE, "64-bit"},
            {SAVANXP_CPU_FEATURE_PAE, "Physical Address Extension"},
            {SAVANXP_CPU_FEATURE_NX, "Execute Disable"},
        };
        size_t index;

        features[0] = '\0';
        for (index = 0; index < sizeof(k_features) / sizeof(k_features[0]); ++index)
        {
            if ((info->cpu_features & k_features[index].flag) == 0)
            {
                continue;
            }
            used += (size_t)snprintf(features + used, sizeof(features) - used, "%s%s",
                                     count > 0 ? ", " : "", k_features[index].name);
            count += 1;
            if (used >= sizeof(features) - 1)
            {
                break;
            }
        }
        if (count > 0)
        {
            snprintf(block_line(&g_computer), ABOUT_LINE_CAPACITY, "%s", features);
        }
    }

    snprintf(block_line(&g_computer), ABOUT_LINE_CAPACITY, "%u processor%s%s",
             (unsigned)(info->cpu_online != 0 ? info->cpu_online : 1u),
             (info->cpu_online > 1u) ? "s" : "",
             (info->cpu_features & SAVANXP_CPU_FEATURE_HYPERVISOR) != 0 ? ", virtualized" : "");
}

static void build_hardware_rows(const struct savanxp_system_info *info)
{
    char memory[32];

    g_hardware_count = 0;

    add_hardware_row("Processor", "%s",
                     info->cpu_brand[0] != '\0'
                         ? info->cpu_brand
                         : (info->cpu_vendor[0] != '\0' ? info->cpu_vendor : "unidentified"));
    if (info->cpu_khz != 0)
    {
        add_hardware_row("Processor speed", "%u.%02u GHz",
                         (unsigned)(info->cpu_khz / 1000000u),
                         (unsigned)((info->cpu_khz / 10000u) % 100u));
    }
    else
    {
        add_hardware_row("Processor speed", "not calibrated");
    }
    add_hardware_row("Cores", "%u online of %u reported",
                     (unsigned)info->cpu_online, (unsigned)info->cpu_count);

    format_memory(info->memory_usable_bytes, memory, sizeof(memory));
    add_hardware_row("Memory", "%s usable, %llu MiB reclaimable",
                     memory,
                     (unsigned long long)(info->memory_reclaimable_bytes / (1024ull * 1024ull)));

    if (info->framebuffer_ready != 0)
    {
        add_hardware_row("Display", "%u x %u, %u bpp",
                         (unsigned)info->framebuffer_width,
                         (unsigned)info->framebuffer_height,
                         (unsigned)info->framebuffer_bpp);
    }
    else
    {
        add_hardware_row("Display", "no framebuffer");
    }

    add_hardware_row("Keyboard/mouse", "PS/2 controller %s", present_absent(info->input_ready));
    add_hardware_row("Storage", "block device %s", present_absent(info->block_ready));
    add_hardware_row("File system", info->sxfs_mounted != 0 ? "SxFS mounted at /" : "not mounted");
    /* "No hay placa" y "hay una que no sabemos manejar" son dos respuestas
     * distintas sobre la maquina, y la segunda es la que da VirtualBox. El
     * id del fabricante va en la linea porque es lo unico que se puede hacer
     * con esa informacion: buscar de que placa se trata. */
    if (info->net_present != 0)
    {
        add_hardware_row("Network", "adapter present");
    }
    else if (info->net_hardware != 0)
    {
        add_hardware_row("Network", "adapter present, no driver (%04x:%04x)",
                         (unsigned)info->net_hardware_vendor,
                         (unsigned)info->net_hardware_device);
    }
    else
    {
        add_hardware_row("Network", "no adapter");
    }
    add_hardware_row("Speaker", "PC speaker %s", present_absent(info->speaker_ready));
    add_hardware_row("PCI devices", "%u found", (unsigned)info->pci_device_count);
    add_hardware_row("Timer", "%s at %u Hz",
                     timer_backend_name(info->timer_backend),
                     (unsigned)info->timer_frequency_hz);
    add_hardware_row("Firmware", "%s", info->firmware[0] != '\0' ? info->firmware : "unknown");
    add_hardware_row("Bootloader", "%s %s",
                     info->bootloader_name[0] != '\0' ? info->bootloader_name : "unknown",
                     info->bootloader_version);
    add_hardware_row("Boot image", "initramfs of %llu KiB",
                     (unsigned long long)(info->initramfs_size / 1024ull));
}

static void refresh_info(void)
{
    struct savanxp_system_info info;

    memset(&info, 0, sizeof(info));
    (void)system_info(&info);

    build_system_block();
    build_install_block(&info);
    build_computer_block(&info);
    build_hardware_rows(&info);
}

/* ---- licenses -------------------------------------------------------------
 *
 * One directory per installed component under /system/licenses, each with a
 * NOTICE of key=value lines and the license texts it names with text=. There
 * is no index on purpose: a port installs its own directory, so scanning is
 * what makes an installed port show up and an absent one not.
 *
 * Only the selected component is loaded. Its notice and texts are word
 * wrapped here to the text view's width, since the view draws one line per
 * row and clips the rest. That, the copied row height and inner width, and
 * the tab expansion are workarounds for toolkit gaps listed in
 * docs/desktop/PLATFORM_GRAPHICS.md (Known gaps in SXGUI-C). */

#define LICENSE_DIR "/system/licenses"
#define LICENSE_MAX 64
#define LICENSE_TEXT_CAPACITY (160 * 1024)
#define LICENSE_LINE_MAX 4096
#define LICENSE_FILE_MAX (64 * 1024)

struct license_entry {
    char id[48];
    char name[96];
};

static struct license_entry g_licenses[LICENSE_MAX];
static const char *g_license_items[LICENSE_MAX];
static int g_license_count;

static char g_license_text[LICENSE_TEXT_CAPACITY];
static size_t g_license_text_used;
static const char *g_license_lines[LICENSE_LINE_MAX];
static int g_license_line_count;
static int g_license_wrap_width;
static char g_license_file[LICENSE_FILE_MAX + 1];

/* Reads a whole file into g_license_file; returns its length or -1. A longer
 * file is cut at LICENSE_FILE_MAX, which no license text comes near. */
static long read_license_file(const char *id, const char *name)
{
    char path[160];
    FILE *file;
    size_t length;

    snprintf(path, sizeof(path), LICENSE_DIR "/%s/%s", id, name);
    file = fopen(path, "rb");
    if (file == 0)
    {
        return -1;
    }
    length = fread(g_license_file, 1, LICENSE_FILE_MAX, file);
    fclose(file);
    g_license_file[length] = '\0';
    return (long)length;
}

/* The value of `key` in a NOTICE line, or 0 when the line is another key. */
static const char *notice_value(const char *line, size_t length, const char *key, size_t *value_length)
{
    size_t key_length = strlen(key);

    if (length <= key_length || strncmp(line, key, key_length) != 0 || line[key_length] != '=')
    {
        return 0;
    }
    *value_length = length - key_length - 1;
    return line + key_length + 1;
}

static int license_compare(const struct license_entry *left, const struct license_entry *right)
{
    /* The system's own license first, then the components by name. */
    int left_system = strcmp(left->id, "savanxp") == 0;
    int right_system = strcmp(right->id, "savanxp") == 0;
    const char *a = left->name;
    const char *b = right->name;

    if (left_system != right_system)
    {
        return left_system ? -1 : 1;
    }
    for (;; ++a, ++b)
    {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;

        if (ca != cb || ca == 0)
        {
            return ca - cb;
        }
    }
}

static void scan_licenses(void)
{
    DIR *directory = opendir(LICENSE_DIR);
    struct dirent *item;
    int index;

    g_license_count = 0;
    if (directory == 0)
    {
        return;
    }
    while ((item = readdir(directory)) != 0 && g_license_count < LICENSE_MAX)
    {
        struct license_entry *entry = &g_licenses[g_license_count];
        const char *cursor;
        long length;

        if (item->d_name[0] == '.' || strlen(item->d_name) >= sizeof(entry->id))
        {
            continue;
        }
        snprintf(entry->id, sizeof(entry->id), "%s", item->d_name);
        length = read_license_file(entry->id, "NOTICE");
        if (length < 0)
        {
            continue;
        }
        snprintf(entry->name, sizeof(entry->name), "%s", entry->id);
        cursor = g_license_file;
        while (*cursor != '\0')
        {
            const char *end = strchr(cursor, '\n');
            size_t line_length = end != 0 ? (size_t)(end - cursor) : strlen(cursor);
            size_t value_length;
            const char *value = notice_value(cursor, line_length, "name", &value_length);

            if (value != 0)
            {
                snprintf(entry->name, sizeof(entry->name), "%.*s", (int)value_length, value);
                break;
            }
            cursor += line_length + (end != 0 ? 1 : 0);
        }
        g_license_count += 1;
    }
    closedir(directory);

    /* Insertion sort: a few dozen entries at most. */
    for (index = 1; index < g_license_count; ++index)
    {
        struct license_entry moving = g_licenses[index];
        int slot = index;

        while (slot > 0 && license_compare(&moving, &g_licenses[slot - 1]) < 0)
        {
            g_licenses[slot] = g_licenses[slot - 1];
            --slot;
        }
        g_licenses[slot] = moving;
    }
    for (index = 0; index < g_license_count; ++index)
    {
        g_license_items[index] = g_licenses[index].name;
    }
}

/* Appends one output line to the text store; returns 0 when it is full. */
static int license_emit(const char *text, size_t length)
{
    char *line;

    if (g_license_line_count >= LICENSE_LINE_MAX ||
        g_license_text_used + length + 1 > sizeof(g_license_text))
    {
        return 0;
    }
    line = g_license_text + g_license_text_used;
    memcpy(line, text, length);
    line[length] = '\0';
    g_license_text_used += length + 1;
    g_license_lines[g_license_line_count++] = line;
    return 1;
}

static int license_fits(const char *text, size_t length)
{
    char probe[512];

    if (length >= sizeof(probe))
    {
        return 0;
    }
    memcpy(probe, text, length);
    probe[length] = '\0';
    return gfx_text_width(probe) <= g_license_wrap_width;
}

/* Word wraps one source line (no newline in it) into view lines. Tabs become
 * spaces; a word wider than the view is cut between UTF-8 sequences. */
static int license_wrap(const char *source, size_t source_length)
{
    char line[512];
    size_t length = 0;
    size_t index;

    for (index = 0; index < source_length && length < sizeof(line) - 8; ++index)
    {
        if (source[index] == '\t')
        {
            do
            {
                line[length++] = ' ';
            } while ((length % 8) != 0 && length < sizeof(line) - 8);
        }
        else if (source[index] != '\r')
        {
            line[length++] = source[index];
        }
    }

    if (length == 0)
    {
        return license_emit("", 0);
    }
    {
        size_t start = 0;

        while (start < length)
        {
            size_t end = length;
            size_t space = 0;
            size_t cut;

            if (license_fits(line + start, end - start))
            {
                return license_emit(line + start, end - start);
            }
            /* The longest prefix that fits, preferring to end at a space. */
            cut = start;
            for (end = start + 1; end <= length; ++end)
            {
                if (end < length && ((unsigned char)line[end] & 0xc0u) == 0x80u)
                {
                    continue;
                }
                if (!license_fits(line + start, end - start))
                {
                    break;
                }
                cut = end;
                if (end < length && line[end] == ' ')
                {
                    space = end;
                }
            }
            if (space > start)
            {
                cut = space;
            }
            if (cut <= start)
            {
                cut = start + 1;
            }
            if (!license_emit(line + start, cut - start))
            {
                return 0;
            }
            start = cut;
            while (start < length && line[start] == ' ')
            {
                ++start;
            }
        }
    }
    return 1;
}

static int license_wrap_text(const char *text)
{
    while (*text != '\0')
    {
        const char *end = strchr(text, '\n');
        size_t length = end != 0 ? (size_t)(end - text) : strlen(text);

        if (!license_wrap(text, length))
        {
            return 0;
        }
        if (end == 0)
        {
            break;
        }
        text = end + 1;
    }
    return 1;
}

static void license_wrap_field(const char *label, const char *value, size_t value_length)
{
    char line[512];

    /* "Copyright: Copyright (c) ..." says it twice. */
    if (value_length >= 9 && strncmp(value, "Copyright", 9) == 0)
    {
        label = "";
    }
    snprintf(line, sizeof(line), "%s%.*s", label, (int)value_length, value);
    (void)license_wrap(line, strlen(line));
}

static void load_license(int index)
{
    /* A NOTICE is short; keep a copy, g_license_file is reused for each text. */
    static char notice[4096];
    static const struct {
        const char *key;
        const char *label;
    } k_fields[] = {
        {"license", "License: "},
        {"copyright", "Copyright: "},
        {"origin", "Origin: "},
        {"version", "Version: "},
        {"used-in", "Used in: "},
        {"source", "Source: "},
        {"note", ""},
    };
    const struct license_entry *entry;
    const char *cursor;
    size_t field;
    int truncated = 0;

    g_license_text_used = 0;
    g_license_line_count = 0;
    if (index < 0 || index >= g_license_count)
    {
        (void)license_wrap_text(g_license_count == 0
                                    ? "No license notices are installed in " LICENSE_DIR "."
                                    : "");
        return;
    }
    entry = &g_licenses[index];
    if (read_license_file(entry->id, "NOTICE") < 0)
    {
        (void)license_wrap_text("The notice of this component could not be read.");
        return;
    }
    snprintf(notice, sizeof(notice), "%s", g_license_file);

    (void)license_wrap(entry->name, strlen(entry->name));
    for (field = 0; field < sizeof(k_fields) / sizeof(k_fields[0]); ++field)
    {
        for (cursor = notice; *cursor != '\0';)
        {
            const char *end = strchr(cursor, '\n');
            size_t length = end != 0 ? (size_t)(end - cursor) : strlen(cursor);
            size_t value_length;
            const char *value = notice_value(cursor, length, k_fields[field].key, &value_length);

            if (value != 0)
            {
                license_wrap_field(k_fields[field].label, value, value_length);
            }
            cursor += length + (end != 0 ? 1 : 0);
        }
    }

    for (cursor = notice; *cursor != '\0' && !truncated;)
    {
        const char *end = strchr(cursor, '\n');
        size_t length = end != 0 ? (size_t)(end - cursor) : strlen(cursor);
        size_t value_length;
        const char *value = notice_value(cursor, length, "text", &value_length);

        if (value != 0)
        {
            char name[128];
            char rule[160];

            snprintf(name, sizeof(name), "%.*s", (int)value_length, value);
            snprintf(rule, sizeof(rule), "---- %s ----", name);
            (void)license_emit("", 0);
            (void)license_wrap(rule, strlen(rule));
            (void)license_emit("", 0);
            if (read_license_file(entry->id, name) < 0)
            {
                (void)license_wrap_text("(missing)");
            }
            else if (!license_wrap_text(g_license_file))
            {
                truncated = 1;
            }
        }
        cursor += length + (end != 0 ? 1 : 0);
    }
    if (truncated)
    {
        /* Make room for the warning by dropping the last line if needed. */
        if (g_license_line_count >= LICENSE_LINE_MAX)
        {
            g_license_line_count -= 1;
        }
        g_license_lines[g_license_line_count++] = "[Text truncated: read the full file under " LICENSE_DIR ".]";
    }
}

static void show_license(int index)
{
    load_license(index);
    ABOUT_LICENSE_TEXT->items = g_license_lines;
    ABOUT_LICENSE_TEXT->item_count = g_license_line_count;
    ABOUT_LICENSE_TEXT->scroll = 0;
}

static void refresh_licenses(void)
{
    scan_licenses();
    ABOUT_LICENSE_LIST->items = g_license_items;
    ABOUT_LICENSE_LIST->item_count = g_license_count;
    ABOUT_LICENSE_LIST->scroll = 0;
    ABOUT_LICENSE_LIST->value = g_license_count > 0 ? 0 : -1;
    show_license(ABOUT_LICENSE_LIST->value);
}

/* ---- painting -------------------------------------------------------------
 *
 * The General and Hardware pages are drawn by the app: they are blocks of
 * text, not controls. The Licenses page is two widgets, which exist only while
 * their tab is active (sxgui has no hierarchy, so a hidden page's controls
 * would otherwise paint and take clicks). The tab control -- and with it the
 * page background -- has already been painted by the toolkit by then. */

static int paint_block(struct sx_painter *painter, const struct about_block *block, int x, int y)
{
    int line;

    sx_painter_draw_text(painter, x, y, block->title, SXGUI_COLOR_TEXT);
    y += ABOUT_ROW;
    for (line = 0; line < block->line_count; ++line)
    {
        sx_painter_draw_text(painter, x + ABOUT_INDENT, y, block->lines[line], SXGUI_COLOR_TEXT);
        y += ABOUT_ROW;
    }
    return y;
}

static void paint_general(struct sx_painter *painter, struct sx_rect page)
{
    int x = page.x + ABOUT_TEXT_LEFT;
    int y = page.y + ABOUT_PAGE_PAD;

    sx_painter_blit_bitmap(painter, &g_logo, page.x + ABOUT_PAGE_PAD, page.y + ABOUT_PAGE_PAD + 4);

    if (g_note_shown)
    {
        y = paint_block(painter, &k_note_company, x, y) + ABOUT_BLOCK_GAP;
        (void)paint_block(painter, &k_note_reply, x, y);
        return;
    }

    y = paint_block(painter, &g_system, x, y) + ABOUT_BLOCK_GAP;
    y = paint_block(painter, &g_install, x, y) + ABOUT_BLOCK_GAP;
    (void)paint_block(painter, &g_computer, x, y);
}

static void paint_hardware(struct sx_painter *painter, struct sx_rect page)
{
    /* Dos columnas: el rotulo del dispositivo y lo que se sabe de el. La
     * columna de valores arranca donde termina la mas ancha de las etiquetas,
     * medida y no clavada, para que no dependa de la fuente. */
    int label_x = page.x + ABOUT_PAGE_PAD;
    int value_x = label_x;
    int y = page.y + ABOUT_PAGE_PAD;
    int row;

    for (row = 0; row < g_hardware_count; ++row)
    {
        int width = gfx_text_width(g_hardware_labels[row]);

        if (label_x + width > value_x)
        {
            value_x = label_x + width;
        }
    }
    value_x += SXGUI_GAP * 3;

    for (row = 0; row < g_hardware_count; ++row)
    {
        sx_painter_draw_text(painter, label_x, y, g_hardware_labels[row], SXGUI_COLOR_TEXT);
        sx_painter_draw_text(painter, value_x, y, g_hardware_values[row], SXGUI_COLOR_TEXT);
        y += ABOUT_ROW;
    }
}

static void on_paint(struct sxgui_app *app)
{
    struct sx_painter *painter = &app->ui.painter;
    struct sx_rect page = sxgui_tabs_page(ABOUT_TABS);

    if (!sx_painter_push_clip(painter, page))
    {
        return;
    }
    if (ABOUT_TABS->value == ABOUT_TAB_HARDWARE)
    {
        paint_hardware(painter, page);
    }
    else if (ABOUT_TABS->value == ABOUT_TAB_GENERAL)
    {
        paint_general(painter, page);
    }
    sx_painter_pop_clip(painter);
}

/* ---- interaccion ---------------------------------------------------------- */

static void show_page(void)
{
    if (ABOUT_TABS->value == ABOUT_TAB_LICENSES)
    {
        ABOUT_LICENSE_LIST->flags |= SXGUI_FLAG_VISIBLE;
        ABOUT_LICENSE_TEXT->flags |= SXGUI_FLAG_VISIBLE;
    }
    else
    {
        ABOUT_LICENSE_LIST->flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
        ABOUT_LICENSE_TEXT->flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
    }
}

static void on_tab_changed(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    g_note_shown = 0;
    show_page();
    sxgui_app_request_repaint((struct sxgui_app *)user);
}

static void on_license_selected(struct sxgui_widget *widget, void *user)
{
    if (widget->action == SXGUI_ACTION_CHANGE)
    {
        show_license(widget->value);
        sxgui_app_request_repaint((struct sxgui_app *)user);
    }
}

static void on_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    sxgui_app_quit((struct sxgui_app *)user, 0);
}

static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    g_modifiers = event->modifiers;
    if (event->type == SAVANXP_INPUT_EVENT_KEY_DOWN && event->key == SAVANXP_KEY_F5)
    {
        g_note_shown = 0;
        refresh_info();
        refresh_licenses();
        sxgui_app_request_repaint(app);
        return 1;
    }
    return 0;
}

static struct sx_rect logo_rect(void)
{
    struct sx_rect page = sxgui_tabs_page(ABOUT_TABS);

    return sx_rect_make(page.x + ABOUT_PAGE_PAD, page.y + ABOUT_PAGE_PAD + 4,
                        SX_BRAND_LOGO_W, SX_BRAND_LOGO_H);
}

static int on_pointer(struct sxgui_app *app, const struct savanxp_gui_pointer_event *event)
{
    const uint32_t wanted = SAVANXP_KEY_MOD_CTRL | SAVANXP_KEY_MOD_SHIFT;
    int pressed = (event->buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0u &&
                  (g_last_buttons & SAVANXP_MOUSE_BUTTON_LEFT) == 0u;
    unsigned long now;

    (void)app;
    g_last_buttons = event->buttons;
    if (!pressed)
    {
        return 0;
    }
    if (ABOUT_TABS->value != ABOUT_TAB_GENERAL || (g_modifiers & wanted) != wanted ||
        !sx_rect_contains_point(logo_rect(), event->x, event->y))
    {
        g_logo_clicks = 0;
        return 0;
    }

    now = uptime_ms();
    if (g_logo_clicks > 0 && now - g_last_logo_click_ms <= SXGUI_DOUBLE_CLICK_MS)
    {
        g_logo_clicks = 0;
        g_note_shown = !g_note_shown;
        return 1;
    }
    g_logo_clicks = 1;
    g_last_logo_click_ms = now;
    return 1;
}

int main(void)
{
    struct savanxp_fb_info logo_info;
    int page_bottom;
    int content_width = ABOUT_MARGIN * 2 + ABOUT_PAGE_WIDTH;

    memset(&logo_info, 0, sizeof(logo_info));
    logo_info.width = SX_BRAND_LOGO_W;
    logo_info.height = SX_BRAND_LOGO_H;
    logo_info.pitch = SX_BRAND_LOGO_W * (uint32_t)sizeof(uint32_t);
    logo_info.bpp = 32;
    logo_info.buffer_size = logo_info.pitch * SX_BRAND_LOGO_H;
    sx_bitmap_wrap(&g_logo, (uint32_t *)k_brand_logo_pixels, &logo_info, SX_PIXEL_FORMAT_BGRA8888);

    refresh_info();

    *ABOUT_TABS = sxgui_tabs(
        sx_rect_make(ABOUT_MARGIN, ABOUT_MARGIN, ABOUT_PAGE_WIDTH,
                     sxgui_tabs_height() + ABOUT_PAGE_HEIGHT + SXGUI_BORDER_RAISED * 2),
        k_tab_labels, ABOUT_TAB_COUNT, ABOUT_TAB_GENERAL);
    ABOUT_TABS->on_action = on_tab_changed;
    ABOUT_TABS->user = &g_app;

    page_bottom = ABOUT_TABS->rect.y + ABOUT_TABS->rect.height;
    *ABOUT_OK = sxgui_button(
        sx_rect_make(ABOUT_MARGIN + ABOUT_PAGE_WIDTH - SXGUI_BUTTON_WIDTH,
                     page_bottom + SXGUI_GAP + SXGUI_GAP,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_ok, &g_app);

    {
        struct sx_rect page = sxgui_tabs_page(ABOUT_TABS);
        int inner_x = page.x + SXGUI_GAP * 2;
        int inner_width = page.width - SXGUI_GAP * 4;
        int text_y = page.y + SXGUI_GAP * 2 + ABOUT_LICENSE_LIST_HEIGHT + SXGUI_GAP;

        *ABOUT_LICENSE_LIST = sxgui_listbox(
            sx_rect_make(inner_x, page.y + SXGUI_GAP * 2, inner_width, ABOUT_LICENSE_LIST_HEIGHT),
            g_license_items, 0);
        ABOUT_LICENSE_LIST->on_action = on_license_selected;
        ABOUT_LICENSE_LIST->user = &g_app;
        *ABOUT_LICENSE_TEXT = sxgui_textview(
            sx_rect_make(inner_x, text_y, inner_width, page.y + page.height - SXGUI_GAP * 2 - text_y),
            g_license_lines, 0);
        /* The view always reserves its scrollbar: a license is longer than the page. */
        g_license_wrap_width = inner_width - SXGUI_BORDER_SUNKEN * 2 - SXGUI_SCROLLBAR_THICKNESS -
                               SXGUI_TEXT_PAD * 2;
    }
    refresh_licenses();
    show_page();

    if (sxgui_app_init(&g_app, "aboutapp", g_widgets, ABOUT_WIDGET_COUNT) < 0)
    {
        return 1;
    }
    (void)sxgui_app_set_content_size(
        &g_app,
        content_width,
        ABOUT_OK->rect.y + SXGUI_BUTTON_HEIGHT + ABOUT_MARGIN);
    g_app.on_paint = on_paint;
    g_app.on_key = on_key;
    g_app.on_pointer = on_pointer;
    /* El foco arranca en las pestanias: es el unico control con el que hay algo
     * que recorrer, y dejarlo en el boton haria que las flechas no hicieran
     * nada en una ventana que justamente se navega con flechas. */
    sxgui_focus(&g_app.ui, 0);
    return sxgui_app_run(&g_app);
}
