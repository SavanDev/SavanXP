/*
 * Propiedades del sistema.
 *
 * La ficha de identidad de la maquina, con la forma del "About this Computer"
 * de la epoca: el logo a la izquierda y tres bloques de texto a la derecha --
 * que sistema es, donde esta instalado y sobre que corre --, mas una segunda
 * pestania con el inventario de hardware.
 *
 * Lo que esta ventana NO muestra es estado vivo: la memoria que se esta usando
 * ahora, los procesos, el uso de CPU. Eso es el administrador de tareas
 * (/bin/taskmgr), y tenerlo en dos lados significaba tener dos versiones del
 * mismo numero. Aca va lo que no cambia mientras la maquina esta prendida.
 */

#include "libc.h"
#include "savanxp/sxgui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "brand_logo.h"
#include "shared/version.h"

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

static const char *const k_tab_labels[] = {"General", "Hardware"};
#define ABOUT_TAB_COUNT ((int)(sizeof(k_tab_labels) / sizeof(k_tab_labels[0])))

static struct sxgui_widget g_widgets[2];
#define ABOUT_TABS (&g_widgets[0])
#define ABOUT_OK (&g_widgets[1])
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
    snprintf(block_line(&g_system), ABOUT_LINE_CAPACITY, "Experimental Edition");
    snprintf(block_line(&g_system), ABOUT_LINE_CAPACITY, "Version %d.%d.%d",
             SAVANXP_VERSION_MAJOR, SAVANXP_VERSION_MINOR, SAVANXP_VERSION_PATCH);
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
        snprintf(block_line(&g_install), ABOUT_LINE_CAPACITY, "SxFS volume mounted at /disk");
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
    add_hardware_row("File system", info->sxfs_mounted != 0 ? "SxFS mounted at /disk" : "not mounted");
    add_hardware_row("Network", "adapter %s", present_absent(info->net_present));
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

/* ---- pintado --------------------------------------------------------------
 *
 * El contenido de las dos paginas lo dibuja la app: son bloques de texto, no
 * controles, y como widgets habria que apagarlos y prenderlos a mano en cada
 * cambio de pestania. El control de pestanias -- y con el, el fondo de la
 * pagina -- ya lo pinto el toolkit antes de llegar aca. */

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
    else
    {
        paint_general(painter, page);
    }
    sx_painter_pop_clip(painter);
}

/* ---- interaccion ---------------------------------------------------------- */

static void on_tab_changed(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    sxgui_app_request_repaint((struct sxgui_app *)user);
}

static void on_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    sxgui_app_quit((struct sxgui_app *)user, 0);
}

static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    if (event->type == SAVANXP_INPUT_EVENT_KEY_DOWN && event->key == SAVANXP_KEY_F5)
    {
        refresh_info();
        sxgui_app_request_repaint(app);
        return 1;
    }
    return 0;
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
    /* El foco arranca en las pestanias: es el unico control con el que hay algo
     * que recorrer, y dejarlo en el boton haria que las flechas no hicieran
     * nada en una ventana que justamente se navega con flechas. */
    sxgui_focus(&g_app.ui, 0);
    return sxgui_app_run(&g_app);
}
