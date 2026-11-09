/*
 * Task Manager, a SUN program (docs/userland/SUN.md).
 *
 * The system monitor in the shape of the classic Task Manager: Applications,
 * Processes, Performance and Networking tabs, a status bar with the usual
 * three numbers, and a menu that can start a new task or turn the machine
 * off.
 *
 * HOW CPU USAGE IS MEASURED. The kernel keeps no percentages: it keeps a tick
 * counter per process (savanxp_process_info.cpu_ticks) and the global tick
 * counter (savanxp_system_info.cpu_ticks_total). One tick increments exactly
 * one process, so between two samples the per-process increments add up to
 * the global one, and each process's percentage is its share of that pie. The
 * system's usage is what the idle processes, one per core, did NOT take. That
 * is why everything shown comes from comparing two samples: one alone says
 * nothing (docs/userland/SYSTEM_MONITORING.md).
 *
 * With several cores the pie is all of them together: a process that keeps
 * one core of four busy reads 25%, as in the classic Task Manager.
 *
 * APPLICATIONS lists windows, not processes: a copy of the taskbar's list the
 * WM keeps for this window (sxnwm_task_list_open). There is no Users tab:
 * with one account, who runs each process is the User Name column.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "abi/savanxp_abi.h"
#include "sxgfx/gfx2d.h"
#include "sxgfx/pixels.h"
#include "sxgfx/sxchrome.h"
#include "sxgfx/sxgui.h"
#include "sxn/sxn.h"
#include "sxn/sxnelevate.h"
#include "sxn/sxngui.h"

#include "taskmgr.sunres.h"

/* ---- limits ----------------------------------------------------------------
 *
 * The kernel has 64 process slots (process::kMaxProcesses) and 64 handles per
 * process; enumerating more than that is impossible, so the tables here are
 * sized to that ceiling and need no paging. */
#define TASKMGR_MAX_PROCESSES 64
#define TASKMGR_NAME_CAPACITY 32
#define TASKMGR_ROW_CAPACITY 96
/* Samples of graph history. At the normal speed (1 s) that is two minutes,
 * which is what fits at one pixel per sample in a comfortable window. */
#define TASKMGR_HISTORY 120

#define TASKMGR_PATH "/system/taskmgr.sxe"
/* What POSIX's kill reports for SIGKILL, so a parent's waitpid reads the same
 * whichever subsystem ended its child. */
#define TASKMGR_END_STATUS (128 + 9)

/* ---- tabs ------------------------------------------------------------------- */

#define TASKMGR_TAB_APPLICATIONS 0
#define TASKMGR_TAB_PROCESSES 1
#define TASKMGR_TAB_PERFORMANCE 2
#define TASKMGR_TAB_NETWORKING 3

static const char *const k_tab_labels[] = {"Applications", "Processes", "Performance", "Networking"};
#define TASKMGR_TAB_COUNT ((int)(sizeof(k_tab_labels) / sizeof(k_tab_labels[0])))

/* ---- grid ------------------------------------------------------------------- */

#define TASKMGR_MARGIN SXGUI_MARGIN
#define TASKMGR_ROW 18
#define TASKMGR_PAGE_PAD 10
/* Height a group box needs to show `rows` whole rows. The sum has to include
 * what draw_group keeps for itself: half the height of the caption riding on
 * the top edge, and the air on both sides. Pinning it to "rows * step + a
 * bit" is what left the last row cut off. */
#define TASKMGR_GROUP_HEIGHT(rows) \
    ((rows) * TASKMGR_ROW + TASKMGR_ROW / 2 + (SXGUI_GAP * 2) + 4)
#define TASKMGR_DEFAULT_WIDTH 640
#define TASKMGR_DEFAULT_HEIGHT 460
#define TASKMGR_MIN_WIDTH 420
#define TASKMGR_MIN_HEIGHT 320

/* The graph panel's colours are the original's: black background, dim green
 * grid and bright green trace. They do not come from the toolkit's 3D palette
 * on purpose -- a graph is not a control. */
#define TASKMGR_GRAPH_BACKGROUND SXGUI_RGB(0, 0, 0)
#define TASKMGR_GRAPH_GRID SXGUI_RGB(0, 72, 0)
#define TASKMGR_GRAPH_LINE SXGUI_RGB(0, 255, 0)
#define TASKMGR_GRAPH_LINE_ALT SXGUI_RGB(255, 216, 0)
#define TASKMGR_GRAPH_KERNEL SXGUI_RGB(255, 0, 0)

/* Cores with a graph of their own. Past this, One Graph Per CPU shows the
 * first ones; the combined graph always counts all of them. */
#define TASKMGR_MAX_CPUS 8

/* ---- menu commands ------------------------------------------------------------ */

enum taskmgr_command {
    TASKMGR_CMD_NEW_TASK = 1,
    TASKMGR_CMD_EXIT,
    TASKMGR_CMD_REFRESH,
    TASKMGR_CMD_SPEED_HIGH,
    TASKMGR_CMD_SPEED_NORMAL,
    TASKMGR_CMD_SPEED_LOW,
    TASKMGR_CMD_SPEED_PAUSED,
    TASKMGR_CMD_CPU_ONE_GRAPH,
    TASKMGR_CMD_CPU_PER_CPU,
    TASKMGR_CMD_SHOW_KERNEL,
    TASKMGR_CMD_END_PROCESS,
    TASKMGR_CMD_END_TREE,
    TASKMGR_CMD_SHUTDOWN,
    TASKMGR_CMD_REBOOT,
    TASKMGR_CMD_ABOUT,
    TASKMGR_CMD_SWITCH_TO,
    TASKMGR_CMD_END_TASK,
};

static struct sxgui_menu_item k_file_items[] = {
    {"New Task (Run...)", TASKMGR_CMD_NEW_TASK, 0},
    {0, 0, 0},
    {"Exit Task Manager", TASKMGR_CMD_EXIT, 0},
};

/* The original put the speeds in a submenu of View. SXGUI's menus have one
 * level, so they go in the same drop-down, behind a separator: the same
 * choice, one click shorter. */
static struct sxgui_menu_item k_view_items[] = {
    {"Refresh Now\tF5", TASKMGR_CMD_REFRESH, 0},
    {0, 0, 0},
    {"Update Speed: High", TASKMGR_CMD_SPEED_HIGH, 0},
    {"Update Speed: Normal", TASKMGR_CMD_SPEED_NORMAL, SXGUI_MENU_CHECKED},
    {"Update Speed: Low", TASKMGR_CMD_SPEED_LOW, 0},
    {"Update Speed: Paused", TASKMGR_CMD_SPEED_PAUSED, 0},
    {0, 0, 0},
    {"CPU History: One Graph, All CPUs", TASKMGR_CMD_CPU_ONE_GRAPH, 0},
    {"CPU History: One Graph Per CPU", TASKMGR_CMD_CPU_PER_CPU, SXGUI_MENU_CHECKED},
    {"Show Kernel Times", TASKMGR_CMD_SHOW_KERNEL, 0},
};

