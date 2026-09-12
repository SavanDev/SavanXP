/*
 * Administrador de tareas.
 *
 * El monitor del sistema con la forma del Task Manager clasico: pestanias de
 * Procesos, Rendimiento y Red, barra de estado con los tres numeros de siempre
 * y un menu que puede lanzar una tarea nueva o apagar la maquina.
 *
 * COMO SE MIDE EL USO DE CPU. El kernel no lleva porcentajes: lleva un contador
 * de ticks por proceso (savanxp_process_info.cpu_ticks) y el contador global de
 * ticks (savanxp_system_info.cpu_ticks_total). Un tick incrementa exactamente
 * un proceso, asi que entre dos muestras la suma de los incrementos ES el
 * incremento global, y el porcentaje de cada uno es su parte de esa torta. El
 * del sistema es lo que NO se llevo el proceso ocioso. De ahi que todo lo que
 * se muestra salga de comparar dos muestras: una sola no dice nada.
 *
 * QUE NO ESTA. No hay pestania de Aplicaciones ni de Usuarios. La primera
 * necesita que el WM publique su lista de ventanas, que hoy no es parte del
 * protocolo (docs/WM_SUBSYSTEM.md); la segunda necesita cuentas de usuario, que
 * el sistema no tiene. Las dos son pestanias que faltan, no pestanias vacias.
 */

#include "libc.h"
#include "savanxp/sxgui.h"

#include <stdio.h>
#include <string.h>

/* ---- limites --------------------------------------------------------------
 *
 * El kernel tiene 64 slots de proceso (process::kMaxProcesses) y 64 handles por
 * proceso; enumerar mas que eso no es posible, asi que las tablas de aca se
 * dimensionan con ese tope y no hace falta paginar. */
#define TASKMGR_MAX_PROCESSES 64
#define TASKMGR_NAME_CAPACITY 32
#define TASKMGR_ROW_CAPACITY 96
/* Muestras de historia de los graficos. A la velocidad normal (1 s) son dos
 * minutos, que es lo que entra a un pixel por muestra en una ventana comoda. */
#define TASKMGR_HISTORY 120

/* ---- pestanias ------------------------------------------------------------ */

#define TASKMGR_TAB_PROCESSES 0
#define TASKMGR_TAB_PERFORMANCE 1
#define TASKMGR_TAB_NETWORKING 2

static const char *const k_tab_labels[] = {"Processes", "Performance", "Networking"};
#define TASKMGR_TAB_COUNT ((int)(sizeof(k_tab_labels) / sizeof(k_tab_labels[0])))

/* ---- grilla --------------------------------------------------------------- */

#define TASKMGR_MARGIN SXGUI_MARGIN
#define TASKMGR_ROW 18
#define TASKMGR_PAGE_PAD 10
/* Alto que necesita una caja de grupo para mostrar `rows` filas enteras. La
 * cuenta tiene que incluir lo que draw_group se queda: la media altura del
 * rotulo que se monta sobre el borde de arriba, y el aire de los dos lados.
 * Clavar "filas * paso + algo" fue lo que dejo la ultima fila cortada. */
#define TASKMGR_GROUP_HEIGHT(rows) \
    ((rows) * TASKMGR_ROW + TASKMGR_ROW / 2 + (SXGUI_GAP * 2) + 4)
#define TASKMGR_DEFAULT_WIDTH 560
#define TASKMGR_DEFAULT_HEIGHT 460
#define TASKMGR_MIN_WIDTH 420
#define TASKMGR_MIN_HEIGHT 320

/* Colores del panel de graficos. Son los del original: fondo negro, grilla
 * verde apagada y trazo verde encendido. No salen de la paleta 3D del toolkit
 * a proposito -- un grafico no es un control. */
#define TASKMGR_GRAPH_BACKGROUND SXGUI_RGB(0, 0, 0)
#define TASKMGR_GRAPH_GRID SXGUI_RGB(0, 72, 0)
#define TASKMGR_GRAPH_LINE SXGUI_RGB(0, 255, 0)
#define TASKMGR_GRAPH_LINE_ALT SXGUI_RGB(255, 216, 0)

/* ---- comandos del menu ---------------------------------------------------- */

enum taskmgr_command {
    TASKMGR_CMD_NEW_TASK = 1,
    TASKMGR_CMD_EXIT,
    TASKMGR_CMD_REFRESH,
    TASKMGR_CMD_SPEED_HIGH,
    TASKMGR_CMD_SPEED_NORMAL,
    TASKMGR_CMD_SPEED_LOW,
    TASKMGR_CMD_SPEED_PAUSED,
    TASKMGR_CMD_END_PROCESS,
    TASKMGR_CMD_SHUTDOWN,
    TASKMGR_CMD_REBOOT,
    TASKMGR_CMD_ABOUT,
};

static struct sxgui_menu_item k_file_items[] = {
    {"New Task (Run...)", TASKMGR_CMD_NEW_TASK, 0},
    {0, 0, 0},
    {"Exit Task Manager", TASKMGR_CMD_EXIT, 0},
};

/* El original ponia las velocidades en un submenu de View. sxgui tiene menus de
 * un solo nivel, asi que van en el mismo desplegable, separadas por una linea:
 * es la misma eleccion, con un click menos. */
static struct sxgui_menu_item k_view_items[] = {
    {"Refresh Now\tF5", TASKMGR_CMD_REFRESH, 0},
    {0, 0, 0},
    {"Update Speed: High", TASKMGR_CMD_SPEED_HIGH, 0},
    {"Update Speed: Normal", TASKMGR_CMD_SPEED_NORMAL, SXGUI_MENU_CHECKED},
    {"Update Speed: Low", TASKMGR_CMD_SPEED_LOW, 0},
    {"Update Speed: Paused", TASKMGR_CMD_SPEED_PAUSED, 0},
};

static struct sxgui_menu_item k_process_items[] = {
    {"End Process\tDel", TASKMGR_CMD_END_PROCESS, 0},
};

static struct sxgui_menu_item k_shutdown_items[] = {
    {"Turn Off...", TASKMGR_CMD_SHUTDOWN, 0},
    {"Restart...", TASKMGR_CMD_REBOOT, 0},
};

static struct sxgui_menu_item k_help_items[] = {
    {"System Properties...", TASKMGR_CMD_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"View", k_view_items, (int)(sizeof(k_view_items) / sizeof(k_view_items[0]))},
    {"Process", k_process_items, (int)(sizeof(k_process_items) / sizeof(k_process_items[0]))},
    {"Shut Down", k_shutdown_items, (int)(sizeof(k_shutdown_items) / sizeof(k_shutdown_items[0]))},
    {"Help", k_help_items, (int)(sizeof(k_help_items) / sizeof(k_help_items[0]))},
};

/* Indices dentro de k_view_items de las cuatro velocidades, para mover la
 * tilde: el orden de la tabla y estos numeros tienen que moverse juntos. */
#define TASKMGR_SPEED_FIRST_ITEM 2
#define TASKMGR_SPEED_COUNT 4

static const unsigned long k_speed_ms[TASKMGR_SPEED_COUNT] = {500ul, 1000ul, 4000ul, 0ul};

