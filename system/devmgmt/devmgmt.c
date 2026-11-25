/*
 * Device Manager, a SUN program (docs/userland/DEVICE_MANAGER.md).
 *
 * Windows XP's Device Manager, read-only: the computer's hardware as a tree,
 * by type or by connection, each device with the driver using it, its state
 * and why, and a Properties sheet with what the kernel recorded about it.
 *
 * WHERE THE DATA COMES FROM. One call, SXN_SYS_DEVICE_SNAPSHOT, copies the
 * whole hardware inventory (docs/kernel/DEVICE_INVENTORY.md) in one
 * consistent view. This program never probes hardware itself and changes
 * nothing: there is no operation to offer yet, so none is shown.
 *
 * IDENTITY. Rows are rebuilt on every refresh. What is selected is kept by
 * instance id, never by row: an id is never reused, so a device that went
 * away can never hand its selection to the one that took its place.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi/savanxp_abi.h"
#include "sxgfx/gfx2d.h"
#include "sxgfx/mime_icon.h"
#include "sxgfx/sxchrome.h"
#include "sxgfx/sxgui.h"
#include "sxn/sxn.h"
#include "sxn/sxngui.h"

#include "devmgmt.sunres.h"

#define DV_MAX_DEVICES 512
#define DV_MAX_ROWS (DV_MAX_DEVICES + DV_CATEGORY_COUNT + 1)
#define DV_MAX_DEPTH 24
#define DV_NAME_CAPACITY 96

#define DV_DEFAULT_WIDTH 480
#define DV_DEFAULT_HEIGHT 440
#define DV_MIN_WIDTH 320
#define DV_MIN_HEIGHT 240

#define DV_ROW_HEIGHT 18
#define DV_INDENT 19
#define DV_ICON 16

#define DV_COLOR_LINE SXGUI_RGB(128, 128, 128)

/* ---- categories ------------------------------------------------------------------
 *
 * The groups of the by-type view, in the order they are listed. A device goes
 * to the group its class names; one with no known class goes to Other
 * devices. A device without a driver stays in its group with a problem mark:
 * not having a driver is a state, not a kind of hardware. */

enum dv_category {
    DV_CAT_DISK = 0,
    DV_CAT_DISPLAY,
    DV_CAT_HID,
    DV_CAT_KEYBOARD,
    DV_CAT_MOUSE,
    DV_CAT_NETWORK,
    DV_CAT_OTHER,
    DV_CAT_SOUND,
    DV_CAT_STORAGE,
    DV_CAT_SYSTEM,
    DV_CAT_USB,
    DV_CATEGORY_COUNT
};

static const struct {
    const char *title;
    const char *icon;
} k_categories[DV_CATEGORY_COUNT] = {
    {"Disk drives", "drive-harddisk"},
    {"Display adapters", "device-display"},
    {"Human Interface Devices", "device-hid"},
    {"Keyboards", "device-keyboard"},
    {"Mice and other pointing devices", "device-mouse"},
    {"Network adapters", "device-network"},
    {"Other devices", "device-unknown"},
    {"Sound, video and game controllers", "device-sound"},
    {"Storage controllers", "device-storage"},
    {"System devices", "device-system"},
    {"Universal Serial Bus controllers", "device-usb"},
};

/* ---- the model ------------------------------------------------------------------ */

/* The inventory being shown: the live one, or a self-test fixture. */
struct dv_inventory {
    struct sxn_device_info *devices;
    int count;
    uint32_t generation;
};

static struct sxn_device_info g_live[DV_MAX_DEVICES];
static struct dv_inventory g_inventory = {g_live, 0, 0};
/* The kernel had more than fit: the list says so. */
static int g_truncated;

enum dv_view {
    DV_VIEW_TYPE = 0,
    DV_VIEW_CONNECTION,
};

enum dv_row_kind {
    DV_ROW_COMPUTER = 1,
    DV_ROW_CATEGORY,
    DV_ROW_DEVICE,
};

struct dv_row {
    int kind;
    int category;          /* DV_ROW_CATEGORY, and the group of a device row in the by-type view */
    int device;            /* index into the inventory; the root's for DV_ROW_COMPUTER */
    int depth;
    int has_children;
    int last;              /* the last of its siblings */
    uint32_t continues;    /* bit d: the branch at depth d goes on below this row */
};

/* The whole tree in display order, and the rows of it that are visible. */
static struct dv_row g_tree[DV_MAX_ROWS];
static int g_tree_count;
static int g_visible[DV_MAX_ROWS];
static int g_visible_count;

static int g_view = DV_VIEW_TYPE;

/* Expansion. The by-type view keeps it per group; the by-connection view
 * keeps the ids that are collapsed. The computer row has its own flag in
 * each view. */
static int g_category_expanded[DV_CATEGORY_COUNT];
static int g_category_seen[DV_CATEGORY_COUNT];
static int g_root_collapsed[2];
#define DV_MAX_COLLAPSED 64
static uint32_t g_collapsed[DV_MAX_COLLAPSED];
static int g_collapsed_count;

/* The selection, by identity. g_select_parent and g_select_category say
 * where it goes if the device is removed. */
static int g_select_kind;
static uint32_t g_select_id;       /* device rows and the computer: the instance id */
static int g_select_category;      /* category rows; and a device's group in the by-type view */
static uint32_t g_select_parent;   /* a device's parent, for the by-connection view */
static int g_selected_row = -1;    /* index into g_visible */
static int g_selection_removed;

/* ---- names ---------------------------------------------------------------------- */

static const struct {
    uint16_t id;
    const char *name;
} k_vendors[] = {
    {0x1002, "AMD"},     {0x1022, "AMD"},      {0x1039, "SiS"},       {0x104c, "Texas Instruments"},
    {0x106b, "Apple"},   {0x10b9, "ALi"},      {0x10de, "NVIDIA"},    {0x10ec, "Realtek"},
    {0x1106, "VIA"},     {0x1180, "Ricoh"},    {0x1234, "QEMU"},      {0x126f, "Silicon Motion"},
    {0x144d, "Samsung"}, {0x14e4, "Broadcom"}, {0x15ad, "VMware"},    {0x15b7, "SanDisk"},
    {0x168c, "Qualcomm Atheros"}, {0x1912, "Renesas"}, {0x1969, "Qualcomm Atheros"},
    {0x197b, "JMicron"}, {0x1987, "Phison"},   {0x1af4, "VirtIO"},    {0x1b21, "ASMedia"},
    {0x1b36, "QEMU"},    {0x1b4b, "Marvell"},  {0x1c5c, "SK hynix"},  {0x8086, "Intel"},
    {0x8087, "Intel"},   {0x80ee, "VirtualBox"},
};

static const char *vendor_name(uint16_t id)
{
    size_t index;

    for (index = 0; index < sizeof(k_vendors) / sizeof(k_vendors[0]); ++index)
    {
        if (k_vendors[index].id == id)
        {
            return k_vendors[index].name;
        }
    }
    return 0;
}

/* What a PCI function is, from its class codes alone. */
static const char *pci_class_text(const struct sxn_device_info *device)
{
    const uint8_t c = device->class_code;
    const uint8_t s = device->subclass;
    const uint8_t p = device->protocol;

    switch (c)
    {
    case 0x00:
        return s == 0x01 ? "VGA-compatible device" : 0;
    case 0x01:
        switch (s)
        {
        case 0x00: return "SCSI storage controller";
        case 0x01: return "IDE controller";
        case 0x04: return "RAID controller";
        case 0x05: return "ATA controller";
        case 0x06: return p == 0x01 ? "SATA AHCI controller" : "SATA controller";
        case 0x07: return "SAS controller";
        case 0x08: return p == 0x02 ? "NVM Express controller" : "Non-volatile memory controller";
        default: return "Storage controller";
        }
    case 0x02:
        return s == 0x00 ? "Ethernet controller" : "Network controller";
    case 0x03:
        return s == 0x00 ? "VGA-compatible display controller" : "Display controller";
    case 0x04:
        switch (s)
        {
        case 0x00: return "Video controller";
        case 0x01: return "Multimedia audio controller";
        case 0x03: return "High Definition Audio controller";
        default: return "Multimedia controller";
        }
    case 0x05:
        return "Memory controller";
    case 0x06:
        switch (s)
        {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-to-PCI bridge";
        case 0x07: return "CardBus bridge";
        default: return "Bridge";
        }
    case 0x07:
        return s == 0x00 ? "Serial controller" : s == 0x01 ? "Parallel controller" : "Communication controller";
    case 0x08:
        switch (s)
        {
        case 0x05: return "SD host controller";
        case 0x06: return "IOMMU";
        default: return "System peripheral";
        }
    case 0x09:
        return "Input device controller";
    case 0x0c:
        if (s == 0x03)
        {
            switch (p)
            {
            case 0x00: return "USB UHCI controller";
            case 0x10: return "USB OHCI controller";
            case 0x20: return "USB EHCI controller";
            case 0x30: return "USB xHCI controller";
            default: return "USB controller";
            }
        }
        return s == 0x05 ? "SMBus controller" : s == 0x00 ? "FireWire controller" : "Serial bus controller";
    case 0x0d:
        return "Wireless controller";
    case 0x10:
        return "Encryption controller";
    case 0x11:
        return "Signal processing controller";
    default:
        return 0;
    }
}

/* VirtIO devices are named by what they are, which is all their IDs say. */
static const char *virtio_text(const struct sxn_device_info *device)
{
    const uint16_t type = device->product_id >= 0x1040u ? (uint16_t)(device->product_id - 0x1040u)
                                                        : device->subsystem_id;

    switch (type)
    {
    case 1: return "VirtIO network adapter";
    case 2: return "VirtIO block device";
    case 4: return "VirtIO entropy source";
    case 16: return "VirtIO GPU";
    case 18: return "VirtIO input device";
    case 25: return "VirtIO sound device";
    default: return "VirtIO device";
    }
}

static int find_device(const struct dv_inventory *inventory, uint32_t id)
{
    int index;

    for (index = 0; index < inventory->count; ++index)
    {
        if (inventory->devices[index].id == id)
        {
            return index;
        }
    }
    return -1;
}

static int child_count(const struct dv_inventory *inventory, int index, uint32_t bus)
{
    int count = 0;
    int other;

    for (other = 0; other < inventory->count; ++other)
    {
        if (inventory->devices[other].parent_id == inventory->devices[index].id &&
            (bus == 0u || inventory->devices[other].bus == bus))
        {
            count += 1;
        }
    }
    return count;
}

/* A USB device with one function is that function: the by-type view shows the
 * interface alone, under the device's name, as Windows does. */
static int usb_folded(const struct dv_inventory *inventory, int index)
{
    const struct sxn_device_info *device = &inventory->devices[index];

    return device->bus == SXN_DEVICE_BUS_USB && device->device_class != SXN_DEVICE_CLASS_USB &&
           child_count(inventory, index, SXN_DEVICE_BUS_USB_INTERFACE) == 1;
}

static const char *usb_interface_text(const struct sxn_device_info *device)
{
    switch (device->class_code)
    {
    case 0x01: return "USB audio device";
    case 0x02:
    case 0x0a: return "USB communication device";
    case 0x03:
        if (device->subclass == 0x01 && device->protocol == 0x01)
        {
            return "USB keyboard";
        }
        if (device->subclass == 0x01 && device->protocol == 0x02)
        {
            return "USB mouse";
        }
        return "USB input device";
    case 0x08: return "USB mass storage device";
    case 0x09: return "USB hub";
    default: return 0;
    }
}

