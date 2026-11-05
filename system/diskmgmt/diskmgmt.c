/*
 * Disk Management, a SUN program (docs/userland/DISK_MANAGEMENT.md).
 *
 * The Windows 2000 console of the same name: the volumes as a list on top,
 * every disk below as a row of its partitions and free space drawn to scale,
 * a context menu on each, and Properties.
 *
 * WHERE THE DATA COMES FROM. One call, SXN_SYS_STORAGE_SNAPSHOT, lists the
 * disks, their partitions and the LiveCD volume with what is on each and
 * where it is mounted. Listing is open to every process; anything that
 * changes a disk runs in a second, elevated copy of this program that checks
 * everything again before it writes.
 *
 * THE MAP. A disk row is drawn from its partitions in disk order and the free
 * ranges between them. Free space is what libsxdisk would offer for a new
 * partition (1 MiB aligned), so what the map shows as free is exactly what
 * New Partition can use.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi/savanxp_abi.h"
#include "sxdisk/sxdisk.h"
#include "sxgfx/gfx2d.h"
#include "sxgfx/pixels.h"
#include "sxgfx/sxchrome.h"
#include "sxgfx/sxgui.h"
#include "sxn/sxn.h"
#include "sxn/sxnelevate.h"
#include "sxn/sxngui.h"
#include "sxfs_core.h"

#include "diskmgmt.sunres.h"

#define DM_MAX_DEVICES 32
#define DM_MAX_SEGMENTS 24
#define DM_ROW_CAPACITY 160

#define DM_DEFAULT_WIDTH 640
#define DM_DEFAULT_HEIGHT 480
#define DM_MIN_WIDTH 480
#define DM_MIN_HEIGHT 360

/* The disk map. Each disk is a row: a grey box with what it is, then its
 * segments. */
#define DM_DISK_ROW_HEIGHT 64
#define DM_DISK_ROW_GAP 4
#define DM_LABEL_WIDTH 104
#define DM_SEGMENT_MIN_WIDTH 72
#define DM_SEGMENT_HEADER 10
#define DM_LEGEND_HEIGHT 20

/* The colours of the original's legend. */
#define DM_COLOR_PRIMARY SXGUI_RGB(0, 0, 128)
#define DM_COLOR_VOLUME SXGUI_RGB(0, 128, 128)
#define DM_COLOR_UNALLOCATED SXGUI_RGB(0, 0, 0)
#define DM_COLOR_FREE SXGUI_RGB(0, 128, 0)
#define DM_COLOR_UNREADABLE SXGUI_RGB(128, 0, 0)
#define DM_COLOR_HATCH SXGUI_RGB(0, 0, 0)

/* ---- the model ------------------------------------------------------------- */

static struct sxn_storage_device g_devices[DM_MAX_DEVICES];
static int g_device_count;
static uint32_t g_generation;

enum dm_segment_kind {
    DM_SEG_PARTITION = 1, /* a partition: device names it */
    DM_SEG_VOLUME,        /* a whole disk or the LiveCD holding a file system */
    DM_SEG_FREE,          /* free space inside a partition table */
    DM_SEG_UNALLOCATED,   /* a disk with no table and nothing on it */
    DM_SEG_UNREADABLE,    /* a damaged table */
};

struct dm_segment {
    int kind;
    int device;        /* index into g_devices, or -1 for free space */
    uint64_t start;    /* sectors, on the disk */
    uint64_t sectors;
    struct sx_rect rect;
};

struct dm_disk {
    int device;        /* index into g_devices */
    int number;        /* "Disk N" or "LiveCD N" */
    struct dm_segment segments[DM_MAX_SEGMENTS];
    int segment_count;
    struct sx_rect rect;
    struct sx_rect label_rect;
};

static struct dm_disk g_disks[DM_MAX_DEVICES];
static int g_disk_count;

/* What is selected, by identity rather than position: a refresh rebuilds
 * every array. A free range is named by its disk and its first sector. */
enum dm_selection_kind {
    DM_SELECT_NONE = 0,
    DM_SELECT_DEVICE,   /* a partition or a whole-disk volume */
    DM_SELECT_DISK,     /* the disk's grey box */
    DM_SELECT_FREE,     /* free space or an unallocated disk */
};

static int g_select_kind;
static uint32_t g_select_id;
static uint64_t g_select_start;

/* ---- the volume list --------------------------------------------------------- */

static struct sxgui_column g_columns[] = {
    {"Volume", 0, 0},
    {"Layout", 64, 0},
    {"Type", 44, 0},
    {"File System", 72, 0},
    {"Status", 112, 0},
    {"Capacity", 72, SXGUI_COLUMN_RIGHT},
    {"Free Space", 72, SXGUI_COLUMN_RIGHT},
    {"% Free", 48, SXGUI_COLUMN_RIGHT},
};
#define DM_COLUMN_COUNT ((int)(sizeof(g_columns) / sizeof(g_columns[0])))

static char g_rows[DM_MAX_DEVICES][DM_ROW_CAPACITY];
static const char *g_row_pointers[DM_MAX_DEVICES];
static int g_row_device[DM_MAX_DEVICES]; /* row -> index into g_devices */
static int g_row_count;

/* ---- widgets ------------------------------------------------------------------- */

static struct sxngui_app g_app;
static struct sxgui_menubar g_menubar;
static uint32_t g_pointer_buttons;
static int g_map_scroll; /* pixels */

static struct sxgui_widget g_widgets[4];
#define DM_LIST (&g_widgets[0])
#define DM_SCROLL (&g_widgets[1])
#define DM_STATUS (&g_widgets[2])
#define DM_STATUS_RIGHT (&g_widgets[3])
#define DM_WIDGET_COUNT ((int)(sizeof(g_widgets) / sizeof(g_widgets[0])))

static char g_status[96];
static char g_status_right[64];
static struct sx_rect g_map_rect;
static struct sx_rect g_legend_rect;

/* ---- menus ---------------------------------------------------------------------- */

enum dm_command {
    DM_CMD_EXIT = 1,
    DM_CMD_REFRESH,
    DM_CMD_RESCAN,
    DM_CMD_INITIALIZE,
    DM_CMD_NEW_PARTITION,
    DM_CMD_FORMAT,
    DM_CMD_DELETE,
    DM_CMD_PROPERTIES,
    DM_CMD_ABOUT,
};

static struct sxgui_menu_item k_file_items[] = {
    {"Exit", DM_CMD_EXIT, 0},
};

/* The original keeps the operations in Action > All Tasks; SXGUI's menus have
 * one level, so they sit in Action itself. The indices below move the
 * disabled marks and have to follow the table. */
static struct sxgui_menu_item k_action_items[] = {
    {"Refresh\tF5", DM_CMD_REFRESH, 0},
    {"Rescan Disks", DM_CMD_RESCAN, 0},
    {0, 0, 0},
    {"Initialize Disk...", DM_CMD_INITIALIZE, SXGUI_MENU_DISABLED},
    {"New Partition...", DM_CMD_NEW_PARTITION, SXGUI_MENU_DISABLED},
    {"Format...", DM_CMD_FORMAT, SXGUI_MENU_DISABLED},
    {"Delete Partition...\tDel", DM_CMD_DELETE, SXGUI_MENU_DISABLED},
    {0, 0, 0},
    {"Properties\tAlt+Enter", DM_CMD_PROPERTIES, 0},
};
#define DM_ITEM_INITIALIZE 3
#define DM_ITEM_NEW_PARTITION 4
#define DM_ITEM_FORMAT 5
#define DM_ITEM_DELETE 6
#define DM_ITEM_PROPERTIES 8

static struct sxgui_menu_item k_help_items[] = {
    {"About Disk Management", DM_CMD_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"Action", k_action_items, (int)(sizeof(k_action_items) / sizeof(k_action_items[0]))},
    {"Help", k_help_items, (int)(sizeof(k_help_items) / sizeof(k_help_items[0]))},
};

/* The context menu is built for whatever was clicked. */
static struct sxgui_menu_item g_context_items[10];
static struct sxgui_menu g_context_menu = {"", g_context_items, 0};

/* ---- dialogs -------------------------------------------------------------------- */

#define DM_DIALOG_MARGIN SXGUI_DIALOG_MARGIN
#define DM_TEXT_ROW 18
#define DM_PROPERTIES_LINES 12
#define DM_PROPERTIES_WIDTH 420
#define DM_PROPERTIES_HEIGHT \
    (DM_DIALOG_MARGIN * 2 + DM_PROPERTIES_LINES * DM_TEXT_ROW + SXGUI_GAP + SXGUI_BUTTON_HEIGHT)
#define DM_MESSAGE_WIDTH 380
#define DM_MESSAGE_HEIGHT 120

static struct sxgui_dialog g_properties_dialog;
/* Two labels per line, name and value, so the values line up in a
 * proportional font. */
#define DM_PROPERTIES_VALUE_X 112
static struct sxgui_widget g_properties_widgets[DM_PROPERTIES_LINES * 2 + 1];
static char g_properties_names[DM_PROPERTIES_LINES][24];
static char g_properties_lines[DM_PROPERTIES_LINES][72];
static char g_properties_title[64];

static struct sxgui_dialog g_message_dialog;
static struct sxgui_widget g_message_widgets[3];
static char g_message_line[96];
static char g_message_detail[96];

/* Yes/No, No by default: every question here is about erasing something. */
#define DM_CONFIRM_WIDTH 440
#define DM_CONFIRM_HEIGHT 120
static struct sxgui_dialog g_confirm_dialog;
static struct sxgui_widget g_confirm_widgets[4];
static char g_confirm_line[128];
static char g_confirm_detail[96];

/* New Partition and Format share the label field; New Partition adds the
 * size and the format check box. */
#define DM_FORM_WIDTH 400
#define DM_FORM_HEIGHT 200
#define DM_LABEL_CAPACITY 37
static struct sxgui_dialog g_new_dialog;
static struct sxgui_widget g_new_widgets[8];
static char g_new_size[16];
static char g_new_label[DM_LABEL_CAPACITY];
static char g_new_note[96];
static struct sxgui_dialog g_format_dialog;
static struct sxgui_widget g_format_widgets[6];
static char g_format_label[DM_LABEL_CAPACITY];
static char g_format_note[96];

/* What the open dialog is about, copied when it opened: a refresh while it
 * is up must not change what Yes does. */
enum dm_pending {
    DM_PENDING_NONE = 0,
    DM_PENDING_INITIALIZE,
    DM_PENDING_DELETE,
};
static int g_pending;
static uint32_t g_pending_id;
static char g_pending_name[SXN_STORAGE_NAME_CAPACITY];
static uint64_t g_pending_start;
static uint64_t g_pending_sectors;

/* ---- formatting ---------------------------------------------------------------- */

static int centred_button_x(int dialog_width, int count, int index)
{
    return (dialog_width - (count * SXGUI_BUTTON_WIDTH) - ((count - 1) * SXGUI_GAP)) / 2 +
        (index * (SXGUI_BUTTON_WIDTH + SXGUI_GAP));
}

/* "512 KB", "64 MB", "1.95 GB": two decimals only for gigabytes, as the
 * original writes them. */
static void format_bytes(uint64_t bytes, char *out, size_t capacity)
{
    const uint64_t kib = 1024ull;
    const uint64_t mib = kib * 1024ull;
    const uint64_t gib = mib * 1024ull;

    if (bytes >= gib)
    {
        uint64_t hundredths = (bytes * 100ull + gib / 2ull) / gib;
        snprintf(out, capacity, "%lu.%02lu GB", (unsigned long)(hundredths / 100ull),
                 (unsigned long)(hundredths % 100ull));
    }
    else if (bytes >= mib)
    {
        snprintf(out, capacity, "%lu MB", (unsigned long)((bytes + mib / 2ull) / mib));
    }
    else
    {
        snprintf(out, capacity, "%lu KB", (unsigned long)((bytes + kib / 2ull) / kib));
    }
}

static uint64_t sectors_to_bytes(uint64_t sectors)
{
    return sectors * (uint64_t)SXDISK_SECTOR_SIZE;
}

static const char *fs_display_name(const struct sxn_storage_device *device)
{
    if (strcmp(device->fs_name, "sxfs") == 0)
    {
        return "SxFS";
    }
    if (strcmp(device->fs_name, "fat") == 0)
    {
        return "FAT";
    }
    return "";
}

static int is_disk(const struct sxn_storage_device *device)
{
    return device->kind == SXN_STORAGE_DISK || device->kind == SXN_STORAGE_MEMORY;
}

static int find_device(uint32_t id)
{
    int index;

    for (index = 0; index < g_device_count; ++index)
    {
        if (g_devices[index].id == id)
        {
            return index;
        }
    }
    return -1;
}

