#pragma once

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "desktop_icons.h"

/*
 * Program Manager's program registry (A2.3, see docs/desktop/WM_SUBSYSTEM.md).
 *
 * It replaces the hard-coded k_menu_items table of windowd_menu.c: the groups
 * and items live in a text file on the disk, editable without recompiling the
 * OS. The format follows the idea of the original PROGMAN.INI: [group]/[item]
 * sections with key=value pairs.
 *
 *     # comment (';' too)
 *     [group]
 *     name=Main
 *
 *     [item]
 *     name=Doom
 *     path=/bin/doomgeneric
 *     desc=Classic FPS test port
 *     icon=/bin/doomgeneric
 *     flags=fullscreen
 *
 * Parser rules:
 *   - Unknown keys and sections are ignored (forward compatibility).
 *   - An item without a path, or with a path not starting at '/', is dropped.
 *   - Items declared before the first [group] fall into an implicit group.
 *   - It tolerates CRLF and spaces around keys and values.
 *   - No malloc (this libc has none): everything fits in fixed-capacity arrays
 *     and strings are truncated instead of overflowing.
 *   - No system call: reading and listing go through shell/shell_io.h.
 *
 * SINCE SXE PHASE 3 (docs/userland/SXE_FORMAT.md) this file changed role: it
 * is NO LONGER the catalog of the programs' identity, it is the user's
 * ARRANGEMENT. Which groups there are, what goes in each, in what order, and
 * one-off overrides. What a program is called, what it does and which icon it
 * has is said by the binary itself, through its .sxmeta/.sxicon section.
 *
 * Each field's precedence, from highest to lowest:
 *
 *   1. The key written in the .ini      -- what the USER decided
 *   2. The binary's .sxmeta/.sxicon     -- what the PROGRAM declares of itself
 *   3. The baked-in default below       -- safety net
 *   4. Generic (desktop icon)           -- last resort
 *
 * Step 2 is applied by progman_registry_apply_sxe(). Binaries without
 * resources -- still first-class executables -- simply skip that step.
 *
 * AND THE FILE IS NO LONGER THE ONLY SOURCE OF ITEMS.
 * progman_registry_scan_programs() walks /bin and /system, adding each executable that
 * declares SXE_TAG_CATEGORY, which is how a program asks to appear in the
 * menu. The .ini stopped being required for something to show: it is the
 * ARRANGEMENT (which groups, in what order, what to rename) on top of a
 * catalog that discovers itself. Installing is copying the binary;
 * uninstalling is deleting it.
 *
 * `icon=` in an [item] NO LONGER picks from a catalog baked in by name: it
 * points at a PROGRAM whose .sxicon to borrow (icon=/bin/notepad). The
 * referenced binary does not have to be the one this item launches -- asking
 * for another program's icon is exactly the use case, e.g. a second entry for
 * the same executable under another name. Short names from before this change
 * (shell, notepad, gfxdemo, keytest, mousetest) still work for compatibility:
 * they resolve to the PATH of the program that draws that icon today, no
 * longer to a baked-in id. "desktop" and any name without an alias fall to the
 * generic icon, as does a path that cannot be read.
 */

#define PROGMAN_MAX_GROUPS 8
#define PROGMAN_MAX_ITEMS 48
#define PROGMAN_NAME_CAPACITY 32
#define PROGMAN_DESC_CAPACITY 64
#define PROGMAN_PATH_CAPACITY SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY
#define PROGMAN_REGISTRY_NAME "progman.ini" /* in the profile: shell_io_settings_path */
/* The most of the file read into a static buffer. */
#define PROGMAN_REGISTRY_MAX_BYTES 8192

enum progman_registry_source
{
    /* Baked-in defaults: neither the file nor the scan gave a single item. */
    PROGMAN_REGISTRY_SOURCE_DEFAULTS = 0,
    PROGMAN_REGISTRY_SOURCE_FILE = 1,
    /* There was no usable .ini and the disk scan built the catalog. */
    PROGMAN_REGISTRY_SOURCE_SCAN = 2,
};

/*
 * The fields the .ini declared EXPLICITLY. Without this "the user chose this
 * name" cannot be told from "the default was left", and the .sxe would
 * overwrite the user's decisions or the other way round.
 */