static void device_name(const struct dv_inventory *inventory, int index, char *out, size_t capacity)
{
    const struct sxn_device_info *device = &inventory->devices[index];
    const char *text;

    if (device->bus == SXN_DEVICE_BUS_ROOT)
    {
        snprintf(out, capacity, "Computer");
        return;
    }
    if (device->name[0] != '\0')
    {
        snprintf(out, capacity, "%s", device->name);
        return;
    }
    switch (device->bus)
    {
    case SXN_DEVICE_BUS_PCI:
        if (device->vendor_id == 0x1af4u)
        {
            snprintf(out, capacity, "%s", virtio_text(device));
            return;
        }
        text = pci_class_text(device);
        if (text != 0 && vendor_name(device->vendor_id) != 0)
        {
            snprintf(out, capacity, "%s %s", vendor_name(device->vendor_id), text);
        }
        else if (text != 0)
        {
            snprintf(out, capacity, "%s", text);
        }
        else
        {
            snprintf(out, capacity, "PCI device (class %02x.%02x)", (unsigned)device->class_code,
                     (unsigned)device->subclass);
        }
        return;
    case SXN_DEVICE_BUS_USB:
        if (device->device_class == SXN_DEVICE_CLASS_USB)
        {
            snprintf(out, capacity, "Generic USB hub");
        }
        else if (child_count(inventory, index, SXN_DEVICE_BUS_USB_INTERFACE) > 1)
        {
            snprintf(out, capacity, "USB composite device");
        }
        else
        {
            snprintf(out, capacity, "USB device %04x:%04x", (unsigned)device->vendor_id,
                     (unsigned)device->product_id);
        }
        return;
    case SXN_DEVICE_BUS_USB_INTERFACE:
    {
        const int parent = find_device(inventory, device->parent_id);

        if (parent >= 0 && usb_folded(inventory, parent) && inventory->devices[parent].name[0] != '\0')
        {
            snprintf(out, capacity, "%s", inventory->devices[parent].name);
            return;
        }
        text = usb_interface_text(device);
        if (text != 0)
        {
            snprintf(out, capacity, "%s", text);
        }
        else
        {
            snprintf(out, capacity, "USB interface %u (class %02x)", (unsigned)device->function_number,
                     (unsigned)device->class_code);
        }
        return;
    }
    default:
        snprintf(out, capacity, "Unnamed device");
        return;
    }
}

/* The group a device is listed under in the by-type view; -1 for the
 * computer and for a USB device shown through its only interface. */
static int device_category(const struct dv_inventory *inventory, int index)
{
    const struct sxn_device_info *device = &inventory->devices[index];

    if (device->bus == SXN_DEVICE_BUS_ROOT || usb_folded(inventory, index))
    {
        return -1;
    }
    switch (device->device_class)
    {
    case SXN_DEVICE_CLASS_DISK: return DV_CAT_DISK;
    case SXN_DEVICE_CLASS_DISPLAY: return DV_CAT_DISPLAY;
    case SXN_DEVICE_CLASS_INPUT: return DV_CAT_HID;
    case SXN_DEVICE_CLASS_KEYBOARD: return DV_CAT_KEYBOARD;
    case SXN_DEVICE_CLASS_POINTER: return DV_CAT_MOUSE;
    case SXN_DEVICE_CLASS_NETWORK: return DV_CAT_NETWORK;
    case SXN_DEVICE_CLASS_SOUND: return DV_CAT_SOUND;
    case SXN_DEVICE_CLASS_STORAGE: return DV_CAT_STORAGE;
    case SXN_DEVICE_CLASS_SYSTEM: return DV_CAT_SYSTEM;
    case SXN_DEVICE_CLASS_USB:
    case SXN_DEVICE_CLASS_USB_DEVICE: return DV_CAT_USB;
    default: return DV_CAT_OTHER;
    }
}

static const char *device_icon(const struct dv_inventory *inventory, int index)
{
    const int category = device_category(inventory, index);

    if (inventory->devices[index].bus == SXN_DEVICE_BUS_ROOT)
    {
        return "device-computer";
    }
    if (category < 0)
    {
        return "device-usb";
    }
    return k_categories[category].icon;
}

static int has_problem(const struct sxn_device_info *device)
{
    return device->state == SXN_DEVICE_FAILED || device->state == SXN_DEVICE_UNBOUND ||
           device->state == SXN_DEVICE_DISABLED;
}

/* ---- the tree ---------------------------------------------------------------- */

static int is_collapsed(uint32_t id)
{
    int index;

    for (index = 0; index < g_collapsed_count; ++index)
    {
        if (g_collapsed[index] == id)
        {
            return 1;
        }
    }
    return 0;
}

static void set_collapsed(uint32_t id, int collapsed)
{
    int index;

    for (index = 0; index < g_collapsed_count; ++index)
    {
        if (g_collapsed[index] == id)
        {
            if (!collapsed)
            {
                g_collapsed[index] = g_collapsed[--g_collapsed_count];
            }
            return;
        }
    }
    if (collapsed && g_collapsed_count < DV_MAX_COLLAPSED)
    {
        g_collapsed[g_collapsed_count++] = id;
    }
}

static int root_index(const struct dv_inventory *inventory)
{
    int index;

    for (index = 0; index < inventory->count; ++index)
    {
        if (inventory->devices[index].bus == SXN_DEVICE_BUS_ROOT)
        {
            return index;
        }
    }
    return -1;
}

static struct dv_row *add_row(int kind, int category, int device, int depth)
{
    struct dv_row *row;

    if (g_tree_count >= DV_MAX_ROWS)
    {
        return 0;
    }
    row = &g_tree[g_tree_count++];
    memset(row, 0, sizeof(*row));
    row->kind = kind;
    row->category = category;
    row->device = device;
    row->depth = depth;
    return row;
}

/* Group members by name, so a group reads as a list. */
static char g_sort_names[DV_MAX_DEVICES][DV_NAME_CAPACITY];

static void build_by_type(const struct dv_inventory *inventory)
{
    int category;
    int index;
    int members[DV_MAX_DEVICES];
    const int root = root_index(inventory);

    if (add_row(DV_ROW_COMPUTER, -1, root, 0) == 0)
    {
        return;
    }
    for (index = 0; index < inventory->count; ++index)
    {
        device_name(inventory, index, g_sort_names[index], sizeof(g_sort_names[index]));
    }
    for (category = 0; category < DV_CATEGORY_COUNT; ++category)
    {
        int count = 0;
        int problem = 0;
        int a;

        for (index = 0; index < inventory->count; ++index)
        {
            if (device_category(inventory, index) == category)
            {
                members[count++] = index;
                problem |= has_problem(&inventory->devices[index]);
            }
        }
        if (count == 0)
        {
            continue;
        }
        /* A group is opened the first time it shows, if something in it has
         * a problem, as Windows does; after that it stays as the user left it. */
        if (!g_category_seen[category])
        {
            g_category_seen[category] = 1;
            g_category_expanded[category] = problem;
        }
        for (a = 1; a < count; ++a)
        {
            const int value = members[a];
            int b = a - 1;

            while (b >= 0 && strcmp(g_sort_names[members[b]], g_sort_names[value]) > 0)
            {
                members[b + 1] = members[b];
                b -= 1;
            }
            members[b + 1] = value;
        }
        if (add_row(DV_ROW_CATEGORY, category, -1, 1) == 0)
        {
            return;
        }
        g_tree[g_tree_count - 1].has_children = 1;
        for (a = 0; a < count; ++a)
        {
            struct dv_row *row = add_row(DV_ROW_DEVICE, category, members[a], 2);

            if (row == 0)
            {
                return;
            }
        }
    }
    g_tree[0].has_children = g_tree_count > 1;
}

static void build_children(const struct dv_inventory *inventory, int parent, int depth)
{
    int index;

    if (depth >= DV_MAX_DEPTH)
    {
        return;
    }
    for (index = 0; index < inventory->count; ++index)
    {
        if (index == parent || inventory->devices[index].parent_id != inventory->devices[parent].id)
        {
            continue;
        }
        if (add_row(DV_ROW_DEVICE, device_category(inventory, index), index, depth) == 0)
        {
            return;
        }
        g_tree[g_tree_count - 1].has_children = child_count(inventory, index, 0u) > 0;
        build_children(inventory, index, depth + 1);
    }
}

static void build_by_connection(const struct dv_inventory *inventory)
{
    const int root = root_index(inventory);

    if (root < 0 || add_row(DV_ROW_COMPUTER, -1, root, 0) == 0)
    {
        return;
    }
    g_tree[0].has_children = child_count(inventory, root, 0u) > 0;
    build_children(inventory, root, 1);
}

static int row_expanded(const struct dv_inventory *inventory, const struct dv_row *row)
{
    if (!row->has_children)
    {
        return 0;
    }
    switch (row->kind)
    {
    case DV_ROW_COMPUTER:
        return !g_root_collapsed[g_view];
    case DV_ROW_CATEGORY:
        return g_category_expanded[row->category];
    default:
        return g_view == DV_VIEW_CONNECTION && !is_collapsed(inventory->devices[row->device].id);
    }
}

/* The whole tree, then its tree lines, then what is visible. */
static void build_tree(const struct dv_inventory *inventory)
{
    int index;
    int hidden_below = -1;

    g_tree_count = 0;
    g_visible_count = 0;
    if (inventory->count == 0)
    {
        return;
    }
    if (g_view == DV_VIEW_TYPE)
    {
        build_by_type(inventory);
    }
    else
    {
        build_by_connection(inventory);
    }

    /* A row is the last of its siblings when no later row at its depth comes
     * before one shallower. Its branch goes on below it otherwise. */
    for (index = 0; index < g_tree_count; ++index)
    {
        int next;

        g_tree[index].last = 1;
        for (next = index + 1; next < g_tree_count && g_tree[next].depth >= g_tree[index].depth; ++next)
        {
            if (g_tree[next].depth == g_tree[index].depth)
            {
                g_tree[index].last = 0;
                break;
            }
        }
    }
    {
        uint32_t continues = 0;

        for (index = 0; index < g_tree_count; ++index)
        {
            const int depth = g_tree[index].depth;

            continues &= (1u << depth) - 1u;
            g_tree[index].continues = continues;
            if (!g_tree[index].last && depth < 32)
            {
                continues |= 1u << depth;
            }
        }
    }

    for (index = 0; index < g_tree_count; ++index)
    {
        if (hidden_below >= 0 && g_tree[index].depth > hidden_below)
        {
            continue;
        }
        hidden_below = -1;
        g_visible[g_visible_count++] = index;
        if (g_tree[index].has_children && !row_expanded(inventory, &g_tree[index]))
        {
            hidden_below = g_tree[index].depth;
        }
    }
}

/* ---- selection --------------------------------------------------------------- */

static void select_row(const struct dv_inventory *inventory, int visible)
{
    const struct dv_row *row;

    if (visible < 0 || visible >= g_visible_count)
    {
        return;
    }
    row = &g_tree[g_visible[visible]];
    g_selected_row = visible;
    g_select_kind = row->kind;
    g_select_category = row->category;
    g_select_id = row->device >= 0 ? inventory->devices[row->device].id : 0u;
    g_select_parent = row->device >= 0 ? inventory->devices[row->device].parent_id : 0u;
    g_selection_removed = 0;
}