/* What the volume is called in the list and on the map: where it is mounted,
 * else its label, else its device name. */
static void volume_name(const struct sxn_storage_device *device, char *out, size_t capacity)
{
    if (device->mount_point[0] != '\0')
    {
        snprintf(out, capacity, "%s", device->mount_point);
    }
    else if (device->label[0] != '\0')
    {
        snprintf(out, capacity, "%s", device->label);
    }
    else
    {
        snprintf(out, capacity, "(%s)", device->name);
    }
}

static void volume_status(const struct sxn_storage_device *device, char *out, size_t capacity)
{
    const char *extra = "";

    if ((device->flags & SXN_STORAGE_SYSTEM) != 0u)
    {
        extra = " (System)";
    }
    else if ((device->flags & SXN_STORAGE_ESP) != 0u)
    {
        extra = " (EFI System)";
    }
    else if ((device->flags & SXN_STORAGE_MOUNTED) != 0u)
    {
        extra = " (Mounted)";
    }
    snprintf(out, capacity, "Healthy%s", extra);
}

static const char *partition_type_name(const struct sxn_storage_device *device)
{
    const char *name = 0;

    if (device->scheme == SXN_STORAGE_SCHEME_GPT)
    {
        name = sxdisk_type_name(device->type_guid);
    }
    else if (device->scheme == SXN_STORAGE_SCHEME_MBR)
    {
        name = sxdisk_mbr_type_name((uint8_t)device->mbr_type);
    }
    return name != 0 ? name : "Unknown";
}

/* ---- loading ------------------------------------------------------------------- */

/* Free space of a disk with a table: build the table libsxdisk would see from
 * the partitions the kernel sliced, and ask it. MBR disks get the same
 * computation over the whole disk, for display. */
static struct sxdisk_gpt g_layout;

static int add_segment(struct dm_disk *disk, int kind, int device, uint64_t start, uint64_t sectors)
{
    struct dm_segment *segment;

    if (disk->segment_count >= DM_MAX_SEGMENTS)
    {
        return 0;
    }
    segment = &disk->segments[disk->segment_count++];
    memset(segment, 0, sizeof(*segment));
    segment->kind = kind;
    segment->device = device;
    segment->start = start;
    segment->sectors = sectors;
    return 1;
}

static void build_partitioned(struct dm_disk *disk, const struct sxn_storage_device *device)
{
    struct sxdisk_extent extents[DM_MAX_SEGMENTS];
    int children[DM_MAX_SEGMENTS];
    int child_count = 0;
    size_t extent_count;
    size_t next_extent = 0;
    int next_child = 0;
    int index;

    memset(&g_layout, 0, sizeof(g_layout));
    g_layout.disk_sectors = device->sector_count;
    if (device->scheme == SXN_STORAGE_SCHEME_GPT && device->sector_count > 2u * (1u + SXDISK_GPT_ARRAY_SECTORS))
    {
        g_layout.first_usable = 2u + SXDISK_GPT_ARRAY_SECTORS;
        g_layout.last_usable = device->sector_count - 2u - SXDISK_GPT_ARRAY_SECTORS;
    }
    else
    {
        g_layout.first_usable = 1u;
        g_layout.last_usable = device->sector_count - 1u;
    }

    /* The partitions in disk order (insertion sort; a disk has a handful). */
    for (index = 0; index < g_device_count && child_count < DM_MAX_SEGMENTS; ++index)
    {
        const struct sxn_storage_device *child = &g_devices[index];
        int at;

        if (child->kind != SXN_STORAGE_PARTITION || child->parent_id != device->id)
        {
            continue;
        }
        at = child_count++;
        while (at > 0 && g_devices[children[at - 1]].start_lba > child->start_lba)
        {
            children[at] = children[at - 1];
            at -= 1;
        }
        children[at] = index;
        if (child_count <= (int)SXDISK_GPT_ENTRY_COUNT)
        {
            struct sxdisk_gpt_entry *entry = &g_layout.entries[child_count - 1];

            entry->type_guid[0] = 1; /* in use; the type itself does not matter here */
            entry->first_lba = child->start_lba;
            entry->last_lba = child->start_lba + child->sector_count - 1u;
        }
    }
    extent_count = sxdisk_gpt_free_extents(&g_layout, extents, DM_MAX_SEGMENTS);
    if (extent_count > DM_MAX_SEGMENTS)
    {
        extent_count = DM_MAX_SEGMENTS;
    }

    /* Merge the two sorted lists. */
    while (next_child < child_count || next_extent < extent_count)
    {
        int take_child = next_extent >= extent_count ||
            (next_child < child_count &&
             g_devices[children[next_child]].start_lba < extents[next_extent].first_lba);

        if (take_child)
        {
            const struct sxn_storage_device *child = &g_devices[children[next_child]];

            (void)add_segment(disk, DM_SEG_PARTITION, children[next_child], child->start_lba, child->sector_count);
            next_child += 1;
        }
        else
        {
            (void)add_segment(disk, DM_SEG_FREE, -1, extents[next_extent].first_lba,
                              extents[next_extent].sector_count);
            next_extent += 1;
        }
    }
}

static void build_disks(void)
{
    int index;
    int disk_number = 0;
    int memory_number = 0;

    g_disk_count = 0;
    for (index = 0; index < g_device_count; ++index)
    {
        const struct sxn_storage_device *device = &g_devices[index];
        struct dm_disk *disk;

        if (!is_disk(device))
        {
            continue;
        }
        disk = &g_disks[g_disk_count++];
        memset(disk, 0, sizeof(*disk));
        disk->device = index;
        disk->number = device->kind == SXN_STORAGE_MEMORY ? memory_number++ : disk_number++;

        if (device->scheme == SXN_STORAGE_SCHEME_GPT || device->scheme == SXN_STORAGE_SCHEME_MBR)
        {
            build_partitioned(disk, device);
        }
        else if (device->scheme == SXN_STORAGE_SCHEME_DAMAGED)
        {
            (void)add_segment(disk, DM_SEG_UNREADABLE, index, 0, device->sector_count);
        }
        else if (device->fs_name[0] != '\0')
        {
            (void)add_segment(disk, DM_SEG_VOLUME, index, 0, device->sector_count);
        }
        else
        {
            (void)add_segment(disk, DM_SEG_UNALLOCATED, -1, 0, device->sector_count);
        }
    }
}

static void build_rows(void)
{
    int index;

    g_row_count = 0;
    for (index = 0; index < g_device_count && g_row_count < DM_MAX_DEVICES; ++index)
    {
        const struct sxn_storage_device *device = &g_devices[index];
        char name[48];
        char status[40];
        char capacity[24];
        char free_text[24] = "";
        char percent[8] = "";
        const char *layout = "Partition";

        if (device->kind == SXN_STORAGE_PARTITION)
        {
            layout = "Partition";
        }
        else if (device->fs_name[0] != '\0' && device->scheme == SXN_STORAGE_SCHEME_NONE)
        {
            layout = "Simple";
        }
        else
        {
            continue;
        }
        volume_name(device, name, sizeof(name));
        volume_status(device, status, sizeof(status));
        format_bytes(sectors_to_bytes(device->sector_count), capacity, sizeof(capacity));
        if ((device->flags & SXN_STORAGE_MOUNTED) != 0u && device->total_bytes != 0u)
        {
            format_bytes(device->free_bytes, free_text, sizeof(free_text));
            snprintf(percent, sizeof(percent), "%u %%",
                     (unsigned)((device->free_bytes * 100ull) / device->total_bytes));
        }
        snprintf(g_rows[g_row_count], DM_ROW_CAPACITY, "%s\t%s\tBasic\t%s\t%s\t%s\t%s\t%s", name, layout,
                 fs_display_name(device), status, capacity, free_text, percent);
        g_row_pointers[g_row_count] = g_rows[g_row_count];
        g_row_device[g_row_count] = index;
        g_row_count += 1;
    }
    DM_LIST->items = g_row_pointers;
    DM_LIST->item_count = g_row_count;
}

/* Reads every device. Returns 1 if anything changed since the last load. */
static int load_devices(void)
{
    struct sxn_storage_device device;
    unsigned long index;
    int count = 0;
    int changed;

    for (index = 0; index < DM_MAX_DEVICES; ++index)
    {
        long result = sxn_storage_snapshot(index, &device);

        if (result <= 0)
        {
            break;
        }
        g_devices[count++] = device;
    }
    changed = count != g_device_count || (count > 0 && g_devices[0].generation != g_generation);
    g_device_count = count;
    g_generation = count > 0 ? g_devices[0].generation : 0u;
    build_disks();
    return changed;
}

/* ---- selection ------------------------------------------------------------------ */

static void sync_list_to_selection(void)
{
    int row;

    DM_LIST->value = -1;
    if (g_select_kind != DM_SELECT_DEVICE)
    {
        return;
    }
    for (row = 0; row < g_row_count; ++row)
    {
        if (g_devices[g_row_device[row]].id == g_select_id)
        {
            DM_LIST->value = row;
            return;
        }
    }
}

static const struct sxn_storage_device *selected_device(void)
{
    int index;

    if (g_select_kind != DM_SELECT_DEVICE && g_select_kind != DM_SELECT_DISK)
    {
        return 0;
    }
    index = find_device(g_select_id);
    return index >= 0 ? &g_devices[index] : 0;
}

static void update_status(void)
{
    const struct sxn_storage_device *device = selected_device();
    char size[24];
    int disks = 0;
    int index;

    for (index = 0; index < g_disk_count; ++index)
    {
        if (g_devices[g_disks[index].device].kind == SXN_STORAGE_DISK)
        {
            disks += 1;
        }
    }
    if (device != 0)
    {
        format_bytes(sectors_to_bytes(device->sector_count), size, sizeof(size));
        snprintf(g_status, sizeof(g_status), "%s: %s, %s", device->name,
                 device->kind == SXN_STORAGE_PARTITION ? partition_type_name(device) : device->model, size);
    }
    else if (g_select_kind == DM_SELECT_FREE)
    {
        snprintf(g_status, sizeof(g_status), "%s", "Unallocated space");
    }
    else
    {
        g_status[0] = '\0';
    }
    snprintf(g_status_right, sizeof(g_status_right), "%d disk%s, %d volume%s", disks, disks == 1 ? "" : "s",
             g_row_count, g_row_count == 1 ? "" : "s");
}

static void select_device(uint32_t id, int kind)
{
    g_select_kind = kind;
    g_select_id = id;
    g_select_start = 0;
    sync_list_to_selection();
    update_status();
    sxngui_app_request_repaint(&g_app);
}

static void select_free(uint32_t disk_id, uint64_t start)
{
    g_select_kind = DM_SELECT_FREE;
    g_select_id = disk_id;
    g_select_start = start;
    sync_list_to_selection();
    update_status();
    sxngui_app_request_repaint(&g_app);
}

/* After a reload: keep the selection if what it named still exists. */
static void revalidate_selection(void)
{
    int index;
    int segment;

    if (g_select_kind == DM_SELECT_DEVICE || g_select_kind == DM_SELECT_DISK)
    {
        if (find_device(g_select_id) < 0)
        {
            g_select_kind = DM_SELECT_NONE;
        }
    }
    else if (g_select_kind == DM_SELECT_FREE)
    {
        int found = 0;

        for (index = 0; index < g_disk_count && !found; ++index)
        {
            const struct dm_disk *disk = &g_disks[index];

            if (g_devices[disk->device].id != g_select_id)
            {
                continue;
            }
            for (segment = 0; segment < disk->segment_count; ++segment)
            {
                const struct dm_segment *item = &disk->segments[segment];

                if ((item->kind == DM_SEG_FREE || item->kind == DM_SEG_UNALLOCATED) &&
                    item->start == g_select_start)
                {
                    found = 1;
                    break;
                }
            }
        }
        if (!found)
        {
            g_select_kind = DM_SELECT_NONE;
        }
    }
    sync_list_to_selection();
    update_status();
}

static void reload(void)
{
    (void)load_devices();
    build_rows();
    revalidate_selection();
    sxngui_app_request_repaint(&g_app);
}

/* ---- the rules ---------------------------------------------------------------------
 *
 * One set of answers for both halves: the window greys out what they refuse,
 * and the elevated copy asks them again on a fresh listing before it writes
 * (docs/userland/DISK_MANAGEMENT.md#rules-for-the-operations-that-write). */

/* A disk the system may lay a new GPT on: a real disk with no table and
 * nothing on it, not past 2 TiB. */
