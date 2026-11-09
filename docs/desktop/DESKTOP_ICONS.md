# Desktop icons, shortcuts and context menus

> **Status: built** (phases 1 to 4). `tools/shoot.sh` scenarios
> `contextmenu`, `desktopicons`, `filesmenu` and `taskmgrmenu` drive them end
> to end;
> `windowd-smoke` asserts the application popup rules and `filesapp-smoke`
> the shortcut format.

The reference is the Windows 95 / NT 4 desktop: icons on the background that
are the contents of a folder, shortcuts as small files that point at a program
or a document, and a context menu on a right click, on the desktop and in any
application.

## What it builds on

| Piece | Where |
|---|---|
| The background is the desktop's window and gets every button | `desktop.c`, role `background` ([DESKTOP_ARCHITECTURE.md](DESKTOP_ARCHITECTURE.md#window-roles)) |
| Frameless windows at any point, closed by a press elsewhere | role `popup`, `SAVANXP_WM_POPUP_DISMISS_ON_OUTSIDE` |
| A program's icon | its `.sxicon` section ([SXE_FORMAT.md](../userland/SXE_FORMAT.md)) |
| A file's icon by type | `mime_icon`, `mimeicon.ini`, the `.sxicon` files in `/icons` |
| Which program opens a file | `file_assoc`, `assoc.ini` |
| Where the user's files live | `savanxp_profile_dir()`, `/users/<name>` |

## Phase 1: popup menus

**The taskbar's context menu.** Right-click an empty area of the taskbar to
open `Task Manager...`. The menu appears above the bar, uses the same popup
renderer and accepts the mouse, arrows, Enter and Escape. A right press on
the panel gives the desktop the keyboard so these keys work with an
application open too. Start, window buttons and tray controls retain their
own interactions.

**The toolkit's popup menu.** `sxgui_popup_menu_size`, `_paint`, `_item_at`,
`_item_selectable` and `_step` measure, paint and hit-test one
`struct sxgui_menu` on its own. The menu bar draws its drop-downs with the
same calls, so every menu in the system looks the same. The caller owns where
the list is shown, which row is hot and what a click does.

**Right click is on the release.** A context menu opens when the right button
comes **up**, as Windows sends `WM_CONTEXTMENU` after `WM_RBUTTONUP`: by the
time the menu is under the pointer the press is over. An item runs when
either button is released over it, so pressing, dragging into the menu and
releasing works too.

## Phase 2: the Desktop folder and shortcuts

**The desktop shows a folder**: `/users/<name>/Desktop`. What is on the
desktop is what is in that folder, so Files and any other program can show
and change it with no protocol between them and the desktop. When the folder
does not exist, the desktop creates it with three shortcuts: Command Prompt,
Files and Notepad.

**A shortcut is a text file**, `.sxlnk`, of `key=value` lines like `assoc.ini`:

```ini
target=/bin/notepad.sxe
args=/users/user/notes.txt
workdir=/users/user
icon=/bin/notepad.sxe
```

| Key | Meaning |
|---|---|
| `target` | What it opens: a program, a document or a folder. Required. |
| `args` | One argument, the program's `argv[1]` (the launch API passes one). |
| `workdir` | The directory the program starts in; it inherits the working directory at spawn, so the opener changes into it around the launch. |
| `icon` | A binary whose `.sxicon` to show, or an icon name from `/icons`. Without it, the target's icon. |

`#` and `;` start a comment and unknown keys are ignored, so the format can
grow without breaking older readers. It is text and not a binary layout like
`.lnk` because people and tools write it, and there is nothing to hide.

**One meaning of "open"** (`shortcut_open_path` in `shortcut.c`, shared by the
desktop and Files): a program starts; a folder opens in Files (which takes the
folder as `argv[1]`); a document goes to the program `file_assoc` associates
with its extension, or to Notepad; a shortcut opens its target, with its
argument and working directory. A shortcut to a shortcut is not followed, as
on Windows.

Programs get their name from their `.sxmeta` (`SXE_TAG_NAME`) when a
shortcut is made for them: `/system/console.sxe` becomes `Command Prompt.sxlnk`.

## Phase 3: the icon grid

`desktop_view.c` draws the folder over the wallpaper:

- **Grid.** Cells of 76x74 px from the top left, filled top to bottom and then
  left to right, as Windows arranges a desktop. Each cell is a 32x32 icon with
  its name below, wrapped to two lines and ending in "..." when it does not
  fit, white with a black shadow so it reads on any wallpaper. A shortcut
  carries the arrow in its lower left corner.