static struct sxgui_menu_item k_process_items[] = {
    {"End Process\tDel", TASKMGR_CMD_END_PROCESS, 0},
    {"End Process Tree", TASKMGR_CMD_END_TREE, 0},
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

/* Indices of the four speeds inside k_view_items, to move the check mark:
 * the table's order and these numbers have to move together. */
#define TASKMGR_SPEED_FIRST_ITEM 2
#define TASKMGR_SPEED_COUNT 4
#define TASKMGR_ONE_GRAPH_ITEM 7
#define TASKMGR_PER_CPU_ITEM 8
#define TASKMGR_SHOW_KERNEL_ITEM 9

static const unsigned long k_speed_ms[TASKMGR_SPEED_COUNT] = {500ul, 1000ul, 4000ul, 0ul};

/* ---- state -------------------------------------------------------------------- */

struct taskmgr_sample {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;
    uint32_t flags;
    uint32_t handle_count;
    uint32_t thread_count;
    uint32_t uid;
    uint64_t cpu_ticks;
    uint64_t kernel_ticks;
    uint64_t memory_bytes;
    char name[TASKMGR_NAME_CAPACITY];
    /* Percentage against the previous sample; 0 on the first. */
    unsigned int cpu_percent;
};

static struct sxngui_app g_app;
static struct sxgui_menubar g_menubar;
static uint32_t g_self_pid;

static struct taskmgr_sample g_processes[TASKMGR_MAX_PROCESSES];
static int g_process_count;

/* The previous sample, for the differences. Kept whole and not only the
 * counter because every process has to be found again by pid: the indices
 * shift as soon as something starts or ends. */
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

/* Kernel time: the share of the interval that found a process other than
 * the idle in ring 0. The idle halts in ring 0 too, and that is not work. */
static unsigned int g_kernel_percent;
static unsigned char g_kernel_history[TASKMGR_HISTORY];
static int g_show_kernel;

/* One history per core. The kernel has one idle process per core, created
 * in core order, so the n-th idle in the snapshot is core n, and each core
 * gets an equal share of the global ticks (every core's timer runs at the
 * same rate). What a core's idle did not take is that core's usage. */
static int g_core_count;
static unsigned int g_core_percent[TASKMGR_MAX_CPUS];
static unsigned char g_core_history[TASKMGR_MAX_CPUS][TASKMGR_HISTORY];
static int g_cpu_per_core = 1;
static uint32_t g_total_threads;
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

/* Programs started from New Task and System Properties, to be reaped once
 * they end. One that does not fit is reaped by init when this one exits. */
#define TASKMGR_LAUNCHED_CAPACITY 8
static long g_launched[TASKMGR_LAUNCHED_CAPACITY];
static int g_launched_count;

/* ---- widgets ------------------------------------------------------------------- */

/* The order is the Tab order: the tabs, then each page's controls. A hidden
 * page's controls take no focus. */
static struct sxgui_widget g_widgets[10];
#define TASKMGR_TABS (&g_widgets[0])
#define TASKMGR_TASK_LIST (&g_widgets[1])
#define TASKMGR_END_TASK_BUTTON (&g_widgets[2])
#define TASKMGR_SWITCH_BUTTON (&g_widgets[3])
#define TASKMGR_NEW_TASK_BUTTON (&g_widgets[4])
#define TASKMGR_LIST (&g_widgets[5])
#define TASKMGR_END_BUTTON (&g_widgets[6])
#define TASKMGR_STATUS_PROCESSES (&g_widgets[7])
#define TASKMGR_STATUS_CPU (&g_widgets[8])
#define TASKMGR_STATUS_MEMORY (&g_widgets[9])
#define TASKMGR_TASK_WIDGET_FIRST 1
#define TASKMGR_WIDGET_COUNT ((int)(sizeof(g_widgets) / sizeof(g_widgets[0])))

/* The columns, in the order of the table below. Image Name takes whatever
 * width the others leave. Every header sorts (SXGUI_COLUMN_CLICKABLE). */
enum taskmgr_column {
    TASKMGR_COL_NAME = 0,
    TASKMGR_COL_USER,
    TASKMGR_COL_PID,
    TASKMGR_COL_CPU,
    TASKMGR_COL_CPU_TIME,
    TASKMGR_COL_MEMORY,
    TASKMGR_COL_THREADS,
    TASKMGR_COL_HANDLES,
};

#define TASKMGR_SORTS (SXGUI_COLUMN_CLICKABLE)
#define TASKMGR_SORTS_RIGHT (SXGUI_COLUMN_CLICKABLE | SXGUI_COLUMN_RIGHT)

static struct sxgui_column g_columns[] = {
    {"Image Name", 0, TASKMGR_SORTS},
    {"User Name", 72, TASKMGR_SORTS},
    {"PID", 40, TASKMGR_SORTS_RIGHT},
    {"CPU", 36, TASKMGR_SORTS_RIGHT},
    {"CPU Time", 66, TASKMGR_SORTS_RIGHT},
    {"Mem Usage", 80, TASKMGR_SORTS_RIGHT},
    {"Threads", 56, TASKMGR_SORTS_RIGHT},
    {"Handles", 56, TASKMGR_SORTS_RIGHT},
};
#define TASKMGR_COLUMN_COUNT ((int)(sizeof(g_columns) / sizeof(g_columns[0])))

/* ---- Applications ----
 *
 * The windows, not the processes: the list the taskbar shows, which the WM
 * gives this window a copy of (sxnwm_task_list_open,
 * docs/userland/SYSTEM_MONITORING.md). Task ids are pids. */
static struct sxgui_column g_task_columns[] = {
    {"Task", 0, 0},
    {"Status", 110, 0},
};
static const struct savanxp_wm_window_list *g_task_source;
static struct savanxp_wm_window_list g_tasks;
static char g_task_rows[SAVANXP_WM_MAX_WINDOWS][SAVANXP_WM_WINDOW_TITLE_CAPACITY + 24];
static const char *g_task_row_pointers[SAVANXP_WM_MAX_WINDOWS];
static uint32_t g_selected_task;

/* Sorting. The list starts in the kernel's order, which is creation order,
 * and a header click sorts by that column; a second click on the same one
 * reverses it, as in the original. */
static int g_sort_column = -1;
static int g_sort_descending;

/* The selection follows the process, not the row: sorting and a process
 * ending both move rows, and End Process has to aim at what the user
 * clicked. */
static uint32_t g_selected_pid;

/* ---- dialogs -------------------------------------------------------------------- */

#define TASKMGR_DIALOG_MARGIN SXGUI_DIALOG_MARGIN
/* The width is set by the warning about ending a process, the longest line
 * the dialog shows. A dialog has no scrolling: the only way the text reads
 * whole is for the dialog to hold it. */
#define TASKMGR_CONFIRM_WIDTH 404
#define TASKMGR_CONFIRM_HEIGHT 128
#define TASKMGR_RUN_WIDTH 360
#define TASKMGR_RUN_HEIGHT 140

static struct sxgui_dialog g_confirm_dialog;
static struct sxgui_widget g_confirm_widgets[4];
static char g_confirm_line[TASKMGR_NAME_CAPACITY + 64];
static char g_confirm_detail[96];
static uint32_t g_confirm_pid;
static int g_confirm_tree;
static int g_pending_power;

static struct sxgui_dialog g_run_dialog;
static struct sxgui_widget g_run_widgets[4];
static char g_run_path[128] = "/bin/";

/* The one-button box that reports why End Process could not finish. */
static struct sxgui_dialog g_message_dialog;
static struct sxgui_widget g_message_widgets[3];
static char g_message_line[96];
static char g_message_detail[96];

/* ---- utilities -------------------------------------------------------------------- */

static int centred_button_x(int dialog_width, int count, int index)
{
    return (dialog_width - (count * SXGUI_BUTTON_WIDTH) - ((count - 1) * SXGUI_GAP)) / 2 +
        (index * (SXGUI_BUTTON_WIDTH + SXGUI_GAP));
}

/* Thousands separated by commas, like the original's "1,234 K". The SDK's
 * printf does not group, so the number is built backwards and turned
 * around. */
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

/* ---- starting programs -------------------------------------------------------------- *
 *
 * A program connects to windowd on its own, so starting one is a spawn here;
 * a POSIX program starts from a SUN parent like from any other. Only stdio
 * crosses: the window's channels are this process's alone. */

static void reap_launched(void)
{
    int index = 0;

    while (index < g_launched_count)
    {
        struct sxn_process_query query;
        int status = 0;

        if (sxn_process_query(g_launched[index], &query) == 0 && query.state == SXN_PROCESS_RUNNING)
        {
            index += 1;
            continue;
        }
        /* Ended (or already gone): the wait returns at once. */
        (void)sxn_waitpid(g_launched[index], &status);
        g_launched[index] = g_launched[g_launched_count - 1];
        g_launched_count -= 1;
    }
}

static long launch(const char *path)
{
    char *argv[2];
    long pid;

    if (path == 0 || path[0] != '/')
    {
        return -SAVANXP_EINVAL;
    }
    reap_launched();
    argv[0] = (char *)path;
    argv[1] = 0;
    pid = sxn_spawn_stdio(path, argv, 1);
    if (pid == -SAVANXP_ENOENT)
    {
        /* The path as typed, then with .sxe, as the shell does: a program is
         * <name>.sxe on disk (docs/userland/SXE_FORMAT.md), and "/bin/notepad"
         * has to find notepad.sxe the way PATHEXT lets Windows find a .exe. */
        size_t length = strlen(path);
        char with_extension[160];

        if ((length < 4 || strcmp(path + length - 4, ".sxe") != 0) && length + 5 <= sizeof(with_extension))
        {
            memcpy(with_extension, path, length);
            memcpy(with_extension + length, ".sxe", 5);
            argv[0] = with_extension;
            pid = sxn_spawn_stdio(with_extension, argv, 1);
        }
    }
    if (pid > 0 && g_launched_count < TASKMGR_LAUNCHED_CAPACITY)
    {
        g_launched[g_launched_count++] = pid;
    }
    return pid;
}

/* ---- sampling ---------------------------------------------------------------------- *
 *
 * One pass: read the system, read the processes, compute the percentages
 * against the previous sample, and only then keep this one as the previous. */

static void sample_network(void)
{
    struct savanxp_net_info info;
    long device;
    unsigned long now_ms = sxngui_uptime_ms();
    unsigned long elapsed_ms;
    uint32_t frames;

    memset(&info, 0, sizeof(info));
    /* Opened and closed on every sample: keeping the device open would cost
     * a permanent handle for a tab that may never be looked at. GET_INFO only
     * -- this program observes, it does not bring the interface up. */
    device = sxn_open_device("net0", SXN_DEVICE_READ | SXN_DEVICE_WRITE);
    if (device < 0)
    {
        g_net_available = 0;
        return;
    }
    if (sxn_device_control(device, NET_IOC_GET_INFO, (unsigned long)&info) < 0)
    {
        (void)sxn_close(device);
        g_net_available = 0;
        return;
    }
    (void)sxn_close(device);

    g_net = info;
    g_net_available = info.present != 0;
    if (!g_net_available)
    {
        return;
    }

    /* The interval is MEASURED, not derived from the chosen refresh: with
     * updates paused the only sample is a manual Refresh, and dividing by the
     * menu's period would make a rate up. */
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
 * The idle process cannot be ended -- it is where the scheduler goes when
 * nothing is ready -- so the button and the menu command are disabled while
 * it is the one selected. Disabled, not hidden: that it exists but cannot be
 * ended is information about that process; a button that disappears and
 * comes back with the row is a window changing shape on its own.
 */
static void update_end_command(void)
{
    const struct taskmgr_sample *sample = selected_sample();
    int can_end = sample != 0 && (sample->flags & SAVANXP_PROC_FLAG_IDLE) == 0u;

    if (can_end)
    {
        TASKMGR_END_BUTTON->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
        k_process_items[0].flags &= ~(uint32_t)SXGUI_MENU_DISABLED;
        k_process_items[1].flags &= ~(uint32_t)SXGUI_MENU_DISABLED;
    }
    else
    {
        TASKMGR_END_BUTTON->flags |= SXGUI_FLAG_DISABLED;
        k_process_items[0].flags |= SXGUI_MENU_DISABLED;
        k_process_items[1].flags |= SXGUI_MENU_DISABLED;
    }
}

/* The account a uid belongs to. There is one user, `user`, uid 1000
 * (docs/kernel/USERS_AND_PERMISSIONS.md#decided), and the system, which the
 * original Task Manager calls SYSTEM. Anything else is shown as its number:
 * it means the list has outgrown this table. */
static void format_user(uint32_t uid, char *out, size_t capacity)
{
    if (uid == 0u)
    {
        snprintf(out, capacity, "%s", "SYSTEM");
    }
    else if (uid == 1000u)
    {
        snprintf(out, capacity, "%s", "user");
    }
    else
    {
        snprintf(out, capacity, "%u", (unsigned)uid);
    }
}

/* CPU Time is H:MM:SS of ticks, at the timer's rate. */
static void format_cpu_time(uint64_t ticks, char *out, size_t capacity)
{
    uint64_t hz = g_info.timer_frequency_hz != 0u ? g_info.timer_frequency_hz : 1u;
    uint64_t seconds = ticks / hz;

    snprintf(out, capacity, "%u:%02u:%02u",
             (unsigned)(seconds / 3600u),
             (unsigned)((seconds / 60u) % 60u),
             (unsigned)(seconds % 60u));
}

static int compare_text(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0')
    {
        int a = (*left >= 'A' && *left <= 'Z') ? *left + ('a' - 'A') : *left;
        int b = (*right >= 'A' && *right <= 'Z') ? *right + ('a' - 'A') : *right;

        if (a != b)
        {
            return a < b ? -1 : 1;
        }
        left += 1;
        right += 1;
    }
    return *left == *right ? 0 : (*left == '\0' ? -1 : 1);
}

#define TASKMGR_COMPARE(a, b) ((a) < (b) ? -1 : ((a) > (b) ? 1 : 0))

static int compare_samples(const struct taskmgr_sample *left, const struct taskmgr_sample *right)
{
    int result = 0;

    switch (g_sort_column)
    {
    case TASKMGR_COL_NAME:
        result = compare_text(left->name, right->name);
        break;
    case TASKMGR_COL_USER:
    {
        char a[16];
        char b[16];

        format_user(left->uid, a, sizeof(a));
        format_user(right->uid, b, sizeof(b));
        result = compare_text(a, b);
        break;
    }
    case TASKMGR_COL_CPU:
        result = TASKMGR_COMPARE(left->cpu_percent, right->cpu_percent);
        break;
    case TASKMGR_COL_CPU_TIME:
        result = TASKMGR_COMPARE(left->cpu_ticks, right->cpu_ticks);
        break;
    case TASKMGR_COL_MEMORY:
        result = TASKMGR_COMPARE(left->memory_bytes, right->memory_bytes);
        break;
    case TASKMGR_COL_THREADS:
        result = TASKMGR_COMPARE(left->thread_count, right->thread_count);
        break;
    case TASKMGR_COL_HANDLES:
        result = TASKMGR_COMPARE(left->handle_count, right->handle_count);
        break;
    default:
        break;
    }
    /* Ties, and the PID column itself, fall back to the PID, so equal rows
     * do not trade places on every refresh. */
    if (result == 0)
    {
        result = TASKMGR_COMPARE(left->pid, right->pid);
    }
    return g_sort_descending ? -result : result;
}

/* Insertion sort: the list is a few dozen rows, already almost in order
 * from the previous refresh. */
static void sort_samples(void)
{
    int index;

    if (g_sort_column < 0)
    {
        return;
    }
    for (index = 1; index < g_process_count; ++index)
    {
        struct taskmgr_sample moving = g_processes[index];
        int slot = index;

        while (slot > 0 && compare_samples(&moving, &g_processes[slot - 1]) < 0)
        {
            g_processes[slot] = g_processes[slot - 1];
            slot -= 1;
        }
        g_processes[slot] = moving;
    }
}

static void build_rows(void)
{
    int index;

    sort_samples();
    for (index = 0; index < g_process_count; ++index)
    {
        const struct taskmgr_sample *sample = &g_processes[index];
        char memory[24];
        char user[16];
        char cpu_time[24];

        format_grouped(sample->memory_bytes / 1024ull, memory, sizeof(memory));
        format_user(sample->uid, user, sizeof(user));
        format_cpu_time(sample->cpu_ticks, cpu_time, sizeof(cpu_time));
        snprintf(g_rows[index], sizeof(g_rows[index]), "%s\t%s\t%u\t%02u\t%s\t%s K\t%u\t%u",
                 sample->name[0] != '\0' ? sample->name : "(unnamed)",
                 user,
                 (unsigned)sample->pid,
                 sample->cpu_percent > 99u ? 99u : sample->cpu_percent,
                 cpu_time,
                 memory,
                 (unsigned)sample->thread_count,
                 (unsigned)sample->handle_count);
        g_row_pointers[index] = g_rows[index];
        if (sample->pid == g_selected_pid)
        {
            TASKMGR_LIST->value = index;
        }
    }

    TASKMGR_LIST->items = g_row_pointers;
    TASKMGR_LIST->item_count = g_process_count;
    if (TASKMGR_LIST->value >= g_process_count)
    {
        TASKMGR_LIST->value = g_process_count > 0 ? g_process_count - 1 : 0;
    }
    if (TASKMGR_LIST->value >= 0 && TASKMGR_LIST->value < g_process_count)
    {
        g_selected_pid = g_processes[TASKMGR_LIST->value].pid;
    }
    update_end_command();
}

static const struct savanxp_wm_window_entry *selected_task(void)
{
    if (TASKMGR_TASK_LIST->value < 0 || TASKMGR_TASK_LIST->value >= (int)g_tasks.count)
    {
        return 0;
    }
    return &g_tasks.windows[TASKMGR_TASK_LIST->value];
}

static void update_task_commands(void)
{
    int has = selected_task() != 0;

    if (has)
    {
        TASKMGR_END_TASK_BUTTON->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
        TASKMGR_SWITCH_BUTTON->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
    }
    else
    {
        TASKMGR_END_TASK_BUTTON->flags |= SXGUI_FLAG_DISABLED;
        TASKMGR_SWITCH_BUTTON->flags |= SXGUI_FLAG_DISABLED;
    }
}

/* Copies the WM's list and rebuilds the rows, keeping the selection on the
 * same task. Without a list (no WM: the self-test) the tab stays empty. */
static void refresh_tasks(void)
{
    uint32_t index;

    if (g_task_source == 0 || sxnwm_task_list_read(g_task_source, &g_tasks) != 0)
    {
        g_tasks.count = 0;
    }
    for (index = 0; index < g_tasks.count; ++index)
    {
        const struct savanxp_wm_window_entry *entry = &g_tasks.windows[index];

        snprintf(g_task_rows[index], sizeof(g_task_rows[index]), "%s\t%s",
                 entry->title[0] != '\0' ? entry->title : "(untitled)",
                 (entry->flags & SAVANXP_WM_WINDOW_FLAG_NOT_RESPONDING) != 0u ? "Not Responding" : "Running");
        g_task_row_pointers[index] = g_task_rows[index];
        if (entry->window_id == g_selected_task)
        {
            TASKMGR_TASK_LIST->value = (int)index;
        }
    }
    TASKMGR_TASK_LIST->items = g_task_row_pointers;
    TASKMGR_TASK_LIST->item_count = (int)g_tasks.count;
    if (TASKMGR_TASK_LIST->value >= (int)g_tasks.count)
    {
        TASKMGR_TASK_LIST->value = g_tasks.count > 0 ? (int)g_tasks.count - 1 : 0;
    }
    g_selected_task = selected_task() != 0 ? selected_task()->window_id : 0u;
    update_task_commands();
}

static void refresh_sample(void)
{
    struct savanxp_process_info info;
    unsigned long proc_index = 0;
    uint64_t ticks_delta = 0;
    uint64_t idle_delta = 0;
    uint64_t kernel_delta = 0;
    uint64_t core_idle_delta[TASKMGR_MAX_CPUS];
    int idle_seen = 0;
    int index;

    memset(&g_info, 0, sizeof(g_info));
    (void)sxn_system_info(&g_info);

    g_process_count = 0;
    g_total_handles = 0;
    g_total_threads = 0;
    memset(&info, 0, sizeof(info));
    while (g_process_count < TASKMGR_MAX_PROCESSES && sxn_process_snapshot(proc_index, &info) > 0)
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
        sample->thread_count = info.thread_count;
        sample->uid = info.uid;
        sample->cpu_ticks = info.cpu_ticks;
        sample->kernel_ticks = info.kernel_ticks;
        sample->memory_bytes = info.memory_bytes;
        sample->cpu_percent = 0;
        snprintf(sample->name, sizeof(sample->name), "%s", info.name);
        g_total_handles += info.handle_count;
        g_total_threads += info.thread_count;
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
            idle_delta += delta;
            if (idle_seen < TASKMGR_MAX_CPUS)
            {
                core_idle_delta[idle_seen] = delta;
            }
            idle_seen += 1;
        }
        else if (previous != 0 && sample->kernel_ticks > previous->kernel_ticks)
        {
            kernel_delta += sample->kernel_ticks - previous->kernel_ticks;
        }
    }