static int can_initialize(const struct sxn_storage_device *disk)
{
    return disk != 0 && disk->kind == SXN_STORAGE_DISK && disk->scheme == SXN_STORAGE_SCHEME_NONE &&
        disk->fs_name[0] == '\0' && (disk->flags & SXN_STORAGE_WRITABLE) != 0u &&
        (disk->flags & (SXN_STORAGE_MOUNTED | SXN_STORAGE_CHILD_MOUNTED | SXN_STORAGE_SYSTEM |
                        SXN_STORAGE_TRUNCATED)) == 0u &&
        disk->sector_count >= SXDISK_MIN_DISK_SECTORS;
}

/* A disk whose table may change: GPT, writable, not holding "/", nothing on
 * it mounted (its partitions could not be read again), not past 2 TiB. */
static int can_partition(const struct sxn_storage_device *disk)
{
    return disk != 0 && disk->kind == SXN_STORAGE_DISK && disk->scheme == SXN_STORAGE_SCHEME_GPT &&
        (disk->flags & SXN_STORAGE_WRITABLE) != 0u &&
        (disk->flags & (SXN_STORAGE_CHILD_MOUNTED | SXN_STORAGE_SYSTEM | SXN_STORAGE_TRUNCATED |
                        SXN_STORAGE_GPT_BACKUP)) == 0u;
}

/* A partition that may be formatted or deleted: on such a disk, not the ESP,
 * not mounted. */
static int can_modify_partition(const struct sxn_storage_device *partition)
{
    int parent;

    if (partition == 0 || partition->kind != SXN_STORAGE_PARTITION ||
        partition->scheme != SXN_STORAGE_SCHEME_GPT ||
        (partition->flags & (SXN_STORAGE_ESP | SXN_STORAGE_MOUNTED | SXN_STORAGE_SYSTEM)) != 0u)
    {
        return 0;
    }
    parent = find_device(partition->parent_id);
    return parent >= 0 && can_partition(&g_devices[parent]);
}

/* ---- sector I/O ------------------------------------------------------------------- */

/* The last error a storage call returned, so an operation can report why the
 * library's "I/O error" happened: -EPERM for a caller who is not the system,
 * -EBUSY for a mounted volume. */
static long g_io_error;

static int io_read(void *cookie, uint64_t lba, uint32_t count, void *buffer)
{
    uint32_t id = *(const uint32_t *)cookie;
    uint8_t *bytes = (uint8_t *)buffer;

    while (count > 0u)
    {
        uint32_t chunk = count > SXN_STORAGE_MAX_TRANSFER ? SXN_STORAGE_MAX_TRANSFER : count;
        long result = sxn_storage_read(id, lba, chunk, bytes);

        if (result < 0)
        {
            g_io_error = result;
            return -1;
        }
        lba += chunk;
        count -= chunk;
        bytes += (size_t)chunk * SXDISK_SECTOR_SIZE;
    }
    return 0;
}

static int io_write(void *cookie, uint64_t lba, uint32_t count, const void *buffer)
{
    uint32_t id = *(const uint32_t *)cookie;
    const uint8_t *bytes = (const uint8_t *)buffer;

    while (count > 0u)
    {
        uint32_t chunk = count > SXN_STORAGE_MAX_TRANSFER ? SXN_STORAGE_MAX_TRANSFER : count;
        long result = sxn_storage_write(id, lba, chunk, bytes);

        if (result < 0)
        {
            g_io_error = result;
            return -1;
        }
        lba += chunk;
        count -= chunk;
        bytes += (size_t)chunk * SXDISK_SECTOR_SIZE;
    }
    return 0;
}

static int sxfs_io_read(void *cookie, uint32_t lba, uint32_t count, void *buffer)
{
    return io_read(cookie, lba, count, buffer);
}

static int sxfs_io_write(void *cookie, uint32_t lba, uint32_t count, const void *buffer)
{
    return io_write(cookie, lba, count, buffer);
}

/* GUIDs only have to be unique, not secret: xorshift64* seeded from the clock
 * and the pid. */
static uint64_t g_random_state;

static void io_random(void *cookie, void *out, size_t size)
{
    uint8_t *bytes = (uint8_t *)out;
    size_t index;

    (void)cookie;
    if (g_random_state == 0u)
    {
        struct sxn_native_info info;

        g_random_state = sxn_clock_ns() ^ 0x9e3779b97f4a7c15ull;
        if (sxn_info(&info) == 0)
        {
            g_random_state ^= (uint64_t)info.pid << 32;
        }
    }
    for (index = 0; index < size; ++index)
    {
        g_random_state ^= g_random_state >> 12;
        g_random_state ^= g_random_state << 25;
        g_random_state ^= g_random_state >> 27;
        bytes[index] = (uint8_t)((g_random_state * 0x2545f4914f6cdd1dull) >> 56);
    }
}

/* ---- the operations ----------------------------------------------------------------
 *
 * Run by the elevated copy (or directly when this process already acts as the
 * system). Each takes what the window showed -- the id and name, the range --
 * lists the devices again and refuses if the disk is no longer what the user
 * looked at. They return 0 or a positive errno, which becomes the exit status
 * the window reads back. */

static struct sxdisk_gpt g_table;

/* The device with this id, if its name still matches. */
static const struct sxn_storage_device *recheck(uint32_t id, const char *name)
{
    int index;

    (void)load_devices();
    index = find_device(id);
    if (index < 0 || name == 0 || strcmp(g_devices[index].name, name) != 0)
    {
        return 0;
    }
    return &g_devices[index];
}

static int io_status(int fallback)
{
    long error = g_io_error;

    g_io_error = 0;
    return error < 0 ? (int)-error : fallback;
}

static int rescan_disk(uint32_t id)
{
    long result = sxn_storage_rescan(id);

    return result < 0 ? (int)-result : 0;
}

static int op_init(uint32_t id, const char *name)
{
    const struct sxn_storage_device *disk = recheck(id, name);
    struct sxdisk_io io = {&id, io_read, io_write, io_random};

    if (disk == 0)
    {
        return SAVANXP_ENOENT;
    }
    if (!can_initialize(disk))
    {
        return SAVANXP_EBUSY;
    }
    g_io_error = 0;
    if (sxdisk_gpt_init(&g_table, disk->sector_count, &io) != SXDISK_OK)
    {
        return SAVANXP_EINVAL;
    }
    if (sxdisk_gpt_write(&g_table, &io) != SXDISK_OK)
    {
        return io_status(SAVANXP_EIO);
    }
    return rescan_disk(id);
}

/* Sets a GPT entry's type and, if given, its name, then reads the disk again. */
static int retag_partition(uint32_t disk_id, uint64_t disk_sectors, uint32_t number, uint64_t start,
                           const char *label)
{
    struct sxdisk_io io = {&disk_id, io_read, io_write, io_random};
    struct sxdisk_gpt_entry *entry;

    if (sxdisk_gpt_read(&g_table, disk_sectors, &io) != SXDISK_OK)
    {
        return io_status(SAVANXP_EIO);
    }
    entry = sxdisk_gpt_entry_at(&g_table, number);
    if (entry == 0 || entry->first_lba != start)
    {
        return SAVANXP_ENOENT;
    }
    memcpy(entry->type_guid, sxdisk_type_sxfs, SXDISK_GUID_SIZE);
    if (label != 0)
    {
        sxdisk_entry_set_name(entry, label);
    }
    if (sxdisk_gpt_write(&g_table, &io) != SXDISK_OK)
    {
        return io_status(SAVANXP_EIO);
    }
    return rescan_disk(disk_id);
}

/* The caller's real uid: the person behind an elevated copy, who should own
 * the volume it formats. */
static uint32_t real_uid(void)
{
    struct sxn_native_info info;
    struct savanxp_process_info entry;
    unsigned long index = 0;

    if (sxn_info(&info) != 0)
    {
        return 0u;
    }
    while (sxn_process_snapshot(index, &entry) == 1)
    {
        if (entry.pid == info.pid)
        {
            return entry.uid;
        }
        index += 1;
    }
    return 0u;
}

/* The user's group (docs/kernel/USERS_AND_PERMISSIONS.md, "users"). */
#define DM_USERS_GID 100u

static int format_partition(const struct sxn_storage_device *partition, const char *label)
{
    struct sxfs_ctx *ctx;
    uint32_t id = partition->id;
    uint32_t disk_id = partition->parent_id;
    uint32_t number = partition->partition_number;
    uint64_t start = partition->start_lba;
    uint64_t sectors = partition->sector_count;
    uint64_t disk_sectors;
    uint32_t uid = real_uid();
    int parent = find_device(disk_id);
    int rc;

    if (parent < 0)
    {
        return SAVANXP_ENOENT;
    }
    disk_sectors = g_devices[parent].sector_count;
    if (sectors > SXFS_MAX_TOTAL_SECTORS)
    {
        sectors = SXFS_MAX_TOTAL_SECTORS;
    }
    if (sectors <= SXFS_DATA_LBA)
    {
        return SAVANXP_ENOSPC;
    }
    ctx = (struct sxfs_ctx *)malloc(sizeof(*ctx));
    if (ctx == 0)
    {
        return SAVANXP_ENOMEM;
    }
    g_io_error = 0;
    sxfs_ctx_init(ctx, &id, sxfs_io_read, sxfs_io_write);
    rc = sxfs_mkfs(ctx, (uint32_t)sectors, uid, uid == 0u ? 0u : DM_USERS_GID, 040755);
    free(ctx);
    if (rc != SXFS_OK)
    {
        return rc == SXFS_ERR_IO ? io_status(SAVANXP_EIO) : SAVANXP_EINVAL;
    }
    return retag_partition(disk_id, disk_sectors, number, start, label);
}

static int op_create(uint32_t disk_id, const char *name, uint64_t start, uint64_t sectors, int format,
                     const char *label)
{
    const struct sxn_storage_device *disk = recheck(disk_id, name);
    struct sxdisk_io io = {&disk_id, io_read, io_write, io_random};
    struct sxdisk_extent extents[16];
    size_t count;
    size_t index;
    int inside = 0;
    int rc;

    if (disk == 0)
    {
        return SAVANXP_ENOENT;
    }
    if (!can_partition(disk))
    {
        return SAVANXP_EBUSY;
    }
    g_io_error = 0;
    if (sxdisk_gpt_read(&g_table, disk->sector_count, &io) != SXDISK_OK)
    {
        return io_status(SAVANXP_EIO);
    }
    count = sxdisk_gpt_free_extents(&g_table, extents, 16);
    for (index = 0; index < count && index < 16; ++index)
    {
        if (start >= extents[index].first_lba &&
            start + sectors <= extents[index].first_lba + extents[index].sector_count)
        {
            inside = 1;
        }
    }
    if (!inside || sectors == 0u)
    {
        return SAVANXP_ENOSPC;
    }
    rc = sxdisk_gpt_add(&g_table, start, sectors, format ? sxdisk_type_sxfs : sxdisk_type_basic_data, label, &io, 0);
    if (rc != SXDISK_OK)
    {
        return rc == SXDISK_ERR_FULL ? SAVANXP_ENOSPC : SAVANXP_EINVAL;
    }
    if (sxdisk_gpt_write(&g_table, &io) != SXDISK_OK)
    {
        return io_status(SAVANXP_EIO);
    }
    rc = rescan_disk(disk_id);
    if (rc != 0 || !format)
    {
        return rc;
    }
    (void)load_devices();
    for (index = 0; index < (size_t)g_device_count; ++index)
    {
        if (g_devices[index].kind == SXN_STORAGE_PARTITION && g_devices[index].parent_id == disk_id &&
            g_devices[index].start_lba == start)
        {
            return format_partition(&g_devices[index], 0);
        }
    }
    return SAVANXP_ENOENT;
}

static int op_format(uint32_t id, const char *name, const char *label)
{
    const struct sxn_storage_device *partition = recheck(id, name);

    if (partition == 0)
    {
        return SAVANXP_ENOENT;
    }
    if (!can_modify_partition(partition))
    {
        return SAVANXP_EBUSY;
    }
    return format_partition(partition, label);
}

static int op_delete(uint32_t id, const char *name, uint64_t start)
{
    const struct sxn_storage_device *partition = recheck(id, name);
    struct sxdisk_gpt_entry *entry;
    uint32_t disk_id;
    int parent;
    struct sxdisk_io io;

    if (partition == 0 || partition->start_lba != start)
    {
        return SAVANXP_ENOENT;
    }
    if (!can_modify_partition(partition))
    {
        return SAVANXP_EBUSY;
    }
    disk_id = partition->parent_id;
    parent = find_device(disk_id);
    io = (struct sxdisk_io){&disk_id, io_read, io_write, io_random};
    g_io_error = 0;
    if (parent < 0 || sxdisk_gpt_read(&g_table, g_devices[parent].sector_count, &io) != SXDISK_OK)
    {
        return io_status(SAVANXP_EIO);
    }
    entry = sxdisk_gpt_entry_at(&g_table, partition->partition_number);
    if (entry == 0 || entry->first_lba != start)
    {
        return SAVANXP_ENOENT;
    }
    (void)sxdisk_gpt_remove(&g_table, partition->partition_number);
    if (sxdisk_gpt_write(&g_table, &io) != SXDISK_OK)
    {
        return io_status(SAVANXP_EIO);
    }
    return rescan_disk(disk_id);
}