static int find_visible(const struct dv_inventory *inventory, int kind, uint32_t id, int category)
{
    int visible;

    for (visible = 0; visible < g_visible_count; ++visible)
    {
        const struct dv_row *row = &g_tree[g_visible[visible]];

        if (row->kind != kind)
        {
            continue;
        }
        if (kind == DV_ROW_CATEGORY ? row->category == category
                                    : (row->device >= 0 && inventory->devices[row->device].id == id))
        {
            return visible;
        }
    }
    return -1;
}

/* Finds the selection again after a rebuild. A device still listed but
 * hidden under a collapsed row selects that row; a device that went away
 * selects where it was -- its group, or the device it hung from -- and says
 * so. Never another device that happens to sit where it was. */
static void restore_selection(const struct dv_inventory *inventory)
{
    int visible = -1;
    int index;

    g_selected_row = -1;
    if (g_visible_count == 0)
    {
        return;
    }
    if (g_select_kind == 0)
    {
        select_row(inventory, 0);
        return;
    }
    if (g_select_kind == DV_ROW_CATEGORY)
    {
        visible = find_visible(inventory, DV_ROW_CATEGORY, 0u, g_select_category);
        if (visible >= 0)
        {
            g_selected_row = visible;
        }
        else
        {
            select_row(inventory, 0);
        }
        return;
    }

    index = find_device(inventory, g_select_id);
    if (index >= 0)
    {
        visible = find_visible(inventory, g_select_kind, g_select_id, 0);
        if (visible >= 0)
        {
            g_selected_row = visible;
            return;
        }
        /* Hidden: the nearest row above it that is visible and an ancestor. */
        {
            int tree;
            int at = -1;

            for (tree = 0; tree < g_tree_count; ++tree)
            {
                if (g_tree[tree].device == index && g_tree[tree].kind == DV_ROW_DEVICE)
                {
                    at = tree;
                    break;
                }
            }
            for (visible = g_visible_count - 1; visible >= 0 && at >= 0; --visible)
            {
                if (g_visible[visible] < at && g_tree[g_visible[visible]].depth < g_tree[at].depth)
                {
                    g_selected_row = visible;
                    return;
                }
            }
        }
        select_row(inventory, 0);
        return;
    }

    /* Removed. */
    if (g_view == DV_VIEW_TYPE)
    {
        visible = find_visible(inventory, DV_ROW_CATEGORY, 0u, g_select_category);
    }
    else if (find_device(inventory, g_select_parent) >= 0)
    {
        visible = find_visible(inventory, DV_ROW_DEVICE, g_select_parent, 0);
        if (visible < 0)
        {
            visible = find_visible(inventory, DV_ROW_COMPUTER, g_select_parent, 0);
        }
    }
    if (visible < 0)
    {
        visible = 0;
    }
    select_row(inventory, visible);
    g_selection_removed = 1;
}

static const struct dv_row *selected_row(void)
{
    if (g_selected_row < 0 || g_selected_row >= g_visible_count)
    {
        return 0;
    }
    return &g_tree[g_visible[g_selected_row]];
}

/* ---- properties ----------------------------------------------------------------
 *
 * Four tabs of name/value lines, filled from one record. What was not
 * recorded says so; nothing is made up to fill a field. */

enum dv_tab {
    DV_TAB_GENERAL = 0,
    DV_TAB_DRIVER,
    DV_TAB_DETAILS,
    DV_TAB_RESOURCES,
    DV_TAB_COUNT
};

#define DV_PROP_LINES 13
#define DV_PROP_NAME_CAPACITY 28
#define DV_PROP_VALUE_CAPACITY 96

static char g_prop_names[DV_PROP_LINES][DV_PROP_NAME_CAPACITY];
static char g_prop_values[DV_PROP_LINES][DV_PROP_VALUE_CAPACITY];
static int g_prop_count;

static void prop(const char *name, const char *value)
{
    if (g_prop_count >= DV_PROP_LINES)
    {
        return;
    }
    snprintf(g_prop_names[g_prop_count], DV_PROP_NAME_CAPACITY, "%s", name);
    snprintf(g_prop_values[g_prop_count], DV_PROP_VALUE_CAPACITY, "%s", value != 0 ? value : "");
    g_prop_count += 1;
}

/* A line across the whole page: a sentence rather than a field. */
static void prop_sentence(const char *text)
{
    prop("", text);
}

static const char *bus_text(uint32_t bus)
{
    switch (bus)
    {
    case SXN_DEVICE_BUS_ROOT: return "The computer";
    case SXN_DEVICE_BUS_PCI: return "PCI";
    case SXN_DEVICE_BUS_PLATFORM: return "Platform (no bus)";
    case SXN_DEVICE_BUS_USB: return "USB device";
    case SXN_DEVICE_BUS_USB_INTERFACE: return "USB interface";
    default: return "Unknown";
    }
}

static const char *state_text(uint32_t state)
{
    switch (state)
    {
    case SXN_DEVICE_STARTED: return "Started";
    case SXN_DEVICE_UNBOUND: return "No driver";
    case SXN_DEVICE_FAILED: return "Failed to start";
    case SXN_DEVICE_DISABLED: return "Disabled";
    case SXN_DEVICE_DISCOVERED: return "Discovered";
    case SXN_DEVICE_STARTING: return "Starting";
    case SXN_DEVICE_QUIESCING: return "Stopping";
    default: return "Unknown";
    }
}

static const char *speed_text(uint8_t speed)
{
    switch (speed)
    {
    case 1: return "Full speed (12 Mbit/s)";
    case 2: return "Low speed (1.5 Mbit/s)";
    case 3: return "High speed (480 Mbit/s)";
    case 4: return "SuperSpeed (5 Gbit/s)";
    case 5: return "SuperSpeed+ (10 Gbit/s)";
    default: return "Unknown";
    }
}

/* The status sentences of the General tab: what the state means, then why. */
static void status_sentences(const struct sxn_device_info *device, const char **first, const char **second)
{
    *second = "";
    switch (device->state)
    {
    case SXN_DEVICE_STARTED:
        *first = "This device is working properly.";
        if ((device->flags & SXN_DEVICE_FIRMWARE) != 0u)
        {
            *second = "It is used as the firmware set it up.";
        }
        return;
    case SXN_DEVICE_FAILED:
        *first = "This device cannot start.";
        *second = "Its driver gave up while starting it; the boot log says where.";
        return;
    case SXN_DEVICE_UNBOUND:
        switch (device->reason)
        {
        case SXN_DEVICE_REASON_BOOT_OPTION:
            *first = "No driver is using this device.";
            *second = "A boot option (nousb) kept the drivers away from it.";
            return;
        case SXN_DEVICE_REASON_UNSUPPORTED:
            *first = "No driver is using this device.";
            *second = "Its driver found nothing on it that it supports.";
            return;
        default:
            *first = "No driver is using this device.";
            *second = "No built-in driver took it, or its class uses another device.";
            return;
        }
    case SXN_DEVICE_DISABLED:
        *first = "This device is disabled.";
        return;
    default:
        *first = "The state of this device is not known.";
        return;
    }
}

static void fill_properties(const struct dv_inventory *inventory, int index, int tab)
{
    const struct sxn_device_info *device = &inventory->devices[index];
    char text[DV_PROP_VALUE_CAPACITY];
    const char *first;
    const char *second;
    const int category = device_category(inventory, index);
    const int parent = find_device(inventory, device->parent_id);
    uint32_t resource;
    int unmeasured = 0;

    g_prop_count = 0;
    switch (tab)
    {
    case DV_TAB_GENERAL:
        device_name(inventory, index, text, sizeof(text));
        prop("Device:", text);
        prop("Device type:", device->bus == SXN_DEVICE_BUS_ROOT ? "Computer"
                             : category >= 0                     ? k_categories[category].title
                                                                 : "USB device");
        if (device->manufacturer[0] != '\0')
        {
            prop("Manufacturer:", device->manufacturer);
        }
        else if (device->bus == SXN_DEVICE_BUS_PCI && vendor_name(device->vendor_id) != 0)
        {
            prop("Manufacturer:", vendor_name(device->vendor_id));
        }
        else
        {
            prop("Manufacturer:", "Not reported");
        }
        prop("Location:", device->location[0] != '\0' ? device->location : "Not recorded");
        prop("State:", state_text(device->state));
        prop("", "");
        status_sentences(device, &first, &second);
        prop_sentence(first);
        if (second[0] != '\0')
        {
            prop_sentence(second);
        }
        return;

    case DV_TAB_DRIVER:
        if (device->driver[0] == '\0')
        {
            prop("Driver:", device->bus == SXN_DEVICE_BUS_ROOT ? "The kernel" : "None");
        }
        else
        {
            prop("Driver:", device->driver);
        }
        prop("Origin:", device->driver[0] != '\0' || device->bus == SXN_DEVICE_BUS_ROOT ? "Built into the kernel"
                                                                                       : "Not applicable");
        prop("Provider:", "Not recorded");
        prop("Version:", "Not recorded");
        prop("Date:", "Not recorded");
        prop("", "");
        prop_sentence("Built-in drivers are part of the kernel: they are");
        prop_sentence("updated with it and cannot be removed on their own.");
        return;

    case DV_TAB_DETAILS:
        prop("Bus:", bus_text(device->bus));
        snprintf(text, sizeof(text), "%u", (unsigned)device->id);
        prop("Instance id:", text);
        if (parent >= 0)
        {
            char name[DV_NAME_CAPACITY];

            device_name(inventory, parent, name, sizeof(name));
            snprintf(text, sizeof(text), "%u (%s)", (unsigned)device->parent_id, name);
        }
        else
        {
            snprintf(text, sizeof(text), "None");
        }
        prop("Parent:", text);
        if (device->bus == SXN_DEVICE_BUS_PCI)
        {
            snprintf(text, sizeof(text), "PCI %04x:%04x", (unsigned)device->vendor_id, (unsigned)device->product_id);
            prop("Hardware id:", text);
            if (device->subsystem_vendor_id != 0u || device->subsystem_id != 0u)
            {
                snprintf(text, sizeof(text), "%04x:%04x", (unsigned)device->subsystem_vendor_id,
                         (unsigned)device->subsystem_id);
            }
            else
            {
                snprintf(text, sizeof(text), "Not reported");
            }
            prop("Subsystem:", text);
            snprintf(text, sizeof(text), "%02x.%02x.%02x, revision %02x", (unsigned)device->class_code,
                     (unsigned)device->subclass, (unsigned)device->protocol, (unsigned)device->revision);
            prop("Class code:", text);
        }
        else if (device->bus == SXN_DEVICE_BUS_USB || device->bus == SXN_DEVICE_BUS_USB_INTERFACE)
        {
            snprintf(text, sizeof(text), "USB %04x:%04x", (unsigned)device->vendor_id, (unsigned)device->product_id);
            prop("Hardware id:", text);
            snprintf(text, sizeof(text), "%02x.%02x.%02x", (unsigned)device->class_code, (unsigned)device->subclass,
                     (unsigned)device->protocol);
            prop(device->bus == SXN_DEVICE_BUS_USB ? "Device class:" : "Interface class:", text);
            if (device->bus == SXN_DEVICE_BUS_USB)
            {
                snprintf(text, sizeof(text), "USB %x.%02x, device release %x.%02x",
                         (unsigned)(device->usb_version >> 8), (unsigned)(device->usb_version & 0xffu),
                         (unsigned)(device->device_version >> 8), (unsigned)(device->device_version & 0xffu));
                prop("Versions:", text);
            }
            else
            {
                snprintf(text, sizeof(text), "%u", (unsigned)device->function_number);
                prop("Interface:", text);
            }
            prop("Speed:", speed_text(device->usb_speed));
        }
        else
        {
            prop("Hardware id:", "None: not on a bus");
        }
        snprintf(text, sizeof(text), "%s%s%s%s",
                 (device->flags & SXN_DEVICE_SYNTHETIC) != 0u ? "synthetic " : "",
                 (device->flags & SXN_DEVICE_FIRMWARE) != 0u ? "firmware-configured " : "",
                 (device->flags & SXN_DEVICE_REMOVABLE) != 0u ? "removable " : "",
                 (device->flags & (SXN_DEVICE_SYNTHETIC | SXN_DEVICE_FIRMWARE | SXN_DEVICE_REMOVABLE)) == 0u
                     ? "none"
                     : "");
        prop("Flags:", text);
        return;

    case DV_TAB_RESOURCES:
    default:
        if (device->resource_count == 0u)
        {
            prop_sentence("This device has no recorded resources.");
            return;
        }
        for (resource = 0; resource < device->resource_count && resource < SXN_DEVICE_MAX_RESOURCES; ++resource)
        {
            const struct sxn_device_resource *entry = &device->resources[resource];

            switch (entry->kind)
            {
            case SXN_DEVICE_RESOURCE_MEMORY:
                if (entry->length != 0u)
                {
                    snprintf(text, sizeof(text), "%08llx-%08llx%s", (unsigned long long)entry->start,
                             (unsigned long long)(entry->start + entry->length - 1u),
                             (entry->flags & SXN_DEVICE_RESOURCE_SHARED) != 0u ? " (shared)" : "");
                }
                else
                {
                    unmeasured = 1;
                    snprintf(text, sizeof(text), "%08llx%s%s", (unsigned long long)entry->start,
                             (entry->flags & SXN_DEVICE_RESOURCE_64BIT) != 0u ? ", 64-bit" : "",
                             (entry->flags & SXN_DEVICE_RESOURCE_PREFETCHABLE) != 0u ? ", prefetchable" : "");
                }
                prop("Memory range:", text);
                break;
            case SXN_DEVICE_RESOURCE_IO_PORT:
                if (entry->length == 0u)
                {
                    unmeasured = 1;
                }
                if (entry->length > 1u)
                {
                    snprintf(text, sizeof(text), "%04llx-%04llx%s", (unsigned long long)entry->start,
                             (unsigned long long)(entry->start + entry->length - 1u),
                             (entry->flags & SXN_DEVICE_RESOURCE_SHARED) != 0u ? " (shared)" : "");
                }
                else
                {
                    snprintf(text, sizeof(text), "%04llx%s", (unsigned long long)entry->start,
                             (entry->flags & SXN_DEVICE_RESOURCE_SHARED) != 0u ? " (shared)" : "");
                }
                prop("I/O range:", text);
                break;
            case SXN_DEVICE_RESOURCE_IRQ:
                snprintf(text, sizeof(text), "%llu%s", (unsigned long long)entry->start,
                         (entry->flags & SXN_DEVICE_RESOURCE_SHARED) != 0u ? " (shared)" : "");
                prop("IRQ:", text);
                break;
            default:
                prop("Resource:", "Of a kind this version does not know");
                break;
            }
        }
        if (unmeasured)
        {
            prop("", "");
            prop_sentence("A range shown by its base alone is as the");
            prop_sentence("firmware assigned it, and no driver claimed it:");
            prop_sentence("its size is not recorded, as reading it writes");
            prop_sentence("to the device.");
        }
        return;
    }
}

