# Files: the file browser

`/bin/filesapp` ("Files") is the in-tree file browser, shaped after the
Windows 98/2000 Explorer without its web-view side panel. Source in
`subsystems/posix/userland/filesapp.c`, built as PIE against `libgfx2d`,
`libsxgui` and `libsxgfx`, with `file_assoc.c` (which program opens a file),
`mime_icon.c` and `mime_icon_large.c` (which icon a file gets) and
`shortcut.c` (what "open" means, shared with the desktop).

This document pins how its views work and why, and what is planned next.
Opening files and the context menu are covered in
[`DESKTOP_ICONS.md`](DESKTOP_ICONS.md#phase-4-context-menus-in-applications)
and [`../userland/SXE_FORMAT.md`](../userland/SXE_FORMAT.md) (phase 5).

## Views

The folder is shown in one of two views, picked from the View menu (the
checked item is the current one):

- **Large Icons** — the default, as Windows 98's Explorer opens a folder. A
  grid of 32x32 icons with the name under each, in up to two lines. It has
  no ".." entry, as Explorer's icon view has none: Up One Level and
  Backspace go up.
- **Details** — the Name / Size / Type list with 16x16 icons and column
  headers, with ".." first outside the root.

In both, a folder opens with **nothing selected**, a click on the
background clears the selection, and the first arrow key selects the first
item (End the last). Enter, Delete and the keyboard's context-menu key do
nothing for an item when none is selected; the latter opens the folder's
menu instead.

The choice is saved in the user's profile as `filesapp.cfg` (one line,
`view=icons` or `view=details`; anything else is Large Icons) and applies
to every Files window opened afterwards. A disk that refuses the write only
loses the preference.

### One widget, two kinds

Both views are the same `sxgui_widget`: Files changes its `kind` between
`SXGUI_LISTBOX` and `SXGUI_ICONVIEW` and points `item_icons` at the array of
the matching size. The icon view reads the listbox's own fields:

| Field | Listbox | Icon view |
|---|---|---|
| `items` | the rows, cells split by `SXGUI_COLUMN_SEPARATOR` | only the first cell (the name) |
| `item_icons` | 16x16, `SXGUI_LISTBOX_ICON_SIZE` | 32x32, `SXGUI_ICONVIEW_ICON_SIZE` |
| `value` | selected row | selected item |
| `scroll` | first visible row | first visible row **of the grid** |

The selection may be empty (`value` -1) because Files sets
`SXGUI_FLAG_EMPTY_SELECTION` on the widget. The flag is opt-in so that the
other listboxes (Task Manager's, for one) keep a selection always.

The widget's items are `g_entries`, except that Large Icons leaves out
"..". Since ".." is always entry 0 when present, the icon view's item *i*
is entry *i* + 1: Files points `items` and `item_icons` one past the start
of its arrays and converts with `filesapp_entry_of` / `filesapp_item_of`.
Everything keyed on an entry (Open, the context menu, Rename, Delete) goes
through that conversion, so it does not depend on the view. On a switch the
selected entry stays selected if the new view shows it (a selected ".."
ends with nothing selected in Large Icons), and `scroll` is reset to 0
because its unit changes.

Two reasons for a new widget kind rather than a grid drawn inside Files, as
Program Manager and Paint draw theirs:

- `struct sxgui_widget` is shared by every program through `libsxgui`, and
  its layout must not change. A new `kind` appended to `enum sxgui_kind`
  reusing existing fields adds nothing to the struct.
- Hit testing, keyboard, wheel, the embedded scrollbar and focus come from
  the toolkit, exactly as for the listbox, instead of being rewritten in the
  app.

### The icon view's grid

The cell is the desktop's (`SXGUI_ICONVIEW_CELL_WIDTH` = 76 px, the same as
`VIEW_CELL_WIDTH` in `desktop_view.c`), and the name wraps the same way: at
most two lines of 72 px, cut at a space when one fits, a word too long cut
by characters, `...` when the rest does not fit. A folder therefore looks
alike in Files and on the desktop. The cell height follows the font (two
label lines).