/* Rescan Disks: every disk with nothing mounted reads its table again. */
static int op_rescan_all(void)
{
    uint32_t ids[DM_MAX_DEVICES];
    int count = 0;
    int index;
    int failed = 0;

    (void)load_devices();
    for (index = 0; index < g_device_count; ++index)
    {
        const struct sxn_storage_device *device = &g_devices[index];

        if (device->kind == SXN_STORAGE_DISK &&
            (device->flags & (SXN_STORAGE_MOUNTED | SXN_STORAGE_CHILD_MOUNTED)) == 0u)
        {
            ids[count++] = device->id;
        }
    }
    for (index = 0; index < count; ++index)
    {
        int rc = rescan_disk(ids[index]);

        if (rc != 0)
        {
            failed = rc;
        }
    }
    return failed;
}

static int parse_u64(const char *text, uint64_t *out)
{
    uint64_t value = 0;

    if (text == 0 || *text == '\0')
    {
        return 0;
    }
    while (*text >= '0' && *text <= '9')
    {
        value = value * 10u + (uint64_t)(*text - '0');
        text += 1;
    }
    *out = value;
    return *text == '\0';
}

/* The command line of the elevated copy. Returns -1 when argv names no
 * operation. */
static int run_operation(int argc, char **argv)
{
    uint64_t id = 0;
    uint64_t start = 0;
    uint64_t sectors = 0;
    const char *label = 0;
    int format = 0;
    int index;

    if (argc < 2 || argv[1] == 0)
    {
        return -1;
    }
    for (index = 2; index + 1 < argc; ++index)
    {
        if (strcmp(argv[index], "--label") == 0)
        {
            label = argv[index + 1];
        }
    }
    for (index = 2; index < argc; ++index)
    {
        if (strcmp(argv[index], "--format") == 0)
        {
            format = 1;
        }
    }
    if (strcmp(argv[1], "--rescan") == 0)
    {
        return op_rescan_all();
    }
    if (argc < 4 || !parse_u64(argv[2], &id) || id == 0u || id > 0xffffffffull)
    {
        return strcmp(argv[1], "--init") == 0 || strcmp(argv[1], "--create") == 0 ||
                       strcmp(argv[1], "--format") == 0 || strcmp(argv[1], "--delete") == 0
                   ? SAVANXP_EINVAL
                   : -1;
    }
    if (strcmp(argv[1], "--init") == 0)
    {
        return op_init((uint32_t)id, argv[3]);
    }
    if (strcmp(argv[1], "--format") == 0)
    {
        return op_format((uint32_t)id, argv[3], label);
    }
    if (strcmp(argv[1], "--delete") == 0)
    {
        if (argc < 5 || !parse_u64(argv[4], &start))
        {
            return SAVANXP_EINVAL;
        }
        return op_delete((uint32_t)id, argv[3], start);
    }
    if (strcmp(argv[1], "--create") == 0)
    {
        if (argc < 6 || !parse_u64(argv[4], &start) || !parse_u64(argv[5], &sectors))
        {
            return SAVANXP_EINVAL;
        }
        return op_create((uint32_t)id, argv[3], start, sectors, format, label);
    }
    return -1;
}

/* ---- layout ---------------------------------------------------------------------- */

static int map_content_height(void)
{
    return g_disk_count * (DM_DISK_ROW_HEIGHT + DM_DISK_ROW_GAP);
}

static void update_map_scroll(void)
{
    int range = map_content_height() - g_map_rect.height;

    if (range < 0)
    {
        range = 0;
    }
    if (g_map_scroll > range)
    {
        g_map_scroll = range;
    }
    if (g_map_scroll < 0)
    {
        g_map_scroll = 0;
    }
    DM_SCROLL->range_min = 0;
    DM_SCROLL->range_max = range;
    DM_SCROLL->page = g_map_rect.height > 0 ? g_map_rect.height : 1;
    DM_SCROLL->value = g_map_scroll;
}

static void relayout(void)
{
    int width = (int)g_app.info.width;
    int height = (int)g_app.info.height;
    int top = sxgui_menubar_height() + SXGUI_MARGIN;
    int status_y = height - SXGUI_MARGIN - SXGUI_STATUS_HEIGHT;
    int legend_y = status_y - SXGUI_GAP - DM_LEGEND_HEIGHT;
    int list_height;
    int map_top;
    int column;
    int others = 0;

    if (width < DM_MIN_WIDTH)
    {
        width = DM_MIN_WIDTH;
    }
    /* The list takes a third, as the original's default split does. */
    list_height = (legend_y - top) / 3;
    if (list_height < 72)
    {
        list_height = 72;
    }
    DM_LIST->rect = sx_rect_make(SXGUI_MARGIN, top, width - SXGUI_MARGIN * 2, list_height);
    for (column = 1; column < DM_COLUMN_COUNT; ++column)
    {
        others += g_columns[column].width;
    }
    g_columns[0].width = DM_LIST->rect.width - (SXGUI_BORDER_SUNKEN * 2) - SXGUI_SCROLLBAR_THICKNESS - others;
    if (g_columns[0].width < 72)
    {
        g_columns[0].width = 72;
    }

    map_top = DM_LIST->rect.y + DM_LIST->rect.height + SXGUI_GAP;
    g_map_rect = sx_rect_make(SXGUI_MARGIN, map_top, width - SXGUI_MARGIN * 2 - SXGUI_SCROLLBAR_THICKNESS,
                              legend_y - SXGUI_GAP - map_top);
    DM_SCROLL->rect = sx_rect_make(g_map_rect.x + g_map_rect.width, g_map_rect.y, SXGUI_SCROLLBAR_THICKNESS,
                                   g_map_rect.height);
    g_legend_rect = sx_rect_make(SXGUI_MARGIN, legend_y, width - SXGUI_MARGIN * 2, DM_LEGEND_HEIGHT);

    DM_STATUS_RIGHT->rect = sx_rect_make(width - SXGUI_MARGIN - 160, status_y, 160, SXGUI_STATUS_HEIGHT);
    DM_STATUS->rect = sx_rect_make(SXGUI_MARGIN, status_y, DM_STATUS_RIGHT->rect.x - SXGUI_GAP - SXGUI_MARGIN,
                                   SXGUI_STATUS_HEIGHT);
    update_map_scroll();
}

/* Segment widths: each one gets DM_SEGMENT_MIN_WIDTH, and what is left is
 * shared by size, so a 1 MiB partition stays clickable next to a 1 TiB one. */
static void layout_disk(struct dm_disk *disk, struct sx_rect row)
{
    const struct sxn_storage_device *device = &g_devices[disk->device];
    struct sx_rect bars;
    int fixed;
    int spare;
    int x;
    int index;
    uint64_t total = device->sector_count != 0u ? device->sector_count : 1u;
    uint64_t shown = 0;

    disk->rect = row;
    disk->label_rect = sx_rect_make(row.x, row.y, DM_LABEL_WIDTH, row.height);
    bars = sx_rect_make(row.x + DM_LABEL_WIDTH, row.y, row.width - DM_LABEL_WIDTH, row.height);
    for (index = 0; index < disk->segment_count; ++index)
    {
        shown += disk->segments[index].sectors;
    }
    if (shown > total)
    {
        total = shown;
    }
    fixed = disk->segment_count * DM_SEGMENT_MIN_WIDTH;
    spare = bars.width - fixed;
    if (spare < 0)
    {
        spare = 0;
    }
    x = bars.x;
    for (index = 0; index < disk->segment_count; ++index)
    {
        struct dm_segment *segment = &disk->segments[index];
        int width = DM_SEGMENT_MIN_WIDTH + (int)(((uint64_t)spare * segment->sectors) / total);

        if (index == disk->segment_count - 1)
        {
            width = bars.x + bars.width - x;
        }
        segment->rect = sx_rect_make(x, bars.y, width, bars.height);
        x += width;
    }
}

static void layout_map(void)
{
    int index;
    int y = g_map_rect.y - g_map_scroll;

    for (index = 0; index < g_disk_count; ++index)
    {
        layout_disk(&g_disks[index], sx_rect_make(g_map_rect.x, y, g_map_rect.width, DM_DISK_ROW_HEIGHT));
        y += DM_DISK_ROW_HEIGHT + DM_DISK_ROW_GAP;
    }
}

/* ---- painting ------------------------------------------------------------------- */

static void draw_lines(struct sx_painter *painter, struct sx_rect area, const char *const *lines, int count,
                       uint32_t colour)
{
    int line;
    int y = area.y;
    int step = gfx_text_height() + 1;

    for (line = 0; line < count; ++line)
    {
        if (lines[line] == 0 || lines[line][0] == '\0')
        {
            continue;
        }
        if (y + step > area.y + area.height)
        {
            break;
        }
        sx_painter_draw_text(painter, area.x, y, lines[line], colour);
        y += step;
    }
}

static void paint_disk_label(struct sx_painter *painter, const struct dm_disk *disk)
{
    const struct sxn_storage_device *device = &g_devices[disk->device];
    struct sx_rect rect = disk->label_rect;
    char title[32];
    char size[24];
    const char *kind = "Basic";
    const char *state = "Online";
    const char *lines[4];
    int selected = g_select_kind == DM_SELECT_DISK && g_select_id == device->id;

    if (device->kind == SXN_STORAGE_MEMORY)
    {
        snprintf(title, sizeof(title), "LiveCD %d", disk->number);
        kind = "RAM";
    }
    else
    {
        snprintf(title, sizeof(title), "Disk %d", disk->number);
        if (device->scheme == SXN_STORAGE_SCHEME_NONE && device->fs_name[0] == '\0')
        {
            kind = "Unknown";
            state = "Not Initialized";
        }
        else if (device->scheme == SXN_STORAGE_SCHEME_DAMAGED)
        {
            state = "Unreadable";
        }
    }
    format_bytes(sectors_to_bytes(device->sector_count), size, sizeof(size));

    sxchrome_fill_raised(painter, rect, SXGUI_COLOR_FACE, 0);
    if (selected)
    {
        struct sx_brush brush = sx_brush_pattern_transparent(sx_pattern_checker_50, SXGUI_COLOR_SHADOW);
        sx_painter_fill_rect_brush(painter, sx_rect_make(rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4),
                                   &brush);
    }
    lines[0] = title;
    lines[1] = kind;
    lines[2] = size;
    lines[3] = state;
    sx_painter_set_font(painter, SX_FONT_UI_TITLE);
    sx_painter_draw_text(painter, rect.x + 6, rect.y + 4, title, SXGUI_COLOR_TEXT);
    sx_painter_set_font(painter, SX_FONT_UI);
    draw_lines(painter, sx_rect_make(rect.x + 6, rect.y + 4 + gfx_text_height() + 1, rect.width - 8,
                                     rect.height - 8 - gfx_text_height()),
               lines + 1, 3, SXGUI_COLOR_TEXT);
}

static int segment_selected(const struct dm_disk *disk, const struct dm_segment *segment)
{
    if (segment->device >= 0 && segment->kind != DM_SEG_UNREADABLE)
    {
        return g_select_kind == DM_SELECT_DEVICE && g_devices[segment->device].id == g_select_id;
    }
    return g_select_kind == DM_SELECT_FREE && g_devices[disk->device].id == g_select_id &&
        segment->start == g_select_start;
}