    /* The system's usage is what the idle processes did NOT take. When the
     * idle did not appear in the previous sample -- the first pass -- it stays
     * at zero, which is the honest answer: there is no interval to measure
     * yet. */
    g_cpu_percent = 0;
    if (ticks_delta != 0ull && idle_delta <= ticks_delta)
    {
        g_cpu_percent = (unsigned int)(((ticks_delta - idle_delta) * 100ull) / ticks_delta);
    }
    g_kernel_percent = 0;
    if (ticks_delta != 0ull)
    {
        g_kernel_percent = (unsigned int)((kernel_delta * 100ull) / ticks_delta);
        if (g_kernel_percent > g_cpu_percent)
        {
            g_kernel_percent = g_cpu_percent;
        }
    }
    g_core_count = idle_seen < TASKMGR_MAX_CPUS ? idle_seen : TASKMGR_MAX_CPUS;
    for (index = 0; index < g_core_count; ++index)
    {
        uint64_t share = idle_seen > 0 ? ticks_delta / (uint64_t)idle_seen : 0ull;

        g_core_percent[index] = 0;
        if (share != 0ull && core_idle_delta[index] <= share)
        {
            g_core_percent[index] = (unsigned int)(((share - core_idle_delta[index]) * 100ull) / share);
        }
        push_history(g_core_history[index], (unsigned char)g_core_percent[index]);
    }