/* ---- loading -------------------------------------------------------------------- */

/* Reads the inventory; 1 when it changed since the last read. */
static int load_live(void)
{
    long total = sxn_device_snapshot(g_live, DV_MAX_DEVICES);

    if (total < 0)
    {
        g_inventory.count = 0;
        return 1;
    }
    g_truncated = total > DV_MAX_DEVICES;
    g_inventory.count = total > DV_MAX_DEVICES ? DV_MAX_DEVICES : (int)total;
    if (g_inventory.count > 0 && g_live[0].generation == g_inventory.generation)
    {
        return 0;
    }
    g_inventory.generation = g_inventory.count > 0 ? g_live[0].generation : 0u;
    return 1;
}

/* ---- icons ------------------------------------------------------------------------ */

#define DV_ICON_CACHE 16
static struct mime_icon_cache_slot g_icon_slots[DV_ICON_CACHE];
static uint32_t g_icon_pixels[DV_ICON_CACHE * MIME_ICON_SMALL_SIZE * MIME_ICON_SMALL_SIZE];
static struct mime_icon_cache g_icons = {MIME_ICON_SMALL_SIZE, 0, g_icon_slots, g_icon_pixels};

static long read_icon_file(const char *path, void *buffer, size_t capacity)
{
    long handle = sxn_open_file(path, SXN_FILE_READ);
    long total = 0;

    if (handle < 0)
    {
        return -1;
    }
    while ((size_t)total < capacity)
    {
        long got = sxn_read(handle, (char *)buffer + total, capacity - (size_t)total);

        if (got <= 0)
        {
            break;
        }
        total += got;
    }
    (void)sxn_close(handle);
    return total;
}

static const struct sx_bitmap *icon(const char *name)
{
    return mime_icon_cache_load(&g_icons, name);
}

/* ---- the window ------------------------------------------------------------------ */

static struct sxngui_app g_app;
static struct sxgui_menubar g_menubar;
static uint32_t g_pointer_buttons;
static int g_scroll; /* first visible row */
static struct sx_rect g_tree_rect;
static unsigned long g_last_click_ms;
static int g_last_click_row = -1;

static struct sxgui_widget g_widgets[3];
#define DV_SCROLL (&g_widgets[0])
#define DV_STATUS (&g_widgets[1])
#define DV_STATUS_RIGHT (&g_widgets[2])
#define DV_WIDGET_COUNT ((int)(sizeof(g_widgets) / sizeof(g_widgets[0])))

static char g_status[96];
static char g_status_right[48];

enum dv_command {
    DV_CMD_EXIT = 1,
    DV_CMD_REFRESH,
    DV_CMD_PROPERTIES,
    DV_CMD_BY_TYPE,
    DV_CMD_BY_CONNECTION,
    DV_CMD_EXPAND,
    DV_CMD_COLLAPSE,
    DV_CMD_ABOUT,
};

static struct sxgui_menu_item k_file_items[] = {
    {"Exit", DV_CMD_EXIT, 0},
};
static struct sxgui_menu_item k_action_items[] = {
    {"Refresh\tF5", DV_CMD_REFRESH, 0},
    {0, 0, 0},
    {"Properties\tAlt+Enter", DV_CMD_PROPERTIES, 0},
};
#define DV_ITEM_PROPERTIES 2
static struct sxgui_menu_item k_view_items[] = {
    {"Devices by type", DV_CMD_BY_TYPE, SXGUI_MENU_CHECKED},
    {"Devices by connection", DV_CMD_BY_CONNECTION, 0},
};
static struct sxgui_menu_item k_help_items[] = {
    {"About Device Manager", DV_CMD_ABOUT, 0},
};
static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"Action", k_action_items, (int)(sizeof(k_action_items) / sizeof(k_action_items[0]))},
    {"View", k_view_items, (int)(sizeof(k_view_items) / sizeof(k_view_items[0]))},
    {"Help", k_help_items, (int)(sizeof(k_help_items) / sizeof(k_help_items[0]))},
};

static struct sxgui_menu_item g_context_items[4];
static struct sxgui_menu g_context_menu = {"", g_context_items, 0};

/* Properties. */
#define DV_DIALOG_MARGIN SXGUI_DIALOG_MARGIN
#define DV_TEXT_ROW 18
#define DV_PROPERTIES_WIDTH 440
#define DV_PROPERTIES_VALUE_X 104
static const char *const k_tab_labels[DV_TAB_COUNT] = {"General", "Driver", "Details", "Resources"};
static struct sxgui_dialog g_properties_dialog;
static struct sxgui_widget g_properties_widgets[2 + DV_PROP_LINES * 2];
#define DV_PROPERTIES_TABS (&g_properties_widgets[0])
#define DV_PROPERTIES_OK (&g_properties_widgets[1])
static char g_properties_title[DV_NAME_CAPACITY + 16];
/* The device the sheet shows, copied: it stays readable after the device
 * is gone from the list. */
static struct sxn_device_info g_properties_copy[DV_MAX_DEVICES];
static struct dv_inventory g_properties_inventory = {g_properties_copy, 0, 0};
static int g_properties_index;

static struct sxgui_dialog g_message_dialog;
static struct sxgui_widget g_message_widgets[3];
static char g_message_line[96];
static char g_message_detail[96];

static int properties_height(void)
{
    return DV_DIALOG_MARGIN * 2 + sxgui_tabs_height() + 12 + DV_PROP_LINES * DV_TEXT_ROW + SXGUI_GAP +
           SXGUI_BUTTON_HEIGHT;
}

static void update_status(void)
{
    int problems = 0;
    int index;

    for (index = 0; index < g_inventory.count; ++index)
    {
        if (g_inventory.devices[index].bus != SXN_DEVICE_BUS_ROOT && has_problem(&g_inventory.devices[index]))
        {
            problems += 1;
        }
    }
    if (g_selection_removed)
    {
        snprintf(g_status, sizeof(g_status), "The selected device was removed.");
    }
    else if (g_truncated)
    {
        snprintf(g_status, sizeof(g_status), "Only the first %d devices are shown.", DV_MAX_DEVICES);
    }
    else
    {
        snprintf(g_status, sizeof(g_status), "%d devices, %d without a working driver",
                 g_inventory.count > 0 ? g_inventory.count - 1 : 0, problems);
    }
    snprintf(g_status_right, sizeof(g_status_right), "Generation %u", (unsigned)g_inventory.generation);
}

static int page_rows(void)
{
    const int inner = g_tree_rect.height - SXGUI_BORDER_SUNKEN * 2;

    return inner > DV_ROW_HEIGHT ? inner / DV_ROW_HEIGHT : 1;
}

static void update_scroll(void)
{
    const int page = page_rows();
    int range = g_visible_count - page;

    if (range < 0)
    {
        range = 0;
    }
    if (g_scroll > range)
    {
        g_scroll = range;
    }
    if (g_scroll < 0)
    {
        g_scroll = 0;
    }
    DV_SCROLL->range_min = 0;
    DV_SCROLL->range_max = range;
    DV_SCROLL->page = page;
    DV_SCROLL->value = g_scroll;
}

static void ensure_visible(void)
{
    const int page = page_rows();

    if (g_selected_row < 0)
    {
        return;
    }
    if (g_selected_row < g_scroll)
    {
        g_scroll = g_selected_row;
    }
    else if (g_selected_row >= g_scroll + page)
    {
        g_scroll = g_selected_row - page + 1;
    }
    update_scroll();
}

static void update_menus(void)
{
    const struct dv_row *row = selected_row();

    if (row != 0 && row->kind == DV_ROW_DEVICE)
    {
        k_action_items[DV_ITEM_PROPERTIES].flags &= ~SXGUI_MENU_DISABLED;
    }
    else
    {
        k_action_items[DV_ITEM_PROPERTIES].flags |= SXGUI_MENU_DISABLED;
    }
    k_view_items[0].flags = g_view == DV_VIEW_TYPE ? SXGUI_MENU_CHECKED : 0u;
    k_view_items[1].flags = g_view == DV_VIEW_CONNECTION ? SXGUI_MENU_CHECKED : 0u;
}