/* ---- estado --------------------------------------------------------------- */

struct taskmgr_sample {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;
    uint32_t flags;
    uint32_t handle_count;
    uint64_t cpu_ticks;
    uint64_t memory_bytes;
    char name[TASKMGR_NAME_CAPACITY];
    /* Porcentaje calculado contra la muestra anterior; 0 en la primera. */
    unsigned int cpu_percent;
};

static struct sxgui_app g_app;
static struct sxgui_menubar g_menubar;

static struct taskmgr_sample g_processes[TASKMGR_MAX_PROCESSES];
static int g_process_count;

/* La muestra anterior, para las diferencias. Se guarda entera y no solo el
 * contador porque hay que reencontrar cada proceso por pid: los indices se
 * corren en cuanto alguien arranca o termina. */
static struct taskmgr_sample g_previous[TASKMGR_MAX_PROCESSES];
static int g_previous_count;
static uint64_t g_previous_ticks_total;
static int g_have_previous;

static char g_rows[TASKMGR_MAX_PROCESSES][TASKMGR_ROW_CAPACITY];
static const char *g_row_pointers[TASKMGR_MAX_PROCESSES];

static struct savanxp_system_info g_info;
static unsigned int g_cpu_percent;
static unsigned int g_memory_percent;
static uint64_t g_memory_total_bytes;
static uint64_t g_memory_used_bytes;
static uint32_t g_total_handles;

static unsigned char g_cpu_history[TASKMGR_HISTORY];
static unsigned char g_memory_history[TASKMGR_HISTORY];
static unsigned char g_net_history[TASKMGR_HISTORY];
static int g_history_length;

static struct savanxp_net_info g_net;
static int g_net_available;
static uint32_t g_net_previous_frames;
static unsigned long g_net_previous_ms;
static unsigned long g_net_peak_rate = 1ul;
static unsigned long g_net_rate;

static int g_speed_index = 1;
static char g_status_processes[48];
static char g_status_cpu[48];
static char g_status_memory[64];

/* ---- widgets -------------------------------------------------------------- */

static struct sxgui_widget g_widgets[6];
#define TASKMGR_TABS (&g_widgets[0])
#define TASKMGR_LIST (&g_widgets[1])
#define TASKMGR_END_BUTTON (&g_widgets[2])
#define TASKMGR_STATUS_PROCESSES (&g_widgets[3])
#define TASKMGR_STATUS_CPU (&g_widgets[4])
#define TASKMGR_STATUS_MEMORY (&g_widgets[5])
#define TASKMGR_WIDGET_COUNT ((int)(sizeof(g_widgets) / sizeof(g_widgets[0])))

#define TASKMGR_COLUMN_PID_WIDTH 52
#define TASKMGR_COLUMN_CPU_WIDTH 44
#define TASKMGR_COLUMN_MEMORY_WIDTH 92
#define TASKMGR_COLUMN_HANDLES_WIDTH 64

static struct sxgui_column g_columns[] = {
    {"Image Name", 0, 0},
    {"PID", TASKMGR_COLUMN_PID_WIDTH, SXGUI_COLUMN_RIGHT},
    {"CPU", TASKMGR_COLUMN_CPU_WIDTH, SXGUI_COLUMN_RIGHT},
    {"Mem Usage", TASKMGR_COLUMN_MEMORY_WIDTH, SXGUI_COLUMN_RIGHT},
    {"Handles", TASKMGR_COLUMN_HANDLES_WIDTH, SXGUI_COLUMN_RIGHT},
};
#define TASKMGR_COLUMN_COUNT ((int)(sizeof(g_columns) / sizeof(g_columns[0])))

/* ---- dialogos ------------------------------------------------------------- */

#define TASKMGR_DIALOG_MARGIN SXGUI_DIALOG_MARGIN
/* El ancho lo fija la advertencia de terminar un proceso, que es la linea mas
 * larga que muestra el dialogo. Un dialogo no tiene scroll: la unica forma de
 * que el texto se lea entero es que el dialogo lo contenga. */
#define TASKMGR_CONFIRM_WIDTH 404
#define TASKMGR_CONFIRM_HEIGHT 128
#define TASKMGR_RUN_WIDTH 360
#define TASKMGR_RUN_HEIGHT 140

static struct sxgui_dialog g_confirm_dialog;
static struct sxgui_widget g_confirm_widgets[4];
static char g_confirm_line[TASKMGR_NAME_CAPACITY + 64];
static char g_confirm_detail[96];
static uint32_t g_confirm_pid;
static int g_pending_power;

static struct sxgui_dialog g_run_dialog;
static struct sxgui_widget g_run_widgets[4];
static char g_run_path[128] = "/bin/";

/* ---- utilidades ----------------------------------------------------------- */

static int centred_button_x(int dialog_width, int count, int index)
{
    return (dialog_width - (count * SXGUI_BUTTON_WIDTH) - ((count - 1) * SXGUI_GAP)) / 2 +
        (index * (SXGUI_BUTTON_WIDTH + SXGUI_GAP));
}

/* Miles separados por coma, como los "1,234 K" del original. El printf del SDK
 * no agrupa, asi que el numero se arma al reves y se da vuelta. */
static void format_grouped(uint64_t value, char *out, size_t capacity)
{
    char reversed[32];
    size_t length = 0;
    size_t digits = 0;
    size_t index;

    if (capacity == 0)
    {
        return;
    }
    do
    {
        if (digits > 0 && (digits % 3u) == 0u && length + 1u < sizeof(reversed))
        {
            reversed[length++] = ',';
        }
        if (length + 1u >= sizeof(reversed))
        {
            break;
        }
        reversed[length++] = (char)('0' + (int)(value % 10ull));
        value /= 10ull;
        digits += 1u;
    } while (value != 0ull);

    for (index = 0; index < length && index + 1u < capacity; ++index)
    {
        out[index] = reversed[length - 1u - index];
    }
    out[index < capacity ? index : capacity - 1u] = '\0';
}

static const char *state_name(uint32_t state)
{
    switch (state)
    {
    case SAVANXP_PROC_READY:
        return "Ready";
    case SAVANXP_PROC_RUNNING:
        return "Running";
    case SAVANXP_PROC_BLOCKED_READ:
        return "Blocked (read)";
    case SAVANXP_PROC_BLOCKED_WRITE:
        return "Blocked (write)";
    case SAVANXP_PROC_BLOCKED_WAIT:
        return "Waiting";
    case SAVANXP_PROC_SLEEPING:
        return "Sleeping";
    case SAVANXP_PROC_ZOMBIE:
        return "Terminated";
    default:
        return "Unknown";
    }
}

static const struct taskmgr_sample *find_previous(uint32_t pid)
{
    int index;

    for (index = 0; index < g_previous_count; ++index)
    {
        if (g_previous[index].pid == pid)
        {
            return &g_previous[index];
        }
    }
    return 0;
}

static void push_history(unsigned char *history, unsigned char value)
{
    int index;

    for (index = 0; index + 1 < TASKMGR_HISTORY; ++index)
    {
        history[index] = history[index + 1];
    }
    history[TASKMGR_HISTORY - 1] = value;
}