    /* Memory comes from the page allocator's pool, not from the boot memory
     * map: total and free have to come from the same count or "in use" does
     * not add up. */
    g_memory_total_bytes = g_info.memory_total_pages * 4096ull;
    g_memory_used_bytes = g_memory_total_bytes > g_info.memory_free_bytes
        ? g_memory_total_bytes - g_info.memory_free_bytes
        : 0ull;
    g_memory_percent = g_memory_total_bytes != 0ull
        ? (unsigned int)((g_memory_used_bytes * 100ull) / g_memory_total_bytes)
        : 0u;

    sample_network();

    push_history(g_cpu_history, (unsigned char)(g_cpu_percent > 100u ? 100u : g_cpu_percent));
    push_history(g_kernel_history, (unsigned char)g_kernel_percent);
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
    refresh_tasks();

    snprintf(g_status_processes, sizeof(g_status_processes), "Processes: %d", g_process_count);
    snprintf(g_status_cpu, sizeof(g_status_cpu), "CPU Usage: %u%%", g_cpu_percent);
    snprintf(g_status_memory, sizeof(g_status_memory), "Mem Usage: %lluM / %lluM",
             (unsigned long long)(g_memory_used_bytes / (1024ull * 1024ull)),
             (unsigned long long)(g_memory_total_bytes / (1024ull * 1024ull)));
}

/* ---- layout -------------------------------------------------------------------------
 *
 * Computed from the window's size, not the other way round: this window
 * stretches, and its content is a list and some graphs that have to use
 * whatever room there is. */

static int content_top(void)
{
    return sxgui_menubar_height();
}

static void relayout(void)
{
    int width = (int)g_app.info.width;
    int height = (int)g_app.info.height;
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
    {
        int column;
        int others = 0;

        for (column = 1; column < TASKMGR_COLUMN_COUNT; ++column)
        {
            others += g_columns[column].width;
        }
        g_columns[TASKMGR_COL_NAME].width = TASKMGR_LIST->rect.width - (SXGUI_BORDER_SUNKEN * 2) -
            SXGUI_SCROLLBAR_THICKNESS - others;
        if (g_columns[TASKMGR_COL_NAME].width < 80)
        {
            g_columns[TASKMGR_COL_NAME].width = 80;
        }
    }

    TASKMGR_END_BUTTON->rect = sx_rect_make(
        TASKMGR_LIST->rect.x + TASKMGR_LIST->rect.width - SXGUI_BUTTON_WIDTH - 26,
        TASKMGR_LIST->rect.y + TASKMGR_LIST->rect.height + SXGUI_GAP,
        SXGUI_BUTTON_WIDTH + 26,
        SXGUI_BUTTON_HEIGHT);

    /* Applications: the same list area, and its three buttons at the right
     * of one row, End Task first, as in the original. */
    TASKMGR_TASK_LIST->rect = TASKMGR_LIST->rect;
    g_task_columns[0].width = TASKMGR_TASK_LIST->rect.width - (SXGUI_BORDER_SUNKEN * 2) -
        SXGUI_SCROLLBAR_THICKNESS - g_task_columns[1].width;
    if (g_task_columns[0].width < 80)
    {
        g_task_columns[0].width = 80;
    }
    TASKMGR_NEW_TASK_BUTTON->rect = sx_rect_make(
        TASKMGR_LIST->rect.x + TASKMGR_LIST->rect.width - (SXGUI_BUTTON_WIDTH + 26),
        TASKMGR_END_BUTTON->rect.y, SXGUI_BUTTON_WIDTH + 26, SXGUI_BUTTON_HEIGHT);
    TASKMGR_SWITCH_BUTTON->rect = sx_rect_make(
        TASKMGR_NEW_TASK_BUTTON->rect.x - SXGUI_GAP - (SXGUI_BUTTON_WIDTH + 10),
        TASKMGR_END_BUTTON->rect.y, SXGUI_BUTTON_WIDTH + 10, SXGUI_BUTTON_HEIGHT);
    TASKMGR_END_TASK_BUTTON->rect = sx_rect_make(
        TASKMGR_SWITCH_BUTTON->rect.x - SXGUI_GAP - (SXGUI_BUTTON_WIDTH + 10),
        TASKMGR_END_BUTTON->rect.y, SXGUI_BUTTON_WIDTH + 10, SXGUI_BUTTON_HEIGHT);

    /* The status bar is three sunken panels, like the original's: the first
     * takes what is left and the other two measure their own. */
    panel_width = 150;
    TASKMGR_STATUS_MEMORY->rect = sx_rect_make(
        width - TASKMGR_MARGIN - panel_width, status_y, panel_width, SXGUI_STATUS_HEIGHT);
    TASKMGR_STATUS_CPU->rect = sx_rect_make(
        TASKMGR_STATUS_MEMORY->rect.x - SXGUI_GAP - 110, status_y, 110, SXGUI_STATUS_HEIGHT);
    TASKMGR_STATUS_PROCESSES->rect = sx_rect_make(
        TASKMGR_MARGIN, status_y,
        TASKMGR_STATUS_CPU->rect.x - SXGUI_GAP - TASKMGR_MARGIN, SXGUI_STATUS_HEIGHT);
}

/* The list's controls exist only on their tab; on the other two the app
 * paints the content, and leaving them visible would draw them on top. */
static void set_visible(struct sxgui_widget *widget, int visible)
{
    if (visible)
    {
        widget->flags |= SXGUI_FLAG_VISIBLE;
    }
    else
    {
        widget->flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
    }
}

static void apply_tab_visibility(void)
{
    int on_processes = TASKMGR_TABS->value == TASKMGR_TAB_PROCESSES;
    int on_applications = TASKMGR_TABS->value == TASKMGR_TAB_APPLICATIONS;

    set_visible(TASKMGR_LIST, on_processes);
    set_visible(TASKMGR_END_BUTTON, on_processes);
    set_visible(TASKMGR_TASK_LIST, on_applications);
    set_visible(TASKMGR_END_TASK_BUTTON, on_applications);
    set_visible(TASKMGR_SWITCH_BUTTON, on_applications);
    set_visible(TASKMGR_NEW_TASK_BUTTON, on_applications);
}

/* ---- painting ------------------------------------------------------------------------- */

static void draw_etched(struct sx_painter *painter, struct sx_rect rect)
{
    sx_painter_draw_frame(painter,
                          sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 1, rect.height - 1),
                          SXGUI_COLOR_LIGHT);
    sx_painter_draw_frame(painter,
                          sx_rect_make(rect.x, rect.y, rect.width - 1, rect.height - 1),
                          SXGUI_COLOR_SHADOW);
}

/* A group frame identical to the toolkit's (sxgui_paint_groupbox): the
 * caption rides on the top edge, with the window's background behind it.
 * Drawn by hand and not as widgets because these frames change with the
 * active tab, and as widgets they would have to be switched on and off on
 * every change. */
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

    /* The usable area: inside the frame, with some air, where content goes. */
    return sx_rect_make(frame.x + SXGUI_GAP, frame.y + SXGUI_GAP,
                        frame.width - (SXGUI_GAP * 2), frame.height - (SXGUI_GAP * 2));
}

/* The vertical meter the original draws beside each graph. The fill goes in
 * blocks with a one-pixel gap, which is what makes it look segmented rather
 * than a plain bar. */
static void draw_meter(struct sx_painter *painter, struct sx_rect rect, unsigned int percent,
                       unsigned int kernel_percent, uint32_t colour)
{
    int kernel_filled;

    struct sx_rect inner;
    int filled;
    int y;

    sxchrome_draw_inset(painter, rect);
    inner = sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2);
    sx_painter_fill_rect(painter, inner, TASKMGR_GRAPH_BACKGROUND);

    if (percent > 100u)
    {
        percent = 100u;
    }
    if (kernel_percent > percent)
    {
        kernel_percent = percent;
    }
    filled = (inner.height * (int)percent) / 100;
    kernel_filled = (inner.height * (int)kernel_percent) / 100;
    for (y = 0; y < filled; y += 3)
    {
        int height = (filled - y) < 2 ? (filled - y) : 2;

        sx_painter_fill_rect(
            painter,
            sx_rect_make(inner.x, inner.y + inner.height - y - height, inner.width, height),
            y < kernel_filled ? TASKMGR_GRAPH_KERNEL : colour);
    }
}

/* A history graph: a fixed grid and one sample per pixel, right to left. The
 * old samples live at the start of the array, so the newest always lands on
 * the right edge even before the history has filled. */
