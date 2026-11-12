# Files: the file browser

`/bin/filesapp` ("Files") is the in-tree file browser, shaped after the
Windows 98/2000 Explorer without its web-view side panel. Source in
`subsystems/posix/userland/filesapp.c`, built as PIE against `libgfx2d`,
`libsxgui` and `libsxgfx`, with `file_assoc.c` (which program opens a file),
`system/gfx/mime_icon.c` and `mime_icon_large.c` (which icon a file gets,
shared with the SUN file dialog, with `mime_icon_posix.c` as their POSIX
file reader) and
`shortcut.c` (what "open" means, shared with the desktop). The toolbar
glyphs are drawn by `system/gfx/toolbar_glyphs/gen_icons.py`, shared with the file dialog.

This document pins how its window, views and commands work and why, and
what is planned next.
Opening files and the context menu are covered in
[`DESKTOP_ICONS.md`](DESKTOP_ICONS.md#phase-4-context-menus-in-applications)
and [`../userland/SXE_FORMAT.md`](../userland/SXE_FORMAT.md) (phase 5).

## The window

Under the menu bar (File, View, Go, Help) come three bands, each ruled off
by an etched line as in the Explorer:

1. **Toolbar** — Back, Forward | Up | Delete, Properties | Views.
2. **Address bar** — the folder's path, editable.
3. **The folder** — as Large Icons or Details, down to the two-pane status
   bar (object count and total size).

### Commands

Each command is in a menu and most are on the toolbar too. Both are enabled
from one function (`filesapp_update_commands`), run whenever the folder, the
history or the selection changes, so a grey button and a grey menu item never
disagree.

| Command | Toolbar | Menu | Keys | Enabled when |
|---|---|---|---|---|
| Back | Back | Go | Alt+Left | there is a folder to go back to |
| Forward | Forward | Go | Alt+Right | after Back |
| Up One Level | Up | Go, folder's context menu | Backspace | not at the root |
| Delete | Delete | File, item's context menu | Delete | an item is selected (not "..") |
| Rename... | — | File, item's context menu | — | an item is selected (not "..") |
| Properties | Properties | File, both context menus | Alt+Enter | always |
| Views | Views (drops down) | View | — | always |
| Refresh | — | View, folder's context menu | F5 | always |

**History.** Going to a folder (opening one, Up, the address bar) pushes the
folder left on Back and clears Forward, as a browser does; Back and Forward
move one folder between the two. Refresh, Rename and Delete reload in place
and record nothing. Each side keeps `FILESAPP_HISTORY_DEPTH` (16) folders and
drops the oldest. A folder that no longer exists is dropped when Back or
Forward reaches it.

**Address bar.** A text field holding its own copy of the path
(`g_address`); every folder change writes the current path back. Enter goes
to what it holds, absolute or relative to the folder shown, ignoring trailing
slashes, and returns the focus to the folder; a path that does not open says
so in the status bar and leaves the text for correcting. Esc puts the path
back instead of closing the window. While it has the focus, Backspace and
Delete edit its text instead of going up or deleting a file.

**Properties.** Of the selected item, or of the folder itself when nothing
is selected, as Explorer's File > Properties does: name, type, location and
size, and for a folder what it contains. A folder's size and contents count
only what is directly in it, not its subfolders: a recursive walk could take
long on a big tree, and the window would hang meanwhile.

### Toolbar widgets

The toolbar is ordinary SXGUI widgets with flags, so `struct sxgui_widget`
keeps its layout:

- `sxgui_toolbar_button` — a `SXGUI_BUTTON` with `SXGUI_FLAG_TOOLBAR`: flat,
  a one-pixel raised edge under the pointer and a sunken one while pressed,
  its 20x20 glyph (`SXGUI_TOOLBAR_ICON_SIZE`, read from `item_icons[0]`) over
  its caption. Disabled, the glyph is drawn as its embossed silhouette, as
  Windows draws a toolbar's disabled image from its mask, so each glyph must
  read by its shape alone. `SXGUI_FLAG_DROPDOWN` adds the small arrow of a
  button that opens a menu (Views opens it with `sxgui_context_menu_open`).
- A toolbar button never takes the focus, and pressing one leaves the focus
  where it was: Delete acts on the selection of the list that keeps the
  focus.
- `sxgui_separator` — a label with `SXGUI_FLAG_SEPARATOR`: an etched line,
  vertical between button groups and horizontal between bands.

All buttons share the widest one's width (`sxgui_toolbar_button_width`), as
in the Explorer. Buttons that do not fit a narrow window are hidden, not
squeezed.

The glyphs are embedded rather than shipped as `.sxicon` files, like the
media player's: six 20x20 images are about 9 KiB, and nothing else uses them.

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
| 16x16 (`mime_icon_small`) | 1 KiB | 12 KiB | `system/gfx/mime_icon.c` |
| 32x32 (`mime_icon_large`) | 4 KiB | 48 KiB | `system/gfx/mime_icon_large.c` |

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
- `./tools/shoot.sh --scenario filestoolbar` checks that Back starts
  disabled, types `/bin` in the address bar, then follows Back, Forward and
  Up by their toolbar buttons, opens the folder's Properties, and picks
  Details from the Views button.
- `./tools/shoot.sh --scenario filesmenu` right-clicks an icon's name for its
  context menu, then switches to Details to measure the folder menu against
  the Type column.

The visual scenarios assume a 1280x800 session: their pointer mapping and
text search are sized for it. A disk image whose profile saved another
resolution (`display.cfg`) has to be run without that file.

## Roadmap

Toward the Windows 98/2000 Explorer, in this order. The web-view side panel
is out of scope for now.

1. **Large Icons view** — done.
2. **Toolbar, address bar, history and Properties** — done. Not yet: the
   drop-down lists of recent folders on Back and Forward, and the address
   bar's drop-down of parent folders.

### Pending

Not started. In the order they depend on each other:

3. **A clipboard with formats.** The clipboard is `/dev/clipboard`
   ([`WM_SUBSYSTEM.md`](WM_SUBSYSTEM.md)), one blob of text read and written
   through the toolkit's host (`clipboard_get_text`/`clipboard_set_text`).
   Copying files needs a second kind of content, a list of paths, that a
   program can tell apart from text, as Windows' `CF_HDROP` sits next to
   `CF_TEXT`. Two ways:
   - **A tagged text payload** (a marker line, then one path per line).
     Nothing changes outside Files, but any other program pastes the tag as
     text, and nothing stops a text that happens to start with it.
   - **Formats in the clipboard itself**: the device keeps one entry per
     format, a writer replaces them all at once, a reader asks for the one it
     wants. A file list also offers its paths as text, so Notepad pastes
     them. This is the one to build; it touches the device, the host
     interface and the SUN host ([`SUN.md`](../userland/SUN.md)).
4. **Cut / Copy / Paste.** On the clipboard above. Paste copies into the
   folder shown; copying a folder is a recursive copy, a name already taken
   becomes "Copy of ..." (and "Copy (2) of ..."), and a failure halfway
   reports what was left behind instead of rolling back silently. Cut moves:
   `rename` within one volume, copy-then-delete across volumes. A cut item
   is drawn faded until it is pasted, as in the Explorer. The toolbar gets
   Cut, Copy and Paste between Up and Delete, and the Edit menu returns.
5. **Recycle Bin.** Delete moves to a per-user bin instead of removing:
   the item goes into a folder in the profile together with a record of its
   original path and the time, so it can be put back. The bin needs a view
   (Restore, Empty Recycle Bin, Delete for good) and an icon on the desktop
   that shows whether it is empty. Moving to it across volumes is the copy
   of step 4. With it, deleting a folder that is not empty stops being
   refused ([`DESKTOP_ICONS.md`](DESKTOP_ICONS.md) refuses it today because
   nothing could bring it back), on the desktop and in Files alike.

**Undo** comes after both: it needs an operation log (what was moved,
renamed or deleted, from where), and undoing a delete needs the Recycle Bin
to take the item back from.
