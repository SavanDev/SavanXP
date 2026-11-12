# Win32 profile: Windows XP Solitaire

> **Status: complete. Static inventory recorded 2026-11-10; run-time trace and
> reference captures recorded 2026-11-12.** This is the W0 record of the
> [Win32 roadmap](../roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md#w0--profile-and-inventory).
> It describes two files the user owns. Neither file, nor any capture or log
> taken from them, is in this repository, and nothing here is implemented.

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
- **Four calls take guest callbacks**: the window and dialog procedures,
  `LineDDA`, and the timer procedure `SetTimer` is given (found by the
  trace). All stay on the 32-bit side.
- **`SetCursorPos` moves the pointer**, and only keyboard play calls it
  ([decisions](#decisions)). `windowd` has no such request today.
- Text is drawn through the wide-character calls with a font made by
  `CreateFontW`, so the font mapper is on the path from the first paint.

The registry key is `HKEY_CURRENT_USER\Software\Microsoft\Solitaire`. The
strings `intl`, `iCurrency` and `sCurrency` name the currency format used by
Vegas scoring, which the program looks up under its own key
([decisions](#decisions)).

## Resources

`sol.exe`, all in Spanish:

| Type | Count | Notes |
| --- | --- | --- |
| Menu | 1 | Two popups, nine commands, three separators: Game (Deal `F2`, Undo, Deck, Options, Exit) and Help (Contents `F1`, Search, Using Help, About) |
| Accelerators | 1 | `HIDDENACCEL`: `F2` for Deal, and `Alt+Shift+2` for command 1010, which wins the game at once and starts the end-of-game animation |
| Dialogs | 4 | See below. All use `MS Shell Dlg` 8 pt and the classic `DLGTEMPLATE` |
| String tables | 7 blocks | 34 strings |
| Icons | 8 in one group | 16, 32 and 48 px at 4, 8 and 32 bpp |
| Manifest, version | 1 each | |

| Dialog | Controls |
| --- | --- |
| 101, Deck | Twelve **owner-drawn** buttons (the card backs), a default button and a push button |
| 103, Options | Two group boxes, five radio buttons (not automatic: the program checks them with `CheckRadioButton`), four automatic check boxes, one static text, two buttons |
| 102, Game number | Two static texts, one **edit** control, two buttons. **Never created** |
| 999, Assertion | Nine static texts, two buttons. A debugging dialog. **Never created** |

`sol.exe` calls `DialogBoxParamW` from exactly two places, for 101 and 103.
No code opens 102 or 999, so the controls in scope are Button (push,
default, owner-drawn, radio, check box, group box) and Static (left and right
aligned). Edit is not.

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

## Run-time trace

Taken on 2026-11-12 on the build host, with Wine 11.21 (WOW64, X11 driver)
in a fresh prefix under the `es_AR` locale.

- `WINEDEBUG=+relay` records every call `sol.exe` and `cards.dll` make into
  Wine's DLLs, with arguments, and every message delivered to their window
  and dialog procedures. Calls from one native file to the other
  (`sol.exe` into `cards.dll`) need a second run with `+snoop`.
- **Wine ships its own `cards.dll` and loads it instead of the one next to
  `sol.exe`.** A trace without `WINEDLLOVERRIDES=cards=n` records Wine's card
  library, not Microsoft's. The native one loads at its preferred base,
  `0x70140000`.
- A small driver program in the same prefix plays a fixed script with
  `SendInput`, and marks each phase by posting `WM_NULL` with `wParam`
  `0x573000nn` to the main window. The phases: start-up; mouse play (stock,
  waste, every column dragged onto every other, double clicks); keyboard play;
  both menus opened; Deck; Options (Las Vegas, three cards, outline dragging,
  cumulative score) and play with them; `F2`; `Alt+Shift+2`; About; `F1`;
  Exit. A last run repeats `F1` with Wine's `hhctrl.ocx` disabled, as on
  SavanXP.
- The driver, the scripts and the compressed logs are kept with the user's
  files under `$SAVANXP_WIN32_FIXTURES/w0/`, not in the repository.

### Classification

113 of the 124 imports were called; so were `cdtInit`, `cdtDrawExt` and
`cdtTerm`. Under Wine, `DefWindowProcW` resolves to a forwarder in `ntdll`
and `InitCommonControlsEx` to version 6 of `comctl32`; both count as called.

| Needed | Count | Functions |
| --- | --- | --- |
| Start-up | 72 | `USER32` (31): `AdjustWindowRect`, `BeginPaint`, `CreateWindowExW`, `DefWindowProcW`, `DispatchMessageW`, `DrawTextW`, `EndPaint`, `GetClientRect`, `GetDC`, `GetMessageW`, `IntersectRect`, `InvalidateRect`, `IsIconic`, `LoadAcceleratorsW`, `LoadBitmapA`, `LoadCursorW`, `LoadIconW`, `LoadImageW`, `LoadStringW`, `MoveWindow`, `PostMessageW`, `RegisterClassExW`, `RegisterClassW`, `RegisterWindowMessageW`, `ReleaseDC`, `SetTimer`, `ShowCursor`, `ShowWindow`, `TranslateAcceleratorW`, `TranslateMessage`, `UpdateWindow`. `GDI32` (21): `BitBlt`, `CreateCompatibleBitmap`, `CreateCompatibleDC`, `CreateFontW`, `CreateSolidBrush`, `DeleteDC`, `DeleteObject`, `GetDCOrgEx`, `GetDeviceCaps`, `GetObjectA`, `GetPixel`, `GetStockObject`, `GetTextExtentPoint32W`, `GetTextMetricsW`, `PatBlt`, `SelectObject`, `SetBkColor`, `SetBrushOrgEx`, `SetPixel`, `SetTextColor`, `TextOutW`. `KERNEL32` (7): `GetCommandLineW`, `GetModuleHandleA`, `GetStartupInfoA`, `LocalAlloc`, `LocalFree`, `MulDiv`, `lstrlenW`. `msvcrt` (9): `__getmainargs`, `__p__commode`, `__p__fmode`, `__set_app_type`, `_controlfp`, `_initterm`, `rand`, `srand`, `time`. `ADVAPI32` (3): `RegCloseKey`, `RegCreateKeyExW`, `RegQueryValueExW`. `COMCTL32`: `InitCommonControlsEx` |
| Mouse play | 6 | `SetCapture`, `ReleaseCapture`, `GetKeyState`, `PtInRect`, `PtVisible`, `LineDDA` |
| Keyboard play | 2 | `SetCursorPos`, `ClientToScreen` |
| Menus | 2 | `GetMenu`, `EnableMenuItem` |
| Outline dragging (an Options setting) | 4 | `SetROP2`, `MoveToEx`, `LineTo`, `InvertRect` |
| Deck dialog | 10 | `DialogBoxParamW`, `EndDialog`, `GetDlgItem`, `SetFocus`, `StretchBlt`, `FrameRect`, `InflateRect`, `CopyRect`, `GetSysColor`, `RegSetValueExW` (settings saved on OK, by Options too) |
| Options dialog | 4 | `CheckDlgButton`, `CheckRadioButton`, `IsDlgButtonChecked`, `EnableWindow` |
| End of game | 3 | `PeekMessageW`, `MsgWaitForMultipleObjects`, `MessageBoxW` (also Help's error) |
| About | 1 | `ShellAboutW` |
| Exit | 3 | `KillTimer`, `PostQuitMessage`, `exit` |
| Help only (out of scope) | 6 | `LoadStringA`, `GetDesktopWindow`, `RegOpenKeyExA`, `RegQueryValueExA`, `LoadLibraryA`, `GetProcAddress` |
| Never called | 11 + 1 | `msvcrt`: `_adjust_fdiv` and `_acmdln` (data), `_except_handler3` and `_XcptFilter` (exception paths), `__setusermatherr`, `_cexit`, `_c_exit`, `_exit`. `USER32`: `DestroyWindow`, `GetCapture`, `WinHelpW`. `CARDS`: `cdtDraw` |

A function that is never called still has to resolve: an unresolved import
fails the load. It needs an entry point, not behaviour.

### Run-time lists

- **Raster operations.** `BitBlt` uses `SRCCOPY` only (2,493 calls).
  `PatBlt` uses `PATCOPY` and `BLACKNESS`. `StretchBlt` uses `SRCCOPY`, for
  the Deck dialog's thumbnails. `SetROP2` alternates `R2_NOT` and
  `R2_COPYPEN` around the outline of a dragged pile.
- **Cards.** Every card is drawn with `cdtDrawExt` at 71x96 in mode 0
  (face), 1 (back) or 3 (empty pile), over background `0x008000`; the Deck
  dialog draws backs at 33x62. Mode `0x80000000` appears only during the
  end-of-game animation, 3,586 calls in 16 seconds.
- **System metrics:** none; the program does not import `GetSystemMetrics`.
  `GetDeviceCaps` reads `LOGPIXELSY`, `HORZRES`, `VERTRES` and `NUMCOLORS`.
  `GetSysColor` reads `COLOR_BTNFACE` and `COLOR_HIGHLIGHT`, for the Deck
  dialog's selection. One stock object: `WHITE_BRUSH`.
- **Font.** `CreateFontW(-12, ..., FW_BOLD, ..., DEFAULT_CHARSET, ...,
  "MS Shell Dlg")`, created and deleted on every paint of the status bar.
- **Windows.** Class `Solitaire`, `WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN`,
  593x437 at the default position; class `Stat`, a child that is the status
  bar. One timer, id 666, every 250 ms, with its own timer procedure. One
  registered message, `"Sacar carta"`. Icon 500, the arrow cursor.
- **Messages delivered** to the main procedure: `WM_NCCREATE`, `WM_CREATE`,
  `WM_SIZE`, `WM_MOVE`, `WM_SHOWWINDOW`, `WM_WINDOWPOSCHANGING`/`CHANGED`,
  `WM_ACTIVATE`, `WM_ACTIVATEAPP`, `WM_NCACTIVATE`, `WM_SETFOCUS`,
  `WM_KILLFOCUS`, `WM_ENABLE`, `WM_NCPAINT`, `WM_ERASEBKGND`, `WM_PAINT`,
  `WM_NCHITTEST`, `WM_SETCURSOR`, `WM_MOUSEMOVE`, `WM_LBUTTONDOWN`,
  `WM_LBUTTONUP`, `WM_LBUTTONDBLCLK`, `WM_CAPTURECHANGED`, `WM_CANCELMODE`,
  `WM_KEYDOWN`, `WM_KEYUP`, `WM_CHAR`, `WM_SYSKEYDOWN`, `WM_SYSKEYUP`,
  `WM_SYSCHAR`, `WM_SYSCOMMAND` (`SC_KEYMENU`, `SC_CLOSE`), `WM_INITMENU`,
  `WM_INITMENUPOPUP`, `WM_UNINITMENUPOPUP`, `WM_MENUSELECT`,
  `WM_ENTERMENULOOP`, `WM_EXITMENULOOP`, `WM_ENTERIDLE`, `WM_COMMAND`
  (1000 to 1004, 1010, 2000), `WM_HELP` after the undocumented `0x004D`
  that `F1` produces, `WM_PARENTNOTIFY`, `WM_QUERYNEWPALETTE`, `WM_GETICON`,
  `WM_CLOSE`, `WM_DESTROY`, `WM_NCDESTROY`. The procedure calls
  `DefWindowProcW` even for messages it acts on, so default processing must
  be right for each. The status bar receives the creation, sizing and paint
  messages only. The dialog procedures act on `WM_INITDIALOG`, `WM_COMMAND`,
  `WM_DRAWITEM` (Deck) and `WM_CTLCOLORDLG`, `WM_CTLCOLORBTN` and
  `WM_CTLCOLORSTATIC`.
- **Registry.** Each access opens `HKCU\Software\Microsoft\Solitaire` with
  `RegCreateKeyExW`. Two `REG_DWORD` values with localized names, `Opciones`
  and `Atrás`, are read at start-up and written when Deck or Options closes
  with OK. `iCurrency` and `sCurrency` are looked up there too, and are
  absent. Help reads
  `HKCR\CLSID\{ADB880A6-D8FF-11CF-9377-00AA003B7A11}\InprocServer32`, the
  HTML Help control.
- **Memory.** `LocalAlloc(LPTR)` only, 115 blocks of at most 860 bytes.
- **The end-of-game loop** polls with `PeekMessageW` and waits with
  `MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT)`, about 4,100
  times. The wait must honour its 5 ms timeout.

### Decisions

- **`SetCursorPos`: keyboard play only.** Every call follows a
  `WM_KEYDOWN` (Tab, arrows, Space, Enter), to put the pointer on the
  selected card. Mouse play never calls it. The first profile can be played
  completely with the mouse without a request to move the pointer.
- **The edit control is out of scope.** The game-number dialog is never
  created (see [Resources](#resources)). `Alt+Shift+2` is in scope: it is
  the program's own command, and the quickest way to the end-of-game
  animation in a test.
- **Currency: nothing to read from the locale.** The program looks up
  `iCurrency` and `sCurrency` under its own key, where Windows 3.x kept the
  `[intl]` section of `win.ini`. Neither exists, so it prints `$` before the
  amount. Windows XP in Spanish shows `-$52` as Wine does.
- **Help fails cleanly.** `F1` looks up the HTML Help control and loads it.
  When either step fails, the program shows its own message box, "No se ha
  podido cargar la aplicación de ayuda de Windows". `WinHelpW` was not
  reached with or without `hhctrl.ocx`; the Search and Using Help menu items
  were not exercised and may reach it.

## Reference captures

Taken on 2026-11-12 from Windows XP Professional SP3 in Spanish, in a QEMU
virtual machine run with `-snapshot`, classic theme, 1024x768 at 32 bits,
driven over QMP. Its `sol.exe` and `cards.dll` have the sizes of the files
above and the date of the original release, 2001-08-24; their hashes were
not read inside the machine.

They are kept with the user's files under
`$SAVANXP_WIN32_FIXTURES/reference/`, never in the repository. Its
`SHA256SUMS` lists 118 images and has SHA-256
`77ca9437ea4e99010c993788d3c15f649f27af66dc6add5d3dd2dec4fd22d25b`.

| Directory | Contents |
| --- | --- |
| `screens/` | The main window; both menus open; Deck; Options before and after choosing Las Vegas; the table after Options, with `-$52`; the "¿Barajar de nuevo?" message box; the table after it; About. About shows the machine's registered owner and stays local |
| `faces/` | The 52 faces, 71x96, named by `cards.dll` id |
| `backs/` | The 12 backs, 71x96, named by id (54 to 65) |
| `win/` | 42 frames of the end-of-game animation, 1.5 s apart |

What they establish:

- **Every face and back equals the `cards.dll` bitmap of the same id**,
  decoded on the build host, except the corner pixels `cdtDrawExt` rounds off.
  That checks both header formats at 1, 4 and 24 bits per pixel. Ids 1 to 52
  are clubs, diamonds, hearts and spades, each from ace to king.
- The main window is 593 pixels wide with a 585x384 client area, and the
  Deck and Options dialogs are 273 and 252 pixels wide, on Windows XP and
  under Wine alike.
- The end-of-game animation runs about two minutes on Windows XP unless
  stopped, then clears the table and asks "¿Barajar de nuevo?" with Yes and
  No (`MB_YESNO | MB_ICONEXCLAMATION`).