static void rebuild(void)
{
    build_tree(&g_inventory);
    restore_selection(&g_inventory);
    update_scroll();
    ensure_visible();
    update_menus();
    update_status();
}

static void reload(void)
{
    if (load_live())
    {
        rebuild();
    }
}

static void relayout(void)
{
    const int width = (int)g_app.info.width < DV_MIN_WIDTH ? DV_MIN_WIDTH : (int)g_app.info.width;
    const int height = (int)g_app.info.height < DV_MIN_HEIGHT ? DV_MIN_HEIGHT : (int)g_app.info.height;
    const int top = sxgui_menubar_height() + SXGUI_MARGIN;
    const int status_y = height - SXGUI_MARGIN - SXGUI_STATUS_HEIGHT;

    g_tree_rect = sx_rect_make(SXGUI_MARGIN, top, width - SXGUI_MARGIN * 2 - SXGUI_SCROLLBAR_THICKNESS,
                               status_y - SXGUI_GAP - top);
    DV_SCROLL->rect = sx_rect_make(g_tree_rect.x + g_tree_rect.width, g_tree_rect.y, SXGUI_SCROLLBAR_THICKNESS,
                                   g_tree_rect.height);
    DV_STATUS_RIGHT->rect = sx_rect_make(width - SXGUI_MARGIN - 120, status_y, 120, SXGUI_STATUS_HEIGHT);
    DV_STATUS->rect = sx_rect_make(SXGUI_MARGIN, status_y, DV_STATUS_RIGHT->rect.x - SXGUI_GAP - SXGUI_MARGIN,
                                   SXGUI_STATUS_HEIGHT);
    update_scroll();
}

/* ---- painting the tree ------------------------------------------------------------ */

/* Where a row's +/- box sits: the column of its depth. */
static int column_x(int depth)
{
    return g_tree_rect.x + SXGUI_BORDER_SUNKEN + 9 + depth * DV_INDENT;
}

static void dotted_vline(struct sx_painter *painter, int x, int y0, int y1)
{
    int y;

    for (y = y0 + ((x + y0) & 1); y <= y1; y += 2)
    {
        sx_painter_set_pixel(painter, x, y, DV_COLOR_LINE);
    }
}

static void dotted_hline(struct sx_painter *painter, int x0, int x1, int y)
{
    int x;

    for (x = x0 + ((x0 + y) & 1); x <= x1; x += 2)
    {
        sx_painter_set_pixel(painter, x, y, DV_COLOR_LINE);
    }
}

static void paint_expander(struct sx_painter *painter, int cx, int cy, int expanded)
{
    const struct sx_rect box = sx_rect_make(cx - 4, cy - 4, 9, 9);

    sx_painter_fill_rect(painter, box, SXGUI_COLOR_FIELD);
    sx_painter_draw_frame(painter, box, DV_COLOR_LINE);
    sx_painter_hline(painter, cx - 2, cy, 5, SXGUI_COLOR_TEXT);
    if (!expanded)
    {
        sx_painter_vline(painter, cx, cy - 2, 5, SXGUI_COLOR_TEXT);
    }
}

/* The problem marks over an icon's lower left corner: a yellow "!" for a
 * device that failed, a white "?" for one with no driver. They are drawn,
 * not looked up, so a missing icon file never hides a problem. */
static void paint_overlay(struct sx_painter *painter, int x, int y, uint32_t state)
{
    if (state == SXN_DEVICE_FAILED)
    {
        const struct sx_point triangle[3] = {{x + 4, y + 8}, {x, y + 15}, {x + 8, y + 15}};

        sx_painter_fill_polygon(painter, triangle, 3, SXGUI_RGB(255, 216, 32));
        sx_painter_draw_polyline(painter, triangle, 3, 1, SXGUI_RGB(96, 72, 0));
        sx_painter_vline(painter, x + 4, y + 10, 3, SXGUI_COLOR_TEXT);
        sx_painter_set_pixel(painter, x + 4, y + 14, SXGUI_COLOR_TEXT);
    }
    else if (state == SXN_DEVICE_UNBOUND)
    {
        const struct sx_rect badge = sx_rect_make(x, y + 8, 7, 8);

        sx_painter_fill_rect(painter, badge, SXGUI_COLOR_FIELD);
        sx_painter_draw_frame(painter, badge, SXGUI_RGB(0, 0, 128));
        sx_painter_hline(painter, x + 2, y + 9 + 1, 3, SXGUI_RGB(0, 0, 128));
        sx_painter_set_pixel(painter, x + 4, y + 11, SXGUI_RGB(0, 0, 128));
        sx_painter_set_pixel(painter, x + 3, y + 12, SXGUI_RGB(0, 0, 128));
        sx_painter_set_pixel(painter, x + 3, y + 14, SXGUI_RGB(0, 0, 128));
    }
}

static void row_label(const struct dv_row *row, char *out, size_t capacity)
{
    if (row->kind == DV_ROW_CATEGORY)
    {
        snprintf(out, capacity, "%s", k_categories[row->category].title);
    }
    else if (row->device >= 0)
    {
        device_name(&g_inventory, row->device, out, capacity);
    }
    else
    {
        snprintf(out, capacity, "Computer");
    }
}

static void paint_row(struct sx_painter *painter, int visible, int y)
{
    const struct dv_row *row = &g_tree[g_visible[visible]];
    const int cy = y + DV_ROW_HEIGHT / 2;
    const int cx = column_x(row->depth);
    const int icon_x = cx + 9;
    const int text_x = icon_x + DV_ICON + 4;
    const struct sx_bitmap *bitmap;
    char label[DV_NAME_CAPACITY];
    int depth;

    /* The lines: the branches still open above, then this row's own. */
    for (depth = 1; depth < row->depth; ++depth)
    {
        if ((row->continues & (1u << depth)) != 0u)
        {
            dotted_vline(painter, column_x(depth), y, y + DV_ROW_HEIGHT - 1);
        }
    }
    if (row->depth > 0)
    {
        dotted_vline(painter, cx, y, row->last ? cy : y + DV_ROW_HEIGHT - 1);
        dotted_hline(painter, cx, icon_x - 2, cy);
    }
    if (row->has_children)
    {
        paint_expander(painter, cx, cy, row_expanded(&g_inventory, row));
    }

    bitmap = row->kind == DV_ROW_CATEGORY ? icon(k_categories[row->category].icon)
                                          : icon(row->device >= 0 ? device_icon(&g_inventory, row->device)
                                                                  : "device-computer");
    if (bitmap != 0)
    {
        sx_painter_blit_bitmap(painter, bitmap, icon_x, cy - DV_ICON / 2);
    }
    if (row->kind == DV_ROW_DEVICE)
    {
        paint_overlay(painter, icon_x, cy - DV_ICON / 2, g_inventory.devices[row->device].state);
    }

    row_label(row, label, sizeof(label));
    if (visible == g_selected_row)
    {
        const struct sx_rect highlight =
            sx_rect_make(text_x - 2, y + 1, gfx_text_width(label) + 4, DV_ROW_HEIGHT - 2);

        sx_painter_fill_rect(painter, highlight, SXGUI_COLOR_SELECT);
        sx_painter_draw_text(painter, text_x, cy - gfx_text_height() / 2, label, SXGUI_COLOR_SELECT_TEXT);
        sxchrome_draw_focus_rect(painter, highlight);
    }
    else
    {
        sx_painter_draw_text(painter, text_x, cy - gfx_text_height() / 2, label, SXGUI_COLOR_TEXT);
    }
}

static void on_paint(struct sxngui_app *app)
{
    struct sx_painter *painter = &app->ui.painter;
    const struct sx_rect inner =
        sx_rect_make(g_tree_rect.x + SXGUI_BORDER_SUNKEN, g_tree_rect.y + SXGUI_BORDER_SUNKEN,
                     g_tree_rect.width - SXGUI_BORDER_SUNKEN * 2, g_tree_rect.height - SXGUI_BORDER_SUNKEN * 2);
    int visible;

    sxchrome_draw_sunken(painter, g_tree_rect);
    sx_painter_fill_rect(painter, inner, SXGUI_COLOR_FIELD);
    if (!sx_painter_push_clip(painter, inner))
    {
        return;
    }
    if (g_visible_count == 0)
    {
        sx_painter_draw_text(painter, inner.x + 8, inner.y + 8, "The hardware inventory could not be read.",
                             SXGUI_COLOR_TEXT);
    }
    for (visible = g_scroll; visible < g_visible_count; ++visible)
    {
        const int y = inner.y + (visible - g_scroll) * DV_ROW_HEIGHT;

        if (y >= inner.y + inner.height)
        {
            break;
        }
        paint_row(painter, visible, y);
    }
    sx_painter_pop_clip(painter);
}

/* ---- dialogs ---------------------------------------------------------------------- */

static int centred_button_x(int width, int count, int index)
{
    const int total = count * SXGUI_BUTTON_WIDTH + (count - 1) * SXGUI_GAP;

    return (width - total) / 2 + index * (SXGUI_BUTTON_WIDTH + SXGUI_GAP);
}

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
    sxgui_dialog_begin(&g_app.ui, &g_message_dialog, 380, 120);
}

/* Lays the current tab's lines out on the page: a name and its value side by
 * side, or a sentence across the whole page. */
static void layout_properties_page(void)
{
    const struct sx_rect page = sxgui_tabs_page(DV_PROPERTIES_TABS);
    int line;

    fill_properties(&g_properties_inventory, g_properties_index, DV_PROPERTIES_TABS->value);
    for (line = 0; line < DV_PROP_LINES; ++line)
    {
        struct sxgui_widget *name = &g_properties_widgets[2 + line * 2];
        struct sxgui_widget *value = &g_properties_widgets[3 + line * 2];
        const int y = page.y + 6 + line * DV_TEXT_ROW;

        if (line >= g_prop_count)
        {
            name->text = "";
            value->text = "";
            continue;
        }
        name->text = g_prop_names[line];
        value->text = g_prop_values[line];
        if (g_prop_names[line][0] == '\0')
        {
            name->rect = sx_rect_make(page.x + 8, y, 0, DV_TEXT_ROW);
            value->rect = sx_rect_make(page.x + 8, y, page.width - 16, DV_TEXT_ROW);
        }
        else
        {
            name->rect = sx_rect_make(page.x + 8, y, DV_PROPERTIES_VALUE_X - SXGUI_GAP, DV_TEXT_ROW);
            value->rect = sx_rect_make(page.x + 8 + DV_PROPERTIES_VALUE_X, y,
                                       page.width - 16 - DV_PROPERTIES_VALUE_X, DV_TEXT_ROW);
        }
    }
}

static void on_properties_tab(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    layout_properties_page();
}