static void paint_segment(struct sx_painter *painter, const struct dm_disk *disk, const struct dm_segment *segment)
{
    struct sx_rect rect = segment->rect;
    struct sx_rect body = sx_rect_make(rect.x + 1, rect.y + DM_SEGMENT_HEADER, rect.width - 2,
                                       rect.height - DM_SEGMENT_HEADER - 1);
    uint32_t header = DM_COLOR_PRIMARY;
    char name[48] = "";
    char size[48] = "";
    char status[40] = "";
    const char *lines[3];

    switch (segment->kind)
    {
    case DM_SEG_PARTITION:
    case DM_SEG_VOLUME:
    {
        const struct sxn_storage_device *device = &g_devices[segment->device];
        char bytes[24];
        const char *fs = fs_display_name(device);

        header = segment->kind == DM_SEG_VOLUME ? DM_COLOR_VOLUME : DM_COLOR_PRIMARY;
        volume_name(device, name, sizeof(name));
        format_bytes(sectors_to_bytes(segment->sectors), bytes, sizeof(bytes));
        snprintf(size, sizeof(size), "%s%s%s", bytes, fs[0] != '\0' ? " " : "", fs);
        volume_status(device, status, sizeof(status));
        break;
    }
    case DM_SEG_FREE:
        header = DM_COLOR_FREE;
        format_bytes(sectors_to_bytes(segment->sectors), size, sizeof(size));
        snprintf(status, sizeof(status), "%s", "Free space");
        break;
    case DM_SEG_UNREADABLE:
        header = DM_COLOR_UNREADABLE;
        format_bytes(sectors_to_bytes(segment->sectors), size, sizeof(size));
        snprintf(status, sizeof(status), "%s", "Damaged partition table");
        break;
    default:
        header = DM_COLOR_UNALLOCATED;
        format_bytes(sectors_to_bytes(segment->sectors), size, sizeof(size));
        snprintf(status, sizeof(status), "%s", "Unallocated");
        break;
    }

    sx_painter_fill_rect(painter, rect, SXGUI_COLOR_FIELD);
    sx_painter_draw_frame(painter, rect, SXGUI_COLOR_SHADOW);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, DM_SEGMENT_HEADER - 2),
                         header);
    if (segment_selected(disk, segment))
    {
        /* The original marks the selection with a hatch over the segment. */
        struct sx_brush brush = sx_brush_pattern_transparent(sx_pattern_checker_50, SXGUI_COLOR_SHADOW);
        sx_painter_fill_rect_brush(painter, body, &brush);
        sx_painter_draw_frame(painter, sx_rect_make(body.x + 1, body.y + 1, body.width - 2, body.height - 2),
                              DM_COLOR_HATCH);
    }
    if (!sx_painter_push_clip(painter, sx_rect_make(body.x + 4, body.y + 2, body.width - 8, body.height - 4)))
    {
        return;
    }
    lines[0] = name;
    lines[1] = size;
    lines[2] = status;
    draw_lines(painter, sx_rect_make(body.x + 4, body.y + 3, body.width - 8, body.height - 4), lines, 3,
               SXGUI_COLOR_TEXT);
    sx_painter_pop_clip(painter);
}

static void paint_legend(struct sx_painter *painter)
{
    static const struct {
        uint32_t colour;
        const char *text;
    } entries[] = {
        {DM_COLOR_UNALLOCATED, "Unallocated"},
        {DM_COLOR_PRIMARY, "Primary partition"},
        {DM_COLOR_FREE, "Free space"},
        {DM_COLOR_VOLUME, "Whole-disk volume"},
    };
    int x = g_legend_rect.x + 4;
    int y = g_legend_rect.y + (g_legend_rect.height - 10) / 2;
    size_t index;

    sxchrome_draw_sunken(painter, g_legend_rect);
    for (index = 0; index < sizeof(entries) / sizeof(entries[0]); ++index)
    {
        struct sx_rect swatch = sx_rect_make(x, y, 10, 10);

        sx_painter_fill_rect(painter, swatch, entries[index].colour);
        sx_painter_draw_frame(painter, swatch, SXGUI_COLOR_SHADOW);
        sx_painter_draw_text(painter, x + 14, g_legend_rect.y + (g_legend_rect.height - gfx_text_height()) / 2,
                             entries[index].text, SXGUI_COLOR_TEXT);
        x += 14 + gfx_text_width(entries[index].text) + 16;
    }
}

static void on_paint(struct sxngui_app *app)
{
    struct sx_painter *painter = &app->ui.painter;
    int index;

    layout_map();
    sx_painter_fill_rect(painter, g_map_rect, SXGUI_COLOR_WINDOW);
    if (sx_painter_push_clip(painter, g_map_rect))
    {
        for (index = 0; index < g_disk_count; ++index)
        {
            const struct dm_disk *disk = &g_disks[index];
            int segment;

            if (disk->rect.y + disk->rect.height < g_map_rect.y || disk->rect.y > g_map_rect.y + g_map_rect.height)
            {
                continue;
            }
            paint_disk_label(painter, disk);
            for (segment = 0; segment < disk->segment_count; ++segment)
            {
                paint_segment(painter, disk, &disk->segments[segment]);
            }
        }
        if (g_disk_count == 0)
        {
            sx_painter_draw_text(painter, g_map_rect.x + 8, g_map_rect.y + 8, "No disks were found.",
                                 SXGUI_COLOR_TEXT);
        }
        sx_painter_pop_clip(painter);
    }
    paint_legend(painter);
}

/* ---- dialogs ---------------------------------------------------------------------- */

static void on_dialog_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
}

static void show_message(const char *title, const char *line, const char *detail)
{
    snprintf(g_message_line, sizeof(g_message_line), "%s", line);
    snprintf(g_message_detail, sizeof(g_message_detail), "%s", detail != 0 ? detail : "");
    memset(&g_message_dialog, 0, sizeof(g_message_dialog));
    g_message_dialog.title = title;
    g_message_dialog.widgets = g_message_widgets;
    g_message_dialog.widget_count = 3;
    g_message_dialog.initial_focus = 2;
    g_message_dialog.default_button = 2;
    sxgui_dialog_begin(&g_app.ui, &g_message_dialog, DM_MESSAGE_WIDTH, DM_MESSAGE_HEIGHT);
}

static int g_property_line;

static void property(const char *label, const char *value)
{
    if (g_property_line >= DM_PROPERTIES_LINES)
    {
        return;
    }
    snprintf(g_properties_names[g_property_line], sizeof(g_properties_names[0]), "%s", label);
    snprintf(g_properties_lines[g_property_line], sizeof(g_properties_lines[0]), "%s", value);
    g_property_line += 1;
}

static void show_properties(void)
{
    const struct sxn_storage_device *device = selected_device();
    char text[64];
    char guid[40];
    int index;

    if (device == 0)
    {
        return;
    }
    g_property_line = 0;
    memset(g_properties_lines, 0, sizeof(g_properties_lines));
    memset(g_properties_names, 0, sizeof(g_properties_names));

    if (device->kind == SXN_STORAGE_PARTITION)
    {
        int parent = find_device(device->parent_id);

        volume_name(device, text, sizeof(text));
        snprintf(g_properties_title, sizeof(g_properties_title), "%s Properties", text);
        property("Device:", device->name);
        property("Label:", device->label[0] != '\0' ? device->label : "(none)");
        property("Type:", partition_type_name(device));
        property("File system:", fs_display_name(device)[0] != '\0' ? fs_display_name(device) : "Unknown");
        format_bytes(sectors_to_bytes(device->sector_count), text, sizeof(text));
        property("Capacity:", text);
        snprintf(text, sizeof(text), "%lu, partition %u", (unsigned long)device->start_lba,
                 (unsigned)device->partition_number);
        property("Start sector:", text);
        if (device->scheme == SXN_STORAGE_SCHEME_GPT)
        {
            sxdisk_guid_format(device->unique_guid, guid);
            property("Partition GUID:", guid);
        }
        property("Disk:", parent >= 0 ? g_devices[parent].model : "?");
    }
    else
    {
        snprintf(g_properties_title, sizeof(g_properties_title), "%s Properties", device->name);
        property("Device:", device->name);
        property("Model:", device->model[0] != '\0' ? device->model : "Unknown");
        property("Serial:", device->serial[0] != '\0' ? device->serial : "Unknown");
        format_bytes(sectors_to_bytes(device->sector_count), text, sizeof(text));
        property("Capacity:", text);
        if ((device->flags & SXN_STORAGE_TRUNCATED) != 0u)
        {
            format_bytes(sectors_to_bytes(device->device_sectors), text, sizeof(text));
            property("Real size:", text);
        }
        property("Partition style:",
                 device->scheme == SXN_STORAGE_SCHEME_GPT ? "GUID Partition Table (GPT)" :
                 device->scheme == SXN_STORAGE_SCHEME_MBR ? "Master Boot Record (MBR)" :
                 device->scheme == SXN_STORAGE_SCHEME_DAMAGED ? "Damaged GPT" : "None");
        if (device->scheme == SXN_STORAGE_SCHEME_GPT)
        {
            sxdisk_guid_format(device->unique_guid, guid);
            property("Disk GUID:", guid);
        }
        if (device->fs_name[0] != '\0' && device->scheme == SXN_STORAGE_SCHEME_NONE)
        {
            property("File system:", fs_display_name(device));
        }
    }
    if ((device->flags & SXN_STORAGE_MOUNTED) != 0u)
    {
        property("Mounted at:", device->mount_point);
        format_bytes(device->used_bytes, text, sizeof(text));
        property("Used space:", text);
        format_bytes(device->free_bytes, text, sizeof(text));
        property("Free space:", text);
        snprintf(text, sizeof(text), "%u", (unsigned)device->file_count);
        property("Files:", text);
    }

    for (index = 0; index < DM_PROPERTIES_LINES; ++index)
    {
        g_properties_widgets[index * 2] = sxgui_label(
            sx_rect_make(DM_DIALOG_MARGIN, DM_DIALOG_MARGIN + index * DM_TEXT_ROW,
                         DM_PROPERTIES_VALUE_X - SXGUI_GAP, DM_TEXT_ROW),
            g_properties_names[index]);
        g_properties_widgets[index * 2 + 1] = sxgui_label(
            sx_rect_make(DM_DIALOG_MARGIN + DM_PROPERTIES_VALUE_X, DM_DIALOG_MARGIN + index * DM_TEXT_ROW,
                         DM_PROPERTIES_WIDTH - DM_DIALOG_MARGIN * 2 - DM_PROPERTIES_VALUE_X, DM_TEXT_ROW),
            g_properties_lines[index]);
    }
    g_properties_widgets[DM_PROPERTIES_LINES * 2] = sxgui_button(
        sx_rect_make(centred_button_x(DM_PROPERTIES_WIDTH, 1, 0),
                     DM_PROPERTIES_HEIGHT - DM_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT, SXGUI_BUTTON_WIDTH,
                     SXGUI_BUTTON_HEIGHT),
        "OK", on_dialog_ok, 0);
    memset(&g_properties_dialog, 0, sizeof(g_properties_dialog));
    g_properties_dialog.title = g_properties_title;
    g_properties_dialog.widgets = g_properties_widgets;
    g_properties_dialog.widget_count = DM_PROPERTIES_LINES * 2 + 1;
    g_properties_dialog.initial_focus = DM_PROPERTIES_LINES * 2;
    g_properties_dialog.default_button = DM_PROPERTIES_LINES * 2;
    sxgui_dialog_begin(&g_app.ui, &g_properties_dialog, DM_PROPERTIES_WIDTH, DM_PROPERTIES_HEIGHT);
}

/* ---- commands --------------------------------------------------------------------- */

#define DM_PATH "/bin/diskmgmt.sxe"

static int acts_as_system(void)
{
    struct sxn_native_info info;
    struct savanxp_process_info entry;
    unsigned long index = 0;

    if (sxn_info(&info) != 0)
    {
        return 0;
    }
    while (sxn_process_snapshot(index, &entry) == 1)
    {
        if (entry.pid == info.pid)
        {
            return (entry.flags & SAVANXP_PROC_FLAG_SYSTEM) != 0u;
        }
        index += 1;
    }
    return 0;
}

/* Runs an operation as the system: in this process if it already is, else in
 * an elevated copy, which asks the user first. 0, a positive errno, or -1 when
 * the user answered No. */
static int perform(const char **argv, int argc)
{
    int status = -1;
    long result;

    if (acts_as_system())
    {
        return run_operation(argc, (char **)argv);
    }
    result = sxn_elevate(DM_PATH, argv, argc, &status);
    if (result == -SAVANXP_EACCES)
    {
        return -1;
    }
    if (result < 0)
    {
        return SAVANXP_EPERM;
    }
    return status;
}

static const char *error_text(int code)
{
    switch (code)
    {
    case SAVANXP_EBUSY:
        return "The disk or volume is in use, or holds the system.";
    case SAVANXP_ENOENT:
        return "The disk changed since it was shown. Refresh and try again.";
    case SAVANXP_EPERM:
    case SAVANXP_EACCES:
        return "Access is denied.";
    case SAVANXP_ENOSPC:
        return "There is not enough free space for it.";
    case SAVANXP_EIO:
        return "The disk could not be read or written.";
    case SAVANXP_ENOMEM:
        return "There is not enough memory.";
    default:
        return "The disk could not be changed.";
    }
}

static void report(const char *title, int code)
{
    reload();
    if (code > 0)
    {
        show_message(title, "The operation could not be completed.", error_text(code));
    }
}