static void draw_series(struct sx_painter *painter, struct sx_rect inner,
                        const unsigned char *history, uint32_t colour)
{
    int index;
    int previous_x = 0;
    int previous_y = 0;
    int have_previous = 0;

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

/* A graph: the grid, then `kernel` in red when it is given, then `history`
 * on top, as the original draws its kernel times under the total. */
static void draw_history(
    struct sx_painter *painter,
    struct sx_rect rect,
    const unsigned char *history,
    uint32_t colour,
    const unsigned char *kernel)
{
    struct sx_rect inner;
    int step = 12;
    int offset;

    sxchrome_draw_inset(painter, rect);
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
    if (kernel != 0)
    {
        draw_series(painter, inner, kernel, TASKMGR_GRAPH_KERNEL);
    }
    draw_series(painter, inner, history, colour);
}

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
    int totals_height = TASKMGR_GROUP_HEIGHT(4);
    int graph_height = (page.height - (pad * 3) - totals_height) / 2;
    char number[32];
    struct sx_rect area;

    if (graph_height < 56)
    {
        graph_height = 56;
    }

    /* Row 1: the CPU meter and the CPU history. */
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
               g_show_kernel ? g_kernel_percent : 0u,
               TASKMGR_GRAPH_LINE);

    area = draw_group(painter,
                      sx_rect_make(x + meter_width + 24 + SXGUI_GAP, y,
                                   full_width - meter_width - 24 - SXGUI_GAP, graph_height),
                      "CPU Usage History");
    if (g_cpu_per_core && g_core_count > 1)
    {
        /* One graph per core, side by side. Kernel time is not split by
         * core -- the counter is per process -- so these show none. */
        int core;
        int gap = 4;
        int each = (area.width - gap * (g_core_count - 1)) / g_core_count;

        for (core = 0; core < g_core_count; ++core)
        {
            draw_history(painter,
                         sx_rect_make(area.x + core * (each + gap), area.y, each, area.height),
                         g_core_history[core], TASKMGR_GRAPH_LINE, 0);
        }
    }
    else
    {
        draw_history(painter, area, g_cpu_history, TASKMGR_GRAPH_LINE,
                     g_show_kernel ? g_kernel_history : 0);
    }
    y += graph_height + pad;

    /* Row 2: the same for memory. */
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
               0u,
               TASKMGR_GRAPH_LINE_ALT);

    area = draw_group(painter,
                      sx_rect_make(x + meter_width + 24 + SXGUI_GAP, y,
                                   full_width - meter_width - 24 - SXGUI_GAP, graph_height),
                      "Memory Usage History");
    draw_history(painter, area, g_memory_history, TASKMGR_GRAPH_LINE_ALT, 0);
    y += graph_height + pad;

    /* Row 3: the two boxes of numbers. */
    {
        int half = (full_width - SXGUI_GAP) / 2;
        unsigned long long uptime_seconds = g_info.uptime_ms / 1000ull;

        area = draw_group(painter, sx_rect_make(x, y, half, totals_height), "Totals");
        format_grouped(g_total_handles, number, sizeof(number));
        draw_field(painter, area, 0, "Handles", number);
        format_grouped(g_total_threads, number, sizeof(number));
        draw_field(painter, area, 1, "Threads", number);
        snprintf(number, sizeof(number), "%d", g_process_count);
        draw_field(painter, area, 2, "Processes", number);
        snprintf(number, sizeof(number), "%llu:%02llu:%02llu",
                 uptime_seconds / 3600ull,
                 (uptime_seconds / 60ull) % 60ull,
                 uptime_seconds % 60ull);
        draw_field(painter, area, 3, "Up Time", number);

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

static const char *no_adapter_headline(void)
{
    return g_info.net_hardware != 0
        ? "The installed network adapter has no driver in this system."
        : "No network adapter installed.";
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
        draw_history(painter, area, g_net_history, TASKMGR_GRAPH_LINE, 0);
    }
    else
    {
        sx_painter_draw_text(painter, area.x, area.y, no_adapter_headline(), SXGUI_COLOR_TEXT);
    }
    y += graph_height + pad;

    /* Without an adapter it can drive there are no rows to show, but there
     * IS something to say, and two different things: that the machine has no
     * card, or that it has one this system has no driver for -- VirtualBox's
     * case, whose default card is an Intel PRO/1000. Calling the second "not
     * present" sends the user to look for the problem in the VM's settings,
     * which are fine. */
    area = draw_group(painter, sx_rect_make(x, y, full_width, adapter_height), "Adapter");
    if (!g_net_available)
    {
        if (g_info.net_hardware != 0)
        {
            snprintf(number, sizeof(number), "PCI %04x:%04x, no driver in this system",
                     (unsigned)g_info.net_hardware_vendor,
                     (unsigned)g_info.net_hardware_device);
            draw_field(painter, area, 0, "Adapter found", number);
            draw_field(painter, area, 1, "net0", "unavailable");
        }
        else
        {
            draw_field(painter, area, 0, "net0", "no adapter installed");
        }
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

/* Under the list, the selected row's detail: state and parent, which have no
 * column of their own but are the first thing one wants to know about a
 * process one is about to end. */
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

static void on_paint(struct sxngui_app *app)
{
    struct sx_painter *painter = &app->ui.painter;
    struct sx_rect page = sxgui_tabs_page(TASKMGR_TABS);

    if (!sx_painter_push_clip(painter, page))
    {
        return;
    }
    switch (TASKMGR_TABS->value)
    {
    case TASKMGR_TAB_APPLICATIONS:
        break;
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

/* ---- actions ------------------------------------------------------------------------- */

static void on_tick(struct sxngui_app *app)
{
    refresh_sample();
    reap_launched();
    sxngui_app_request_repaint(app);
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
    sxngui_app_request_repaint((struct sxngui_app *)user);
}

static void on_list(struct sxgui_widget *widget, void *user)
{
    if (widget->action == SXGUI_ACTION_HEADER)
    {
        int column = sxgui_header_column(widget);

        if (column < 0)
        {
            return;
        }
        if (column == g_sort_column)
        {
            g_sort_descending = !g_sort_descending;
        }
        else
        {
            g_sort_column = column;
            g_sort_descending = 0;
        }
        build_rows();
    }
    else if (widget->value >= 0 && widget->value < g_process_count)
    {
        g_selected_pid = g_processes[widget->value].pid;
    }
    update_end_command();
    sxngui_app_request_repaint((struct sxngui_app *)user);
}

/* ---- ending a process -----------------------------------------------------------------
 *
 * The desktop runs as the user, and a process that belongs to the system (or
 * to anyone else) is not the user's to end: TERMINATE answers EPERM
 * (docs/kernel/USERS_AND_PERMISSIONS.md#what-requires-elevation). Ending it
 * anyway takes elevation, as uninstalling does in Add or Remove Programs: the
 * elevated `taskmgr --end <pid>` does it, and this window never gains the
 * rights itself. EACCES is different: the idle processes and init cannot be
 * ended by anyone, so there is nothing to elevate for. */

/* ---- End Process Tree ----
 *
 * The process and everything it started, children before their parent: a
 * parent ended first would leave its children to be adopted by init, and
 * they would drop out of the tree half-way through. The tree is read from a
 * fresh snapshot rather than from the list on screen, which may be a second
 * old, and the Task Manager leaves itself out of it: it may well be a
 * descendant of what the user is ending (the desktop, say), and it still has
 * to report how that went. */

struct taskmgr_tree_node {
    uint32_t pid;
    uint32_t parent_pid;
};

static int tree_snapshot(struct taskmgr_tree_node *nodes, int capacity)
{
    struct savanxp_process_info info;
    unsigned long index = 0;
    int count = 0;

    memset(&info, 0, sizeof(info));
    while (count < capacity && sxn_process_snapshot(index, &info) > 0)
    {
        index += 1;
        if (info.state == SAVANXP_PROC_UNUSED || info.state == SAVANXP_PROC_ZOMBIE)
        {
            continue;
        }
        nodes[count].pid = info.pid;
        nodes[count].parent_pid = info.parent_pid;
        count += 1;
    }
    return count;
}

/* Appends `pid`'s descendants and then `pid` itself to `out`. The depth is
 * bounded by the 64 process slots. */
static int tree_order(const struct taskmgr_tree_node *nodes, int count, uint32_t pid,
                      uint32_t *out, int used, int capacity)
{
    int index;

    for (index = 0; index < count; ++index)
    {
        if (nodes[index].parent_pid == pid && nodes[index].pid != pid)
        {
            used = tree_order(nodes, count, nodes[index].pid, out, used, capacity);
        }
    }
    if (used < capacity)
    {
        out[used] = pid;
        used += 1;
    }
    return used;
}

/* Ends `root` (and its tree, when `tree` is set) with this process's rights.
 * Returns 0, or the first error worth reporting; `denied` counts the
 * processes that answered EPERM, which elevation may still end. A process
 * already gone (ENOENT) is not an error, it is the goal. */
static long terminate_now(uint32_t root, int tree, int *denied)
{
    struct taskmgr_tree_node nodes[TASKMGR_MAX_PROCESSES];
    uint32_t order[TASKMGR_MAX_PROCESSES];
    int count;
    int index;
    long first_error = 0;

    *denied = 0;
    if (!tree)
    {
        order[0] = root;
        count = 1;
    }
    else
    {
        count = tree_snapshot(nodes, TASKMGR_MAX_PROCESSES);
        count = tree_order(nodes, count, root, order, 0, TASKMGR_MAX_PROCESSES);
    }
    for (index = 0; index < count; ++index)
    {
        long result;

        if (order[index] == g_self_pid && tree)
        {
            continue;
        }
        result = sxn_terminate((long)order[index], TASKMGR_END_STATUS);
        if (result == -SAVANXP_EPERM)
        {
            *denied += 1;
        }
        else if (result < 0 && result != -SAVANXP_ENOENT && first_error == 0)
        {
            first_error = result;
        }
    }
    return first_error;
}

/* Returns 0 when the processes are gone or the user declined the prompt, or a
 * negative status to report. */
static long end_process(uint32_t pid, int tree)
{
    const char *argv[3];
    char pid_text[16];
    int status = -1;
    int denied = 0;
    long result = terminate_now(pid, tree, &denied);

    if (denied == 0)
    {
        return result;
    }
    snprintf(pid_text, sizeof(pid_text), "%u", (unsigned)pid);
    argv[0] = "taskmgr";
    argv[1] = tree ? "--end-tree" : "--end";
    argv[2] = pid_text;
    result = sxn_elevate(TASKMGR_PATH, argv, 3, &status);
    if (result == -SAVANXP_EACCES)
    {
        /* The user answered No: that is a decision, not an error. */
        return 0;
    }
    if (result < 0)
    {
        return -SAVANXP_EPERM;
    }
    /* The elevated half exits with the errno of its TERMINATE, 0 on success. */
    return status == 0 ? 0 : -(long)(status > 0 ? status : SAVANXP_EPERM);
}

/* The elevated half, run by the broker as the system. It opens no window.
 * For a tree it walks it again: what the user could end is already gone,
 * and what is left is what needed the rights. */
static int run_end(const char *pid_text, int tree)
{
    unsigned long pid = 0;
    const char *cursor = pid_text;
    int denied = 0;
    long result;

    if (cursor == 0 || *cursor == '\0')
    {
        return SAVANXP_EINVAL;
    }
    while (*cursor >= '0' && *cursor <= '9')
    {
        pid = pid * 10ul + (unsigned long)(*cursor - '0');
        cursor += 1;
    }
    if (*cursor != '\0' || pid == 0ul)
    {
        return SAVANXP_EINVAL;
    }
    result = terminate_now((uint32_t)pid, tree, &denied);
    if (result == 0 && denied != 0)
    {
        result = -SAVANXP_EPERM;
    }
    return result < 0 ? (int)-result : 0;
}

static void on_message_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
}

static void show_failure(const char *title, long result)
{
    /* Gone before the request reached it: the next sample drops the row,
     * which is all the user needs to see. */
    if (result == -SAVANXP_ENOENT)
    {
        return;
    }
    snprintf(g_message_line, sizeof(g_message_line), "%s", "The operation could not be completed.");
    if (result == -SAVANXP_EPERM || result == -SAVANXP_EACCES)
    {
        snprintf(g_message_detail, sizeof(g_message_detail), "%s", "Access is denied.");
    }
    else
    {
        snprintf(g_message_detail, sizeof(g_message_detail), "The process could not be ended (error %ld).", -result);
    }

    memset(&g_message_dialog, 0, sizeof(g_message_dialog));
    g_message_dialog.title = title;
    g_message_dialog.widgets = g_message_widgets;
    g_message_dialog.widget_count = 3;
    g_message_dialog.initial_focus = 2;
    g_message_dialog.default_button = 2;
    sxgui_dialog_begin(&g_app.ui, &g_message_dialog, TASKMGR_CONFIRM_WIDTH, TASKMGR_CONFIRM_HEIGHT);
}

static void power_request(unsigned long code)
{
    long device = sxn_open_device("power", SXN_DEVICE_WRITE);

    if (device >= 0)
    {
        /* Does not return when it succeeds. */
        (void)sxn_device_control(device, code, 0);
        (void)sxn_close(device);
    }
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
        g_pending_power = 0;
        power_request(POWER_IOC_SHUTDOWN);
        return;
    }
    if (g_pending_power == TASKMGR_CMD_REBOOT)
    {
        g_pending_power = 0;
        power_request(POWER_IOC_REBOOT);
        return;
    }

    if (g_confirm_pid != 0u)
    {
        long result = end_process(g_confirm_pid, g_confirm_tree);

        g_confirm_pid = 0u;
        if (result < 0)
        {
            show_failure("Unable to Terminate Process", result);
        }
        /* Sample again now: waiting for the next tick would leave the dead
         * row in the list for up to a second after it was ended. */
        refresh_sample();
        sxngui_app_request_repaint(&g_app);
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

static void end_selected_process(int tree)
{
    const struct taskmgr_sample *sample = selected_sample();
    char line[TASKMGR_NAME_CAPACITY + 64];

    if (TASKMGR_TABS->value != TASKMGR_TAB_PROCESSES || sample == 0)
    {
        return;
    }
    /* Ending the idle would leave the scheduler nowhere to go. The kernel
     * refuses it anyway, but asking first is worse than not offering it. */
    if ((sample->flags & SAVANXP_PROC_FLAG_IDLE) != 0u)
    {
        return;
    }

    g_pending_power = 0;
    g_confirm_pid = sample->pid;
    g_confirm_tree = tree;
    snprintf(line, sizeof(line), tree ? "End '%s' (PID %u) and every process it started?" : "End '%s' (PID %u)?",
             sample->name, (unsigned)sample->pid);
    begin_confirm(tree ? "End Process Tree" : "End Process", line,
                  "Unsaved data will be lost and the system may become unstable.", 1);
}

static void on_end_process(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    end_selected_process(0);
}

static void begin_run_dialog(void);

/* ---- Applications: End Task and Switch To ----
 *
 * End Task asks the window to close, as its X button does (the WM's
 * TASK_CLOSE): the program decides, and may ask to save. Only a window the
 * WM already shows as not responding is ended outright, after asking, which
 * is the original's "End Now". */

static void on_task_list(struct sxgui_widget *widget, void *user)
{
    const struct savanxp_wm_window_entry *entry;

    (void)user;
    entry = selected_task();
    g_selected_task = entry != 0 ? entry->window_id : 0u;
    if (widget->action == SXGUI_ACTION_ACTIVATE && entry != 0)
    {
        (void)sxnwm_task_activate(&g_app.wm, entry->window_id);
    }
    update_task_commands();
    sxngui_app_request_repaint(&g_app);
}

static void on_switch_to(struct sxgui_widget *widget, void *user)
{
    const struct savanxp_wm_window_entry *entry = selected_task();

    (void)widget;
    (void)user;
    if (entry != 0)
    {
        (void)sxnwm_task_activate(&g_app.wm, entry->window_id);
    }
}

static void on_end_task(struct sxgui_widget *widget, void *user)
{
    const struct savanxp_wm_window_entry *entry = selected_task();
    long result;

    (void)widget;
    (void)user;
    if (entry == 0)
    {
        return;
    }
    if ((entry->flags & SAVANXP_WM_WINDOW_FLAG_NOT_RESPONDING) != 0u)
    {
        char line[SAVANXP_WM_WINDOW_TITLE_CAPACITY + 32];

        g_pending_power = 0;
        g_confirm_pid = entry->window_id;
        g_confirm_tree = 0;
        snprintf(line, sizeof(line), "'%s' is not responding.", entry->title);
        begin_confirm("End Program", line, "End it now? Unsaved data will be lost.", 1);
        return;
    }
    result = sxnwm_task_close(&g_app.wm, entry->window_id);
    if (result < 0 && result != -SAVANXP_ENOENT)
    {
        show_failure("Unable to End Task", result);
    }
}

static void on_new_task(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    begin_run_dialog();
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
    (void)launch(g_run_path);
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
        sxngui_app_quit(&g_app, 0);
        break;
    case TASKMGR_CMD_REFRESH:
        refresh_sample();
        sxngui_app_request_repaint(&g_app);
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
    case TASKMGR_CMD_CPU_ONE_GRAPH:
    case TASKMGR_CMD_CPU_PER_CPU:
        g_cpu_per_core = id == TASKMGR_CMD_CPU_PER_CPU;
        k_view_items[TASKMGR_ONE_GRAPH_ITEM].flags =
            g_cpu_per_core ? 0u : (uint32_t)SXGUI_MENU_CHECKED;
        k_view_items[TASKMGR_PER_CPU_ITEM].flags =
            g_cpu_per_core ? (uint32_t)SXGUI_MENU_CHECKED : 0u;
        sxngui_app_request_repaint(&g_app);
        break;
    case TASKMGR_CMD_SHOW_KERNEL:
        g_show_kernel = !g_show_kernel;
        k_view_items[TASKMGR_SHOW_KERNEL_ITEM].flags = g_show_kernel ? (uint32_t)SXGUI_MENU_CHECKED : 0u;
        sxngui_app_request_repaint(&g_app);
        break;
    case TASKMGR_CMD_END_PROCESS:
        end_selected_process(0);
        break;
    case TASKMGR_CMD_END_TREE:
        end_selected_process(1);
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
        (void)launch("/bin/aboutapp");
        break;
    case TASKMGR_CMD_SWITCH_TO:
        on_switch_to(0, 0);
        break;
    case TASKMGR_CMD_END_TASK:
        on_end_task(0, 0);
        break;
    default:
        break;
    }
}

/* ---- context menus -------------------------------------------------------------------
 * (docs/desktop/DESKTOP_ICONS.md#phase-4-context-menus-in-applications)
 *
 * A right click on a row, Shift+F10 or the Menu key open the row's menu, as
 * the original's: the window's own commands for that row. */

static const struct sxgui_menu_item k_process_context_items[] = {
    {"End Process", TASKMGR_CMD_END_PROCESS, 0},
    {"End Process Tree", TASKMGR_CMD_END_TREE, 0},
};

static const struct sxgui_menu k_process_context_menu = {
    "Process",
    k_process_context_items,
    (int)(sizeof(k_process_context_items) / sizeof(k_process_context_items[0])),
};

static const struct sxgui_menu_item k_task_context_items[] = {
    {"Switch To", TASKMGR_CMD_SWITCH_TO, 0},
    {0, 0, 0},
    {"End Task", TASKMGR_CMD_END_TASK, 0},
};

static const struct sxgui_menu k_task_context_menu = {
    "Task",
    k_task_context_items,
    (int)(sizeof(k_task_context_items) / sizeof(k_task_context_items[0])),
};

static uint32_t g_pointer_buttons;

/* The list the current tab shows, and its menu; 0 on a tab without one. */
static struct sxgui_widget *context_list(const struct sxgui_menu **menu)
{
    if (TASKMGR_TABS->value == TASKMGR_TAB_PROCESSES)
    {
        *menu = &k_process_context_menu;
        return TASKMGR_LIST;
    }
    if (TASKMGR_TABS->value == TASKMGR_TAB_APPLICATIONS)
    {
        *menu = &k_task_context_menu;
        return TASKMGR_TASK_LIST;
    }
    return 0;
}

/* Selects `row` of the current tab's list, as a click on it would. */
static void select_row(struct sxgui_widget *list, int row)
{
    list->value = row;
    if (list == TASKMGR_LIST && row < g_process_count)
    {
        g_selected_pid = g_processes[row].pid;
        update_end_command();
    }
    else if (list == TASKMGR_TASK_LIST)
    {
        g_selected_task = selected_task() != 0 ? selected_task()->window_id : 0u;
        update_task_commands();
    }
}

/* On the release, as Windows sends WM_CONTEXTMENU. */
static int on_pointer(struct sxngui_app *app, const struct savanxp_gui_pointer_event *event)
{
    uint32_t released = g_pointer_buttons & ~event->buttons;
    const struct sxgui_menu *menu = 0;
    struct sxgui_widget *list;
    int row;

    g_pointer_buttons = event->buttons;
    if ((released & SAVANXP_MOUSE_BUTTON_RIGHT) == 0 || sxgui_dialog_active(&app->ui))
    {
        return 0;
    }
    list = context_list(&menu);
    row = list != 0 ? sxgui_listbox_row_at(list, event->x, event->y) : -1;
    if (row < 0)
    {
        return 0;
    }
    select_row(list, row);
    sxgui_context_menu_open(&app->ui, menu, event->x, event->y, on_menu_command, 0);
    return 1;
}

/* The keyboard's right click: the selected row's menu, next to the row. */
static int open_context_menu_from_keyboard(struct sxngui_app *app)
{
    const struct sxgui_menu *menu = 0;
    struct sxgui_widget *list = context_list(&menu);
    int row_height = gfx_text_height() + 4;
    int visible;
    int y;

    if (list == 0 || list->value < 0 || list->value >= list->item_count)
    {
        return 0;
    }
    visible = list->value - list->scroll;
    y = list->rect.y + 2 + row_height + (visible + 1) * row_height;
    if (visible < 0 || y > list->rect.y + list->rect.height)
    {
        y = list->rect.y + row_height * 2;
    }
    sxgui_context_menu_open(&app->ui, menu, list->rect.x + 24, y, on_menu_command, 0);
    return 1;
}

static int on_key(struct sxngui_app *app, const struct savanxp_input_event *event)
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
        end_selected_process(0);
        return 1;
    }
    if (event->key == SAVANXP_KEY_MENU ||
        (event->key == SAVANXP_KEY_F10 && (event->modifiers & SAVANXP_KEY_MOD_SHIFT) != 0))
    {
        return open_context_menu_from_keyboard(app);
    }
    return 0;
}

static void on_resize(struct sxngui_app *app)
{
    (void)app;
    relayout();
}

/* ---- self-test -----------------------------------------------------------------------
 *
 * Headless, with no window: checks that the sampling agrees with itself,
 * which is all that can be asserted without looking at the screen. If the
 * kernel stops exporting the counters, this says so. Failures go to the
 * kernel log; init reads the verdict from the exit status. */

static int g_selftest_failures;

static void selftest_fail(const char *what)
{
    char line[160];

    snprintf(line, sizeof(line), "TASKMGR SMOKE FAIL %s", what);
    (void)sxn_log(line);
    g_selftest_failures += 1;
}

static void selftest_sleep(long ms)
{
    long event = sxn_event_create(SXN_EVENT_MANUAL_RESET);

    if (event >= 0)
    {
        (void)sxn_wait_one(event, ms);
        (void)sxn_close(event);
    }
}

static int selftest(void)
{
    char text[96];
    int index;
    uint64_t ticks_before;
    char grouped[32];
    /* Cores that schedule: one idle process for each. A process that spins
     * keeps a whole core busy, which is 100/cores of the pie. */
    unsigned int cores = 1;

    refresh_sample();
    ticks_before = g_info.cpu_ticks_total;
    if (g_process_count <= 0)
    {
        selftest_fail("no process was listed");
    }

    {
        int idle_seen = 0;
        int self_seen = 0;

        for (index = 0; index < g_process_count; ++index)
        {
            if ((g_processes[index].flags & SAVANXP_PROC_FLAG_IDLE) != 0u)
            {
                idle_seen += 1;
            }
            if (g_processes[index].pid == g_self_pid)
            {
                self_seen = 1;
                if (g_processes[index].memory_bytes == 0ull)
                {
                    selftest_fail("this process reports 0 bytes mapped");
                }
            }
        }
        cores = g_info.cpu_online != 0u ? g_info.cpu_online : 1u;
        if (idle_seen != (int)cores)
        {
            snprintf(text, sizeof(text), "expected one idle process per core (%u), found %d", cores, idle_seen);
            selftest_fail(text);
        }
        if (!self_seen)
        {
            selftest_fail("this process is not in the list");
        }
    }

    /* A second sample, after sleeping: the global counter must have moved,
     * and the idle must have taken the time. A system usage of "almost 0"
     * over an interval spent ASLEEP is the proof that the idle's counter
     * advances: if it did not, this would read 100%. */
    selftest_sleep(600);
    refresh_sample();
    if (g_info.cpu_ticks_total <= ticks_before)
    {
        selftest_fail("the global tick counter did not advance");
    }
    if (g_cpu_percent > 25u)
    {
        snprintf(text, sizeof(text), "asleep, the system reports %u%% CPU", g_cpu_percent);
        selftest_fail(text);
    }
    if (g_memory_total_bytes == 0ull || g_memory_used_bytes > g_memory_total_bytes)
    {
        snprintf(text, sizeof(text), "memory does not add up: %llu used of %llu",
                 (unsigned long long)g_memory_used_bytes, (unsigned long long)g_memory_total_bytes);
        selftest_fail(text);
    }
    for (index = 0; index < g_process_count; ++index)
    {
        if (g_processes[index].cpu_percent > 100u)
        {
            snprintf(text, sizeof(text), "pid %u at %u%% CPU",
                     (unsigned)g_processes[index].pid, g_processes[index].cpu_percent);
            selftest_fail(text);
            break;
        }
    }

    /* A third sample, after burning CPU for real: now the interval must be
     * charged to THIS process. It is the other side of the second, and what
     * tells "the counters move" from "the counters move in the right
     * process". */
    {
        unsigned long deadline = sxngui_uptime_ms() + 600ul;
        unsigned int self_percent = 0;

        while (sxngui_uptime_ms() < deadline)
        {
            /* Spin without sleeping: the only way to keep the time. */
        }
        refresh_sample();
        for (index = 0; index < g_process_count; ++index)
        {
            if (g_processes[index].pid == g_self_pid)
            {
                self_percent = g_processes[index].cpu_percent;
            }
        }
        if (g_cpu_percent < 50u / cores)
        {
            snprintf(text, sizeof(text), "spinning, the system reports %u%% CPU", g_cpu_percent);
            selftest_fail(text);
        }
        if (self_percent < 50u / cores)
        {
            snprintf(text, sizeof(text), "spinning, this process reports %u%% CPU", self_percent);
            selftest_fail(text);
        }
        /* The spin ran on one core, which must look busy in its own graph;
         * and the kernel share is part of the total, never more. */
        {
            unsigned int busiest = 0;
            int core;

            if (g_core_count != (int)(cores < TASKMGR_MAX_CPUS ? cores : TASKMGR_MAX_CPUS))
            {
                snprintf(text, sizeof(text), "%d per-core graphs for %u cores", g_core_count, cores);
                selftest_fail(text);
            }
            for (core = 0; core < g_core_count; ++core)
            {
                if (g_core_percent[core] > 100u)
                {
                    selftest_fail("a core reports more than 100%");
                }
                if (g_core_percent[core] > busiest)
                {
                    busiest = g_core_percent[core];
                }
            }
            if (busiest < 50u)
            {
                snprintf(text, sizeof(text), "spinning, the busiest core reports %u%%", busiest);
                selftest_fail(text);
            }
            if (g_kernel_percent > g_cpu_percent)
            {
                selftest_fail("kernel time is larger than the CPU usage");
            }
        }
    }

    format_grouped(1234567ull, grouped, sizeof(grouped));
    if (strcmp(grouped, "1,234,567") != 0)
    {
        snprintf(text, sizeof(text), "thousands grouping: '%s'", grouped);
        selftest_fail(text);
    }

    /* The v2 counters: this process has its main thread, and runs as whoever
     * started it (init, the system, under the smoke). */
    for (index = 0; index < g_process_count; ++index)
    {
        const struct taskmgr_sample *sample = &g_processes[index];

        if (sample->pid != g_self_pid)
        {
            continue;
        }
        if (sample->thread_count < 1u)
        {
            selftest_fail("this process reports no live thread");
        }
        if (sample->kernel_ticks > sample->cpu_ticks)
        {
            selftest_fail("kernel time is larger than the whole CPU time");
        }
    }
    {
        char user[16];

        format_user(0u, user, sizeof(user));
        if (strcmp(user, "SYSTEM") != 0)
        {
            selftest_fail("uid 0 is not shown as SYSTEM");
        }
    }
    if (g_info.timer_frequency_hz != 0u)
    {
        char cpu_time[24];

        format_cpu_time((uint64_t)3723u * g_info.timer_frequency_hz, cpu_time, sizeof(cpu_time));
        if (strcmp(cpu_time, "1:02:03") != 0)
        {
            snprintf(text, sizeof(text), "CPU Time format: '%s'", cpu_time);
            selftest_fail(text);
        }
    }

    /* Sorting by a header: PID descending must come out strictly
     * decreasing, and the name ascending must not go backwards. */
    g_sort_column = TASKMGR_COL_PID;
    g_sort_descending = 1;
    build_rows();
    for (index = 1; index < g_process_count; ++index)
    {
        if (g_processes[index - 1].pid <= g_processes[index].pid)
        {
            selftest_fail("sorting by PID, descending, is out of order");
            break;
        }
    }
    g_sort_column = TASKMGR_COL_NAME;
    g_sort_descending = 0;
    build_rows();
    for (index = 1; index < g_process_count; ++index)
    {
        if (compare_text(g_processes[index - 1].name, g_processes[index].name) > 0)
        {
            selftest_fail("sorting by name is out of order");
            break;
        }
    }
    g_sort_column = -1;

    /* Applications: rows come from the WM's list, the status from its flag,
     * and the selection stays on the same task when the list reorders. */
    {
        static struct savanxp_wm_window_list fake;

        memset(&fake, 0, sizeof(fake));
        fake.version = SAVANXP_WM_SHELL_PROTOCOL_VERSION;
        fake.count = 2;
        fake.windows[0].window_id = 21u;
        snprintf(fake.windows[0].title, sizeof(fake.windows[0].title), "%s", "Notepad");
        fake.windows[1].window_id = 22u;
        fake.windows[1].flags = SAVANXP_WM_WINDOW_FLAG_NOT_RESPONDING;
        snprintf(fake.windows[1].title, sizeof(fake.windows[1].title), "%s", "Paint");
        g_task_source = &fake;
        TASKMGR_TASK_LIST->value = 1;
        refresh_tasks();
        if (g_tasks.count != 2u || strcmp(g_task_rows[0], "Notepad\tRunning") != 0 ||
            strcmp(g_task_rows[1], "Paint\tNot Responding") != 0)
        {
            selftest_fail("the Applications rows do not match the WM's list");
        }
        fake.windows[0] = fake.windows[1];
        fake.windows[1].window_id = 21u;
        fake.windows[1].flags = 0u;
        snprintf(fake.windows[1].title, sizeof(fake.windows[1].title), "%s", "Notepad");
        fake.sequence += 2u;
        refresh_tasks();
        if (g_selected_task != 22u || TASKMGR_TASK_LIST->value != 0)
        {
            selftest_fail("the Applications selection did not follow its task");
        }
        g_task_source = 0;
    }

    /* End Process Tree's order: children before their parent, all the way
     * down. 10 started 11 and 12, and 11 started 13. */
    {
        static const struct taskmgr_tree_node nodes[] = {
            {10u, 1u}, {11u, 10u}, {12u, 10u}, {13u, 11u}, {14u, 1u},
        };
        static const uint32_t expected[] = {13u, 11u, 12u, 10u};
        uint32_t order[8];
        int count = tree_order(nodes, (int)(sizeof(nodes) / sizeof(nodes[0])), 10u, order, 0, 8);

        if (count != 4 || memcmp(order, expected, sizeof(expected)) != 0)
        {
            selftest_fail("End Process Tree would not end children first");
        }
    }

    if (g_selftest_failures != 0)
    {
        return 1;
    }
    snprintf(text, sizeof(text), "taskmgr: self-test passed, %d processes, cpu %u%%, memory %u%%, handles %u",
             g_process_count, g_cpu_percent, g_memory_percent, (unsigned)g_total_handles);
    (void)sxn_log(text);
    return 0;
}

/* ---- start-up ------------------------------------------------------------------------- */

static void build_dialogs(void)
{
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

    g_message_widgets[0] = sxgui_label(
        sx_rect_make(TASKMGR_DIALOG_MARGIN, TASKMGR_DIALOG_MARGIN,
                     TASKMGR_CONFIRM_WIDTH - (TASKMGR_DIALOG_MARGIN * 2), TASKMGR_ROW),
        g_message_line);
    g_message_widgets[1] = sxgui_label(
        sx_rect_make(TASKMGR_DIALOG_MARGIN, TASKMGR_DIALOG_MARGIN + TASKMGR_ROW + 2,
                     TASKMGR_CONFIRM_WIDTH - (TASKMGR_DIALOG_MARGIN * 2), TASKMGR_ROW),
        g_message_detail);
    g_message_widgets[2] = sxgui_button(
        sx_rect_make(centred_button_x(TASKMGR_CONFIRM_WIDTH, 1, 0),
                     TASKMGR_CONFIRM_HEIGHT - TASKMGR_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_message_ok, 0);

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
}

static int run_window(void)
{
    int result;

    *TASKMGR_TABS = sxgui_tabs(sx_rect_make(0, 0, 0, 0), k_tab_labels, TASKMGR_TAB_COUNT,
                               TASKMGR_TAB_APPLICATIONS);
    TASKMGR_TABS->on_action = on_tab_changed;
    TASKMGR_TABS->user = &g_app;

    *TASKMGR_LIST = sxgui_listbox(sx_rect_make(0, 0, 0, 0), g_row_pointers, 0);
    TASKMGR_LIST->columns = g_columns;
    TASKMGR_LIST->column_count = TASKMGR_COLUMN_COUNT;
    TASKMGR_LIST->on_action = on_list;
    TASKMGR_LIST->user = &g_app;

    *TASKMGR_END_BUTTON = sxgui_button(sx_rect_make(0, 0, 0, 0), "End Process", on_end_process, 0);

    *TASKMGR_TASK_LIST = sxgui_listbox(sx_rect_make(0, 0, 0, 0), g_task_row_pointers, 0);
    TASKMGR_TASK_LIST->columns = g_task_columns;
    TASKMGR_TASK_LIST->column_count = (int)(sizeof(g_task_columns) / sizeof(g_task_columns[0]));
    TASKMGR_TASK_LIST->on_action = on_task_list;
    *TASKMGR_END_TASK_BUTTON = sxgui_button(sx_rect_make(0, 0, 0, 0), "End Task", on_end_task, 0);
    *TASKMGR_SWITCH_BUTTON = sxgui_button(sx_rect_make(0, 0, 0, 0), "Switch To", on_switch_to, 0);
    *TASKMGR_NEW_TASK_BUTTON = sxgui_button(sx_rect_make(0, 0, 0, 0), "New Task...", on_new_task, 0);

    *TASKMGR_STATUS_PROCESSES = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status_processes);
    TASKMGR_STATUS_PROCESSES->flags |= SXGUI_FLAG_SUNKEN;
    *TASKMGR_STATUS_CPU = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status_cpu);
    TASKMGR_STATUS_CPU->flags |= SXGUI_FLAG_SUNKEN;
    *TASKMGR_STATUS_MEMORY = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status_memory);
    TASKMGR_STATUS_MEMORY->flags |= SXGUI_FLAG_SUNKEN;

    build_dialogs();

    result = sxngui_app_init(&g_app, &sun_resources, g_widgets, TASKMGR_WIDGET_COUNT);
    if (result < 0)
    {
        (void)sxn_log("taskmgr: no window from windowd");
        return 1;
    }

    /* No list (an older WM, or no room for the section): the tab stays
     * empty rather than the window failing. */
    if (sxnwm_task_list_open(&g_app.wm, &g_task_source) < 0)
    {
        g_task_source = 0;
    }

    g_menubar.menus = k_menus;
    g_menubar.menu_count = (int)(sizeof(k_menus) / sizeof(k_menus[0]));
    g_menubar.on_command = on_menu_command;
    sxgui_set_menubar(&g_app.ui, &g_menubar);

    (void)sxngui_app_set_content_size(&g_app, TASKMGR_DEFAULT_WIDTH, TASKMGR_DEFAULT_HEIGHT);
    relayout();
    apply_tab_visibility();
    refresh_sample();

    g_app.on_paint = on_paint;
    g_app.on_key = on_key;
    g_app.on_pointer = on_pointer;
    g_app.on_resize = on_resize;
    g_app.on_tick = on_tick;
    set_update_speed(g_speed_index);
    sxgui_focus(&g_app.ui, TASKMGR_TASK_WIDGET_FIRST);
    return sxngui_app_run(&g_app);
}

int main(int argc, char **argv)
{
    struct sxn_native_info info;

    /* A SUN program never returns from main: the stock crt0 would fall
     * through to a POSIX exit, which is ENOSYS here. */
    if (sxn_info(&info) == 0)
    {
        g_self_pid = info.pid;
    }
    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        sxn_exit(selftest());
    }
    if (argc > 2 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--end") == 0)
    {
        sxn_exit(run_end(argv[2], 0));
    }
    if (argc > 2 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--end-tree") == 0)
    {
        sxn_exit(run_end(argv[2], 1));
    }
    sxn_exit(run_window());
    return 0;
}