static void show_properties(void)
{
    const struct dv_row *row = selected_row();
    const int height = properties_height();
    char name[DV_NAME_CAPACITY];
    int line;

    if (row == 0 || row->kind != DV_ROW_DEVICE)
    {
        return;
    }
    memcpy(g_properties_copy, g_inventory.devices, sizeof(g_properties_copy[0]) * (size_t)g_inventory.count);
    g_properties_inventory.count = g_inventory.count;
    g_properties_inventory.generation = g_inventory.generation;
    g_properties_index = row->device;
    device_name(&g_properties_inventory, g_properties_index, name, sizeof(name));
    /* The sheet is an owned window, whose request carries
     * SAVANXP_WM_WINDOW_TITLE_CAPACITY - 1 characters; the caption ends in
     * "..." when the frame is narrower than the title. */
    snprintf(g_properties_title, sizeof(g_properties_title), "%s Properties", name);

    *DV_PROPERTIES_TABS = sxgui_tabs(
        sx_rect_make(DV_DIALOG_MARGIN, DV_DIALOG_MARGIN, DV_PROPERTIES_WIDTH - DV_DIALOG_MARGIN * 2,
                     height - DV_DIALOG_MARGIN * 2 - SXGUI_GAP - SXGUI_BUTTON_HEIGHT),
        k_tab_labels, DV_TAB_COUNT, DV_TAB_GENERAL);
    DV_PROPERTIES_TABS->on_action = on_properties_tab;
    *DV_PROPERTIES_OK = sxgui_button(
        sx_rect_make(DV_PROPERTIES_WIDTH - DV_DIALOG_MARGIN - SXGUI_BUTTON_WIDTH,
                     height - DV_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_dialog_ok, 0);
    for (line = 0; line < DV_PROP_LINES; ++line)
    {
        g_properties_widgets[2 + line * 2] = sxgui_label(sx_rect_make(0, 0, 0, 0), "");
        g_properties_widgets[3 + line * 2] = sxgui_label(sx_rect_make(0, 0, 0, 0), "");
    }
    layout_properties_page();

    memset(&g_properties_dialog, 0, sizeof(g_properties_dialog));
    g_properties_dialog.title = g_properties_title;
    g_properties_dialog.widgets = g_properties_widgets;
    g_properties_dialog.widget_count = 2 + DV_PROP_LINES * 2;
    g_properties_dialog.initial_focus = 1;
    g_properties_dialog.default_button = 1;
    sxgui_dialog_begin(&g_app.ui, &g_properties_dialog, DV_PROPERTIES_WIDTH, height);
}

/* ---- commands --------------------------------------------------------------------- */

static void toggle_row(int visible, int expand)
{
    const struct dv_row *row;
    int now;

    if (visible < 0 || visible >= g_visible_count)
    {
        return;
    }
    row = &g_tree[g_visible[visible]];
    if (!row->has_children)
    {
        return;
    }
    now = row_expanded(&g_inventory, row);
    if (expand < 0)
    {
        expand = !now;
    }
    if (expand == now)
    {
        return;
    }
    switch (row->kind)
    {
    case DV_ROW_COMPUTER:
        g_root_collapsed[g_view] = !expand;
        break;
    case DV_ROW_CATEGORY:
        g_category_expanded[row->category] = expand;
        break;
    default:
        set_collapsed(g_inventory.devices[row->device].id, !expand);
        break;
    }
    rebuild();
}

static void set_view(int view)
{
    if (g_view == view)
    {
        return;
    }
    g_view = view;
    g_scroll = 0;
    rebuild();
}

static void on_menu_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case DV_CMD_EXIT:
        sxngui_app_quit(&g_app, 0);
        break;
    case DV_CMD_REFRESH:
        g_inventory.generation = 0u;
        reload();
        break;
    case DV_CMD_PROPERTIES:
        show_properties();
        break;
    case DV_CMD_BY_TYPE:
        set_view(DV_VIEW_TYPE);
        break;
    case DV_CMD_BY_CONNECTION:
        set_view(DV_VIEW_CONNECTION);
        break;
    case DV_CMD_EXPAND:
        toggle_row(g_selected_row, 1);
        break;
    case DV_CMD_COLLAPSE:
        toggle_row(g_selected_row, 0);
        break;
    case DV_CMD_ABOUT:
        show_message("About Device Manager", "Device Manager",
                     "The hardware the kernel found, and the drivers using it.");
        break;
    default:
        break;
    }
    sxngui_app_request_repaint(&g_app);
}

static int build_context_menu(void)
{
    const struct dv_row *row = selected_row();
    int count = 0;

    if (row == 0)
    {
        return 0;
    }
    if (row->has_children)
    {
        const int expanded = row_expanded(&g_inventory, row);

        g_context_items[count].text = expanded ? "Collapse" : "Expand";
        g_context_items[count].id = expanded ? DV_CMD_COLLAPSE : DV_CMD_EXPAND;
        g_context_items[count].flags = 0;
        count += 1;
    }
    g_context_items[count].text = "Refresh";
    g_context_items[count].id = DV_CMD_REFRESH;
    g_context_items[count].flags = 0;
    count += 1;
    if (row->kind == DV_ROW_DEVICE)
    {
        g_context_items[count].text = 0;
        g_context_items[count].id = 0;
        g_context_items[count].flags = 0;
        count += 1;
        g_context_items[count].text = "Properties";
        g_context_items[count].id = DV_CMD_PROPERTIES;
        g_context_items[count].flags = 0;
        count += 1;
    }
    g_context_menu.item_count = count;
    return count;
}

/* The row under (x, y), and whether the point is on its +/- box. */
static int row_at(int x, int y, int *on_expander)
{
    const int inner_y = g_tree_rect.y + SXGUI_BORDER_SUNKEN;
    int visible;

    *on_expander = 0;
    if (!sx_rect_contains_point(g_tree_rect, x, y))
    {
        return -1;
    }
    visible = g_scroll + (y - inner_y) / DV_ROW_HEIGHT;
    if (y < inner_y || visible >= g_visible_count)
    {
        return -1;
    }
    {
        const struct dv_row *row = &g_tree[g_visible[visible]];
        const int cx = column_x(row->depth);

        *on_expander = row->has_children && x >= cx - 6 && x <= cx + 6;
    }
    return visible;
}

static int on_pointer(struct sxngui_app *app, const struct savanxp_gui_pointer_event *event)
{
    const uint32_t pressed = event->buttons & ~g_pointer_buttons;
    const uint32_t released = g_pointer_buttons & ~event->buttons;
    int on_expander = 0;
    int visible;

    g_pointer_buttons = event->buttons;
    if (sxgui_dialog_active(&app->ui) || g_menubar.open_menu >= 0)
    {
        return 0;
    }
    if (!sx_rect_contains_point(g_tree_rect, event->x, event->y))
    {
        return 0;
    }
    if (event->wheel != 0)
    {
        g_scroll -= event->wheel * 3;
        update_scroll();
        sxngui_app_request_repaint(app);
        return 1;
    }
    visible = row_at(event->x, event->y, &on_expander);
    if ((pressed & SAVANXP_MOUSE_BUTTON_LEFT) != 0u && visible >= 0)
    {
        const unsigned long now = sxngui_uptime_ms();
        const int twice = visible == g_last_click_row && now - g_last_click_ms <= SXGUI_DOUBLE_CLICK_MS;

        select_row(&g_inventory, visible);
        update_menus();
        update_status();
        if (on_expander)
        {
            toggle_row(visible, -1);
            g_last_click_row = -1;
        }
        else if (twice)
        {
            g_last_click_row = -1;
            if (g_tree[g_visible[visible]].kind == DV_ROW_DEVICE)
            {
                show_properties();
            }
            else
            {
                toggle_row(visible, -1);
            }
        }
        else
        {
            g_last_click_row = visible;
            g_last_click_ms = now;
        }
        return 1;
    }
    if ((pressed & SAVANXP_MOUSE_BUTTON_RIGHT) != 0u && visible >= 0)
    {
        select_row(&g_inventory, visible);
        update_menus();
        return 1;
    }
    if ((released & SAVANXP_MOUSE_BUTTON_RIGHT) != 0u && visible >= 0 && build_context_menu() > 0)
    {
        sxgui_context_menu_open(&app->ui, &g_context_menu, event->x, event->y, on_menu_command, 0);
        return 1;
    }
    return (pressed | released) != 0u;
}

static void move_selection(int visible)
{
    if (g_visible_count == 0)
    {
        return;
    }
    if (visible < 0)
    {
        visible = 0;
    }
    if (visible >= g_visible_count)
    {
        visible = g_visible_count - 1;
    }
    select_row(&g_inventory, visible);
    ensure_visible();
    update_menus();
    update_status();
}

/* The row of the selection's parent in the tree, for the Left key. */
static int parent_row(int visible)
{
    const int depth = g_tree[g_visible[visible]].depth;

    while (--visible >= 0)
    {
        if (g_tree[g_visible[visible]].depth < depth)
        {
            return visible;
        }
    }
    return -1;
}

static int on_key(struct sxngui_app *app, const struct savanxp_input_event *event)
{
    const struct dv_row *row = selected_row();

    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || sxgui_dialog_active(&app->ui) || g_menubar.open_menu >= 0)
    {
        return 0;
    }
    switch (event->key)
    {
    case SAVANXP_KEY_F5:
        on_menu_command(DV_CMD_REFRESH, 0);
        return 1;
    case SAVANXP_KEY_UP:
        move_selection(g_selected_row - 1);
        return 1;
    case SAVANXP_KEY_DOWN:
        move_selection(g_selected_row + 1);
        return 1;
    case SAVANXP_KEY_HOME:
        move_selection(0);
        return 1;
    case SAVANXP_KEY_END:
        move_selection(g_visible_count - 1);
        return 1;
    case SAVANXP_KEY_PAGE_UP:
        move_selection(g_selected_row - page_rows());
        return 1;
    case SAVANXP_KEY_PAGE_DOWN:
        move_selection(g_selected_row + page_rows());
        return 1;
    case SAVANXP_KEY_LEFT:
        if (row != 0 && row->has_children && row_expanded(&g_inventory, row))
        {
            toggle_row(g_selected_row, 0);
        }
        else if (g_selected_row >= 0)
        {
            const int parent = parent_row(g_selected_row);

            if (parent >= 0)
            {
                move_selection(parent);
            }
        }
        return 1;
    case SAVANXP_KEY_RIGHT:
        if (row != 0 && row->has_children && !row_expanded(&g_inventory, row))
        {
            toggle_row(g_selected_row, 1);
        }
        else if (row != 0 && row->has_children)
        {
            move_selection(g_selected_row + 1);
        }
        return 1;
    case SAVANXP_KEY_ENTER:
        if ((event->modifiers & SAVANXP_KEY_MOD_ALT) != 0u || (row != 0 && row->kind == DV_ROW_DEVICE))
        {
            show_properties();
        }
        else
        {
            toggle_row(g_selected_row, -1);
        }
        return 1;
    case SAVANXP_KEY_MENU:
        if (row != 0 && build_context_menu() > 0)
        {
            const int y = g_tree_rect.y + SXGUI_BORDER_SUNKEN + (g_selected_row - g_scroll) * DV_ROW_HEIGHT +
                          DV_ROW_HEIGHT;

            sxgui_context_menu_open(&app->ui, &g_context_menu, column_x(row->depth) + 24, y, on_menu_command, 0);
        }
        return 1;
    default:
        break;
    }
    if (event->key == SAVANXP_KEY_F10 && (event->modifiers & SAVANXP_KEY_MOD_SHIFT) != 0u)
    {
        if (row != 0 && build_context_menu() > 0)
        {
            sxgui_context_menu_open(&app->ui, &g_context_menu, g_tree_rect.x + 32, g_tree_rect.y + 32,
                                    on_menu_command, 0);
        }
        return 1;
    }
    /* The keypad's + and - and *, as a tree view takes them. */
    if (event->ascii == '+' || event->ascii == '-')
    {
        toggle_row(g_selected_row, event->ascii == '+');
        return 1;
    }
    return 0;
}