static const struct dm_disk *disk_of_device(uint32_t id)
{
    int index;

    for (index = 0; index < g_disk_count; ++index)
    {
        if (g_devices[g_disks[index].device].id == id)
        {
            return &g_disks[index];
        }
    }
    return 0;
}

/* The selected free range and its disk, or 0. */
static const struct dm_segment *selected_free(const struct sxn_storage_device **disk_out)
{
    const struct dm_disk *disk;
    int segment;

    if (g_select_kind != DM_SELECT_FREE || (disk = disk_of_device(g_select_id)) == 0)
    {
        return 0;
    }
    for (segment = 0; segment < disk->segment_count; ++segment)
    {
        const struct dm_segment *item = &disk->segments[segment];

        if ((item->kind == DM_SEG_FREE || item->kind == DM_SEG_UNALLOCATED) && item->start == g_select_start)
        {
            *disk_out = &g_devices[disk->device];
            return item;
        }
    }
    return 0;
}

/* The disk Initialize would act on: the selected disk box, or an
 * unallocated disk's segment. */
static const struct sxn_storage_device *initialize_target(void)
{
    const struct sxn_storage_device *disk = 0;

    if (g_select_kind == DM_SELECT_DISK)
    {
        disk = selected_device();
    }
    else if (selected_free(&disk) == 0)
    {
        disk = 0;
    }
    return can_initialize(disk) ? disk : 0;
}

/* The free range New Partition would use: the selected one, or, with a disk's
 * box selected, the first free range of that disk. */
static const struct dm_segment *new_partition_target(const struct sxn_storage_device **disk)
{
    const struct dm_segment *segment = 0;

    if (g_select_kind == DM_SELECT_DISK)
    {
        const struct dm_disk *row = disk_of_device(g_select_id);
        int index;

        for (index = 0; row != 0 && index < row->segment_count; ++index)
        {
            if (row->segments[index].kind == DM_SEG_FREE)
            {
                segment = &row->segments[index];
                *disk = &g_devices[row->device];
                break;
            }
        }
    }
    else
    {
        segment = selected_free(disk);
    }
    return segment != 0 && segment->kind == DM_SEG_FREE && can_partition(*disk) ? segment : 0;
}

static const struct sxn_storage_device *partition_target(void)
{
    const struct sxn_storage_device *device = g_select_kind == DM_SELECT_DEVICE ? selected_device() : 0;

    return can_modify_partition(device) ? device : 0;
}

static void set_item(struct sxgui_menu_item *item, int enabled)
{
    item->flags = enabled ? 0u : SXGUI_MENU_DISABLED;
}

static void update_menus(void)
{
    const struct sxn_storage_device *disk = 0;

    set_item(&k_action_items[DM_ITEM_INITIALIZE], initialize_target() != 0);
    set_item(&k_action_items[DM_ITEM_NEW_PARTITION], new_partition_target(&disk) != 0);
    set_item(&k_action_items[DM_ITEM_FORMAT], partition_target() != 0);
    set_item(&k_action_items[DM_ITEM_DELETE], partition_target() != 0);
    set_item(&k_action_items[DM_ITEM_PROPERTIES], selected_device() != 0);
}

static void disk_title(const struct sxn_storage_device *disk, char *out, size_t capacity)
{
    const struct dm_disk *row = disk_of_device(disk->id);
    char size[24];

    format_bytes(sectors_to_bytes(disk->sector_count), size, sizeof(size));
    snprintf(out, capacity, "Disk %d (%s, %s)", row != 0 ? row->number : 0,
             disk->model[0] != '\0' ? disk->model : disk->name, size);
}

static void begin_confirm(const char *title, const char *line, const char *detail)
{
    snprintf(g_confirm_line, sizeof(g_confirm_line), "%s", line);
    snprintf(g_confirm_detail, sizeof(g_confirm_detail), "%s", detail);
    memset(&g_confirm_dialog, 0, sizeof(g_confirm_dialog));
    g_confirm_dialog.title = title;
    g_confirm_dialog.widgets = g_confirm_widgets;
    g_confirm_dialog.widget_count = 4;
    g_confirm_dialog.initial_focus = 3;
    g_confirm_dialog.default_button = 3;
    sxgui_dialog_begin(&g_app.ui, &g_confirm_dialog, DM_CONFIRM_WIDTH, DM_CONFIRM_HEIGHT);
}

static void begin_initialize(void)
{
    const struct sxn_storage_device *disk = initialize_target();
    char title[96];
    char line[128];

    if (disk == 0)
    {
        return;
    }
    g_pending = DM_PENDING_INITIALIZE;
    g_pending_id = disk->id;
    snprintf(g_pending_name, sizeof(g_pending_name), "%s", disk->name);
    disk_title(disk, title, sizeof(title));
    snprintf(line, sizeof(line), "Initialize %s with a GPT partition table?", title);
    begin_confirm("Initialize Disk", line, "Its partitions can then be created here.");
}

static void begin_delete(void)
{
    const struct sxn_storage_device *partition = partition_target();
    char name[48];
    char size[24];
    char line[128];

    if (partition == 0)
    {
        return;
    }
    g_pending = DM_PENDING_DELETE;
    g_pending_id = partition->id;
    g_pending_start = partition->start_lba;
    snprintf(g_pending_name, sizeof(g_pending_name), "%s", partition->name);
    volume_name(partition, name, sizeof(name));
    format_bytes(sectors_to_bytes(partition->sector_count), size, sizeof(size));
    snprintf(line, sizeof(line), "Delete the partition %s (%s, %s)?", name, partition->name, size);
    begin_confirm("Delete Partition", line, "All data on it will be lost.");
}

static void on_confirm_no(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    g_pending = DM_PENDING_NONE;
    sxgui_dialog_end(&g_app.ui, 0);
}

static void on_confirm_yes(struct sxgui_widget *widget, void *user)
{
    char id_text[16];
    char start_text[24];
    const char *argv[5];
    int pending = g_pending;

    (void)widget;
    (void)user;
    g_pending = DM_PENDING_NONE;
    sxgui_dialog_end(&g_app.ui, 1);
    snprintf(id_text, sizeof(id_text), "%u", (unsigned)g_pending_id);
    snprintf(start_text, sizeof(start_text), "%lu", (unsigned long)g_pending_start);
    argv[0] = "diskmgmt";
    argv[2] = id_text;
    argv[3] = g_pending_name;
    if (pending == DM_PENDING_INITIALIZE)
    {
        argv[1] = "--init";
        report("Initialize Disk", perform(argv, 4));
    }
    else if (pending == DM_PENDING_DELETE)
    {
        argv[1] = "--delete";
        argv[4] = start_text;
        report("Delete Partition", perform(argv, 5));
    }
}

static void begin_new_partition(void)
{
    const struct sxn_storage_device *disk = 0;
    const struct dm_segment *segment = new_partition_target(&disk);
    char size[24];
    uint64_t megabytes;

    if (segment == 0)
    {
        return;
    }
    g_pending_id = disk->id;
    g_pending_start = segment->start;
    g_pending_sectors = segment->sectors;
    snprintf(g_pending_name, sizeof(g_pending_name), "%s", disk->name);
    megabytes = segment->sectors / SXDISK_ALIGN_SECTORS;
    snprintf(g_new_size, sizeof(g_new_size), "%lu", (unsigned long)megabytes);
    snprintf(g_new_label, sizeof(g_new_label), "%s", "New Volume");
    format_bytes(sectors_to_bytes(segment->sectors), size, sizeof(size));
    snprintf(g_new_note, sizeof(g_new_note), "Free space: %lu MB. SxFS uses at most %u MB of a partition.",
             (unsigned long)megabytes, (unsigned)(SXFS_MAX_TOTAL_SECTORS / SXDISK_ALIGN_SECTORS));
    g_new_widgets[1].caret = (int)strlen(g_new_size);
    g_new_widgets[3].caret = (int)strlen(g_new_label);
    g_new_widgets[4].value = 1;
    memset(&g_new_dialog, 0, sizeof(g_new_dialog));
    g_new_dialog.title = "New Partition";
    g_new_dialog.widgets = g_new_widgets;
    g_new_dialog.widget_count = 8;
    g_new_dialog.initial_focus = 1;
    g_new_dialog.default_button = 6;
    sxgui_dialog_begin(&g_app.ui, &g_new_dialog, DM_FORM_WIDTH, DM_FORM_HEIGHT);
}

static void on_form_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 0);
}

static void on_new_ok(struct sxgui_widget *widget, void *user)
{
    char id_text[16];
    char start_text[24];
    char sectors_text[24];
    const char *argv[9];
    int argc = 6;
    uint64_t megabytes = 0;
    uint64_t sectors;

    (void)widget;
    (void)user;
    if (!parse_u64(g_new_size, &megabytes) || megabytes == 0u)
    {
        return;
    }
    sxgui_dialog_end(&g_app.ui, 1);
    sectors = megabytes * SXDISK_ALIGN_SECTORS;
    if (sectors > g_pending_sectors)
    {
        sectors = g_pending_sectors;
    }
    snprintf(id_text, sizeof(id_text), "%u", (unsigned)g_pending_id);
    snprintf(start_text, sizeof(start_text), "%lu", (unsigned long)g_pending_start);
    snprintf(sectors_text, sizeof(sectors_text), "%lu", (unsigned long)sectors);
    argv[0] = "diskmgmt";
    argv[1] = "--create";
    argv[2] = id_text;
    argv[3] = g_pending_name;
    argv[4] = start_text;
    argv[5] = sectors_text;
    if (g_new_widgets[4].value != 0)
    {
        argv[argc++] = "--format";
    }
    if (g_new_label[0] != '\0')
    {
        argv[argc++] = "--label";
        argv[argc++] = g_new_label;
    }
    report("New Partition", perform(argv, argc));
}

static void begin_format(void)
{
    const struct sxn_storage_device *partition = partition_target();

    if (partition == 0)
    {
        return;
    }
    g_pending_id = partition->id;
    snprintf(g_pending_name, sizeof(g_pending_name), "%s", partition->name);
    snprintf(g_format_label, sizeof(g_format_label), "%s",
             partition->label[0] != '\0' ? partition->label : "New Volume");
    if (partition->sector_count > SXFS_MAX_TOTAL_SECTORS)
    {
        char size[24];

        format_bytes(sectors_to_bytes(partition->sector_count), size, sizeof(size));
        snprintf(g_format_note, sizeof(g_format_note), "SxFS uses its first %u MB of %s.",
                 (unsigned)(SXFS_MAX_TOTAL_SECTORS / SXDISK_ALIGN_SECTORS), size);
    }
    else
    {
        snprintf(g_format_note, sizeof(g_format_note), "%s", "File system: SxFS.");
    }
    g_format_widgets[1].caret = (int)strlen(g_format_label);
    memset(&g_format_dialog, 0, sizeof(g_format_dialog));
    g_format_dialog.title = "Format";
    g_format_dialog.widgets = g_format_widgets;
    g_format_dialog.widget_count = 6;
    g_format_dialog.initial_focus = 1;
    g_format_dialog.default_button = 5;
    sxgui_dialog_begin(&g_app.ui, &g_format_dialog, DM_FORM_WIDTH, DM_FORM_HEIGHT);
}

static void on_format_ok(struct sxgui_widget *widget, void *user)
{
    char id_text[16];
    const char *argv[6];
    int argc = 4;

    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
    snprintf(id_text, sizeof(id_text), "%u", (unsigned)g_pending_id);
    argv[0] = "diskmgmt";
    argv[1] = "--format";
    argv[2] = id_text;
    argv[3] = g_pending_name;
    if (g_format_label[0] != '\0')
    {
        argv[argc++] = "--label";
        argv[argc++] = g_format_label;
    }
    report("Format", perform(argv, argc));
}

static void on_menu_command(int id, void *user)
{
    const char *argv[2] = {"diskmgmt", "--rescan"};

    (void)user;
    switch (id)
    {
    case DM_CMD_EXIT:
        sxngui_app_quit(&g_app, 0);
        break;
    case DM_CMD_REFRESH:
        reload();
        break;
    case DM_CMD_RESCAN:
        report("Rescan Disks", perform(argv, 2));
        break;
    case DM_CMD_INITIALIZE:
        begin_initialize();
        break;
    case DM_CMD_NEW_PARTITION:
        begin_new_partition();
        break;
    case DM_CMD_FORMAT:
        begin_format();
        break;
    case DM_CMD_DELETE:
        begin_delete();
        break;
    case DM_CMD_PROPERTIES:
        show_properties();
        break;
    case DM_CMD_ABOUT:
        show_message("About Disk Management", "SavanXP Disk Management",
                     "Disks, partitions and volumes.");
        break;
    default:
        break;
    }
    update_menus();
}

