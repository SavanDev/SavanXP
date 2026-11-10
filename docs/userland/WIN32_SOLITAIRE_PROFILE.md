# Win32 profile: Windows XP Solitaire

> **Status: static inventory recorded (2026-11-10); the run-time trace and the
> reference captures are pending.** This is the W0 record of the
> [Win32 roadmap](../roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md#w0--profile-and-inventory).
> It describes two files the user owns. Neither file is in this repository,
> and nothing here is implemented.

The API surface of the first Win32 profile is what these two files import.
A function that is not in this document is not written.

## The files

Read with `llvm-readobj` and a resource parser. Nothing was executed.

| | `sol.exe` | `cards.dll` |
| --- | --- | --- |
| SHA-256 | `ff301ce29c449239910c5be7224e6b65b81ed1f5ca7bcc8e6b72b0423d3f6341` | `f90f768014ca4d8413987b758361a8d12a0b7b72e7546800617e21c189b0816d` |
| Size | 57,344 bytes | 359,936 bytes |
| File version | 5.1.2600.0 | 5.1.2600.0 |
| Link time | 2001-08-17 | 2001-08-22 |
| Resource language | Spanish (`0x0C0A`) | English (`0x0409`) |
| Format | PE32, i386, GUI subsystem 4.0, linker 7.0 | PE32, i386, DLL |
| Image base | `0x01000000` | `0x70140000` |
| Size of image | 64 KiB | 364 KiB |
| Base relocations | **stripped** | present |
| Entry point RVA | `0x5F85` | `0x1086` |
| Sections | `.text` RX, `.data` RW, `.rsrc` R | `.text` RX, `.data` RW (no file data), `.rsrc` R, `.reloc` |
| TLS directory, delay imports | none | none |
| Bound imports | present | present |
| Exports | none | `WEP` 1, `cdtAnimate` 2, `cdtDraw` 3, `cdtDrawExt` 4, `cdtInit` 5, `cdtTerm` 6 |

The edition is Windows XP in Spanish. Version 5.1.2600.0 and the 2001 link
dates say these two files are the original release's; a service pack did not
replace them.

What the loader must honour, from these headers:

- **`sol.exe` loads at `0x01000000` or not at all.** `winhost` links at
  `0x400000`, so it has 12 MiB before it collides.
- **The import address table is inside `.text`** in both files, at RVA
  `0x1000`. It is written while the section is mapped writable and before it
  becomes executable, which is the order the loader already uses.
- **The images are bound** to the addresses of Windows XP's own DLLs. The
  loader ignores the bound values and resolves every import from the lookup
  table, which both files keep.
- All imports are by name. `cards.dll` is imported by name too, although it
  exports ordinals.

A scan of both code sections found no load of the FS register, no
`int 0x2e`, no `sysenter` and no `int 0x80`. `sol.exe` reads and writes
`fs:[0]` once each, which is the C runtime's exception frame at start-up.
`cards.dll` does not touch FS.

## Imports

`sol.exe` imports 122 functions from eight DLLs. `cards.dll` imports 15 from
two, six of which `sol.exe` does not use. Leaving `cards.dll` itself aside,
**the profile is 124 functions in seven DLLs.**

| DLL | Count | Functions |
| --- | --- | --- |
| `USER32` | 62 | Window classes and windows: `RegisterClassW`, `RegisterClassExW`, `CreateWindowExW`, `DestroyWindow`, `ShowWindow`, `UpdateWindow`, `MoveWindow`, `EnableWindow`, `SetFocus`, `IsIconic`, `GetClientRect`, `GetDesktopWindow`, `AdjustWindowRect`, `ClientToScreen`, `DefWindowProcW`. Messages: `GetMessageW`, `PeekMessageW`, `DispatchMessageW`, `TranslateMessage`, `TranslateAcceleratorW`, `PostMessageW`, `PostQuitMessage`, `MsgWaitForMultipleObjects`, `RegisterWindowMessageW`. Painting: `BeginPaint`, `EndPaint`, `GetDC`, `ReleaseDC`, `InvalidateRect`, `DrawTextW`, `FrameRect`, `InvertRect`, `GetSysColor`. Input: `SetCapture`, `ReleaseCapture`, `GetCapture`, `GetKeyState`, `SetCursorPos`, `ShowCursor`, `SetTimer`, `KillTimer`. Rectangles: `CopyRect`, `InflateRect`, `IntersectRect`, `PtInRect`. Resources: `LoadStringW`, `LoadStringA`, `LoadIconW`, `LoadCursorW`, `LoadImageW`, `LoadAcceleratorsW`, `LoadBitmapA` (from `cards.dll`). Menus: `GetMenu`, `EnableMenuItem`. Dialogs: `DialogBoxParamW`, `EndDialog`, `GetDlgItem`, `CheckDlgButton`, `IsDlgButtonChecked`, `CheckRadioButton`, `MessageBoxW`. Help: `WinHelpW` |
| `GDI32` | 27 | `CreateCompatibleDC`, `DeleteDC`, `CreateCompatibleBitmap`, `SelectObject`, `DeleteObject`, `GetStockObject`, `CreateSolidBrush`, `SetBrushOrgEx`, `BitBlt`, `PatBlt`, `SetROP2`, `MoveToEx`, `LineTo`, `LineDDA`, `SetPixel`, `PtVisible`, `GetDeviceCaps`, `CreateFontW`, `TextOutW`, `SetTextColor`, `GetTextExtentPoint32W`, `GetTextMetricsW`; and from `cards.dll` only: `StretchBlt`, `GetPixel`, `GetObjectA`, `SetBkColor`, `GetDCOrgEx` |
| `msvcrt` | 18 | Start-up: `__set_app_type`, `__p__fmode`, `__p__commode`, `_adjust_fdiv`, `__setusermatherr`, `_initterm`, `__getmainargs`, `_acmdln`, `_controlfp`, `_except_handler3`, `_XcptFilter`. Exit: `exit`, `_exit`, `_cexit`, `_c_exit`. Other: `rand`, `srand`, `time` |
| `KERNEL32` | 9 | `GetModuleHandleA`, `GetStartupInfoA`, `GetCommandLineW`, `LocalAlloc`, `LocalFree`, `MulDiv`, `lstrlenW`, `LoadLibraryA`, `GetProcAddress` |
| `ADVAPI32` | 6 | `RegCreateKeyExW`, `RegOpenKeyExA`, `RegQueryValueExW`, `RegQueryValueExA`, `RegSetValueExW`, `RegCloseKey` |
| `SHELL32` | 1 | `ShellAboutW` |
| `COMCTL32` | 1 | `InitCommonControlsEx` |
| `CARDS` | 4 | `cdtInit`, `cdtDraw`, `cdtDrawExt`, `cdtTerm` |

What the list says about the work:

- **No file I/O, no heap beyond `LocalAlloc`, no threads, no
  `VirtualAlloc`, no standard I/O.** The base runtime of W2 is small.
- **`cards.dll` needs only `GDI32` and `LoadBitmapA`.** It imports nothing
  from `KERNEL32` or the C runtime, so the W4 gate depends on drawing alone.
- **`COMCTL32` is one call.** The status bar is Solitaire's own window class
  (`Stat`), not a common control. The manifest asks for version 6 of the
  common controls, which only changes how the standard buttons look.
- **`LoadLibraryA` and `GetProcAddress` serve Help.** The strings name
  `hhctrl.ocx`, a class id looked up in the registry, `sol.chm`, `sol.hlp`
  and `NTHelp.chm`. Help is out of scope: the load fails and `WinHelpW`
  reports failure, and Solitaire has a message for that.
- **Three calls take guest callbacks**: the window and dialog procedures,
  and `LineDDA`. All stay on the 32-bit side.
- **`SetCursorPos` moves the pointer**, for playing with the keyboard.
  `windowd` has no such request today. Whether mouse play ever reaches it is
  a question for the run-time trace.
- Text is drawn through the wide-character calls with a font made by
  `CreateFontW`, so the font mapper is on the path from the first paint.

The registry key is `HKEY_CURRENT_USER\Software\Microsoft\Solitaire`. The
strings `intl`, `iCurrency` and `sCurrency` point at the currency format used
by Vegas scoring; which call reads them is left to the trace.

## Resources

`sol.exe`, all in Spanish:

| Type | Count | Notes |
| --- | --- | --- |
| Menu | 1 | Two popups, nine commands, three separators: Game (Deal `F2`, Undo, Deck, Options, Exit) and Help (Contents `F1`, Search, Using Help, About) |
| Accelerators | 1 | `HIDDENACCEL`: `F2` for Deal, and `Alt+Shift+2` for command 1010 |
| Dialogs | 4 | See below. All use `MS Shell Dlg` 8 pt and the classic `DLGTEMPLATE` |
| String tables | 7 blocks | 34 strings |
| Icons | 8 in one group | 16, 32 and 48 px at 4, 8 and 32 bpp |
| Manifest, version | 1 each | |

| Dialog | Controls |
| --- | --- |
| 101, Deck | Twelve **owner-drawn** buttons (the card backs), a default button and a push button |
| 103, Options | Two group boxes, five radio buttons (not automatic: the program checks them with `CheckRadioButton`), four automatic check boxes, one static text, two buttons |
| 102, Game number | Two static texts, one **edit** control, two buttons. Reached through the hidden accelerator |
| 999, Assertion | Nine static texts, two buttons. A debugging dialog |

The control classes are therefore Button (push, default, owner-drawn, radio,
check box, group box), Static (left and right aligned) and Edit. The edit
control exists only in a dialog an ordinary game does not open; the trace
decides whether it is in scope.

`cards.dll` holds 74 bitmaps and a version resource:

| Header | Size | Depth | Count |
| --- | --- | --- | --- |
| `BITMAPCOREHEADER` (12 bytes) | 71x96 | 1 bpp | 21 |
| `BITMAPCOREHEADER` | 71x96 | 4 bpp | 14 |
| `BITMAPCOREHEADER` | 14x12, 24x7, 26x12, 32x22 | 4 bpp | 7 |
| `BITMAPINFOHEADER` (40 bytes) | 71x96 | 1 bpp | 20 |
| `BITMAPINFOHEADER` | 71x96 | 24 bpp | 12 (the card backs, ids 54 to 65) |

None is compressed. The bitmap loader must read **both header formats** at
1, 4 and 24 bits per pixel; the older 12-byte header, with its three-byte
palette entries, is the more common one here.

## Still to record

W0 closes when these are added:

- **A relay trace** of the same two files under Wine on a build host, from
  start-up through a game, the Deck and Options dialogs, About and exit. It
  marks each function above as needed at start-up, needed in play, or
  reachable only from a feature declared out of scope, and it lists the
  raster operations, window messages, system metrics, stock objects and
  registry values actually used.
- **Reference captures** from Windows XP: the main window, each menu and
  dialog, every card face and back, and the end-of-game animation.
- The decision on `SetCursorPos`, the edit control and the hidden
  accelerator, taken from the trace.