/* ---- muestreo -------------------------------------------------------------
 *
 * Una pasada: leer el sistema, leer los procesos, calcular los porcentajes
 * contra la muestra anterior y recien entonces guardar esta como anterior. */

static void sample_network(void)
{
    struct savanxp_net_info info;
    long fd;
    uint32_t frames;
    unsigned long now_ms = uptime_ms();
    unsigned long elapsed_ms;

    memset(&info, 0, sizeof(info));
    /* Se abre y se cierra en cada muestra: mantener el device abierto le
     * costaria un descriptor permanente a una pestania que puede no mirarse
     * nunca. Solo GET_INFO -- este programa observa, no levanta la interfaz. */
    fd = savanxp_open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (fd < 0)
    {
        g_net_available = 0;
        return;
    }
    if (savanxp_ioctl((int)fd, NET_IOC_GET_INFO, (unsigned long)&info) < 0)
    {
        savanxp_close((int)fd);
        g_net_available = 0;
        return;
    }
    savanxp_close((int)fd);

    g_net = info;
    g_net_available = info.present != 0;
    if (!g_net_available)
    {
        return;
    }

    /* El intervalo se MIDE, no se deduce del refresco elegido: con la
     * actualizacion pausada, el unico muestreo que hay es el de un Refresh a
     * mano, y dividir por el periodo del menu daria una tasa inventada. */
    frames = info.tx_frames + info.rx_frames;
    elapsed_ms = now_ms > g_net_previous_ms ? (now_ms - g_net_previous_ms) : 0ul;
    if (elapsed_ms != 0ul && frames >= g_net_previous_frames)
    {
        g_net_rate = ((unsigned long)(frames - g_net_previous_frames) * 1000ul) / elapsed_ms;
    }
    else
    {
        g_net_rate = 0ul;
    }
    g_net_previous_frames = frames;
    g_net_previous_ms = now_ms;
    if (g_net_rate > g_net_peak_rate)
    {
        g_net_peak_rate = g_net_rate;
    }
}

static const struct taskmgr_sample *selected_sample(void)
{
    if (TASKMGR_LIST->value < 0 || TASKMGR_LIST->value >= g_process_count)
    {
        return 0;
    }
    return &g_processes[TASKMGR_LIST->value];
}

/*
 * El proceso ocioso no se puede terminar -- es a donde vuelve el planificador
 * cuando no hay nada listo --, asi que el boton y el comando del menu se apagan
 * cuando es el elegido. Deshabilitarlos y no esconderlos: que exista pero no se
 * pueda es informacion sobre ese proceso; un boton que desaparece y vuelve
 * segun la fila es una ventana que cambia de forma sola.
 */
static void update_end_command(void)
{
    const struct taskmgr_sample *sample = selected_sample();
    int can_end = sample != 0 && (sample->flags & SAVANXP_PROC_FLAG_IDLE) == 0u;

    if (can_end)
    {
        TASKMGR_END_BUTTON->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
        k_process_items[0].flags &= ~(uint32_t)SXGUI_MENU_DISABLED;
    }
    else
    {
        TASKMGR_END_BUTTON->flags |= SXGUI_FLAG_DISABLED;
        k_process_items[0].flags |= SXGUI_MENU_DISABLED;
    }
}

static void build_rows(void)
{
    int index;

    for (index = 0; index < g_process_count; ++index)
    {
        const struct taskmgr_sample *sample = &g_processes[index];
        char memory[24];

        format_grouped(sample->memory_bytes / 1024ull, memory, sizeof(memory));
        snprintf(g_rows[index], sizeof(g_rows[index]), "%s\t%u\t%02u\t%s K\t%u",
                 sample->name[0] != '\0' ? sample->name : "(unnamed)",
                 (unsigned)sample->pid,
                 sample->cpu_percent > 99u ? 99u : sample->cpu_percent,
                 memory,
                 (unsigned)sample->handle_count);
        g_row_pointers[index] = g_rows[index];
    }

    TASKMGR_LIST->items = g_row_pointers;
    TASKMGR_LIST->item_count = g_process_count;
    if (TASKMGR_LIST->value >= g_process_count)
    {
        TASKMGR_LIST->value = g_process_count > 0 ? g_process_count - 1 : 0;
    }
    update_end_command();
}

static void refresh_sample(void)
{
    struct savanxp_process_info info;
    unsigned long proc_index = 0;
    uint64_t ticks_delta = 0;
    uint64_t idle_delta = 0;
    int index;

    memset(&g_info, 0, sizeof(g_info));
    (void)system_info(&g_info);

    g_process_count = 0;
    g_total_handles = 0;
    memset(&info, 0, sizeof(info));
    while (g_process_count < TASKMGR_MAX_PROCESSES && proc_info(proc_index, &info) > 0)
    {
        struct taskmgr_sample *sample = &g_processes[g_process_count];

        proc_index += 1;
        if (info.state == SAVANXP_PROC_UNUSED)
        {
            continue;
        }
        sample->pid = info.pid;
        sample->parent_pid = info.parent_pid;
        sample->state = info.state;
        sample->flags = info.flags;
        sample->handle_count = info.handle_count;
        sample->cpu_ticks = info.cpu_ticks;
        sample->memory_bytes = info.memory_bytes;
        sample->cpu_percent = 0;
        snprintf(sample->name, sizeof(sample->name), "%s", info.name);
        g_total_handles += info.handle_count;
        g_process_count += 1;
    }

    if (g_have_previous && g_info.cpu_ticks_total > g_previous_ticks_total)
    {
        ticks_delta = g_info.cpu_ticks_total - g_previous_ticks_total;
    }

    for (index = 0; index < g_process_count; ++index)
    {
        struct taskmgr_sample *sample = &g_processes[index];
        const struct taskmgr_sample *previous = find_previous(sample->pid);
        uint64_t delta = 0;

        if (previous != 0 && sample->cpu_ticks > previous->cpu_ticks)
        {
            delta = sample->cpu_ticks - previous->cpu_ticks;
        }
        if (ticks_delta != 0ull)
        {
            sample->cpu_percent = (unsigned int)((delta * 100ull) / ticks_delta);
        }
        if ((sample->flags & SAVANXP_PROC_FLAG_IDLE) != 0u)
        {
            idle_delta = delta;
        }
    }

    /* El uso del sistema es lo que el ocioso NO se llevo. Si el ocioso no
     * aparecio en la muestra anterior -- la primera pasada -- queda en cero, que
     * es lo honesto: todavia no hay intervalo que medir. */
    g_cpu_percent = 0;
    if (ticks_delta != 0ull && idle_delta <= ticks_delta)
    {
        g_cpu_percent = (unsigned int)(((ticks_delta - idle_delta) * 100ull) / ticks_delta);
    }

    /* La memoria sale del pool del asignador de paginas, no del mapa de memoria
     * del boot: total y libre tienen que venir de la misma cuenta o el "en uso"
     * no cierra. */
    g_memory_total_bytes = g_info.memory_total_pages * 4096ull;
    g_memory_used_bytes = g_memory_total_bytes > g_info.memory_free_bytes
        ? g_memory_total_bytes - g_info.memory_free_bytes
        : 0ull;
    g_memory_percent = g_memory_total_bytes != 0ull
        ? (unsigned int)((g_memory_used_bytes * 100ull) / g_memory_total_bytes)
        : 0u;

    sample_network();

    push_history(g_cpu_history, (unsigned char)(g_cpu_percent > 100u ? 100u : g_cpu_percent));
    push_history(g_memory_history, (unsigned char)(g_memory_percent > 100u ? 100u : g_memory_percent));
    push_history(g_net_history,
                 (unsigned char)(g_net_peak_rate != 0ul
                                     ? ((g_net_rate * 100ul) / g_net_peak_rate)
                                     : 0ul));
    if (g_history_length < TASKMGR_HISTORY)
    {
        g_history_length += 1;
    }

    memcpy(g_previous, g_processes, sizeof(g_previous));
    g_previous_count = g_process_count;
    g_previous_ticks_total = g_info.cpu_ticks_total;
    g_have_previous = 1;

    build_rows();

    snprintf(g_status_processes, sizeof(g_status_processes), "Processes: %d", g_process_count);
    snprintf(g_status_cpu, sizeof(g_status_cpu), "CPU Usage: %u%%", g_cpu_percent);
    snprintf(g_status_memory, sizeof(g_status_memory), "Mem Usage: %lluM / %lluM",
             (unsigned long long)(g_memory_used_bytes / (1024ull * 1024ull)),
             (unsigned long long)(g_memory_total_bytes / (1024ull * 1024ull)));
}