static void on_scroll(struct sxgui_widget *widget, void *user)
{
    (void)user;
    g_scroll = widget->value;
}

static void on_resize(struct sxngui_app *app)
{
    (void)app;
    relayout();
    ensure_visible();
}

/* The kernel bumps the generation on every change; the tree is rebuilt only
 * then. */
static void on_tick(struct sxngui_app *app)
{
    if (load_live())
    {
        rebuild();
        sxngui_app_request_repaint(app);
    }
}

/* ---- self-test --------------------------------------------------------------------
 *
 * Headless. First the model against fixtures that hold every state and
 * placement, so the result does not depend on the machine; then the live
 * inventory through both views and every Properties tab. Failures go to the
 * system log; init reads the verdict from the exit status. */

static int g_selftest_failures;

static void selftest_fail(const char *what)
{
    char line[160];

    snprintf(line, sizeof(line), "devmgmt: selftest: %s", what);
    (void)sxn_log(line);
    g_selftest_failures += 1;
}

static void expect(int ok, const char *what)
{
    if (!ok)
    {
        selftest_fail(what);
    }
}

static struct sxn_device_info g_fixture[20];

static struct sxn_device_info *fixture(int index, uint32_t id, uint32_t parent, uint32_t bus, uint32_t device_class,
                                       uint32_t state, uint32_t reason, const char *driver, const char *name)
{
    struct sxn_device_info *device = &g_fixture[index];

    memset(device, 0, sizeof(*device));
    device->id = id;
    device->parent_id = parent;
    device->generation = 7u;
    device->bus = bus;
    device->device_class = device_class;
    device->state = state;
    device->reason = reason;
    snprintf(device->driver, sizeof(device->driver), "%s", driver);
    snprintf(device->name, sizeof(device->name), "%s", name);
    return device;
}

static int build_fixture(void)
{
    struct sxn_device_info *device;

    fixture(0, 1, 0, SXN_DEVICE_BUS_ROOT, SXN_DEVICE_CLASS_COMPUTER, SXN_DEVICE_STARTED, 0, "", "");
    device = fixture(1, 2, 1, SXN_DEVICE_BUS_PCI, SXN_DEVICE_CLASS_SYSTEM, SXN_DEVICE_STARTED, 0, "pci", "");
    device->vendor_id = 0x8086;
    device->class_code = 0x06;
    device->flags = SXN_DEVICE_FIRMWARE;
    device = fixture(2, 3, 1, SXN_DEVICE_BUS_PCI, SXN_DEVICE_CLASS_STORAGE, SXN_DEVICE_STARTED, 0, "ahci", "");
    device->vendor_id = 0x8086;
    device->class_code = 0x01;
    device->subclass = 0x06;
    device->protocol = 0x01;
    /* The ABAR its driver claimed, the line it routed, and a BAR nobody
     * claimed, listed by its base alone. */
    device->resource_count = 3;
    device->resources[0].kind = SXN_DEVICE_RESOURCE_MEMORY;
    device->resources[0].flags = SXN_DEVICE_RESOURCE_CLAIMED;
    device->resources[0].start = 0xfebf0000u;
    device->resources[0].length = 0x800u;
    device->resources[1].kind = SXN_DEVICE_RESOURCE_IRQ;
    device->resources[1].flags = SXN_DEVICE_RESOURCE_CLAIMED | SXN_DEVICE_RESOURCE_SHARED;
    device->resources[1].start = 16;
    device->resources[1].length = 1;
    device->resources[2].kind = SXN_DEVICE_RESOURCE_MEMORY;
    device->resources[2].start = 0xfebf1000u;
    /* An unknown function from an unknown vendor, with nothing recorded. */
    device = fixture(3, 4, 1, SXN_DEVICE_BUS_PCI, SXN_DEVICE_CLASS_UNKNOWN, SXN_DEVICE_UNBOUND,
                     SXN_DEVICE_REASON_NOT_CLAIMED, "", "");
    device->vendor_id = 0xabcd;
    device->class_code = 0xff;
    device = fixture(4, 5, 1, SXN_DEVICE_BUS_PCI, SXN_DEVICE_CLASS_NETWORK, SXN_DEVICE_FAILED,
                     SXN_DEVICE_REASON_START_FAILED, "rtl8168", "RTL8168H");
    device->vendor_id = 0x10ec;
    /* Everything measured: a failed driver keeps what it claimed. */
    device->resource_count = 1;
    device->resources[0].kind = SXN_DEVICE_RESOURCE_IO_PORT;
    device->resources[0].flags = SXN_DEVICE_RESOURCE_CLAIMED;
    device->resources[0].start = 0xc000u;
    device->resources[0].length = 0x100u;
    device = fixture(5, 6, 1, SXN_DEVICE_BUS_PCI, SXN_DEVICE_CLASS_SYSTEM, SXN_DEVICE_STARTED, 0, "pci", "");
    device->class_code = 0x06;
    device->subclass = 0x04;
    device = fixture(6, 7, 6, SXN_DEVICE_BUS_PCI, SXN_DEVICE_CLASS_DISPLAY, SXN_DEVICE_UNBOUND,
                     SXN_DEVICE_REASON_NOT_CLAIMED, "", "");
    device->class_code = 0x03;
    fixture(7, 8, 1, SXN_DEVICE_BUS_PLATFORM, SXN_DEVICE_CLASS_KEYBOARD, SXN_DEVICE_STARTED, 0, "ps2",
            "PS/2 keyboard");
    /* A keyboard on one interface, under the xHCI... here the AHCI's id
     * stands in for a controller: only the parent link matters. */
    device = fixture(8, 9, 3, SXN_DEVICE_BUS_USB, SXN_DEVICE_CLASS_USB_DEVICE, SXN_DEVICE_STARTED, 0, "usb",
                     "Fixture Keyboard");
    device->flags = SXN_DEVICE_REMOVABLE;
    device = fixture(9, 10, 9, SXN_DEVICE_BUS_USB_INTERFACE, SXN_DEVICE_CLASS_KEYBOARD, SXN_DEVICE_STARTED, 0,
                     "usb-hid", "");
    device->class_code = 0x03;
    device->subclass = 0x01;
    device->protocol = 0x01;
    /* A composite device: a tablet that works and a function nobody takes. */
    fixture(10, 11, 3, SXN_DEVICE_BUS_USB, SXN_DEVICE_CLASS_USB_DEVICE, SXN_DEVICE_STARTED, 0, "usb", "");
    device = fixture(11, 12, 11, SXN_DEVICE_BUS_USB_INTERFACE, SXN_DEVICE_CLASS_INPUT, SXN_DEVICE_STARTED, 0,
                     "usb-hid", "");
    device->class_code = 0x03;
    device = fixture(12, 13, 11, SXN_DEVICE_BUS_USB_INTERFACE, SXN_DEVICE_CLASS_UNKNOWN, SXN_DEVICE_UNBOUND,
                     SXN_DEVICE_REASON_NOT_CLAIMED, "", "");
    device->class_code = 0xff;
    fixture(13, 14, 3, SXN_DEVICE_BUS_USB, SXN_DEVICE_CLASS_USB, SXN_DEVICE_STARTED, 0, "usb", "");
    fixture(14, 15, 14, SXN_DEVICE_BUS_USB_INTERFACE, SXN_DEVICE_CLASS_USB, SXN_DEVICE_STARTED, 0, "hub", "");
    /* Kept from its driver by nousb. */
    device = fixture(15, 16, 1, SXN_DEVICE_BUS_PCI, SXN_DEVICE_CLASS_USB, SXN_DEVICE_UNBOUND,
                     SXN_DEVICE_REASON_BOOT_OPTION, "", "");
    device->class_code = 0x0c;
    device->subclass = 0x03;
    device->protocol = 0x30;
    return 16;
}

/* How many visible-or-not rows of the tree show the device at `index`. */
static int rows_of(int index)
{
    int count = 0;
    int tree;

    for (tree = 0; tree < g_tree_count; ++tree)
    {
        if (g_tree[tree].kind != DV_ROW_CATEGORY && g_tree[tree].device == index)
        {
            count += 1;
        }
    }
    return count;
}

static int category_row(int category)
{
    int tree;

    for (tree = 0; tree < g_tree_count; ++tree)
    {
        if (g_tree[tree].kind == DV_ROW_CATEGORY && g_tree[tree].category == category)
        {
            return tree;
        }
    }
    return -1;
}

/* Both views of `inventory` agree with it: every device once, groups right,
 * every parent above its children, every tab of every device filled. */
static void check_views(const struct dv_inventory *inventory, const char *label)
{
    char what[96];
    int index;
    int saved_view = g_view;

    g_view = DV_VIEW_TYPE;
    build_tree(inventory);
    for (index = 0; index < inventory->count; ++index)
    {
        const int category = device_category(inventory, index);
        const int expected = inventory->devices[index].bus == SXN_DEVICE_BUS_ROOT || category >= 0;

        if (rows_of(index) != expected)
        {
            snprintf(what, sizeof(what), "%s: by type lists device %u %d times", label,
                     (unsigned)inventory->devices[index].id, rows_of(index));
            selftest_fail(what);
        }
    }
    g_view = DV_VIEW_CONNECTION;
    build_tree(inventory);
    for (index = 0; index < inventory->count; ++index)
    {
        int tree;
        int at = -1;

        if (rows_of(index) != 1)
        {
            snprintf(what, sizeof(what), "%s: by connection lists device %u %d times", label,
                     (unsigned)inventory->devices[index].id, rows_of(index));
            selftest_fail(what);
            continue;
        }
        for (tree = 0; tree < g_tree_count; ++tree)
        {
            if (g_tree[tree].device == index)
            {
                at = tree;
            }
        }
        if (inventory->devices[index].bus != SXN_DEVICE_BUS_ROOT)
        {
            const int parent = find_device(inventory, inventory->devices[index].parent_id);
            int parent_row = -1;

            for (tree = 0; tree < at; ++tree)
            {
                if (g_tree[tree].device == parent)
                {
                    parent_row = tree;
                }
            }
            if (parent < 0 || parent_row < 0 || g_tree[parent_row].depth + 1 != g_tree[at].depth)
            {
                snprintf(what, sizeof(what), "%s: device %u is not under its parent", label,
                         (unsigned)inventory->devices[index].id);
                selftest_fail(what);
            }
        }
    }
    for (index = 0; index < inventory->count; ++index)
    {
        int tab;
        char name[DV_NAME_CAPACITY];

        device_name(inventory, index, name, sizeof(name));
        if (name[0] == '\0')
        {
            selftest_fail("a device has no name");
        }
        for (tab = 0; tab < DV_TAB_COUNT; ++tab)
        {
            int line;

            fill_properties(inventory, index, tab);
            if (g_prop_count == 0)
            {
                selftest_fail("a Properties tab is empty");
            }
            for (line = 0; line < g_prop_count; ++line)
            {
                if (g_prop_names[line][0] != '\0' && g_prop_values[line][0] == '\0')
                {
                    snprintf(what, sizeof(what), "%s: device %u, \"%s\" has no value", label,
                             (unsigned)inventory->devices[index].id, g_prop_names[line]);
                    selftest_fail(what);
                }
            }
        }
    }
    g_view = saved_view;
}