#define PROGMAN_OVERRIDE_NONE 0x00000000u
#define PROGMAN_OVERRIDE_NAME 0x00000001u
#define PROGMAN_OVERRIDE_DESCRIPTION 0x00000002u
#define PROGMAN_OVERRIDE_ICON 0x00000004u
#define PROGMAN_OVERRIDE_FLAGS 0x00000008u

/*
 * sxe_state of an item. The scan reads every candidate's .sxmeta anyway, so
 * for the items it adds it also takes the .sxicon from the same open file and
 * marks them DONE; progman_registry_apply_sxe() then leaves them alone instead
 * of opening the binary a second time. TOUCHED records that the binary gave
 * the item something, which apply_sxe still counts.
 */
#define PROGMAN_SXE_PENDING 0
#define PROGMAN_SXE_DONE 1
#define PROGMAN_SXE_DONE_TOUCHED 2

/* icon_slot when the binary brought no icon of its own: icon_id is used. */
#define PROGMAN_ICON_SLOT_NONE (-1)

struct progman_item
{
    char name[PROGMAN_NAME_CAPACITY];
    char path[PROGMAN_PATH_CAPACITY];
    char description[PROGMAN_DESC_CAPACITY];
    /* Empty unless icon= resolved to a program (its own or a legacy alias's):
     * then apply_sxe reads the .sxicon of THIS path instead of `path`'s.
     * Empty with PROGMAN_OVERRIDE_ICON set = a name without an alias (e.g.
     * "desktop") -- it stays at the generic icon_id, reading nothing. */
    char icon_borrow_path[PROGMAN_PATH_CAPACITY];
    uint32_t icon_id;      /* enum desktop_icon_id; last resort when nothing else resolved */
    uint32_t launch_flags; /* SAVANXP_DESKTOP_LAUNCH_FLAG_* */
    uint32_t overrides;    /* PROGMAN_OVERRIDE_* */
    int icon_slot;         /* index in the .sxicon icon pool, or NONE */
    int group_index;
    int sxe_state;         /* PROGMAN_SXE_*: whether its resources were already read */
};

struct progman_group
{
    char name[PROGMAN_NAME_CAPACITY];
    int item_count;
};

/*
 * Loads the registry from PROGMAN_REGISTRY_NAME. Returns how many items the
 * file contributed; if it is missing, does not fit in the buffer or holds no
 * valid item, returns 0 and LEAVES THE REGISTRY EMPTY.
 *
 * This function used to fall back to the built-in defaults on its own. Not
 * any more: with the scan (progman_registry_scan_programs) the defaults became
 * the LAST-resort safety net, and they only make sense after the scan ALSO
 * came back empty-handed. The caller -- progman.c -- runs that sequence,
 * because it is the only one that can choose between the two sources with the
 * disk in front of it.
 */
int progman_registry_load_file(void);
/* Loads only the baked-in defaults (fresh installation / recovery). */
void progman_registry_load_defaults(void);
/* Parses from memory. Returns how many valid items were loaded. It is the
 * entry point the selftest uses to exercise the parser without a disk. */
int progman_registry_parse(const char *text, size_t length);

/*
 * An existence predicate, injectable so the pruning can be tested without a
 * disk: progman passes the one that looks at the path, the selftest a fake.
 */
typedef int (*progman_path_exists_fn)(const char *path);

/*
 * Drops the items whose path does not exist and, with them, the groups left
 * empty; the surviving items are pointed at the new indices. Returns how many
 * items were dropped.
 *
 * It is a stage SEPARATE from parsing on purpose: parsing is a pure function
 * of the text (and the selftest exercises it that way), and this is the
 * policy that touches the disk. It applies alike to the baked-in defaults and
 * to what comes from the .ini: an entry that cannot be started is noise
 * wherever it comes from.
 *
 * This CAN leave the registry empty -- if there really is nothing to start,
 * showing nothing is the honest thing. The scan runs afterwards and gets its
 * chance.
 */
int progman_registry_prune_missing(progman_path_exists_fn exists);