/* ---- layout ---------------------------------------------------------------
 *
 * Se recalcula a partir del tamano de la ventana, no al reves: esta ventana se
 * estira y su contenido es una lista y unos graficos que tienen que aprovechar
 * lo que haya. */

static int content_top(void)
{
    return sxgui_menubar_height();
}

static void relayout(void)
{
    int width = (int)g_app.gfx.info.width;
    int height = (int)g_app.gfx.info.height;
    int top = content_top() + TASKMGR_MARGIN;
    int status_y = height - TASKMGR_MARGIN - SXGUI_STATUS_HEIGHT;
    int tabs_height = status_y - SXGUI_GAP - top;
    struct sx_rect page;
    int list_height;
    int panel_width;

    if (width < TASKMGR_MIN_WIDTH)
    {
        width = TASKMGR_MIN_WIDTH;
    }
    if (tabs_height < 120)
    {
        tabs_height = 120;
    }

    TASKMGR_TABS->rect = sx_rect_make(
        TASKMGR_MARGIN, top, width - (TASKMGR_MARGIN * 2), tabs_height);

    page = sxgui_tabs_page(TASKMGR_TABS);
    list_height = page.height - (TASKMGR_PAGE_PAD * 2) - SXGUI_BUTTON_HEIGHT - SXGUI_GAP;
    if (list_height < 60)
    {
        list_height = 60;
    }

    TASKMGR_LIST->rect = sx_rect_make(
        page.x + TASKMGR_PAGE_PAD,
        page.y + TASKMGR_PAGE_PAD,
        page.width - (TASKMGR_PAGE_PAD * 2),
        list_height);
    g_columns[0].width = TASKMGR_LIST->rect.width - (SXGUI_BORDER_SUNKEN * 2) -
        SXGUI_SCROLLBAR_THICKNESS - TASKMGR_COLUMN_PID_WIDTH - TASKMGR_COLUMN_CPU_WIDTH -
        TASKMGR_COLUMN_MEMORY_WIDTH - TASKMGR_COLUMN_HANDLES_WIDTH;
    if (g_columns[0].width < 80)
    {
        g_columns[0].width = 80;
    }

    TASKMGR_END_BUTTON->rect = sx_rect_make(
        TASKMGR_LIST->rect.x + TASKMGR_LIST->rect.width - SXGUI_BUTTON_WIDTH - 26,
        TASKMGR_LIST->rect.y + TASKMGR_LIST->rect.height + SXGUI_GAP,
        SXGUI_BUTTON_WIDTH + 26,
        SXGUI_BUTTON_HEIGHT);

    /* La barra de estado son tres paneles hundidos, como la del original: el
     * primero se lleva lo que sobra y los otros dos miden lo suyo. */
    panel_width = 150;
    TASKMGR_STATUS_MEMORY->rect = sx_rect_make(
        width - TASKMGR_MARGIN - panel_width, status_y, panel_width, SXGUI_STATUS_HEIGHT);
    TASKMGR_STATUS_CPU->rect = sx_rect_make(
        TASKMGR_STATUS_MEMORY->rect.x - SXGUI_GAP - 110, status_y, 110, SXGUI_STATUS_HEIGHT);
    TASKMGR_STATUS_PROCESSES->rect = sx_rect_make(
        TASKMGR_MARGIN, status_y,
        TASKMGR_STATUS_CPU->rect.x - SXGUI_GAP - TASKMGR_MARGIN, SXGUI_STATUS_HEIGHT);
}

/* Los controles de la lista solo existen en su pestania; en las otras dos el
 * contenido lo pinta la app y dejarlos visibles seria dibujarlos encima. */
static void apply_tab_visibility(void)
{
    int on_processes = TASKMGR_TABS->value == TASKMGR_TAB_PROCESSES;

    if (on_processes)
    {
        TASKMGR_LIST->flags |= SXGUI_FLAG_VISIBLE;
        TASKMGR_END_BUTTON->flags |= SXGUI_FLAG_VISIBLE;
    }
    else
    {
        TASKMGR_LIST->flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
        TASKMGR_END_BUTTON->flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
    }
}

/* ---- pintado -------------------------------------------------------------- */

static void draw_etched(struct sx_painter *painter, struct sx_rect rect)
{
    sx_painter_draw_frame(painter,
                          sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 1, rect.height - 1),
                          SXGUI_COLOR_LIGHT);
    sx_painter_draw_frame(painter,
                          sx_rect_make(rect.x, rect.y, rect.width - 1, rect.height - 1),
                          SXGUI_COLOR_SHADOW);
}

/* Marco de grupo identico al del toolkit (sxgui_paint_groupbox): el rotulo se
 * monta sobre el borde de arriba, con el fondo de la ventana detras. Se dibuja
 * a mano y no con widgets porque estos marcos cambian con la pestania activa, y
 * como widgets habria que prenderlos y apagarlos en cada cambio. */
static struct sx_rect draw_group(struct sx_painter *painter, struct sx_rect rect, const char *title)
{
    struct sx_rect frame = rect;
    int text_height = gfx_text_height();

    frame.y += text_height / 2;
    frame.height -= text_height / 2;
    draw_etched(painter, frame);

    if (title != 0)
    {
        int text_x = rect.x + SXGUI_MARGIN;
        struct sx_rect caption =
            sx_rect_make(text_x - 3, rect.y, gfx_text_width(title) + 6, text_height);

        sx_painter_fill_rect(painter, caption, SXGUI_COLOR_WINDOW);
        sx_painter_draw_text(painter, text_x, rect.y, title, SXGUI_COLOR_TEXT);
    }

    /* Area util: adentro del marco y con aire, que es donde va el contenido. */
    return sx_rect_make(frame.x + SXGUI_GAP, frame.y + SXGUI_GAP,
                        frame.width - (SXGUI_GAP * 2), frame.height - (SXGUI_GAP * 2));
}