- **Icons.** A program's `.sxicon`, the type's icon from `/icons` (through
  `mimeicon.ini`), or the folder's; for a shortcut, its `icon=` or its
  target's. The generic program icon is the last resort.
- **Positions** are cells, not pixels, kept in `desktopicons.ini` in the
  profile (`name=column,row`), so a change of video mode only pulls back the
  icons that fall off the screen. A new icon takes its saved cell or the first
  free one.
- **The folder is re-read every 2 s**: the filesystem has no change
  notification. What the desktop changes itself it re-reads at once.
- **The wallpaper is drawn once and cached**, so selecting or dragging an icon
  repaints from a copy instead of scaling an image wallpaper again.

Mouse: click selects (the icon is tinted and the label takes the selection
colour), double click opens, dragging moves an icon to the cell it is dropped
on (dropped on another icon, the two trade cells), a right click opens the
icon's menu or the background's.

Keyboard, once the background has been clicked: arrows move the selection,
Enter opens, Delete asks before deleting, F2 renames in place (Enter keeps,
Esc cancels, a click elsewhere keeps), F5 re-reads the folder, Shift+F10 or
the Menu key open the menu, and a letter jumps to the next name that starts
with it.

| Menu | Items |
|---|---|
| Background | Arrange Icons (folders first, then by name), Refresh, New Folder, New Text Document, Open Command Prompt, Properties (opens [Display Properties](./DISPLAY_PROPERTIES.md)) |
| Icon | Open, Rename, Delete |

New Folder and New Text Document create "New Folder" / "New Text
Document.txt" (with " (2)" and on when taken) and start their name being
edited. Delete removes a file or an **empty** folder after a question; a
folder with something inside is refused, since there is no Recycle Bin to
take it back from.

### The desktop's windows

A process has four owned windows (`SAVANXP_WM_MAX_OWNED_WINDOWS`) and the
desktop already used all of them: taskbar, Start menu, tray popup, End
Program. The context menu and the delete question share the Start menu's
(id 2): none of them is ever open with another, as the three tray popups
share id 3. `desktop_open_popup_at` places a popup with its corner at a
screen point and flips it left or up where it would not fit above the
taskbar; a popup whose size depends on its content gives it through
`desktop_popup_ops.measure`.

### Keyboard on the desktop

`windowd` has three focus states: an application window, **the desktop**, or
nothing. A press of either button on the background gives the focus to the
desktop, as clicking the Windows desktop activates its icon view; no
application window is active then and keys go to the desktop's main window.
The focus stays there until an application window is activated. The desktop
sorts its keys: the hotkeys it registered (only with their exact modifiers,
since now it also gets plain keys), then the open menu's or question's, then
the icons'. A popup never takes the focus, so the menus are driven through
the main window.

## Phase 4: context menus in applications

**`windowd`** accepts `SAVANXP_WM_WINDOW_ROLE_POPUP` from an application's
main window, as Windows lets any program call `TrackPopupMenu`. It is an
owned window with the popup's behaviour:

- no frame, at `x`,`y` relative to the owner's client area, clamped to the
  display;
- stacked above its owner, but **never active**: the owner keeps the
  keyboard, so the program drives the menu with its own keys, and the owner
  is not disabled the way a dialog disables it;
- with `SAVANXP_WM_POPUP_DISMISS_ON_OUTSIDE`, closed by a press of either
  button anywhere but on itself, its owner included.

Panels stay the shell's. A right press now also activates the window it
lands on, so the menu it opens has the keyboard.

**`sxgui`** shows a menu at a point with `sxgui_context_menu_open(ctx, menu,
x, y, on_command, user)`. While it is open the toolkit takes the window's
keys (arrows, Enter, Esc) and pointer; the chosen id goes to `on_command`
once the menu is closed. Where it shows is the context's `popup_host`'s
business: `sxgui_app_run` opens a popup window (owned window id 2; dialogs
use id 1) and pumps it, so the menu may leave the window. Without a host, or
if the WM refuses the window, the menu is drawn inside the surface, flipped
and clamped to fit. SUN programs get the same from `sxngui_app_run`, which
opens the popup with `sxnwm_popup_open` (`sxn/sxnwm.h`). `sxgui_listbox_row_at`
tells which row a right click landed on.

**Task Manager**, a SUN program, has the original's row menus: End Process
and End Process Tree on the Processes tab, Switch To and End Task on
Applications, from a right click, Shift+F10 or the Menu key.

**Files**: a right click on a row opens Open, Send To
Desktop (a shortcut in the Desktop folder, named as the desktop would name
it), Rename... and Delete; on `..` or the empty part of the list, Up One
Level and Refresh. Shift+F10 and the Menu key open the selected row's menu,
and Delete asks before deleting.
