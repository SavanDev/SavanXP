# The Start menu

The Start menu is a popup of the `desktop` process
([DESKTOP_ARCHITECTURE.md](./DESKTOP_ARCHITECTURE.md)), anchored at the bottom
left above the taskbar. It opens with the Start button or Ctrl+Esc and is
mouse only.

```
+--+------------------+
|  | [] Solitaire     |
|  |------------------|
|  | [] Programs    > |--+------------------+
|  | [] Settings    > |  | Accessories      |
|  |------------------|  | [] Calculator    |
|  | [] Shut Down...  |  | ...              |
|  | [] Restart...    |  +------------------+
+--+------------------+
```

## What it shows

- **Shortcuts**, at the top above an etched separator, when there are any
  (see [Shortcuts](#shortcuts)).
- **Programs ▸**: a cascade with every group of progman's catalog except
  Settings. Each group appears under a grey header, followed by its items.
- **Settings ▸**: a cascade with the items of the `Settings` group and no
  header. The entry exists only while that group has something in it.
- **Shut Down...** and **Restart...**: these open the Shut Down dialog
  (`desktop_shutdown.c`) with that option chosen.

The catalog is progman's (`progman_registry.h`), so the menu and Program
Manager always agree. A program asks for a place with `category=` in its
`.sxres` ([SXE_FORMAT.md](../userland/SXE_FORMAT.md)). The control-panel
applets (Display Properties, Mouse Properties, Sound, Add/Remove Programs,
System Properties) declare `category=Settings`, the way Windows 95 kept its
Control Panel out of Programs. Any other category is a Programs group. Moving
a program between the two cascades is a change to its manifest, not to the
menu.

## Shortcuts

The menu's own shortcuts are what the user pinned to it: `.sxlnk` files
([DESKTOP_ICONS.md](./DESKTOP_ICONS.md)) in `Start Menu` in the profile
(`/users/user/Start Menu`). `start_shortcuts.c` reads the folder each time the
menu opens and lists, sorted by name, every shortcut whose target exists, up
to eight (`START_PINS_MAX`). This is the area above Programs where Windows'
classic Start menu keeps what is pinned to it, and it is how a program that
progman's catalog cannot discover reaches the menu: a Windows program, which
has no `.sxres`.

The folder is the user's alone; `start_pins.c` owns it. The system only
seeds it, the way Windows copies the Default User profile: the first time
the folder is needed and missing, it is created with a copy of each `.sxlnk`
in `/system/start`, the defaults the build installs. That happens once, so a
default the user unpins stays unpinned across boots and rebuilds.

- **Pinning.** Files' item menu has **Pin to Start Menu**, next to Send To
  Desktop and enabled for a program (`/bin`, an `.sxe`), a Windows program
  (`.exe`) or a shortcut. It writes a shortcut named as Send To Desktop names
  one. A `.exe` loses its extension (`sol`) and starts in its own folder, as
  Windows fills a new shortcut's *Start in*. A shortcut is copied under its
  own name. A target already pinned with the same argument is refused ("It
  is already on the Start menu"), and so is a ninth pin.
- **Unpinning.** A right click on a shortcut row closes the menu and opens
  the row's context menu where the pointer is: **Open** and **Unpin from
  Start Menu**, which deletes the `.sxlnk`. The context menu takes the Start
  menu's window (see the table below), so the menu cannot stay open behind
  it as Windows XP's does.

A row is the file's name without `.sxlnk` and a 16x16 icon: the shortcut's
`icon=` or its target's, from a program's `.sxicon` or, for a `.exe`, the
first icon group of its PE resources (`sxgfx/pe_icon.h`). A click closes the
menu and opens the shortcut as the desktop does (`shortcut_open_path`), so a
`.exe` goes to `winhost` through its association. Resting on a row closes an
open cascade.

The build's one default is `Solitaire.sxlnk`, whose target is the user's own
`sol.exe` ([WIN32.md](../userland/WIN32.md#solitaire)): it shows only once
that file is in place.

## One level of cascade

Programs keeps the groups as headers inside one cascade instead of opening
Programs ▸ Accessories ▸ Calculator. That is a deliberate limit, not an
omission. Each cascade level is one more popup window. A process has
`SAVANXP_WM_MAX_OWNED_WINDOWS` (4) owned windows, and the desktop already
uses all of them:

| Id | Window |
|---|---|
| 1 | Taskbar |
| 2 | Start menu, context menu, or a shell dialog |
| 3 | Tray popups, or the Start menu's cascade |
| 4 | End Program (not responding) |

The cascade borrows id 3 because a tray popup is never needed while the Start
menu is open. If one is opened anyway, it replaces the cascade. A second
level needs a fifth window. The limits under that number, and the order in
which to raise them, are in
[SYSTEM_LIMITS_ROADMAP.md](../roadmaps/open/SYSTEM_LIMITS_ROADMAP.md).

## How the cascade behaves

These rules follow Windows 95:

- Resting the pointer on an entry for 400 ms (`STARTMENU_CASCADE_DELAY_MS`,
  Windows' default menu show delay) opens its cascade. A click opens it at
  once.
- Resting on the other entry switches the cascade after the same delay, and
  resting on the footer closes it. The delay is what lets a pointer cross a
  row on its way into the open cascade without swapping it.
- While the pointer is in the cascade, its entry stays lit. Moving onto the
  cascade cancels any pending switch.
- Leaving for the background or the taskbar keeps the cascade open.
- The cascade opens at the menu's right edge, overlapping it by 3 px, with its
  first row level with the entry. It is sized to its content, as wide as the
  longest name. When it does not fit, it moves up only until it does, so a
  long Programs cascade ends on the taskbar.
- A cascade taller than the work area fills it and scrolls, by the wheel or by
  resting on its arrow strips.
- Launching a program closes the menu and its cascade. So do a press outside
  them, the Start button, and Ctrl+Esc.

A press on the cascade is a press on the desktop's own process, so `windowd`
dismisses neither popup for it (`dismiss_popups_for_press` skips the
presser's pid).

## The banner

The banner is NT 4's: black fading to blue, with the system name and the
edition rotated along it. The edition is a switch,
`desktop_startmenu_set_show_edition()`, which is on by default. Even when it
is on, the edition is drawn only while the banner is long enough for both
parts. The menu with two entries is not, so it shows the name alone, as
Windows 95 shows "Windows95". The edition comes back on its own if the menu
grows. A future visual setting can turn the switch off.

## Verification

`tools/shoot.sh --scenario startmenu` checks the following:

- Clicking Programs opens the cascade with Calculator in it, and Calculator
  launches from it.
- Resting on Settings opens its cascade, which lists Display Properties and
  no programs.
- Resting on Programs replaces the Settings cascade.
- The arrow strips are checked when the Programs cascade overflows.
- Both footer rows open the Shut Down dialog.

The shortcuts' section has no shot of its own: the build's only default
needs the user's `sol.exe`. `pe-icon-test` checks the icon reader, and the
section was checked by hand with Solitaire installed: the default seeded into
a profile without the folder, listed with its icon and started from it;
unpinned from its context menu, and not seeded again; `sol.exe` pinned from
Files as `sol` and started from it, a second pin refused, and Pin to Start
Menu disabled on `cards.dll`.