static void draw_sunken(struct sx_painter *painter, struct sx_rect rect)
{
    int right = rect.x + rect.width - 1;
    int bottom = rect.y + rect.height - 1;

    sx_painter_hline(painter, rect.x, rect.y, rect.width, SXGUI_COLOR_SHADOW);
    sx_painter_vline(painter, rect.x, rect.y, rect.height, SXGUI_COLOR_SHADOW);
    sx_painter_hline(painter, rect.x, bottom, rect.width, SXGUI_COLOR_LIGHT);
    sx_painter_vline(painter, right, rect.y, rect.height, SXGUI_COLOR_LIGHT);
}

/* Medidor vertical: la barra que el original dibuja al lado de cada grafico.
 * El relleno va por bloques con un pixel de corte, que es lo que le da el
 * aspecto de segmentos en vez de barra lisa. */
static void draw_meter(struct sx_painter *painter, struct sx_rect rect, unsigned int percent, uint32_t colour)
{
    struct sx_rect inner;
    int filled;
    int y;

    draw_sunken(painter, rect);
    inner = sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2);
    sx_painter_fill_rect(painter, inner, TASKMGR_GRAPH_BACKGROUND);

    if (percent > 100u)
    {
        percent = 100u;
    }
    filled = (inner.height * (int)percent) / 100;
    for (y = 0; y < filled; y += 3)
    {
        int height = (filled - y) < 2 ? (filled - y) : 2;

        sx_painter_fill_rect(
            painter,
            sx_rect_make(inner.x, inner.y + inner.height - y - height, inner.width, height),
            colour);
    }
}

/* Grafico de historia: grilla fija y una muestra por pixel, de derecha a
 * izquierda. Las muestras viejas viven al principio del arreglo, asi que la
 * ultima siempre cae contra el borde derecho aunque todavia no se haya llenado. */
static void draw_history(
    struct sx_painter *painter,
    struct sx_rect rect,
    const unsigned char *history,
    uint32_t colour)
{
    struct sx_rect inner;
    int step = 12;
    int offset;
    int index;
    int previous_x = 0;
    int previous_y = 0;
    int have_previous = 0;

    draw_sunken(painter, rect);
    inner = sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2);
    if (inner.width <= 0 || inner.height <= 0)
    {
        return;
    }
    sx_painter_fill_rect(painter, inner, TASKMGR_GRAPH_BACKGROUND);

    for (offset = step; offset < inner.width; offset += step)
    {
        sx_painter_vline(painter, inner.x + offset, inner.y, inner.height, TASKMGR_GRAPH_GRID);
    }
    for (offset = step; offset < inner.height; offset += step)
    {
        sx_painter_hline(painter, inner.x, inner.y + offset, inner.width, TASKMGR_GRAPH_GRID);
    }

    for (index = 0; index < inner.width; ++index)
    {
        int slot = TASKMGR_HISTORY - 1 - (inner.width - 1 - index);
        int value;
        int x;
        int y;

        if (slot < TASKMGR_HISTORY - g_history_length || slot < 0)
        {
            continue;
        }
        value = history[slot];
        if (value > 100)
        {
            value = 100;
        }
        x = inner.x + index;
        y = inner.y + inner.height - 1 - ((inner.height - 1) * value) / 100;
        if (have_previous)
        {
            sx_painter_draw_line(painter, previous_x, previous_y, x, y, colour);
        }
        else
        {
            sx_painter_set_pixel(painter, x, y, colour);
        }
        previous_x = x;
        previous_y = y;
        have_previous = 1;
    }
}

/* Fila "rotulo ..... valor" de las cajas de totales, con el valor pegado al
 * borde derecho como en el original. */
static void draw_field(struct sx_painter *painter, struct sx_rect area, int row, const char *label, const char *value)
{
    int y = area.y + (row * TASKMGR_ROW);

    sx_painter_draw_text(painter, area.x, y, label, SXGUI_COLOR_TEXT);
    sx_painter_draw_text(painter,
                         area.x + area.width - gfx_text_width(value),
                         y,
                         value,
                         SXGUI_COLOR_TEXT);
}

static void paint_performance(struct sx_painter *painter, struct sx_rect page)
{
    int pad = TASKMGR_PAGE_PAD;
    int x = page.x + pad;
    int y = page.y + pad;
    int full_width = page.width - (pad * 2);
    int meter_width = 60;
    int totals_height = TASKMGR_GROUP_HEIGHT(3);
    int graph_height = (page.height - (pad * 3) - totals_height) / 2;
    char number[32];
    struct sx_rect area;

    if (graph_height < 56)
    {
        graph_height = 56;
    }

    /* Fila 1: medidor de CPU + historia de CPU. */
    area = draw_group(painter, sx_rect_make(x, y, meter_width + 24, graph_height), "CPU Usage");
    snprintf(number, sizeof(number), "%u %%", g_cpu_percent);
    sx_painter_draw_text(painter,
                         area.x + ((area.width - gfx_text_width(number)) / 2),
                         area.y,
                         number,
                         SXGUI_COLOR_TEXT);
    draw_meter(painter,
               sx_rect_make(area.x + ((area.width - 24) / 2), area.y + TASKMGR_ROW, 24,
                            area.height - TASKMGR_ROW),
               g_cpu_percent,
               TASKMGR_GRAPH_LINE);

    area = draw_group(painter,
                      sx_rect_make(x + meter_width + 24 + SXGUI_GAP, y,
                                   full_width - meter_width - 24 - SXGUI_GAP, graph_height),
                      "CPU Usage History");
    draw_history(painter, area, g_cpu_history, TASKMGR_GRAPH_LINE);
    y += graph_height + pad;

    /* Fila 2: lo mismo para la memoria. */
    area = draw_group(painter, sx_rect_make(x, y, meter_width + 24, graph_height), "Mem Usage");
    snprintf(number, sizeof(number), "%u %%", g_memory_percent);
    sx_painter_draw_text(painter,
                         area.x + ((area.width - gfx_text_width(number)) / 2),
                         area.y,
                         number,
                         SXGUI_COLOR_TEXT);
    draw_meter(painter,
               sx_rect_make(area.x + ((area.width - 24) / 2), area.y + TASKMGR_ROW, 24,
                            area.height - TASKMGR_ROW),
               g_memory_percent,
               TASKMGR_GRAPH_LINE_ALT);

    area = draw_group(painter,
                      sx_rect_make(x + meter_width + 24 + SXGUI_GAP, y,
                                   full_width - meter_width - 24 - SXGUI_GAP, graph_height),
                      "Memory Usage History");
    draw_history(painter, area, g_memory_history, TASKMGR_GRAPH_LINE_ALT);
    y += graph_height + pad;

    /* Fila 3: los dos cuadros de numeros. */
    {
        int half = (full_width - SXGUI_GAP) / 2;
        unsigned long long uptime_seconds = g_info.uptime_ms / 1000ull;

        area = draw_group(painter, sx_rect_make(x, y, half, totals_height), "Totals");
        snprintf(number, sizeof(number), "%d", g_process_count);
        draw_field(painter, area, 0, "Processes", number);
        snprintf(number, sizeof(number), "%u", (unsigned)g_total_handles);
        draw_field(painter, area, 1, "Handles", number);
        snprintf(number, sizeof(number), "%llu:%02llu:%02llu",
                 uptime_seconds / 3600ull,
                 (uptime_seconds / 60ull) % 60ull,
                 uptime_seconds % 60ull);
        draw_field(painter, area, 2, "Up Time", number);

        area = draw_group(painter, sx_rect_make(x + half + SXGUI_GAP, y, half, totals_height),
                          "Physical Memory (K)");
        format_grouped(g_memory_total_bytes / 1024ull, number, sizeof(number));
        draw_field(painter, area, 0, "Total", number);
        format_grouped(g_info.memory_free_bytes / 1024ull, number, sizeof(number));
        draw_field(painter, area, 1, "Available", number);
        format_grouped(g_memory_used_bytes / 1024ull, number, sizeof(number));
        draw_field(painter, area, 2, "In Use", number);
    }
}