/*
 * Fills in the items with what their binary's .sxmeta/.sxicon declares,
 * without overwriting what the .ini declared explicitly. Each binary is
 * opened once for both sections. Items the scan already resolved
 * (PROGMAN_SXE_DONE) are not read again. Returns how many items took
 * something from their executable, those included.
 *
 * It goes AFTER progman_registry_prune_missing(): there is no point opening
 * the binary of an item about to be dropped.
 *
 * A binary without resources, one that does not open, or one with an invalid
 * blob is not an error: that item simply keeps its previous values.
 */
int progman_registry_apply_sxe(void);

/*
 * The directory the scan walks, the same one file_assoc (file_assoc.h) scans.
 * System programs and installed ones share it since the disk became the root,
 * so each program appears once.
 */
#define PROGMAN_SCAN_DIR "/bin"

/*
 * "All Programs": adds to the registry every installed executable that ASKS
 * to appear, that is, one that declares SXE_TAG_CATEGORY in its .sxmeta. The
 * category is the group name; the rest of the identity (name, description,
 * icon, flags) is filled in by progman_registry_apply_sxe(), as for any other
 * item. Returns how many items it added.
 *
 * The category being OPT-IN is the design decision: the program asks to be in
 * the menu, as in the XP era its installer did by creating a shortcut. The
 * opposite -- listing everything stamped -- would need a baked-in exclusion
 * list for the command-line tools (ls, cat, cp, ...) and the diagnostic
 * binaries, and that list drifts the moment someone adds a new *test.
 *
 * Rules:
 *   - It overrides NOTHING already in the registry: if an existing item
 *     already points at a binary with the same BASENAME, the candidate is
 *     skipped. So the user's .ini always wins.
 *   - The items it adds carry no overrides, so their presentation comes
 *     from the binary itself. The scan fills it in right away, from the file
 *     it already has open, and marks them PROGMAN_SXE_DONE.
 *   - It runs AFTER progman_registry_prune_missing() and BEFORE
 *     progman_registry_apply_sxe(): pruning reorders items, and apply_sxe
 *     assigns icon slots against the final indices.
 *   - The groups it creates and the items it adds are sorted alphabetically,
 *     after the ones already there. Listing order is the image's order, not
 *     one that helps anybody find something in a list.
 *
 * `exists` is the same injectable predicate as the pruning's. With 0 it uses
 * its own.
 */
int progman_registry_scan_programs(progman_path_exists_fn exists);

/*
 * A hash of everything on disk the catalog is built from: the .ini, the name,
 * size and mtime of every entry in PROGMAN_SCAN_DIR, and the paths the current
 * catalog points at (items and borrowed icons). It reads only metadata, never
 * a file's contents, so it costs a directory listing plus one stat per entry
 * and no device reads. A caller that keeps the catalog between uses (the
 * Start menu) compares it with the value taken right after building, and
 * rebuilds only when it differs.
 *
 * It cannot see a binary rewritten in place within the same second with the
 * same size: SxFS mtimes are whole seconds. See docs/roadmaps/open/SXFS_ROADMAP.md.
 */
uint32_t progman_registry_disk_signature(void);

/* How many executables the last scan opened. The smoke reports it: it is the
 * number to look at before deciding whether a cache is needed. */
int progman_registry_scan_examined(void);

/*
 * The item's own icon, taken from its .sxicon. Returns 0 when the binary
 * brought none; the caller then falls back to desktop_icon_large(item->icon_id).
 */
const struct desktop_embedded_bitmap *progman_item_icon(const struct progman_item *item);

/*
 * The item's own 16x16 icon, when its .sxicon carries one at exactly that
 * size. Returns 0 otherwise; the caller then shrinks progman_item_icon().
 */
const struct desktop_embedded_bitmap *progman_item_icon_small(const struct progman_item *item);

int progman_registry_source(void);
int progman_group_count(void);
const struct progman_group *progman_group_at(int index);
int progman_item_count(void);
const struct progman_item *progman_item_at(int index);
/* The i-th item within a group (the items keep their group_index). */
const struct progman_item *progman_group_item_at(int group_index, int item_index);

/* Checks the parser, the defaults, the icon/flag mapping, truncation and the
 * capacity limits. Returns 0 when all pass. */
int progman_registry_selftest(void);