- Items flow left to right, then down; the number of columns follows the
  window's width.
- The scrollbar appears when the rows do not fit **without** it. The bar
  narrows the grid, which can only add rows, so the decision cannot flip
  back once the bar is there.
- A selected item has its icon tinted half and half with the selection
  colour (as the desktop and Windows draw it) and its name on the selection
  colour, with the focus rectangle around the name while the view has focus.
- Only the icon and the name are hit targets; the rest of the cell is
  background. A click there clears the selection (with
  `SXGUI_FLAG_EMPTY_SELECTION`; without it the selection stays), and a right
  click there opens the folder's menu.
- Keys: arrows move in two dimensions (Down from a row with nothing under
  it stays put; Up from the first row does not wrap), Page Up/Down move a
  screenful of rows, Home/End go to the ends, Enter opens. The wheel moves
  one grid row per tick, not the listbox's three rows.

`sxgui_listbox_row_at` answers the item under a point for both kinds, and
`sxgui_iconview_item_rect` gives an item's cell, which Files uses to open
the keyboard's context menu (Shift+F10, Menu key) under the selected icon.

## Icons by size and their RAM

`mime_icon` resolves a file to an icon name through `/mimeicon.ini` and
loads the pixels from `/icons/<name>.sxicon`, which carries both 16x16 and
32x32. Each size has its own cache of `MIME_ICON_CACHE_ENTRIES` slots with no
eviction (the catalogue fits by design):

| Cache | Per icon | Total BSS | Where |
|---|---|---|---|
| 16x16 (`mime_icon_small`) | 1 KiB | 12 KiB | `mime_icon.c` |
| 32x32 (`mime_icon_large`) | 4 KiB | 48 KiB | `mime_icon_large.c` |

The large cache is in a file of its own on purpose. The desktop links
`mime_icon.c` for the icon *names* but draws its own 32x32 copies, and BSS
is resident RAM per process. `--gc-sections` cannot drop the large cache
from it: a PIE program links with `--export-dynamic`, so every global
function is exported and kept, along with everything it references. Only
programs that draw large icons list `mime_icon_large.c` in their sources.

## Verification

- `./build.sh smoke filesapp-smoke` runs `filesapp --selftest`: the
  association and icon policies, and probes that resolve both icon sizes
  (`FILESAPP SMOKE mimeicon ... resolved=N large=N`).
- `./tools/shoot.sh --scenario files` opens Files (Large Icons), moves the
  selection, switches to Details and asserts the column headers, then opens
  `/bin` as Large Icons and presses End, which must scroll the grid.
- `./tools/shoot.sh --scenario filesmenu` right-clicks an icon's name for its
  context menu, then switches to Details to measure the folder menu against
  the Type column.

The visual scenarios assume a 1280x800 session: their pointer mapping and
text search are sized for it. A disk image whose profile saved another
resolution (`display.cfg`) has to be run without that file.

## Roadmap

Toward the Windows 98/2000 Explorer, in this order. The web-view side panel
is out of scope for now.

1. **Large Icons view** — done (this document).
2. **Toolbar** — a flat toolbar with icon-over-text buttons (Back, Forward,
   Up, Delete, Properties, Views), hover relief, separators and a disabled
   state. Needs a toolbar widget in SXGUI and a set of toolbar glyphs drawn
   to [`ICON_STYLE.md`](ICON_STYLE.md). Back/Forward is a navigation
   history; Properties is a small dialog (size, type, location). An editable
   address bar fits here too.
3. **Cut / Copy / Paste** — the SXGUI clipboard holds text only, so file
   paths need either a tagged text payload or a clipboard of their own; and
   copying needs a recursive copy with name clashes ("Copy of ...") and
   error handling. Moving within one volume is `rename`.

Undo is left out: it needs an operation log, and a deleted file cannot come
back without a Recycle Bin.