static void paint_networking(struct sx_painter *painter, struct sx_rect page)
{
    int pad = TASKMGR_PAGE_PAD;
    int x = page.x + pad;
    int y = page.y + pad;
    int full_width = page.width - (pad * 2);
    int adapter_height = TASKMGR_GROUP_HEIGHT(5);
    int graph_height = page.height - (pad * 3) - adapter_height;
    char number[64];
    struct sx_rect area;

    if (graph_height < 80)
    {
        graph_height = 80;
    }

    area = draw_group(painter, sx_rect_make(x, y, full_width, graph_height), "Network Utilization");
    if (g_net_available)
    {
        draw_history(painter, area, g_net_history, TASKMGR_GRAPH_LINE);
    }
    else
    {
        sx_painter_draw_text(painter, area.x, area.y, "No network adapter present.", SXGUI_COLOR_TEXT);
    }
    y += graph_height + pad;

    area = draw_group(painter, sx_rect_make(x, y, full_width, adapter_height), "Adapter");
    if (!g_net_available)
    {
        sx_painter_draw_text(painter, area.x, area.y, "net0: not present", SXGUI_COLOR_TEXT);
        return;
    }

    snprintf(number, sizeof(number), "%s, link %s",
             g_net.up != 0 ? "up" : "down",
             g_net.link != 0 ? "yes" : "no");
    draw_field(painter, area, 0, "net0", number);
    snprintf(number, sizeof(number), "%02x:%02x:%02x:%02x:%02x:%02x",
             g_net.mac[0], g_net.mac[1], g_net.mac[2], g_net.mac[3], g_net.mac[4], g_net.mac[5]);
    draw_field(painter, area, 1, "Hardware address", number);
    snprintf(number, sizeof(number), "%u.%u.%u.%u",
             (unsigned)((g_net.ipv4 >> 24) & 0xffu),
             (unsigned)((g_net.ipv4 >> 16) & 0xffu),
             (unsigned)((g_net.ipv4 >> 8) & 0xffu),
             (unsigned)(g_net.ipv4 & 0xffu));
    draw_field(painter, area, 2, "Address", number);
    snprintf(number, sizeof(number), "%lu frames/s (peak %lu)", g_net_rate, g_net_peak_rate);
    draw_field(painter, area, 3, "Throughput", number);
    snprintf(number, sizeof(number), "%u sent, %u received, %u errors",
             (unsigned)g_net.tx_frames, (unsigned)g_net.rx_frames,
             (unsigned)(g_net.tx_errors + g_net.rx_errors));
    draw_field(painter, area, 4, "Frames", number);
}

/* Debajo de la lista, el detalle de la fila elegida: estado y padre, que no
 * tienen columna propia pero son lo primero que uno quiere de un proceso que
 * esta por matar. */
static void paint_processes(struct sx_painter *painter, struct sx_rect page)
{
    const struct taskmgr_sample *sample = selected_sample();
    char line[TASKMGR_NAME_CAPACITY + 96];
    int y = TASKMGR_END_BUTTON->rect.y + ((SXGUI_BUTTON_HEIGHT - gfx_text_height()) / 2);

    (void)page;
    if (sample == 0)
    {
        return;
    }
    snprintf(line, sizeof(line), "%s  -  %s, started by PID %u",
             sample->name, state_name(sample->state), (unsigned)sample->parent_pid);
    sx_painter_draw_text(painter, TASKMGR_LIST->rect.x, y, line, SXGUI_COLOR_TEXT);
}

static void on_paint(struct sxgui_app *app)
{
    struct sx_painter *painter = &app->ui.painter;
    struct sx_rect page = sxgui_tabs_page(TASKMGR_TABS);

    if (!sx_painter_push_clip(painter, page))
    {
        return;
    }
    switch (TASKMGR_TABS->value)
    {
    case TASKMGR_TAB_PERFORMANCE:
        paint_performance(painter, page);
        break;
    case TASKMGR_TAB_NETWORKING:
        paint_networking(painter, page);
        break;
    default:
        paint_processes(painter, page);
        break;
    }
    sx_painter_pop_clip(painter);
}

/* ---- acciones ------------------------------------------------------------- */

static void on_tick(struct sxgui_app *app)
{
    refresh_sample();
    sxgui_app_request_repaint(app);
}

static void set_update_speed(int index)
{
    int item;

    if (index < 0 || index >= TASKMGR_SPEED_COUNT)
    {
        return;
    }
    g_speed_index = index;
    for (item = 0; item < TASKMGR_SPEED_COUNT; ++item)
    {
        if (item == index)
        {
            k_view_items[TASKMGR_SPEED_FIRST_ITEM + item].flags |= SXGUI_MENU_CHECKED;
        }
        else
        {
            k_view_items[TASKMGR_SPEED_FIRST_ITEM + item].flags &= ~(uint32_t)SXGUI_MENU_CHECKED;
        }
    }
    g_app.tick_interval_ms = k_speed_ms[index];
}

static void on_tab_changed(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    apply_tab_visibility();
    sxgui_app_request_repaint((struct sxgui_app *)user);
}

static void on_list(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    update_end_command();
    sxgui_app_request_repaint((struct sxgui_app *)user);
}

static void on_confirm_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 0);
}

static void on_confirm_accept(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);

    if (g_pending_power == TASKMGR_CMD_SHUTDOWN)
    {
        /* No retorna si tiene exito. */
        (void)power_shutdown();
        g_pending_power = 0;
        return;
    }
    if (g_pending_power == TASKMGR_CMD_REBOOT)
    {
        (void)power_reboot();
        g_pending_power = 0;
        return;
    }

    if (g_confirm_pid != 0u)
    {
        (void)savanxp_kill((int)g_confirm_pid, SAVANXP_SIGKILL);
        g_confirm_pid = 0u;
        /* Se remuestrea ya: esperar al proximo tick dejaria la fila muerta en
         * la lista hasta un segundo despues de haberla matado. */
        refresh_sample();
        sxgui_app_request_repaint(&g_app);
    }
}