static int tab_says(const struct dv_inventory *inventory, int index, int tab, const char *text)
{
    int line;

    fill_properties(inventory, index, tab);
    for (line = 0; line < g_prop_count; ++line)
    {
        if (strstr(g_prop_values[line], text) != 0)
        {
            return 1;
        }
    }
    return 0;
}

static int general_says(const struct dv_inventory *inventory, int index, const char *text)
{
    return tab_says(inventory, index, DV_TAB_GENERAL, text);
}

static void selftest_fixtures(void)
{
    struct dv_inventory inventory = {g_fixture, 0, 7u};
    char name[DV_NAME_CAPACITY];
    int visible;

    inventory.count = build_fixture();
    check_views(&inventory, "fixture");

    /* Placement. */
    expect(device_category(&inventory, 3) == DV_CAT_OTHER, "an unknown class goes to Other devices");
    expect(device_category(&inventory, 6) == DV_CAT_DISPLAY, "a display without a driver stays a display");
    expect(device_category(&inventory, 8) == -1, "a one-function USB device shows through its interface");
    expect(device_category(&inventory, 9) == DV_CAT_KEYBOARD, "a USB keyboard is a keyboard");
    expect(device_category(&inventory, 10) == DV_CAT_USB, "a composite USB device is listed under USB");
    expect(device_category(&inventory, 12) == DV_CAT_OTHER, "an unknown USB function goes to Other devices");
    expect(device_category(&inventory, 15) == DV_CAT_USB, "a controller kept by nousb stays a USB controller");

    /* Names: reported, derived from IDs, or said to be unknown; never empty. */
    device_name(&inventory, 9, name, sizeof(name));
    expect(strcmp(name, "Fixture Keyboard") == 0, "a single function takes its device's name");
    device_name(&inventory, 10, name, sizeof(name));
    expect(strcmp(name, "USB composite device") == 0, "a composite device is named so");
    device_name(&inventory, 2, name, sizeof(name));
    expect(strcmp(name, "Intel SATA AHCI controller") == 0, "a PCI function is named from its IDs");
    device_name(&inventory, 3, name, sizeof(name));
    expect(strcmp(name, "PCI device (class ff.00)") == 0, "an unknown PCI function says what it is");
    device_name(&inventory, 4, name, sizeof(name));
    expect(strcmp(name, "RTL8168H") == 0, "a reported name wins");
    device_name(&inventory, 12, name, sizeof(name));
    expect(strstr(name, "interface") != 0, "an unknown USB function says what it is");

    /* States, in words. */
    expect(general_says(&inventory, 2, "working properly"), "started reads as working");
    expect(general_says(&inventory, 4, "cannot start"), "failed reads as failed");
    expect(general_says(&inventory, 3, "No driver"), "unbound reads as no driver");
    expect(general_says(&inventory, 15, "nousb"), "the boot option is named");
    expect(general_says(&inventory, 3, "Not reported"), "an unknown manufacturer says so");
    fill_properties(&inventory, 7, DV_TAB_RESOURCES);
    expect(g_prop_count == 1 && strstr(g_prop_values[0], "no recorded resources") != 0,
           "a device without resources says so");
    fill_properties(&inventory, 2, DV_TAB_RESOURCES);
    expect(g_prop_count >= 3 && strcmp(g_prop_names[0], "Memory range:") == 0 && strcmp(g_prop_names[1], "IRQ:") == 0,
           "recorded resources are listed");
    expect(g_prop_count >= 3 && strcmp(g_prop_values[0], "febf0000-febf07ff") == 0,
           "a claimed range shows where it ends");
    expect(g_prop_count >= 3 && strcmp(g_prop_values[1], "16 (shared)") == 0, "a shared line says so");
    expect(g_prop_count >= 3 && strcmp(g_prop_values[2], "febf1000") == 0, "an unclaimed BAR shows its base");
    expect(tab_says(&inventory, 2, DV_TAB_RESOURCES, "not recorded"), "a base alone is explained");
    fill_properties(&inventory, 4, DV_TAB_RESOURCES);
    expect(g_prop_count == 1 && strcmp(g_prop_values[0], "c000-c0ff") == 0,
           "every range measured: no explanation");

    /* Groups with a problem start open; the rest closed. */
    memset(g_category_seen, 0, sizeof(g_category_seen));
    g_view = DV_VIEW_TYPE;
    build_tree(&inventory);
    expect(g_category_expanded[DV_CAT_NETWORK] && g_category_expanded[DV_CAT_OTHER] &&
               !g_category_expanded[DV_CAT_STORAGE] && !g_category_expanded[DV_CAT_KEYBOARD],
           "groups with a problem start open");
    expect(category_row(DV_CAT_DISK) < 0, "an empty group is not listed");

    /* The selection follows the id through a rebuild, and never moves to
     * another device that took the removed one's place. */
    g_category_expanded[DV_CAT_HID] = 1;
    build_tree(&inventory);
    visible = find_visible(&inventory, DV_ROW_DEVICE, 12u, 0);
    expect(visible >= 0, "the tablet is visible in an open group");
    select_row(&inventory, visible);
    build_tree(&inventory);
    restore_selection(&inventory);
    expect(selected_row() != 0 && selected_row()->kind == DV_ROW_DEVICE &&
               inventory.devices[selected_row()->device].id == 12u,
           "the selection survives a rebuild");

    g_fixture[11].id = 99u; /* the tablet is gone; a new instance sits in its slot */
    g_fixture[11].device_class = SXN_DEVICE_CLASS_UNKNOWN;
    build_tree(&inventory);
    restore_selection(&inventory);
    expect(g_selection_removed && selected_row() != 0 &&
               (selected_row()->kind != DV_ROW_DEVICE || inventory.devices[selected_row()->device].id != 99u),
           "a removed selection never moves to the instance that took its slot");

    g_view = DV_VIEW_CONNECTION;
    build_fixture();
    build_tree(&inventory);
    visible = find_visible(&inventory, DV_ROW_DEVICE, 13u, 0);
    select_row(&inventory, visible);
    g_fixture[12].id = 98u;
    build_tree(&inventory);
    restore_selection(&inventory);
    expect(g_selection_removed && selected_row() != 0 && selected_row()->kind == DV_ROW_DEVICE &&
               inventory.devices[selected_row()->device].id == 11u,
           "by connection, a removed device selects the one it hung from");

    /* Collapsing hides the branch and selects nothing new. */
    build_fixture();
    set_collapsed(3u, 1);
    build_tree(&inventory);
    expect(find_visible(&inventory, DV_ROW_DEVICE, 9u, 0) < 0 && find_visible(&inventory, DV_ROW_DEVICE, 3u, 0) >= 0,
           "a collapsed device hides its children");
    set_collapsed(3u, 0);
    build_tree(&inventory);
    expect(find_visible(&inventory, DV_ROW_DEVICE, 10u, 0) >= 0, "expanding shows them again");
    g_view = DV_VIEW_TYPE;
    memset(g_category_seen, 0, sizeof(g_category_seen));
    g_select_kind = 0;
}

static void selftest_live(void)
{
    char line[96];
    int index;
    int usb_function = 0;

    (void)load_live();
    expect(g_inventory.count >= 2, "the live inventory has the computer and something on it");
    expect(!g_truncated, "the live inventory fits");
    check_views(&g_inventory, "live");
    for (index = 0; index < g_inventory.count; ++index)
    {
        const struct sxn_device_info *device = &g_inventory.devices[index];

        if (device->bus == SXN_DEVICE_BUS_USB_INTERFACE && device->state == SXN_DEVICE_STARTED)
        {
            const int category = device_category(&g_inventory, index);

            usb_function = 1;
            expect(category == DV_CAT_KEYBOARD || category == DV_CAT_MOUSE || category == DV_CAT_HID ||
                       category == DV_CAT_DISK || category == DV_CAT_USB,
                   "a working USB function is listed in its group");
        }
    }
    snprintf(line, sizeof(line), "devmgmt: selftest: %d devices, USB functions %s", g_inventory.count - 1,
             usb_function ? "listed" : "absent");
    (void)sxn_log(line);
}

static int selftest(int need_usb)
{
    g_selftest_failures = 0;
    selftest_fixtures();
    selftest_live();
    if (need_usb)
    {
        int index;
        int found = 0;

        for (index = 0; index < g_inventory.count; ++index)
        {
            if (g_inventory.devices[index].bus == SXN_DEVICE_BUS_USB_INTERFACE &&
                g_inventory.devices[index].state == SXN_DEVICE_STARTED)
            {
                found = 1;
            }
        }
        expect(found, "the scenario's USB devices are listed with their drivers");
    }
    (void)sxn_log(g_selftest_failures == 0 ? "devmgmt: selftest passed" : "devmgmt: selftest failed");
    return g_selftest_failures == 0 ? 0 : 1;
}

/* ---- start -------------------------------------------------------------------------- */

static void build_dialog_widgets(void)
{
    g_message_widgets[0] = sxgui_label(sx_rect_make(DV_DIALOG_MARGIN, DV_DIALOG_MARGIN, 380 - DV_DIALOG_MARGIN * 2,
                                                    DV_TEXT_ROW),
                                       g_message_line);
    g_message_widgets[1] = sxgui_label(sx_rect_make(DV_DIALOG_MARGIN, DV_DIALOG_MARGIN + DV_TEXT_ROW + 2,
                                                    380 - DV_DIALOG_MARGIN * 2, DV_TEXT_ROW),
                                       g_message_detail);
    g_message_widgets[2] = sxgui_button(sx_rect_make(centred_button_x(380, 1, 0),
                                                     120 - DV_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT, SXGUI_BUTTON_WIDTH,
                                                     SXGUI_BUTTON_HEIGHT),
                                        "OK", on_dialog_ok, 0);
}

static int run_window(void)
{
    int result;

    *DV_SCROLL = sxgui_scrollbar(sx_rect_make(0, 0, 0, 0), 0, 0, 1, 0);
    DV_SCROLL->on_action = on_scroll;
    *DV_STATUS = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status);
    DV_STATUS->flags |= SXGUI_FLAG_SUNKEN;
    *DV_STATUS_RIGHT = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status_right);
    DV_STATUS_RIGHT->flags |= SXGUI_FLAG_SUNKEN;
    build_dialog_widgets();

    result = sxngui_app_init(&g_app, &sun_resources, g_widgets, DV_WIDGET_COUNT);
    if (result < 0)
    {
        (void)sxn_log("devmgmt: no window from windowd");
        return 1;
    }
    g_menubar.menus = k_menus;
    g_menubar.menu_count = (int)(sizeof(k_menus) / sizeof(k_menus[0]));
    g_menubar.on_command = on_menu_command;
    sxgui_set_menubar(&g_app.ui, &g_menubar);

    (void)sxngui_app_set_content_size(&g_app, DV_DEFAULT_WIDTH, DV_DEFAULT_HEIGHT);
    relayout();
    g_inventory.generation = 0u;
    reload();

    g_app.on_paint = on_paint;
    g_app.on_key = on_key;
    g_app.on_pointer = on_pointer;
    g_app.on_resize = on_resize;
    g_app.on_tick = on_tick;
    g_app.tick_interval_ms = 1000ul;
    return sxngui_app_run(&g_app);
}

int main(int argc, char **argv)
{
    mime_icon_set_reader(read_icon_file);
    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        sxn_exit(selftest(argc > 2 && argv[2] != 0 && strcmp(argv[2], "usb") == 0));
    }
    sxn_exit(run_window());
    return 0;
}
