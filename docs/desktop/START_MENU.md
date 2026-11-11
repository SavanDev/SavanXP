# The Start menu

The Start menu is a popup of the `desktop` process
([DESKTOP_ARCHITECTURE.md](./DESKTOP_ARCHITECTURE.md)), anchored at the bottom
left above the taskbar. It opens with the Start button or Ctrl+Esc and is
mouse only.

```
+--+------------------+
|  | [] Programs    > |--+------------------+
|  | [] Settings    > |  | Accessories      |
|  |------------------|  | [] Calculator    |
|  | [] Shut Down...  |  | ...              |
|  | [] Restart...    |  +------------------+
+--+------------------+
```

## What it shows

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