static void begin_confirm(const char *title, const char *line, const char *detail, int default_cancel)
{
    snprintf(g_confirm_line, sizeof(g_confirm_line), "%s", line);
    snprintf(g_confirm_detail, sizeof(g_confirm_detail), "%s", detail);

    memset(&g_confirm_dialog, 0, sizeof(g_confirm_dialog));
    g_confirm_dialog.title = title;
    g_confirm_dialog.widgets = g_confirm_widgets;
    g_confirm_dialog.widget_count = 4;
    g_confirm_dialog.initial_focus = default_cancel ? 3 : 2;
    g_confirm_dialog.default_button = default_cancel ? 3 : 2;
    sxgui_dialog_begin(&g_app.ui, &g_confirm_dialog, TASKMGR_CONFIRM_WIDTH, TASKMGR_CONFIRM_HEIGHT);
}

static void end_selected_process(void)
{
    const struct taskmgr_sample *sample = selected_sample();
    char line[TASKMGR_NAME_CAPACITY + 64];

    if (TASKMGR_TABS->value != TASKMGR_TAB_PROCESSES || sample == 0)
    {
        return;
    }
    /* Matar el ocioso dejaria al planificador sin a que volver. El kernel ya lo
     * rechaza, pero preguntarlo primero es peor que no ofrecerlo. */
    if ((sample->flags & SAVANXP_PROC_FLAG_IDLE) != 0u)
    {
        return;
    }

    g_pending_power = 0;
    g_confirm_pid = sample->pid;
    snprintf(line, sizeof(line), "End '%s' (PID %u)?", sample->name, (unsigned)sample->pid);
    begin_confirm("End Process", line,
                  "Unsaved data will be lost and the system may become unstable.", 1);
}

static void on_end_process(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    end_selected_process();
}

static void on_run_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 0);
}

static void on_run_accept(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
    if (g_run_path[0] == '\0')
    {
        return;
    }
    /* El lanzamiento pasa por el WM, que es el que le arma la ventana al hijo:
     * un spawn directo daria un proceso sin superficie. */
    (void)gfx_desktop_launch_ex(&g_app.gfx, g_run_path, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE);
}

static void begin_run_dialog(void)
{
    memset(&g_run_dialog, 0, sizeof(g_run_dialog));
    g_run_dialog.title = "Create New Task";
    g_run_dialog.widgets = g_run_widgets;
    g_run_dialog.widget_count = 4;
    g_run_dialog.initial_focus = 1;
    g_run_dialog.default_button = 2;
    sxgui_dialog_begin(&g_app.ui, &g_run_dialog, TASKMGR_RUN_WIDTH, TASKMGR_RUN_HEIGHT);
}

static void on_menu_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case TASKMGR_CMD_NEW_TASK:
        begin_run_dialog();
        break;
    case TASKMGR_CMD_EXIT:
        sxgui_app_quit(&g_app, 0);
        break;
    case TASKMGR_CMD_REFRESH:
        refresh_sample();
        sxgui_app_request_repaint(&g_app);
        break;
    case TASKMGR_CMD_SPEED_HIGH:
        set_update_speed(0);
        break;
    case TASKMGR_CMD_SPEED_NORMAL:
        set_update_speed(1);
        break;
    case TASKMGR_CMD_SPEED_LOW:
        set_update_speed(2);
        break;
    case TASKMGR_CMD_SPEED_PAUSED:
        set_update_speed(3);
        break;
    case TASKMGR_CMD_END_PROCESS:
        end_selected_process();
        break;
    case TASKMGR_CMD_SHUTDOWN:
        g_pending_power = TASKMGR_CMD_SHUTDOWN;
        g_confirm_pid = 0u;
        begin_confirm("Turn Off", "Turn off the computer?",
                      "Every running program will be closed.", 1);
        break;
    case TASKMGR_CMD_REBOOT:
        g_pending_power = TASKMGR_CMD_REBOOT;
        g_confirm_pid = 0u;
        begin_confirm("Restart", "Restart the computer?",
                      "Every running program will be closed.", 1);
        break;
    case TASKMGR_CMD_ABOUT:
        (void)gfx_desktop_launch_ex(&g_app.gfx, "/bin/aboutapp", SAVANXP_DESKTOP_LAUNCH_FLAG_NONE);
        break;
    default:
        break;
    }
}

static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return 0;
    }
    if (sxgui_dialog_active(&app->ui))
    {
        return 0;
    }
    if (event->key == SAVANXP_KEY_F5)
    {
        refresh_sample();
        return 1;
    }
    if (event->key == SAVANXP_KEY_DELETE)
    {
        end_selected_process();
        return 1;
    }
    return 0;
}

static void on_resize(struct sxgui_app *app)
{
    (void)app;
    relayout();
}

/* ---- autoprueba -----------------------------------------------------------
 *
 * Headless, sin ventana: comprueba que el muestreo sea coherente consigo mismo,
 * que es lo unico que se puede afirmar sin mirar la pantalla. Si el kernel deja
 * de exportar los contadores, esto lo dice.
 */
static int selftest(void)
{
    int failures = 0;
    int index;
    uint64_t ticks_before;
    char grouped[32];

    refresh_sample();
    ticks_before = g_info.cpu_ticks_total;
    if (g_process_count <= 0)
    {
        printf("TASKMGR SMOKE FAIL no se enumero ningun proceso\n");
        failures += 1;
    }

    {
        int idle_seen = 0;
        int self_seen = 0;
        uint32_t self_pid = (uint32_t)savanxp_getpid();

        for (index = 0; index < g_process_count; ++index)
        {
            if ((g_processes[index].flags & SAVANXP_PROC_FLAG_IDLE) != 0u)
            {
                idle_seen += 1;
            }
            if (g_processes[index].pid == self_pid)
            {
                self_seen = 1;
                if (g_processes[index].memory_bytes == 0ull)
                {
                    printf("TASKMGR SMOKE FAIL el propio proceso reporta 0 bytes mapeados\n");
                    failures += 1;
                }
            }
        }
        if (idle_seen != 1)
        {
            printf("TASKMGR SMOKE FAIL se esperaba un proceso ocioso, hay %d\n", idle_seen);
            failures += 1;
        }
        if (!self_seen)
        {
            printf("TASKMGR SMOKE FAIL el propio proceso no aparece en la enumeracion\n");
            failures += 1;
        }
    }

    /* Segunda muestra, tras dormir: el contador global tiene que haber
     * avanzado y el tiempo se lo tiene que haber llevado el ocioso. Que el uso
     * del sistema de un intervalo DORMIDO de "casi 0" es la prueba de que el
     * contador del ocioso avanza: si no avanzara, esto daria 100%. */
    sleep_ms(600);
    refresh_sample();
    if (g_info.cpu_ticks_total <= ticks_before)
    {
        printf("TASKMGR SMOKE FAIL el contador global de ticks no avanzo\n");
        failures += 1;
    }
    if (g_cpu_percent > 25u)
    {
        printf("TASKMGR SMOKE FAIL durmiendo, el sistema reporta %u%% de CPU\n", g_cpu_percent);
        failures += 1;
    }
    if (g_memory_total_bytes == 0ull || g_memory_used_bytes > g_memory_total_bytes)
    {
        printf("TASKMGR SMOKE FAIL memoria incoherente: %llu usados de %llu\n",
               (unsigned long long)g_memory_used_bytes,
               (unsigned long long)g_memory_total_bytes);
        failures += 1;
    }
    for (index = 0; index < g_process_count; ++index)
    {
        if (g_processes[index].cpu_percent > 100u)
        {
            printf("TASKMGR SMOKE FAIL pid %u con %u%% de CPU\n",
                   (unsigned)g_processes[index].pid, g_processes[index].cpu_percent);
            failures += 1;
            break;
        }
    }

    /* Tercera muestra, tras quemar CPU de verdad: ahora el intervalo tiene que
     * atribuirse a ESTE proceso. Es la contracara de la segunda y lo que
     * distingue "los contadores se mueven" de "los contadores se mueven en el
     * proceso correcto". */
    {
        unsigned long deadline = uptime_ms() + 600ul;
        unsigned int self_percent = 0;
        uint32_t self_pid = (uint32_t)savanxp_getpid();

        while (uptime_ms() < deadline)
        {
            /* Girar sin dormir: la unica forma de quedarse con el tiempo. */
        }
        refresh_sample();
        for (index = 0; index < g_process_count; ++index)
        {
            if (g_processes[index].pid == self_pid)
            {
                self_percent = g_processes[index].cpu_percent;
            }
        }
        if (g_cpu_percent < 50u)
        {
            printf("TASKMGR SMOKE FAIL girando, el sistema reporta %u%% de CPU\n", g_cpu_percent);
            failures += 1;
        }
        if (self_percent < 50u)
        {
            printf("TASKMGR SMOKE FAIL girando, el propio proceso reporta %u%% de CPU\n", self_percent);
            failures += 1;
        }
    }

    format_grouped(1234567ull, grouped, sizeof(grouped));
    if (strcmp(grouped, "1,234,567") != 0)
    {
        printf("TASKMGR SMOKE FAIL agrupado de miles: '%s'\n", grouped);
        failures += 1;
    }

    if (failures != 0)
    {
        printf("TASKMGR SMOKE FAIL %d checks\n", failures);
        return 1;
    }
    printf("TASKMGR SMOKE PASS processes=%d cpu=%u%% mem=%u%% handles=%u\n",
           g_process_count, g_cpu_percent, g_memory_percent, (unsigned)g_total_handles);
    return 0;
}

