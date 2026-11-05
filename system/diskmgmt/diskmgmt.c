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
#include <string.h>

#include "abi/savanxp_abi.h"
#include "sxdisk/sxdisk.h"
#include "sxgfx/gfx2d.h"
#include "sxgfx/pixels.h"
#include "sxgfx/sxchrome.h"
#include "sxgfx/sxgui.h"
#include "sxn/sxn.h"
#include "sxn/sxngui.h"

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
    DM_CMD_PROPERTIES,
    DM_CMD_ABOUT,
};

static struct sxgui_menu_item k_file_items[] = {
    {"Exit", DM_CMD_EXIT, 0},
};

static struct sxgui_menu_item k_action_items[] = {
    {"Refresh\tF5", DM_CMD_REFRESH, 0},
    {0, 0, 0},
    {"Properties\tAlt+Enter", DM_CMD_PROPERTIES, 0},
};

static struct sxgui_menu_item k_help_items[] = {
    {"About Disk Management", DM_CMD_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"Action", k_action_items, (int)(sizeof(k_action_items) / sizeof(k_action_items[0]))},
    {"Help", k_help_items, (int)(sizeof(k_help_items) / sizeof(k_help_items[0]))},
};

/* The context menu is built for whatever was clicked. */
static struct sxgui_menu_item g_context_items[8];
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

static void update_menus(void)
{
    const struct sxn_storage_device *device = selected_device();

    k_action_items[2].flags = device != 0 ? 0u : SXGUI_MENU_DISABLED;
}

static void on_menu_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case DM_CMD_EXIT:
        sxngui_app_quit(&g_app, 0);
        break;
    case DM_CMD_REFRESH:
        reload();
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
}

/* The context menu for what is selected now. */
static int build_context_menu(void)
{
    int count = 0;

    if (selected_device() != 0)
    {
        g_context_items[count++] = (struct sxgui_menu_item){"Properties", DM_CMD_PROPERTIES, 0};
    }
    g_context_items[count++] = (struct sxgui_menu_item){"Refresh", DM_CMD_REFRESH, 0};
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

static int selftest(void)
{
    char text[128];

    (void)selftest_layout();
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
    sxn_exit(run_window());
    return 0;
}
