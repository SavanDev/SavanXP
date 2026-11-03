# Desktop icons, shortcuts and context menus

> **Status: phase 1 is built** (the desktop's context menu and the toolkit's
> popup menu). Phases 2 to 4 are the plan; the shortcut format in phase 2 is a
> proposal until it is built.

The reference is the Windows 95 / NT 4 desktop: icons on the background that
are the contents of a folder, shortcuts as small files that point at a program
or a document, and a context menu on a right click.

## What it builds on

None of this needs the kernel or `windowd` to change, except phase 4.

| Piece | Where |
|---|---|
| The background is the desktop's window and gets every button | `desktop.c`, role `background` ([DESKTOP_ARCHITECTURE.md](DESKTOP_ARCHITECTURE.md#window-roles)) |
| Frameless windows at any point, closed by a press elsewhere | role `popup`, `SAVANXP_WM_POPUP_DISMISS_ON_OUTSIDE` |
| A program's icon | its `.sxicon` section ([SXE_FORMAT.md](../userland/SXE_FORMAT.md)) |
| A file's icon by type | `mime_icon`, `mimeicon.ini` |
| Which program opens a file | `file_assoc`, `assoc.ini` |
| The installed programs | `progman_registry` |
| Where the user's files live | `savanxp_profile_dir()`, `/users/<name>` |

## Phase 1: the context menu (built)

**The toolkit's popup menu.** `sxgui_popup_menu_size`, `_paint`, `_item_at`,
`_item_selectable` and `_step` measure, paint and hit-test one
`struct sxgui_menu` on its own. The menu bar draws its drop-downs with the
same calls, so a context menu and a menu bar's menu look the same. The caller
owns where the list is shown, which row is hot and what a click does.

**The desktop's context menu** (`desktop_contextmenu.c`) opens when the right
button is **released** on the background, as Windows sends `WM_CONTEXTMENU`
after the button comes up: by the time the menu is under the pointer the
press is over. An item runs when either button is released over it. A press
on the background, on the taskbar or in another process closes it.

`desktop_open_popup_at` places a popup with its corner at a screen point and
flips it left or up where it would not fit above the taskbar. A popup whose
size depends on its content gives it through `desktop_popup_ops.measure`.

**It shares the Start menu's window.** A process has four owned windows
(`SAVANXP_WM_MAX_OWNED_WINDOWS`) and the desktop already used all of them:
taskbar, Start menu, tray popup, End Program. The Start menu and a context menu
are never open together on Windows either, so they share id 2, as the three
tray popups share id 3. A fifth window would mean raising the limit in the ABI.

**Mouse only.** A popup never takes the keyboard focus, so arrows, Enter and
Esc do not reach it. Keyboard navigation needs the same `windowd` change as
phase 3's keyboard on the desktop.

Its items today are Change Wallpaper and Open Command Prompt. The rest of the
Windows menu (Arrange Icons, Refresh, New) comes with phase 3, when there is
something for it to act on.

## Phase 2: the Desktop folder and the shortcut format (proposed)

- **The desktop shows a folder**: `/users/<name>/Desktop`. What is on the
  desktop is what is in that folder, so Files and any other program can show
  and change it with no protocol between them and the desktop.
- **A shortcut is a file**, `.sxlnk`, in the same `key=value` text as
  `assoc.ini`:

  ```ini
  target=/bin/notepad.sxe
  args=/users/user/notes.txt
  workdir=/users/user
  icon=/bin/notepad.sxe
  ```

  Text and not a binary layout like `.lnk`: it is written by hand and by
  tools, and there is no reason to hide it. `icon=` names a binary (its
  `.sxicon`) or an icon name from `/icons`; without it the shortcut shows its
  target's icon.
- Opening a shortcut launches `target`; opening any other file goes through
  `file_assoc`, as Files does.
- When the folder does not exist, the desktop creates it with a few shortcuts
  (Command Prompt, Files, Notepad).

## Phase 3: the icon grid

- 32x32 icons with their name below, wrapped to two lines, in columns from the
  top left, as Windows arranges them. `mime_icon` needs a large size next to
  `mime_icon_small`; the PNGs are already in `assets/desktop/icons/mime/32x32`.
- Click selects, double click opens, drag moves within the background (the same
  window, so it needs no drag and drop between processes). Positions persist in
  a settings file in the profile.
- There is no change notification in the filesystem: the folder is re-read on
  a timer, as the wallpaper choice is.
- The context menu gains Arrange Icons, Refresh and New ▸ Shortcut, and a menu
  for an icon: Open, Delete, Rename, Properties.
- **The keyboard is the open problem.** The background is never activated, so
  it never has the focus: Enter, Delete, F2 and the arrows cannot reach the
  desktop today. `windowd` has to let the background take the focus when it is
  clicked, as Windows activates the desktop's list view.

## Phase 4: context menus in applications

Only the shell may open a `popup` window, so an application cannot show a menu
outside its own surface. A context menu that has to fit inside the window is
the same problem owned dialogs solved ([OWNED_WINDOWS.md](OWNED_WINDOWS.md)).
It needs:

- `windowd` to accept a frameless popup as an owned window of any client
  (Windows lets any program call `TrackPopupMenu`), dismissed on an outside
  press and placed by the client.
- An `sxgui` call that shows a `struct sxgui_menu` at a point in such a window
  and reports the chosen id, built on the popup menu functions of phase 1.
- Shift+F10 and the Menu key, the keyboard's right click.

Files is the first user: Open, Delete, Rename and Send To ▸ Desktop (which
writes a shortcut into the Desktop folder, phase 2).