/* ---- arranque ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        return selftest();
    }

    *TASKMGR_TABS = sxgui_tabs(sx_rect_make(0, 0, 0, 0), k_tab_labels, TASKMGR_TAB_COUNT,
                               TASKMGR_TAB_PROCESSES);
    TASKMGR_TABS->on_action = on_tab_changed;
    TASKMGR_TABS->user = &g_app;

    *TASKMGR_LIST = sxgui_listbox(sx_rect_make(0, 0, 0, 0), g_row_pointers, 0);
    TASKMGR_LIST->columns = g_columns;
    TASKMGR_LIST->column_count = TASKMGR_COLUMN_COUNT;
    TASKMGR_LIST->on_action = on_list;
    TASKMGR_LIST->user = &g_app;

    *TASKMGR_END_BUTTON = sxgui_button(sx_rect_make(0, 0, 0, 0), "End Process", on_end_process, 0);

    *TASKMGR_STATUS_PROCESSES = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status_processes);
    TASKMGR_STATUS_PROCESSES->flags |= SXGUI_FLAG_SUNKEN;
    *TASKMGR_STATUS_CPU = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status_cpu);
    TASKMGR_STATUS_CPU->flags |= SXGUI_FLAG_SUNKEN;
    *TASKMGR_STATUS_MEMORY = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status_memory);
    TASKMGR_STATUS_MEMORY->flags |= SXGUI_FLAG_SUNKEN;

    g_confirm_widgets[0] = sxgui_label(
        sx_rect_make(TASKMGR_DIALOG_MARGIN, TASKMGR_DIALOG_MARGIN,
                     TASKMGR_CONFIRM_WIDTH - (TASKMGR_DIALOG_MARGIN * 2), TASKMGR_ROW),
        g_confirm_line);
    g_confirm_widgets[1] = sxgui_label(
        sx_rect_make(TASKMGR_DIALOG_MARGIN, TASKMGR_DIALOG_MARGIN + TASKMGR_ROW + 2,
                     TASKMGR_CONFIRM_WIDTH - (TASKMGR_DIALOG_MARGIN * 2), TASKMGR_ROW),
        g_confirm_detail);
    g_confirm_widgets[2] = sxgui_button(
        sx_rect_make(centred_button_x(TASKMGR_CONFIRM_WIDTH, 2, 0),
                     TASKMGR_CONFIRM_HEIGHT - TASKMGR_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Yes", on_confirm_accept, 0);
    g_confirm_widgets[3] = sxgui_button(
        sx_rect_make(centred_button_x(TASKMGR_CONFIRM_WIDTH, 2, 1),
                     TASKMGR_CONFIRM_HEIGHT - TASKMGR_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "No", on_confirm_cancel, 0);

    g_run_widgets[0] = sxgui_label(
        sx_rect_make(TASKMGR_DIALOG_MARGIN, TASKMGR_DIALOG_MARGIN,
                     TASKMGR_RUN_WIDTH - (TASKMGR_DIALOG_MARGIN * 2), TASKMGR_ROW),
        "Type the path of a program and it will be opened:");
    g_run_widgets[1] = sxgui_textfield(
        sx_rect_make(TASKMGR_DIALOG_MARGIN, TASKMGR_DIALOG_MARGIN + TASKMGR_ROW + SXGUI_GAP,
                     TASKMGR_RUN_WIDTH - (TASKMGR_DIALOG_MARGIN * 2), SXGUI_FIELD_HEIGHT),
        g_run_path, (int)sizeof(g_run_path));
    g_run_widgets[2] = sxgui_button(
        sx_rect_make(centred_button_x(TASKMGR_RUN_WIDTH, 2, 0),
                     TASKMGR_RUN_HEIGHT - TASKMGR_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_run_accept, 0);
    g_run_widgets[3] = sxgui_button(
        sx_rect_make(centred_button_x(TASKMGR_RUN_WIDTH, 2, 1),
                     TASKMGR_RUN_HEIGHT - TASKMGR_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Cancel", on_run_cancel, 0);

    if (sxgui_app_init(&g_app, "taskmgr", g_widgets, TASKMGR_WIDGET_COUNT) < 0)
    {
        return 1;
    }

    g_menubar.menus = k_menus;
    g_menubar.menu_count = (int)(sizeof(k_menus) / sizeof(k_menus[0]));
    g_menubar.on_command = on_menu_command;
    sxgui_set_menubar(&g_app.ui, &g_menubar);

    (void)sxgui_app_set_content_size(&g_app, TASKMGR_DEFAULT_WIDTH, TASKMGR_DEFAULT_HEIGHT);
    relayout();
    apply_tab_visibility();
    refresh_sample();

    g_app.on_paint = on_paint;
    g_app.on_key = on_key;
    g_app.on_resize = on_resize;
    g_app.on_tick = on_tick;
    set_update_speed(g_speed_index);
    sxgui_focus(&g_app.ui, 1);
    return sxgui_app_run(&g_app);
}