static void add_context_item(int *count, const char *text, int id, int enabled)
{
    g_context_items[*count] = (struct sxgui_menu_item){text, id, enabled ? 0u : SXGUI_MENU_DISABLED};
    *count += 1;
}

/* The context menu for what is selected now, as the original builds one per
 * kind of object. */
static int build_context_menu(void)
{
    const struct sxn_storage_device *disk = 0;
    int count = 0;

    if (g_select_kind == DM_SELECT_DISK)
    {
        add_context_item(&count, "Initialize Disk...", DM_CMD_INITIALIZE, initialize_target() != 0);
        add_context_item(&count, "New Partition...", DM_CMD_NEW_PARTITION, new_partition_target(&disk) != 0);
        add_context_item(&count, "Rescan Disks", DM_CMD_RESCAN, 1);
        g_context_items[count++] = (struct sxgui_menu_item){0, 0, 0};
        add_context_item(&count, "Properties", DM_CMD_PROPERTIES, 1);
    }
    else if (g_select_kind == DM_SELECT_FREE)
    {
        if (selected_free(&disk) != 0 && disk->scheme == SXN_STORAGE_SCHEME_NONE)
        {
            add_context_item(&count, "Initialize Disk...", DM_CMD_INITIALIZE, initialize_target() != 0);
        }
        else
        {
            add_context_item(&count, "New Partition...", DM_CMD_NEW_PARTITION, new_partition_target(&disk) != 0);
        }
        add_context_item(&count, "Refresh", DM_CMD_REFRESH, 1);
    }
    else if (g_select_kind == DM_SELECT_DEVICE)
    {
        int modifiable = partition_target() != 0;

        add_context_item(&count, "Format...", DM_CMD_FORMAT, modifiable);
        add_context_item(&count, "Delete Partition...", DM_CMD_DELETE, modifiable);
        g_context_items[count++] = (struct sxgui_menu_item){0, 0, 0};
        add_context_item(&count, "Properties", DM_CMD_PROPERTIES, 1);
    }
    else
    {
        add_context_item(&count, "Refresh", DM_CMD_REFRESH, 1);
    }
    g_context_menu.item_count = count;
    return count;
}

/* ---- input ------------------------------------------------------------------------- */

static void on_list(struct sxgui_widget *widget, void *user)
{
    (void)user;
    if (widget->value >= 0 && widget->value < g_row_count)
    {
        select_device(g_devices[g_row_device[widget->value]].id, DM_SELECT_DEVICE);
        if (widget->action == SXGUI_ACTION_ACTIVATE)
        {
            show_properties();
        }
    }
    update_menus();
}

static void on_scroll(struct sxgui_widget *widget, void *user)
{
    (void)user;
    g_map_scroll = widget->value;
    sxngui_app_request_repaint(&g_app);
}

/* Selects what is under (x, y) on the map. 0 if nothing is. */
static int hit_map(int x, int y)
{
    int index;

    if (!sx_rect_contains_point(g_map_rect, x, y))
    {
        return 0;
    }
    layout_map();
    for (index = 0; index < g_disk_count; ++index)
    {
        const struct dm_disk *disk = &g_disks[index];
        int segment;

        if (sx_rect_contains_point(disk->label_rect, x, y))
        {
            select_device(g_devices[disk->device].id, DM_SELECT_DISK);
            return 1;
        }
        for (segment = 0; segment < disk->segment_count; ++segment)
        {
            const struct dm_segment *item = &disk->segments[segment];

            if (!sx_rect_contains_point(item->rect, x, y))
            {
                continue;
            }
            if (item->kind == DM_SEG_PARTITION || item->kind == DM_SEG_VOLUME)
            {
                select_device(g_devices[item->device].id, DM_SELECT_DEVICE);
            }
            else if (item->kind == DM_SEG_UNREADABLE)
            {
                select_device(g_devices[disk->device].id, DM_SELECT_DISK);
            }
            else
            {
                select_free(g_devices[disk->device].id, item->start);
            }
            return 1;
        }
    }
    return 0;
}

static int on_pointer(struct sxngui_app *app, const struct savanxp_gui_pointer_event *event)
{
    uint32_t pressed = event->buttons & ~g_pointer_buttons;
    uint32_t released = g_pointer_buttons & ~event->buttons;
    int row;

    g_pointer_buttons = event->buttons;
    if (sxgui_dialog_active(&app->ui))
    {
        return 0;
    }
    if (event->wheel != 0 && sx_rect_contains_point(g_map_rect, event->x, event->y))
    {
        g_map_scroll -= event->wheel * (DM_DISK_ROW_HEIGHT / 2);
        update_map_scroll();
        sxngui_app_request_repaint(app);
        return 1;
    }
    if ((pressed & (SAVANXP_MOUSE_BUTTON_LEFT | SAVANXP_MOUSE_BUTTON_RIGHT)) != 0u)
    {
        if (hit_map(event->x, event->y))
        {
            update_menus();
            return 1;
        }
    }
    if ((released & SAVANXP_MOUSE_BUTTON_RIGHT) == 0u)
    {
        return 0;
    }
    if (sx_rect_contains_point(g_map_rect, event->x, event->y))
    {
        if (g_select_kind != DM_SELECT_NONE && build_context_menu() > 0)
        {
            sxgui_context_menu_open(&app->ui, &g_context_menu, event->x, event->y, on_menu_command, 0);
        }
        return 1;
    }
    row = sxgui_listbox_row_at(DM_LIST, event->x, event->y);
    if (row < 0)
    {
        return 0;
    }
    DM_LIST->value = row;
    select_device(g_devices[g_row_device[row]].id, DM_SELECT_DEVICE);
    update_menus();
    if (build_context_menu() > 0)
    {
        sxgui_context_menu_open(&app->ui, &g_context_menu, event->x, event->y, on_menu_command, 0);
    }
    return 1;
}

static int on_key(struct sxngui_app *app, const struct savanxp_input_event *event)
{
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || sxgui_dialog_active(&app->ui))
    {
        return 0;
    }
    if (event->key == SAVANXP_KEY_F5)
    {
        reload();
        return 1;
    }
    if (event->key == SAVANXP_KEY_DELETE)
    {
        begin_delete();
        return 1;
    }
    if (event->key == SAVANXP_KEY_ENTER && (event->modifiers & SAVANXP_KEY_MOD_ALT) != 0u)
    {
        show_properties();
        return 1;
    }
    if (event->key == SAVANXP_KEY_MENU ||
        (event->key == SAVANXP_KEY_F10 && (event->modifiers & SAVANXP_KEY_MOD_SHIFT) != 0u))
    {
        if (build_context_menu() > 0)
        {
            sxgui_context_menu_open(&app->ui, &g_context_menu, DM_LIST->rect.x + 24, DM_LIST->rect.y + 32,
                                    on_menu_command, 0);
        }
        return 1;
    }
    return 0;
}

static void on_resize(struct sxngui_app *app)
{
    (void)app;
    relayout();
}

/* The kernel bumps the generation on every change; only then is the map
 * rebuilt. Usage figures move without it, so the list is refreshed anyway. */
static void on_tick(struct sxngui_app *app)
{
    reload();
    update_menus();
    sxngui_app_request_repaint(app);
}

/* ---- self-test -------------------------------------------------------------------
 *
 * Headless: lists the devices and checks they agree with each other and with
 * the map built from them. Failures go to the kernel log; init reads the
 * verdict from the exit status. */

static int g_selftest_failures;

static void selftest_fail(const char *what)
{
    char line[160];

    snprintf(line, sizeof(line), "DISKMGMT SMOKE FAIL %s", what);
    (void)sxn_log(line);
    g_selftest_failures += 1;
}

static int selftest_layout(void)
{
    int root = -1;
    int index;

    (void)load_devices();
    if (g_device_count == 0)
    {
        selftest_fail("no storage device was listed");
        return 1;
    }
    for (index = 0; index < g_device_count; ++index)
    {
        const struct sxn_storage_device *device = &g_devices[index];
        int other;

        if (device->id == 0u || device->name[0] == '\0' || device->sector_count == 0u)
        {
            selftest_fail("a device has no id, name or size");
        }
        if (device->generation != g_generation)
        {
            selftest_fail("the generation changed between two entries of one listing");
        }
        for (other = index + 1; other < g_device_count; ++other)
        {
            if (g_devices[other].id == device->id)
            {
                selftest_fail("two devices share an id");
            }
        }
        if (device->kind == SXN_STORAGE_PARTITION)
        {
            int parent = find_device(device->parent_id);

            if (parent < 0 || g_devices[parent].kind == SXN_STORAGE_PARTITION)
            {
                selftest_fail("a partition's parent is not a listed disk");
            }
            else if (device->start_lba + device->sector_count > g_devices[parent].sector_count)
            {
                selftest_fail("a partition runs past its disk");
            }
        }
        if (strcmp(device->mount_point, "/") == 0)
        {
            root = index;
        }
    }
    if (root < 0)
    {
        selftest_fail("nothing is mounted at /");
    }
    else
    {
        const struct sxn_storage_device *device = &g_devices[root];

        if ((device->flags & (SXN_STORAGE_MOUNTED | SXN_STORAGE_SYSTEM)) != (SXN_STORAGE_MOUNTED | SXN_STORAGE_SYSTEM))
        {
            selftest_fail("the root volume is not flagged mounted and system");
        }
        if (strcmp(device->fs_name, "sxfs") != 0 || device->total_bytes == 0u ||
            device->used_bytes + device->free_bytes != device->total_bytes)
        {
            selftest_fail("the root volume does not report SxFS usage that adds up");
        }
        if (device->kind == SXN_STORAGE_PARTITION)
        {
            int parent = find_device(device->parent_id);

            if (parent < 0 || (g_devices[parent].flags & SXN_STORAGE_CHILD_MOUNTED) == 0u ||
                (g_devices[parent].flags & SXN_STORAGE_SYSTEM) == 0u)
            {
                selftest_fail("the root's disk is not flagged as holding it");
            }
        }
    }

    /* The map covers each disk in order, without overlaps. */
    for (index = 0; index < g_disk_count; ++index)
    {
        const struct dm_disk *disk = &g_disks[index];
        uint64_t end = 0;
        int segment;

        if (disk->segment_count == 0)
        {
            selftest_fail("a disk has nothing on its map");
        }
        for (segment = 0; segment < disk->segment_count; ++segment)
        {
            const struct dm_segment *item = &disk->segments[segment];

            if (item->start < end || item->sectors == 0u ||
                item->start + item->sectors > g_devices[disk->device].sector_count)
            {
                selftest_fail("a disk's map overlaps or runs past the disk");
            }
            end = item->start + item->sectors;
        }
    }
    return root;
}

static int find_serial(const char *serial)
{
    int index;

    for (index = 0; index < g_device_count; ++index)
    {
        if (g_devices[index].kind == SXN_STORAGE_DISK && strcmp(g_devices[index].serial, serial) == 0)
        {
            return index;
        }
    }
    return -1;
}

/* The partitions of the disk with this id, and the first of them. */
static int partitions_of(uint32_t disk_id, int *first)
{
    int index;
    int count = 0;

    *first = -1;
    for (index = 0; index < g_device_count; ++index)
    {
        if (g_devices[index].kind == SXN_STORAGE_PARTITION && g_devices[index].parent_id == disk_id)
        {
            if (count++ == 0)
            {
                *first = index;
            }
        }
    }
    return count;
}

/* Runs `--init` on the blank disk as the user: the storage calls must refuse
 * it, whatever the program would allow. */
static int selftest_init_as_user(uint32_t id, const char *name)
{
    struct sxn_identity user = {1000u, 1000u, DM_USERS_GID, 0u};
    struct sxn_spawn_request request;
    char id_text[16];
    char name_text[SXN_STORAGE_NAME_CAPACITY];
    char *argv[4];
    int status = -1;
    long child;

    snprintf(id_text, sizeof(id_text), "%u", (unsigned)id);
    snprintf(name_text, sizeof(name_text), "%s", name);
    argv[0] = DM_PATH;
    argv[1] = "--init";
    argv[2] = id_text;
    argv[3] = name_text;
    sxn_spawn_request_init(&request, DM_PATH, argv, 4);
    child = sxn_spawn_ex_as(&request, &user);
    if (child <= 0 || sxn_waitpid(child, &status) != child)
    {
        return -1;
    }
    return status;
}

/* The write half, on the scenario's blank disk (serial SXBLANK0), never on
 * the system's: initialize, partition and format, relabel, delete. */
static void selftest_writes(void)
{
    uint8_t sector[SXDISK_SECTOR_SIZE];
    char name[SXN_STORAGE_NAME_CAPACITY];
    uint32_t disk_id;
    uint32_t part_id;
    int disk;
    int part;
    int rc;

    (void)load_devices();
    disk = find_serial("SXBLANK0");
    if (disk < 0)
    {
        selftest_fail("the blank disk SXBLANK0 is not attached");
        return;
    }
    disk_id = g_devices[disk].id;
    snprintf(name, sizeof(name), "%s", g_devices[disk].name);
    if (!can_initialize(&g_devices[disk]))
    {
        selftest_fail("the blank disk is not offered for Initialize");
        return;
    }
    if (selftest_init_as_user(disk_id, name) != SAVANXP_EPERM)
    {
        selftest_fail("the user could initialize a disk");
    }

    rc = op_init(disk_id, name);
    (void)load_devices();
    disk = find_device(disk_id);
    if (rc != 0 || disk < 0 || g_devices[disk].scheme != SXN_STORAGE_SCHEME_GPT ||
        partitions_of(disk_id, &part) != 0)
    {
        char detail[96];

        snprintf(detail, sizeof(detail), "Initialize did not leave an empty GPT disk (status %d, scheme %d)", rc,
                 disk >= 0 ? (int)g_devices[disk].scheme : -1);
        selftest_fail(detail);
        return;
    }
    if (sxn_storage_read(disk_id, 1, 1, sector) != 0 || memcmp(sector, "EFI PART", 8) != 0)
    {
        selftest_fail("the GPT header is not on LBA 1");
    }
    if (op_init(disk_id, name) != SAVANXP_EBUSY)
    {
        selftest_fail("a disk with a table was initialized again");
    }

    rc = op_create(disk_id, name, SXDISK_ALIGN_SECTORS, 32u * SXDISK_ALIGN_SECTORS, 1, "SXTEST");
    (void)load_devices();
    if (rc != 0 || partitions_of(disk_id, &part) != 1)
    {
        selftest_fail("New Partition did not add one partition");
        return;
    }
    if (g_devices[part].start_lba != SXDISK_ALIGN_SECTORS || g_devices[part].sector_count != 32u * SXDISK_ALIGN_SECTORS ||
        strcmp(g_devices[part].label, "SXTEST") != 0 || strcmp(g_devices[part].fs_name, "sxfs") != 0 ||
        memcmp(g_devices[part].type_guid, sxdisk_type_sxfs, SXDISK_GUID_SIZE) != 0)
    {
        selftest_fail("the new partition is not a 32 MB SxFS named SXTEST at 1 MB");
    }
    part_id = g_devices[part].id;
    if (sxn_storage_read(part_id, 0, 1, sector) != 0 || memcmp(sector, "SXFS", 4) != 0)
    {
        selftest_fail("the new partition does not start with an SxFS superblock");
    }
    if (op_create(disk_id, name, SXDISK_ALIGN_SECTORS, SXDISK_ALIGN_SECTORS, 0, "X") != SAVANXP_ENOSPC)
    {
        selftest_fail("a partition was created over another");
    }

    snprintf(name, sizeof(name), "%s", g_devices[part].name);
    if (op_format(part_id, name, "RELABEL") != 0)
    {
        selftest_fail("Format failed");
        return;
    }
    (void)load_devices();
    if (partitions_of(disk_id, &part) != 1 || strcmp(g_devices[part].label, "RELABEL") != 0 ||
        strcmp(g_devices[part].fs_name, "sxfs") != 0)
    {
        selftest_fail("Format did not keep an SxFS partition with the new label");
        return;
    }
    part_id = g_devices[part].id;
    snprintf(name, sizeof(name), "%s", g_devices[part].name);
    if (op_delete(part_id, name, SXDISK_ALIGN_SECTORS + 1u) != SAVANXP_ENOENT)
    {
        selftest_fail("Delete accepted a start the partition does not have");
    }
    if (op_delete(part_id, name, SXDISK_ALIGN_SECTORS) != 0)
    {
        selftest_fail("Delete failed");
        return;
    }
    (void)load_devices();
    if (partitions_of(disk_id, &part) != 0)
    {
        selftest_fail("the deleted partition is still listed");
    }
    if (op_format(part_id, name, 0) != SAVANXP_ENOENT)
    {
        selftest_fail("a deleted partition could still be formatted");
    }
}

static int selftest(void)
{
    char text[128];

    (void)selftest_layout();
    selftest_writes();
    (void)load_devices();
    build_rows();
    if (g_row_count == 0)
    {
        selftest_fail("no volume row was built");
    }
    if (g_selftest_failures != 0)
    {
        return 1;
    }
    snprintf(text, sizeof(text), "diskmgmt: self-test passed, %d devices, %d disks, %d volumes",
             g_device_count, g_disk_count, g_row_count);
    (void)sxn_log(text);
    return 0;
}

/* ---- start-up ---------------------------------------------------------------------- */

static void build_dialogs(void)
{
    int width = DM_FORM_WIDTH - DM_DIALOG_MARGIN * 2;
    int field_x = DM_DIALOG_MARGIN + 80;
    int y = DM_DIALOG_MARGIN;
    int buttons_y = DM_FORM_HEIGHT - DM_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT;

    g_confirm_widgets[0] = sxgui_label(
        sx_rect_make(DM_DIALOG_MARGIN, DM_DIALOG_MARGIN, DM_CONFIRM_WIDTH - DM_DIALOG_MARGIN * 2, DM_TEXT_ROW),
        g_confirm_line);
    g_confirm_widgets[1] = sxgui_label(
        sx_rect_make(DM_DIALOG_MARGIN, DM_DIALOG_MARGIN + DM_TEXT_ROW + 2, DM_CONFIRM_WIDTH - DM_DIALOG_MARGIN * 2,
                     DM_TEXT_ROW),
        g_confirm_detail);
    g_confirm_widgets[2] = sxgui_button(
        sx_rect_make(centred_button_x(DM_CONFIRM_WIDTH, 2, 0),
                     DM_CONFIRM_HEIGHT - DM_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT, SXGUI_BUTTON_WIDTH,
                     SXGUI_BUTTON_HEIGHT),
        "Yes", on_confirm_yes, 0);
    g_confirm_widgets[3] = sxgui_button(
        sx_rect_make(centred_button_x(DM_CONFIRM_WIDTH, 2, 1),
                     DM_CONFIRM_HEIGHT - DM_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT, SXGUI_BUTTON_WIDTH,
                     SXGUI_BUTTON_HEIGHT),
        "No", on_confirm_no, 0);

    g_new_widgets[0] = sxgui_label(sx_rect_make(DM_DIALOG_MARGIN, y + 3, 76, DM_TEXT_ROW), "Size (MB):");
    g_new_widgets[1] = sxgui_textfield(sx_rect_make(field_x, y, 96, SXGUI_FIELD_HEIGHT), g_new_size,
                                       (int)sizeof(g_new_size));
    y += SXGUI_FIELD_HEIGHT + SXGUI_GAP;
    g_new_widgets[2] = sxgui_label(sx_rect_make(DM_DIALOG_MARGIN, y + 3, 76, DM_TEXT_ROW), "Label:");
    g_new_widgets[3] = sxgui_textfield(sx_rect_make(field_x, y, width - 80, SXGUI_FIELD_HEIGHT), g_new_label,
                                       (int)sizeof(g_new_label));
    y += SXGUI_FIELD_HEIGHT + SXGUI_GAP;
    g_new_widgets[4] = sxgui_checkbox(sx_rect_make(field_x, y, width - 80, DM_TEXT_ROW), "Format as SxFS", 1);
    y += DM_TEXT_ROW + SXGUI_GAP;
    g_new_widgets[5] = sxgui_label(sx_rect_make(DM_DIALOG_MARGIN, y, width, DM_TEXT_ROW), g_new_note);
    g_new_widgets[6] = sxgui_button(
        sx_rect_make(centred_button_x(DM_FORM_WIDTH, 2, 0), buttons_y, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_new_ok, 0);
    g_new_widgets[7] = sxgui_button(
        sx_rect_make(centred_button_x(DM_FORM_WIDTH, 2, 1), buttons_y, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Cancel", on_form_cancel, 0);

    y = DM_DIALOG_MARGIN;
    g_format_widgets[0] = sxgui_label(sx_rect_make(DM_DIALOG_MARGIN, y + 3, 76, DM_TEXT_ROW), "Label:");
    g_format_widgets[1] = sxgui_textfield(sx_rect_make(field_x, y, width - 80, SXGUI_FIELD_HEIGHT),
                                          g_format_label, (int)sizeof(g_format_label));
    y += SXGUI_FIELD_HEIGHT + SXGUI_GAP * 2;
    g_format_widgets[2] = sxgui_label(sx_rect_make(DM_DIALOG_MARGIN, y, width, DM_TEXT_ROW),
                                      "Formatting erases ALL data on this partition.");
    y += DM_TEXT_ROW + 2;
    g_format_widgets[3] = sxgui_label(sx_rect_make(DM_DIALOG_MARGIN, y, width, DM_TEXT_ROW), g_format_note);
    g_format_widgets[4] = sxgui_button(
        sx_rect_make(centred_button_x(DM_FORM_WIDTH, 2, 0), buttons_y, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Format", on_format_ok, 0);
    g_format_widgets[5] = sxgui_button(
        sx_rect_make(centred_button_x(DM_FORM_WIDTH, 2, 1), buttons_y, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Cancel", on_form_cancel, 0);

    g_message_widgets[0] = sxgui_label(
        sx_rect_make(DM_DIALOG_MARGIN, DM_DIALOG_MARGIN, DM_MESSAGE_WIDTH - DM_DIALOG_MARGIN * 2, DM_TEXT_ROW),
        g_message_line);
    g_message_widgets[1] = sxgui_label(
        sx_rect_make(DM_DIALOG_MARGIN, DM_DIALOG_MARGIN + DM_TEXT_ROW + 2, DM_MESSAGE_WIDTH - DM_DIALOG_MARGIN * 2,
                     DM_TEXT_ROW),
        g_message_detail);
    g_message_widgets[2] = sxgui_button(
        sx_rect_make(centred_button_x(DM_MESSAGE_WIDTH, 1, 0),
                     DM_MESSAGE_HEIGHT - DM_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT, SXGUI_BUTTON_WIDTH,
                     SXGUI_BUTTON_HEIGHT),
        "OK", on_dialog_ok, 0);
}

static int run_window(void)
{
    int result;

    *DM_LIST = sxgui_listbox(sx_rect_make(0, 0, 0, 0), g_row_pointers, 0);
    DM_LIST->columns = g_columns;
    DM_LIST->column_count = DM_COLUMN_COUNT;
    DM_LIST->flags |= SXGUI_FLAG_EMPTY_SELECTION;
    DM_LIST->value = -1;
    DM_LIST->on_action = on_list;
    *DM_SCROLL = sxgui_scrollbar(sx_rect_make(0, 0, 0, 0), 0, 0, 1, 0);
    DM_SCROLL->on_action = on_scroll;
    *DM_STATUS = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status);
    DM_STATUS->flags |= SXGUI_FLAG_SUNKEN;
    *DM_STATUS_RIGHT = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status_right);
    DM_STATUS_RIGHT->flags |= SXGUI_FLAG_SUNKEN;

    build_dialogs();

    result = sxngui_app_init(&g_app, &sun_resources, g_widgets, DM_WIDGET_COUNT);
    if (result < 0)
    {
        (void)sxn_log("diskmgmt: no window from windowd");
        return 1;
    }
    g_menubar.menus = k_menus;
    g_menubar.menu_count = (int)(sizeof(k_menus) / sizeof(k_menus[0]));
    g_menubar.on_command = on_menu_command;
    sxgui_set_menubar(&g_app.ui, &g_menubar);

    (void)sxngui_app_set_content_size(&g_app, DM_DEFAULT_WIDTH, DM_DEFAULT_HEIGHT);
    relayout();
    reload();
    update_menus();

    g_app.on_paint = on_paint;
    g_app.on_key = on_key;
    g_app.on_pointer = on_pointer;
    g_app.on_resize = on_resize;
    g_app.on_tick = on_tick;
    g_app.tick_interval_ms = 2000ul;
    sxgui_focus(&g_app.ui, 0);
    return sxngui_app_run(&g_app);
}

int main(int argc, char **argv)
{
    /* A SUN program never returns from main: the stock crt0 would fall
     * through to a POSIX exit, which is ENOSYS here. */
    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        sxn_exit(selftest());
    }
    if (argc > 1 && argv != 0)
    {
        /* The elevated half: no window, just the operation and its status. */
        int status = run_operation(argc, argv);

        if (status >= 0)
        {
            sxn_exit(status);
        }
    }
    sxn_exit(run_window());
    return 0;
}
