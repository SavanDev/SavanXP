# SSW — the Savan Subsystem for Windows

SSW is SavanXP's Win32 subsystem: it runs unmodified Windows programs. Win32
is the API it implements, as POSIX is the API of the POSIX subsystem; SSW is
the subsystem's name. Its source tree keeps the API's name
(`subsystems/win32/`), its code the `sxw_` prefix, and its host program is
`winhost`.

> **Status: W6 complete (2026-11-13).** This document describes what exists
> under `subsystems/win32/`. The plan, the phases and the reasons behind them
> are in the [Win32 roadmap](../roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md); the
> API surface of the first profile is in
> [WIN32_SOLITAIRE_PROFILE.md](WIN32_SOLITAIRE_PROFILE.md).

A PE32 program runs on SavanXP: `winhost.sxe` loads it and its DLLs below
4 GiB and runs it as 32-bit code, and its top-level window is a `windowd`
window. The same loader and DLLs run on the build host in a harness, with
offscreen windows and scripted input, and the fixtures that check both give
the bytes, the messages and the pixels Windows XP gives. The user's own
Windows XP `cards.dll` draws every card here exactly as it does there, and a
program's menu bar, popup menus, dialogs, message boxes and the controls
Solitaire's dialogs hold behave, by pointer and by keyboard, as they do there.
The user's own Windows XP Solitaire runs unchanged and is played to a win
with the same pixels Windows XP shows (see [Solitaire](#solitaire)). A `.exe`
opens from Files through its association with `winhost`, and Solitaire has a
shortcut at the top of the Start menu. Nothing of the Windows API beyond what
`sol.exe` and the fixtures import exists.

```text
subsystems/win32/
  include/       windows.h (the subsystem's own), sxw_abi.h (host <-> DLLs)
  pe/            the PE32 parser and mapper, portable C
  loader/        the process loader, portable C
  dlls/          ntdll, kernel32, msvcrt, advapi32, gdi32, user32, shell32,
                 comctl32 (PE32); common/ holds the code page, regions and
                 memcpy they share
  fixtures/      hello, initorder (+ alpha.dll, beta.dll), missing, startup,
                 window (+ its input script), cards, resource (+ its .rc and
                 the script that writes its images), dialog (+ its .rc and
                 input script)
  harness/       win32-harness: the loader and gate on Linux x86-64
  host/          winhost.sxe: the loader and gate on SavanXP
  tests/         win32test.sxe: the runner of the win32 smoke scenarios
  Win32.cmake
```

On the disk image, under `/system/win/drive_c` (the guest's `C:`):

```text
windows/system32/   ntdll, kernel32, msvcrt, advapi32, gdi32, user32, shell32
                    and comctl32 .dll
tests/              the fixtures, when the test applications are built
```

## The PE32 parser — `subsystems/win32/pe/`

`pe.h` and `pe.c` are portable C with no library dependency: no allocation,
no libc, nothing outside the buffers the caller passes. The same two files
build for the host tests and, at the guest half of W1, for `winhost`.

### Stages

1. **`pe_parse()`** reads the headers of a file in memory and checks them. It
   keeps a pointer to the file, so the file must outlive the `pe_image`.
2. **`pe_map()`** lays the image out in a buffer of `size_of_image` bytes, the
   way it sits in guest memory: headers and section data copied, every other
   byte zero.
3. Everything after works **on the mapped buffer, by RVA**, and checks every
   RVA and length against it:
   - `pe_relocate()` applies the base relocations for a move to a new base;
   - `pe_for_each_imported_dll()` and `pe_for_each_import()` walk the import
     descriptors and the lookup tables, giving each symbol's name or ordinal
     and its IAT slot;
   - `pe_find_export_by_name()` and `pe_find_export_by_ordinal()` return an
     RVA or a forwarder string (`DLL.Name` or `DLL.#N`);
   - `pe_read_tls()` and `pe_tls_callback()` read the TLS directory and its
     callbacks, after relocation;
   - `pe_protection_runs()` gives the page protection of each range (headers
     read-only, then each section as R, RW or RX, with equal neighbours
     merged), which is the list of views the loader maps.

Resolving a symbol, choosing a base, ordering `DllMain` calls and naming a
missing DLL are the loader's work, above this library.

### What it accepts

What a linker for Windows XP produces, and what the profile's two files need:

- i386 PE32, marked executable; an EXE or a DLL.
- Section alignment of a page or more, a power of two, at least the file
  alignment (512 bytes to 64 KiB). Low-alignment images are refused, because
  they map headers and code with one protection.
- An image base aligned to 64 KiB, with the image below 4 GiB and at most
  2 GiB long.
- Sections that follow the headers and each other with no gap, as the Windows
  loader requires. A section's extent is its virtual size rounded up to the
  section alignment; the bytes copied are its file data up to that extent, and
  a section with no file data (`cards.dll`'s `.data`) is all zero.
- Base relocations of type `HIGHLOW`, with `ABSOLUTE` as padding.
- Imports by name and by ordinal. A bound IAT (both profile files have one) is
  never read: every import is resolved again from the lookup table.

### What it refuses, by name

Each refusal is a `pe_status` with the section, directory or descriptor index
and the offending value, and `pe_format_error()` turns it into a line such as
`section 2 is both writable and executable` or
`image must load at 0x01000000 and has no relocations`.

- Other machines, PE32+, a ROM or malformed optional header, data directories
  past the optional header.
- Truncation anywhere: headers, section table, section data.
- Zero or more than 96 sections; unaligned, overlapping or out-of-image
  sections; a gap between sections.
- **A section both writable and executable.** A view is never writable and
  executable at once, so no loader of this subsystem could map it.
- An entry point outside an executable section, or an EXE with none.
- A data directory outside the image. **Delay-load imports and CLR images** are
  refused outright, not ignored.
- Moving an image whose relocations are stripped. `sol.exe` is one: it loads
  at `0x01000000` or not at all.
- Relocation blocks that are short, odd-sized or past their directory; other
  relocation types; a relocation that patches outside the image.
- Import descriptors with no IAT, bound descriptors with no lookup table,
  ordinals with stray bits, names that are empty, unterminated or longer than
  255 bytes.
- Export tables outside the image, an ordinal base above 65535, a forwarder
  without a dot.
- A TLS template that ends before it starts, an index slot or callback outside
  the image, a callback outside an executable section.

## The loader — `subsystems/win32/loader/`

Portable C like the parser, shared by the harness and `winhost`. Memory and
files come from a platform table (`map` at an exact address, `protect`,
`unmap`, `open_image`, `close_image`). Guest addresses are below 4 GiB and
both platforms map them at the same host address, so the loader writes guest
memory through plain pointers.

`sxw_load()` does, in order. The platform gives the lowest guest address it
can map (`guest_low`): `0x10000` in the harness, `0x400000` on SavanXP, where
the user half starts.

1. **Map the executable, then each DLL it imports, depth first.** An image
   goes at its preferred base when the range is free, otherwise at the first
   free 64 KiB step from `0x10000000`, and is relocated. It is mapped as one
   platform mapping per protection run (on SavanXP, one section each), so
   every run can take its own final access. Names are compared
   without case and opened lower-case: next to the executable first, then in
   the system folder. `ntdll.dll` is always loaded.
2. **Bind every import** from the lookup table. A forwarder (`DLL.Name` or
   `DLL.#N`) loads its DLL if needed and is followed up to eight times.
3. **Order initialization**: `ntdll.dll` first, then a post-order walk from
   the executable, so every DLL comes after the ones it imports from (both
   the DLL an import names and the one a forwarder ends at). The executable
   is last.
4. **Set up static TLS**: one block per module that has a TLS directory,
   filled from its template, its index written into the image, and the
   pointer array in the TEB.
5. **Map the stack** (the executable's reserve, 64 KiB to 16 MiB, all
   committed: there are no guard pages before S2), **the TEB, the PEB and the
   start block**, one page each.
6. **Give each image its final protections**, one call per protection run.

A failure at any step releases every region and file taken so far and
leaves two things: a line that names the module and the cause, and the NT
status Windows XP ends the process with for the same failure.

| Failure | Diagnostic | Status |
| --- | --- | --- |
| A DLL is not found | `missingdll.exe: missing DLL nosuch.dll` | `0xC0000135` |
| A function is not exported | `missingsym.exe: kernel32.dll!MissingFunction is not exported` | `0xC0000139` |
| An ordinal is not exported | `app.exe: foo.dll ordinal 7 is not exported` | `0xC0000138` |
| The parser refuses an image | `foo.dll: section 2 is both writable and executable` | `0xC000007B` |
| No guest address space left | `out of guest address space` | `0xC0000017` |

After a successful load the files are closed: nothing after the load reads
them.

## The gate and process start

`include/sxw_abi.h` is the private contract between the host and the DLLs,
versioned with `SXW_ABI_VERSION`. It is not the NT system call table.

- **TEB and PEB.** The host fills the fields the DLLs read: exception list
  head (`0xffffffff`), stack base and limit, self pointer, PEB pointer, TLS
  pointer, and at `0xC0`, Windows' `WOW32Reserved`, the address the guest
  calls to reach the host. The PEB holds the image base and points at the
  process parameters (below).
- **Services.** `ntdll!SxwService(number, arguments)` calls through that
  address with a flat array of 32-bit values. The host checks every guest
  address against the regions it mapped and never calls guest code. Version
  4 has twenty-one: version, exit, standard handle, write, debug output; the
  clock; a new region for the heap; find, load and resolve a module; open,
  close, query and set a registry key or value; and for windows, the
  millisecond clock, the screen's size, create, destroy, title and present
  a top-level window, and wait for an event (pointer, key, resize, close).
  A window is created as the program's main window, as a window owned by
  another (a dialog, which the host places), or as a popup at a point of its
  owner's client area (a menu: no frame, never the keyboard, closed by a
  press outside the program's windows). `loader/services.c` answers them for
  both hosts.
- **Process parameters.** `RTL_USER_PROCESS_PARAMETERS` sits in the PEB's
  page with the image path, the command line and the standard handles. A
  path under the host's `drive_c` folder is `C:\...`; any other is `Z:\...`,
  `Z:` being the host's root, as Wine names it. The command line is the
  quoted path followed by the arguments the host was given, each quoted
  when it holds a blank.
- **Process start.** The host enters the guest once, at
  `ntdll!SxwProcessStart`, with a start block listing every module in
  initialization order. `ntdll` runs, for each DLL, its TLS callbacks and
  `DllMain(DLL_PROCESS_ATTACH)` (a failure ends the process with
  `0xC0000142`), then the executable's TLS callbacks, then its entry point.
- **Process exit.** `kernel32!ExitProcess`, or the entry point returning,
  goes to `ntdll!RtlExitUserProcess`. Every DLL whose attach ran is detached
  in reverse order, its TLS callbacks and then `DllMain(DLL_PROCESS_DETACH)`
  with a non-null `lpReserved`; the executable's TLS callbacks come last.
  This order is the one Windows XP produced for `initorder.exe`. A DLL
  loaded at run time is detached first: it was initialized last.
- **The FPU.** `SxwProcessStart` loads the x87 control word `0x27F` (53-bit
  precision, every exception masked), what a Windows XP thread starts with;
  `_controlfp(0, 0)` reads `0x0009001F` there and here.

## The DLLs — `subsystems/win32/dlls/`

Each DLL is a directory with C sources and a `.def` file. They are built at
Windows XP's own bases (`ntdll.dll` at `0x7C900000`, `kernel32.dll` at
`0x7C800000`, `msvcrt.dll` at `0x77C10000`, `advapi32.dll` at `0x77DD0000`,
`gdi32.dll` at `0x77F10000`, `user32.dll` at `0x7E410000`, `shell32.dll` at
`0x7C9C0000`, `comctl32.dll` at `0x5D090000`), so none of them relocates. `include/windows.h` is the subsystem's own header;
it declares what the DLLs and fixtures use and grows with the W0 matrix.
`dlls/common/codepage.c`, compiled into the DLLs that need it, converts
between UTF-16 and the "ANSI" code page, Windows-1252, the one Windows XP uses
in Western European languages.

| DLL | Exports |
| --- | --- |
| `ntdll.dll` | `RtlExitUserProcess`; private: `SxwProcessStart`, `SxwService`, `SxwLoadLibrary`, `SxwGetProcAddress`, `SxwFindModule`, `SxwTime`, `SxwHeapAllocate`, `SxwHeapFree`, `SxwHeapSize` |
| `kernel32.dll` | `ExitProcess`, `GetStdHandle`, `WriteFile`; the matrix's nine: `GetModuleHandleA`, `GetStartupInfoA`, `GetCommandLineW`, `LocalAlloc`, `LocalFree`, `MulDiv`, `lstrlenW`, `LoadLibraryA`, `GetProcAddress` |
| `msvcrt.dll` | The matrix's eighteen: `__set_app_type`, `__p__fmode`, `__p__commode`, `_adjust_fdiv`, `__setusermatherr`, `_initterm`, `__getmainargs`, `_acmdln`, `_controlfp`, `_except_handler3`, `_XcptFilter`, `exit`, `_exit`, `_cexit`, `_c_exit`, `rand`, `srand`, `time` |
| `advapi32.dll` | The matrix's six: `RegCreateKeyExW`, `RegOpenKeyExA`, `RegQueryValueExW`, `RegQueryValueExA`, `RegSetValueExW`, `RegCloseKey` |
| `gdi32.dll` | The matrix's twenty-seven: `GetStockObject`, `SelectObject`, `DeleteObject`, `CreateSolidBrush`, `SetBrushOrgEx`, `PatBlt`, `SetROP2`, `MoveToEx`, `LineTo`, `LineDDA`, `SetPixel`, `GetPixel`, `PtVisible`, `GetDeviceCaps`, `CreateFontW`, `TextOutW`, `SetTextColor`, `SetBkColor`, `GetTextExtentPoint32W`, `GetTextMetricsW`, `CreateCompatibleDC`, `DeleteDC`, `CreateCompatibleBitmap`, `BitBlt`, `StretchBlt`, `GetObjectA`, `GetDCOrgEx`; private: `SxwCreateSurfaceDC`, `SxwSurfaceOfDC`, `SxwCreateBitmapFromDIB`, `SxwSetBkMode` |
| `user32.dll` | The matrix's sixty-two: windows and messages (classes, creation and destruction, showing, moving, enabling, focus, the message calls, painting, capture, keys, timers, the rectangle helpers, `DrawTextW`, `FrameRect`, `InvertRect`, `GetSysColor`, `SetCursorPos`, `ShowCursor`), resources (`LoadBitmapA`, `LoadStringW`, `LoadStringA`, `LoadIconW`, `LoadCursorW`, `LoadImageW`, `LoadAcceleratorsW`, `TranslateAcceleratorW`), menus (`GetMenu`, `EnableMenuItem`), dialogs (`DialogBoxParamW`, `EndDialog`, `GetDlgItem`, `CheckDlgButton`, `IsDlgButtonChecked`, `CheckRadioButton`, `MessageBoxW`) and `WinHelpW`; the full list is `dlls/user32/user32.def`. Private: `SxwDialogBoxIndirectParamW` |
| `shell32.dll` | `ShellAboutW` |
| `comctl32.dll` | `InitCommonControlsEx`, which registers nothing: Solitaire uses no common control |

### What they do, and what they refuse

- **The C runtime's start-up.** `_acmdln` is the command line in the ANSI
  code page, made when `msvcrt.dll` attaches. `__getmainargs` splits it the
  way `msvcrt.dll` does (backslashes before a quote, `""` inside quotes) and
  gives an empty environment: there is no environment yet. `_initterm` calls
  every non-null pointer in its range. `_controlfp` maps its bits onto the
  x87 control word and back. `_adjust_fdiv` is 0.
- **Exceptions.** `_except_handler3` and `_XcptFilter` answer "continue the
  search". Nothing dispatches an exception to a frame before the S3 slice: a
  fault ends the process in the kernel first.
- **Exit.** No exit function can be registered (`atexit` is not in the
  matrix) and there is no stdio, so `_cexit` and `_c_exit` have nothing to run
  and `exit` and `_exit` are `ExitProcess`.
- **`rand` and `srand`** are Microsoft's generator, `x * 214013 + 2531011`,
  bits 16 to 30: a seed deals the same cards here as on Windows. **`time`**
  returns a 32-bit `time_t` from the host's clock.
- **`LocalAlloc`** accepts fixed blocks (`LMEM_FIXED`, `LMEM_ZEROINIT`,
  `LPTR`) and refuses `LMEM_MOVEABLE` with `ERROR_INVALID_PARAMETER`, rather
  than hand out a fixed block as if it were moveable. The heap behind it is
  in `ntdll`: first fit over 256 KiB arenas the host maps, 8-byte aligned
  blocks, free neighbours merged when they are in the same arena (two arenas
  the host maps side by side are still two regions, and a block across them
  would fail the host's range checks). `LocalFree` of a pointer that is not a live
  block fails with `ERROR_INVALID_HANDLE`.
- **`MulDiv`** rounds half away from zero over a 64-bit product and answers
  -1 for a zero divisor or a result that does not fit, as Windows XP did for
  every case in `startup.exe`.
- **`GetStartupInfoA`** fills `cb` and nothing else: nothing asked for a
  window position, title or handles.
- **`GetModuleHandleA` and `LoadLibraryA`** add `.dll` to a name without an
  extension, as Windows does. `LoadLibraryA` and `GetProcAddress` run on the
  host's loader: a DLL loaded at run time maps and binds like one loaded with
  the process, and `ntdll` runs its `DllMain` with a null `lpReserved`. A DLL
  loaded at run time may not have a TLS directory (Windows XP loads one and
  its TLS does not work; this refuses it with `ERROR_DLL_INIT_FAILED`). A
  `DllMain` that refuses leaves its DLL mapped: there is no `FreeLibrary` yet.
  Errors are Windows': 126 for a missing module, 127 for a missing name or
  ordinal, 193 for a bad image.
- **The registry functions** reach the host's store (below). Volatile keys,
  a class and security attributes are not kept; `RegCreateKeyExW` accepts the
  default options only. The ANSI functions convert names and string data
  through Windows-1252.

## user32 and gdi32

`user32` is the window manager of the guest; the host's window manager only
frames its top-level windows. Everything is in guest memory: classes, the
window tree, the message queue, invalid regions, timers, focus, capture and
activation. One thread, so a sent message is a call to the window procedure.
The messages each call sends, and their order, are the ones Windows XP sent
`window.exe` (see [Fixtures](#fixtures)); among them:

- `CreateWindowEx` sends `WM_GETMINMAXINFO` (top-level only), `WM_NCCREATE`,
  `WM_NCCALCSIZE` and `WM_CREATE`. A child then gets `WM_SIZE` and `WM_MOVE`
  at once and its parent `WM_PARENTNOTIFY`; a top-level window gets them from
  `DefWindowProc`'s `WM_WINDOWPOSCHANGED` the first time it is shown.
- The first `ShowWindow` creates the host window and sends `WM_SHOWWINDOW`,
  `WM_WINDOWPOSCHANGING`, the activation (`WM_ACTIVATEAPP`, `WM_NCACTIVATE`,
  `WM_ACTIVATE`, whose default sets the focus), then erases the window and
  its children now (`WM_ERASEBKGND`), then `WM_WINDOWPOSCHANGED`.
- `GetMessage` takes posted messages, then `WM_QUIT`, then input, then
  `WM_PAINT` (parents before children), then `WM_TIMER`, and presents what
  was drawn before it waits. A mouse message, when it is taken, first sends
  `WM_NCHITTEST`, a click's `WM_PARENTNOTIFY` up the parents, a click's
  `WM_MOUSEACTIVATE` when its window is not the active one, and
  `WM_SETCURSOR`; none of them while the pointer is captured. `GetKeyState`
  changes as each input message is taken, not as it arrives.
- `TranslateMessage` posts `WM_CHAR` with the character the host typed for
  the key, and for F1 the undocumented `WM_KEYF1` (`0x004D`), which
  `DefWindowProc` turns into `WM_HELP` with a `HELPINFO` for the window, as
  Windows XP does and Solitaire expects; a double click is two presses within 500 ms and 2 pixels in a
  `CS_DBLCLKS` window.
- `DestroyWindow` hides the window, deactivates it (`WM_NCACTIVATE`,
  `WM_ACTIVATE`, `WM_ACTIVATEAPP`, `WM_KILLFOCUS`), then sends `WM_DESTROY`
  parents first and `WM_NCDESTROY` children first.
- The frame's close (its button, or Alt+F4, which the window manager keeps)
  arrives as an input `WM_SYSCOMMAND(SC_CLOSE)`; `DefWindowProc` makes it
  `WM_CLOSE`, and `WM_CLOSE` `DestroyWindow`.

**Frames.** A child with `WS_BORDER` has a one-pixel `COLOR_WINDOWFRAME`
border outside its client area, which `DefWindowProc` paints on
`WM_NCPAINT`; `BeginPaint` sends that first when the window was shown, moved
or resized since. A top-level window's frame is the window manager's, and a
disabled one draws its menu bar in `COLOR_GRAYTEXT`, flat, as Windows XP
does while a dialog or message box it owns is open. The program's main
window takes its class's small icon (`RegisterClassExW`'s `hIconSm`, else
its icon) to the host as 16x16 pixels (`SXW_SVC_WINDOW_ICON`), for its
caption and taskbar button.

**Drawing.** A top-level window's client area is a buffer in guest memory,
0x00RRGGBB per pixel, which every window of its tree draws on through a
device context: the window's origin on it and a clip region (inside its
ancestors, without its children when it has `WS_CLIPCHILDREN`, and inside
the update region for `BeginPaint`). gdi32 records what each call touched,
and user32 sends those rectangles to the host
(`SXW_SVC_WINDOW_PRESENT`), which copies them onto the window's surface;
copying, rather than mapping the surface a second time below 4 GiB, keeps
the guest out of `windowd`'s surface layout and costs no view. Regions are
up to 32 disjoint rectangles (`dlls/common/region.c`); `gdi32/raster.c`
fills under any ROP3 that reads no source, draws lines under any ROP2 with
the end point left out and a tie going to the smaller coordinate, the rule
Windows XP's lines follow in every octant, and draws glyphs.

**Text.** Every font is drawn with the platform's 13 px UI strike, MS Sans
Serif ([UI_FONTS.md](../desktop/UI_FONTS.md)), compiled into `gdi32` from the
same table the desktop uses. A weight of 600 or more, and the stock System
font, which is bold on Windows, are drawn twice one pixel apart with one more
pixel of advance, as Windows emboldens a bitmap font. The vertical metrics a
font reports are the requested font's on Windows XP, because programs size
windows from them: the System font is 16 pixels high with an ascent of 13,
and a font made with `CreateFontW` has Tahoma's (MS Shell Dlg's) proportions
at its height, an ascent and a descent of 2049 and 423 units of a 2048-unit
em, each rounded (13 pixels at -11, 14 at -12). Solitaire's status bar is
the System font's height plus its border. Glyphs sit on the font's baseline
and the widths are the strike's. Text is therefore laid out alike but not
drawn like Windows XP's TrueType faces, which is why fixtures keep it out of
the pixels they compare with Windows XP. `TextOutW` is
left- and top-aligned and opaque; `DrawTextW` breaks lines, aligns them,
draws `&` prefixes and computes rectangles, but does not clip to its
rectangle.

What is refused or bounded, and why:

- **One main window per process**, the `windowd` main window; every other
  top-level window (a dialog, a message box, a menu's popup) is a window it
  owns, since `windowd` owns windows only from a main one.
- **The window manager owns a top-level window's size and place.** A size
  it does not grant arrives as `WM_SIZE`; `MoveWindow` moves and sizes a
  child but only moves a top-level window; `ClientToScreen` counts from the
  position the program gave, since `windowd` does not say where it put the
  window. A dialog is placed by the window manager, centred over its owner,
  not where its template says.
- **`SetCursorPos` fails** with `ERROR_CALL_NOT_IMPLEMENTED`: the pointer is
  the window manager's. Only keyboard play in Solitaire calls it, to put the
  pointer on the selected card, and the game is complete with the mouse
  ([WIN32_SOLITAIRE_PROFILE.md](WIN32_SOLITAIRE_PROFILE.md#decisions)).
- `GetSysColor` answers Windows XP's classic colours.
- `GetDC(NULL)` answers `GetDeviceCaps` (the screen's size, 96 DPI, 32 bits)
  and draws nothing.

**Bitmaps.** A device-dependent bitmap is monochrome or in the screen's
32-bit format; both keep 0x00RRGGBB pixels, a monochrome one only black and
white. Each rule below was checked against Windows XP (`cards.exe`,
`resource.exe`):

- A memory device context starts with a stock 1x1 monochrome bitmap, so
  `CreateCompatibleBitmap` on a fresh one gives a monochrome bitmap; on any
  other device context, a colour one.
- `LoadBitmap` makes a monochrome bitmap of a one-bit DIB whose palette is
  black then white, and a colour one of anything else (cards.dll's red faces
  are one-bit DIBs too). `gdi32/dib.c` reads both header formats at 1, 4, 8,
  24 and 32 bits, bottom-up or top-down, uncompressed.
- `BitBlt` from monochrome to colour paints black as the destination's text
  colour and white as its background colour; from colour to monochrome, the
  source's background colour becomes white and the rest black. Any ROP3 is
  computed bit by bit.
- `StretchBlt`, when shrinking, ANDs the source pixels that fall on one
  destination pixel (`BLACKONWHITE`, the default mode), spreading them with a
  Bresenham walk from an error of `2 * destination - source` and dropping
  what is left over; when growing, each destination pixel takes the source
  pixel under its centre. Neither size may pass 4096 when they differ.
- `DeleteObject` on a selected bitmap succeeds, and the bitmap goes when it
  is deselected.
- Not done: a solid colour brush on a monochrome bitmap is made the nearer of
  black and white, where Windows dithers it. Nothing in the profile draws
  one.

**Resources** are read from the loaded image: its resource directory, type
then name (an id, or a name compared without case) then the first language.
`LoadStringW` with no room returns a pointer into the resource;
`LoadStringA` converts through Windows-1252. An icon or a cursor group gives
the image nearest the size asked for (32x32 unless `LoadImageW` names one),
the deepest among equals; the handle keeps the resource. The system's own
cursors and icons, by id with a NULL instance, have handles and no image.
`TranslateAcceleratorW` sends `WM_COMMAND` with 1 in the high word for a
`WM_KEYDOWN` or `WM_SYSKEYDOWN` that matches a virtual-key entry with exactly
its Shift, Ctrl and Alt, or a `WM_CHAR` or `WM_SYSCHAR` that matches a
character entry. Files (`LR_LOADFROMFILE`) are refused.

## Menus, dialogs and controls

`dlls/user32/menu.c`, `dialog.c` and `controls.c`; what they share with
`user32.c` is in `user32_private.h`. The messages and their order are the
ones Windows XP sent `dialog.exe` (see [Fixtures](#fixtures)), whose output
is byte for byte Windows XP's.

**Menus.** A `MENU` resource (the classic format) becomes a menu bar when its
class names it, or when `CreateWindowEx` is given one. The bar is drawn on
the window's surface, 20 pixels above the client area, which the host's
frame does not know about; `AdjustWindowRect` and `WM_NCCALCSIZE` count 19
for it, as Windows XP does with a caption one pixel shorter than its metric.
A press on the bar is `WM_NCLBUTTONDOWN` with `HTMENU`, which
`DefWindowProc` turns into `WM_SYSCOMMAND(SC_MOUSEMENU)`; Alt or F10 pressed
and released alone, or Alt with a letter (`WM_SYSCHAR`), is
`WM_SYSCOMMAND(SC_KEYMENU)`. Either runs the menu loop:

- It captures the pointer for the owner and sends `WM_ENTERMENULOOP`,
  `WM_INITMENU`, then for each selection `WM_MENUSELECT` (the item's index
  on the bar, its id in a popup, with `MF_MOUSESELECT` when the pointer chose
  it) and for each popup shown `WM_INITMENUPOPUP` and, when it goes,
  `WM_UNINITMENUPOPUP` followed by the bar item's `WM_MENUSELECT` again.
  It ends with `WM_CAPTURECHANGED`, `WM_MENUSELECT` with `0xFFFF` and no
  menu, and `WM_EXITMENULOOP`; the chosen command is posted as `WM_COMMAND`.
- The pointer moves between bar items and over popup items; a release on an
  item chooses it, a press outside ends the loop. A grayed item can be
  selected but not chosen.
- Keys: arrows move along the bar and the popup (separators skipped), Enter
  chooses, Escape closes the popup and then the menu, Alt or F10 ends it,
  and a letter chooses the item whose mnemonic it is (`WM_MENUCHAR` when
  none is). Keys the menu acts on are not translated into characters. A menu
  entered by keyboard underlines its mnemonics; one entered by pointer does
  not.
- A popup is a host popup at the item's lower left: framed by a raised
  edge, items 17 pixels high, separators 9, as Windows XP's classic menus.
  Windows XP's drop shadow is not drawn.

`EnableMenuItem` answers the old state, or -1 for no such item, and redraws
nothing, as Windows waits for `DrawMenuBar`. `TranslateAcceleratorW`, for a
command the menu has, sends `WM_INITMENU` and `WM_INITMENUPOPUP` first,
nothing more when the item is grayed, and `WM_UNINITMENUPOPUP` after
`WM_COMMAND`. Releasing Alt is `WM_SYSKEYUP` only when no other key went
down or up while it was held; otherwise it is `WM_KEYUP`.

**Dialogs.** `DialogBoxParamW` reads a classic `DLGTEMPLATE`. Its font gives
the dialog base units, the alphabet's average width rounded as Windows does
and the font's height, 6 and 13 for `MS Shell Dlg` 8 here as on Windows XP,
so a dialog and its controls have Windows XP's sizes and places in pixels.
In Windows XP's order: the owner is disabled (`WM_CANCELMODE`,
`WM_KILLFOCUS`, `WM_ENABLE`), the dialog gets `WM_SETFONT`, its controls are
made (each with `WS_EX_NOPARENTNOTIFY` and `WM_SETFONT`), `WM_INITDIALOG`
names the first tab stop and, answered TRUE, focusing it activates the
dialog before it is visible; `WM_CHANGEUISTATE(UIS_INITIALIZE)` follows, and
the modal loop shows the dialog when its queue first runs dry and sends the
owner `WM_ENTERIDLE`. `EndDialog` enables the owner, gives the dialog itself
the focus, hides it without `WM_SHOWWINDOW` and activates the owner; the loop
then destroys it. The dialog manager's procedure erases with the
`WM_CTLCOLORDLG` brush, answers `DM_GETDEFID` with the template's default
button, saves and restores the focus on deactivation, and turns `WM_CLOSE`
into `IDCANCEL`.

The keyboard is the dialog manager's: Tab and Shift+Tab move through the tab
stops, arrows through a group (from a `WS_GROUP` control to the next), Enter
is the focused default button or `DM_GETDEFID`'s, Escape is `IDCANCEL`, and a
mnemonic, with or without Alt, clicks its button or focuses what follows its
static text or group box. A push button that gets the focus becomes the
default one while it has it. The dialog asks itself `DM_GETDEFID` when the
focus moves, except from one push button to another or onto a push button by
pointer, and the button that lost the focus is redrawn: Windows XP's order,
which shows in the messages. Keyboard cues (focus rectangles and mnemonic
underlines) start hidden in a dialog the pointer opened and shown in one the
keyboard opened, and appear with Tab, the arrows or Alt (`WM_CHANGEUISTATE`,
`WM_UPDATEUISTATE`).

`MessageBoxW` is a dialog built in memory, with an icon of its own drawing,
the text and its buttons (`OK`, `Cancel`, `&Yes`, `&No`, `&Abort`, `&Retry`,
`&Ignore`); Escape is `IDCANCEL` only when there is a Cancel button, or `IDOK`
for a lone OK. `ShellAboutW` (`shell32`) is one too, built by `shell32` and
run through `SxwDialogBoxIndirectParamW`; it names SavanXP rather than show
Windows' own box. `WinHelpW` fails: there is no help engine, and Solitaire
shows its own message.

**Controls.** The `Button` and `Static` classes, with Windows XP's classic
look read back pixel by pixel: push and default push buttons, check boxes
(automatic or not), radio buttons (automatic or not), three-state boxes, group
boxes, owner-drawn buttons (`WM_DRAWITEM` with `ODA_DRAWENTIRE`, `ODA_FOCUS`
and `ODA_SELECT`, and the hidden cues as `ODS_NOFOCUSRECT` and `ODS_NOACCEL`),
and left-, centre- and right-aligned static text. A push or owner-drawn button
asks its parent for `WM_CTLCOLORBTN`, the others for `WM_CTLCOLORSTATIC`, and
paints the background the answer gives. A click presses a button while the
pointer is on it and notifies `BN_CLICKED` on release; Space does the same;
`BM_CLICK` too. An owner-drawn or radio button notifies a double click. A
radio button the keyboard moves to is clicked, and a checked radio button is
its group's tab stop. Group boxes and plain static text let the pointer
through to what is under them. State changes are painted at once, as Windows
paints them; a style change waits for the next `WM_PAINT`. The focus
rectangle is `DrawFocusRect`'s: the outline's pixels at odd x + y XORed with
the background colour, the others with the text colour.

What is left out: dialog templates in the extended format, other control
classes (edit, list box, combo box, scroll bar: Solitaire creates none),
the system menu, submenus, owner-drawn menu items and menu bars that wrap.

## The registry

`HKEY_CURRENT_USER` is one text file per user, kept by the host
(`loader/registry.c`) and written whole after every change:
`/users/user/Settings/win32.reg` under `winhost`, the file `--registry` names
under either host. `HKEY_CLASSES_ROOT` and `HKEY_LOCAL_MACHINE` exist and are
empty: opening a subkey of either finds nothing, creating one is refused with
`ERROR_ACCESS_DENIED`. That is what Solitaire's Help path meets: the HTML Help
control is not registered, so it shows its own error.

```text
SavanXP registry 1
[Software\Microsoft\Solitaire]
"Opciones"=dword:00000035
"Atrás"=dword:00000001
"Name"=hex(1):41,00,00,00
@=hex(3):
```

Names are UTF-8 in the file and compare without case for ASCII letters. A
`REG_DWORD` of four bytes is written as `dword:`, anything else as
`hex(type):` bytes. The store holds 64 keys, 256 values and 16 KiB of data; a
value that does not fit is refused with `ERROR_NOT_ENOUGH_MEMORY` and changes
nothing. A file that does not parse is reported and replaced by the next
change.

## Build

`Win32.cmake` compiles each source with the project's Clang for
`i686-pc-windows-msvc` (`-ffreestanding -fno-builtin -fno-stack-protector`)
and links with `lld-link /nodefaultlib /Brepro`. Every image declares
subsystem version 5.01, below which Windows XP refuses to load it, and
`/Brepro` makes the build reproducible, so a fixture has the same hash on
every machine. The outputs, under the build directory:

- `win32/system32/`: the subsystem's DLLs;
- `win32/fixtures/`: the fixtures and the DLLs only a fixture uses;
- `win32/stubs/`: DLLs built only for their import libraries, never
  installed (`nosuch.dll`, and a `kernel32.dll` that exports a function the
  real one does not).

A fixture with resources has a script compiled by `llvm-rc` (`/C 65001`: the
scripts are UTF-8) and linked with its objects. `resource.exe`'s images are
written into the build directory by `fixtures/resource/images.py`, so no
binary image is kept in the tree.

The target is `savanxp_win32`; `./build.sh test` builds it.

## Fixtures

Each fixture is built here and runs unchanged on Windows XP.

| Fixture | What it checks | Output and status on Windows XP |
| --- | --- | --- |
| `hello.exe` | An imported `GetStdHandle` and `WriteFile`; `ExitProcess` | `Hello from a PE32 executable.\r\n`, status 42 |
| `initorder.exe` | Imports from `alpha.dll` and `beta.dll`, which share a preferred base so one is relocated; `alpha.dll` imports from `beta.dll`; static TLS with two callbacks and no C runtime | The attach and detach order below, status 7 |
| `missingdll.exe` | An import from a DLL that is not installed | No output, status `0xC0000135` |
| `missingsym.exe` | An import `kernel32.dll` does not export | No output, status `0xC0000139` |
| `startup.exe` | It starts the way `sol.exe` does: the same runtime imports, an SEH frame at `fs:[0]` with `_except_handler3`, C and C++ initializers through `_initterm`, `__getmainargs`, the tail of `_acmdln`, a `WinMain`. Then every non-GUI function of the matrix, failure paths included: `rand` after two seeds, `MulDiv`'s rounding and overflow, a missing module and procedure, `beta.dll` loaded at run time and found by name and ordinal, 115 `LPTR` blocks, and the registry (a counter, `ERROR_MORE_DATA`, a size query, an ANSI read, the value name `Atrás`, a missing value and key) | Run twice as `startup.exe alpha "two words" 3`: `expected/startup-1.out` then `startup-2.out`, where the counter reads back the first run's 1 as 2; status 5 |
| `window.exe` | A top-level window with a child, painted with the matrix's drawing calls (`PatBlt` with `PATCOPY`, `BLACKNESS` and `DSTINVERT`, `FrameRect`, `InvertRect`, lines in every octant under `R2_COPYPEN` and `R2_NOT`, `SetPixel`, `TextOutW`, a bold `CreateFontW`, `DrawTextW`), two chained timers (one with a timer procedure), and its input script: clicks, a double click, a right click, a click on the child, a captured drag, keys with Shift, an invalidated rectangle, and the frame's close. It writes the messages it gets (the list it keeps is in `logged()`) and, on the key P, its client area outside the text box as runs of colour read back with `GetPixel` | `expected/window.out`, status 3 |
| `cards.exe` | Loads the user's own `cards.dll` from its folder with `LoadLibraryA` and draws, with `cdtInit`, `cdtDrawExt` and `cdtTerm`, the 52 faces and 12 backs at 71x96 on Solitaire's green, the other modes (inverted, the empty pile's marks, X, O) on a few cards, and the 12 backs at 33x62 as the Deck dialog does. It writes a CRC-32 of each drawing's pixels read back with `GetPixel`, or, with the argument `pixels`, the pixels themselves | `expected/cards.out`, status 0. The pixel listing describes Microsoft's art and is kept with the user's files, never here |
| `resource.exe` | Its own resources (`resource.rc`): strings, bitmaps in both header formats at 1, 4 and 24 bits, an icon, a cursor and an accelerator table. It loads them as `sol.exe` does, then copies, converts and stretches the bitmaps (grown by 2, grown to odd sizes, shrunk), and drives the accelerators with posted keys | `expected/resource.out`, status 0 |
| `dialog.exe` | A menu of two popups with separators and an accelerator shown after a tab, an accelerator table, and two dialogs built from the same resource types and styles as `sol.exe`'s (`dialog.rc`): one of owner-drawn buttons with a default and a cancel button, like Deck, and one of group boxes, radio buttons the program checks itself, automatic check boxes and static text left and right, like Options; then `MessageBoxW`, `ShellAboutW` and `InitCommonControlsEx`. Its input script uses the menus by pointer and by keyboard alone, a grayed item, both dialogs by pointer and by keyboard (Tab, arrows, Space, mnemonics, Enter, Escape), an accelerator for a menu command and one for a command no menu has. It writes the messages its procedures get (the list is in `logged()`), where each control is, the buttons' states, and pixels of the controls outside their text | `expected/dialog.out`, status 5 |

The bytes Windows XP wrote are kept in `subsystems/win32/fixtures/expected/`
(`-text` in `.gitattributes`: CRLF and Windows-1252 as they came). Both hosts'
tests compare against them.

`initorder.exe` writes, on Windows XP and in the harness, byte for byte:

```text
beta: attach, static load, pointers agree
alpha: attach, beta says 7, pointers agree
initorder: TLS callback 1, reason 1
initorder: TLS callback 2, reason 1
initorder: entry, thread value 7, alpha says 3, beta says 7
alpha: detach, process ending
beta: detach, process ending
initorder: TLS callback 1, reason 0
initorder: TLS callback 2, reason 0
```

"Pointers agree" means a pointer stored in the DLL's data, which only a base
relocation can make right, equals the address the code computes. The
thread value is the template's 5 plus one from each callback.

Windows XP shows a message box when a console program fails to load, and
its text does not describe the failure (for the missing function it said
"terminated by Ctrl+C"). The status the command prompt received is the
reference: `-1073741515` (`0xC0000135`) and `-1073741511` (`0xC0000139`).

Recorded on 2026-11-12, on the user's Windows XP SP3 virtual machine, from
these files built with Clang and lld 23.1.4:

| File | SHA-256 |
| --- | --- |
| `hello.exe` | `8ced2cebedac96a1585a877257265fce06a44a6b452da031be24a5a98c27b8d3` |
| `initorder.exe` | `a3d7c11df9db3fa33f680d70f3c67ab64b7bc6d58719ccc661c2e51203c8e9b5` |
| `alpha.dll` | `8f01657090eb24064b09da6711bc9ae50af97bdb1317427bf26d15423d917198` |
| `beta.dll` | `2ee1d8a68606b23b5986cb1d707cdcf7e832b7f836207f9383f3571d6163e79a` |
| `missingdll.exe` | `0e81e4bea9b9d08b4fb6fad7b1864e428f86f42207eb40c8567df83d1ad045cf` |
| `missingsym.exe` | `43d78fd4b444d7fee9df762b429e4681a39439ed886637e6a071d799ba5b7b9c` |
| `startup.exe` | `e1d9283206f99e0748495794d9df3a1cc7b1c529a7fb1b4079cc21fe5d7ad9ba` |
| `window.exe` | `1ec8351db7662ff5471b7bd587ef1734da6fca73416def7e4d39b91875e30dba` |
| `cards.exe` | `940f7be09e9f614497ef2b889bebab73233dccdbf8c37b9d68e190c1e60cbcca` |
| `resource.exe` | `bf9ca037356201d43996cf64670e301c4e63699ec8902f5b753d146ab0404c43` |
| `dialog.exe` | `2378b6b12d0b486e7cf59fffca46ce2ef137f909905824dec91d5e9fe57836e8` |

A different compiler version may change the bytes, not the behaviour; when
a fixture changes, it is run on Windows XP again and this table follows. The
files went to the machine on a USB disk image the batch file wrote its
output and `%errorlevel%` back to. The first run showed the one difference
the harness had: Windows XP detaches on exit, which `RtlExitUserProcess` now
does. `startup.exe` was run the same way; the batch file puts its redirection
first (`>s1.out startup.exe ...`), because `cmd` keeps the blank before a
trailing `> file` in the program's command line.

`window.exe` was driven by its input script over QMP (`tools/win32_input.py`)
on the same machine, in the classic style, once with the argument `all`,
which writes every message, and once without. What `all` showed decided the
list the fixture keeps: it leaves out the hit tests and cursor queries every
pointer move brings, the pointer moves themselves (how many a drag makes
depends on the device), the frame's own messages (`WM_NCPAINT`,
`WM_GETICON`, `WM_NCACTIVATE`, the input method's), `WM_MOVE` (the window
manager places the window) and the keys that close it, which differ by
environment while the `WM_SYSCOMMAND` they produce does not.
`expected/window.ppm.gz` is the client area the harness dumps at the end of
the script; outside the text box it is pixel for pixel the one on Windows
XP's screen.

`cards.exe` ran on the same machine with the user's `cards.dll` next to it,
once writing CRCs and once writing pixels; here both are identical, every
pixel of every drawing. Its 71x96 drawings also equal the W0 captures of
Solitaire's own screen ([WIN32_SOLITAIRE_PROFILE.md](WIN32_SOLITAIRE_PROFILE.md#reference-captures))
except the twelve corner pixels `cdtDrawExt` leaves alone, which show the
table there and the fixture's own fill here. `resource.exe` ran three times
there: what it first showed decided the stretch rules and the deferred
delete above, and that a colour brush is dithered on a monochrome bitmap.

`dialog.exe` was driven by its input script on the same machine, once with
the argument `all` and once without, and its output here is identical.
What `all` showed decided the list it keeps: it leaves out what the
window manager decides (`WM_NCPAINT`, `WM_NCACTIVATE`, `WM_GETTEXT` for the
caption, the `WM_WINDOWPOS*` pairs, the input method's), what timing
decides (`WM_ENTERIDLE`, pointer moves, hit tests and cursor queries), paint
messages (on Windows XP the main window repaints where a dialog covered it;
on SavanXP nothing covers it), and `WM_CTLCOLOR*`, which come with every
paint. Screenshots of both dialogs and both menus were compared there too:
the frames, glyphs, edges, separators, focus rectangles and the group boxes'
labels are Windows XP's, pixel for pixel, outside the text.
`expected/dialog-menu.ppm.gz` (the Help menu open) and
`expected/dialog-settings.ppm.gz` (the Settings dialog) are the harness's
images; SavanXP's screen must equal them.

## Input scripts

A fixture with a window comes with an input script, played the same way in
every environment: by the harness itself (`--input`), and over QMP into
Windows XP and into SavanXP (`tools/win32_input.py`, which finds the client
area on a screenshot by the fixture's marker, a 4x4 magenta square at its
top-left corner, and needs an absolute pointer). One action per line, in the
main window's client coordinates (the menu bar is above them, at y < 0), or
a dialog's after `window 1`, found by its cyan marker:

```text
move X Y                 the pointer moves
down|up left|right X Y   a button goes down or up there
keydown VK [CHAR]        a key goes down: virtual key, and the UTF-16
keyup VK                 character it types (numbers in C syntax)
wait MS                  nothing happens for MS milliseconds
close                    the frame's close (Alt+F4 over QMP)
window N                 the N-th dialog open from here on; 0 the main window
dump FILE [WxH]          the client area, W x H or the whole, saved as a
                         binary PPM with any popup over it
```

The harness counts coordinates from each window's marker too, gives the
pointer to a popup it is over, and keys to the newest dialog, as a window
manager would. Without a marker it counts them from the window's top-left
corner, the menu bar's when it has one.

A program that draws no marker, Solitaire, is found by its client area
instead: `win32_input.py --anchor R,G,B:WIDTH:DY` takes the first row of the
screen with a run of exactly WIDTH pixels of that colour and moves DY rows
(`0,128,0:585:-20`: the green board, the menu bar above it), and
`window N WIDTH` finds a dialog by a run of WIDTH pixels of `COLOR_BTNFACE`
along its client top (the harness ignores WIDTH).

In the harness the guest's millisecond clock is virtual: it moves only
through `wait`, and an action is given only when the guest waits for input,
so timers fire at the same point of every run and a program that only polls
sees no input.

## The host harness — `subsystems/win32/harness/`

`savanxp-win32-harness [--dlls DIR] [--registry FILE] [--time SECONDS]
[--input SCRIPT] program.exe [arguments...]` runs a fixture on the
Linux x86-64 build host through the same loader and DLLs as `winhost`. The
guest's standard output is the harness's; the guest's 32-bit exit status, or
the load failure's NT status, is printed as `win32: exit status 0x...` and
its low byte is the harness's exit status.

The gate uses Linux's own selectors: `0x23` for 32-bit code, `0x2b` for data,
`0x33` for 64-bit code. FS holds an LDT entry (`modify_ldt`) whose base is the
TEB while guest code runs; glibc's FS base is put back with `arch_prctl`
before any host C code runs. The 32-bit halves of each transition, and the
64-bit landings they far-jump to, live in a page at `0x7FFE0000`, since a
32-bit far jump cannot reach the harness's own code. A fault in guest code
prints its address and ends the harness with 125.

## winhost — `subsystems/win32/host/`

`winhost.sxe [--dlls DIR] [--registry FILE] [--time SECONDS]
/path/to/program.exe [arguments...]` is a SUN program in `/system`. `--time`
fixes the guest's clock at that second, so a program that seeds `srand` from
`time` repeats its run however long its start takes; the millisecond clock
still runs. DLLs are looked up next to the executable, then in `DIR`, by
default `/system/win/drive_c/windows/system32`. The guest's standard output
and error are `winhost`'s; its exit status is the guest's full 32-bit status,
or the NT status of a failed load, with `win32: exit status 0x...` and any
diagnostic on standard error. A failed load is also checked to have released
every section and file; if not, `winhost` exits with 124.

It is the same loader as the harness over a different platform:

- **Memory.** Each region is its own section, granted read, write and execute
  and viewed at its address with `SXN_SYS_MAP_VIEW_AT`. An image run is
  filled through a read-write view, then unmapped and mapped again read-only
  or read-execute. No view is ever writable and executable.
- **Files.** `sxn_query_path`, `sxn_open_file` and `sxn_read`, the whole file
  read into the native heap and freed once the load ends.
- **The gate.** The 32-bit code segment is `0x2b`, 64-bit code `0x23`, data
  `0x1b` ([ARCH.md](../kernel/ARCH.md#segments)). `winhost` loads the data
  selector into DS, ES and FS once, then sets FS's base to the TEB with
  `sxn_set_thread_base`, and never switches them again: the native runtime
  does not use FS, and the kernel keeps all three per thread. The 32-bit
  halves of each transition live in a section at `0x7FFE0000`, as in the
  harness.
- **Services** come from `loader/services.c`, shared with the harness.
- **Windows** are `windowd`'s, through `sxnwm` (SUN's windowing layer). The
  first top-level window connects; the title and the guest's icon go in its
  presentation, the
  client size is asked for and waited on for up to a second, and a size the
  window manager gives later, or its close, becomes an event. Every other
  window is an owned window of that connection (`sxnwm_window_open_ex`): a
  dialog with its title, a menu as a popup that a press elsewhere dismisses,
  which arrives as the popup's close. Presenting copies the guest's
  rectangles onto the right surface and submits them. Input comes from the
  native ring for the main window and from the event pipe for the others:
  pointer records as they are, keys as Windows virtual keys with the
  character the keyboard layout typed (none with Ctrl held), the window
  manager's own shortcuts (Alt+F4, Alt+Tab) never reaching the guest. A key
  arrives as the character it typed, so Shift and a digit are mapped back to
  the digit's key by the layout the kernel applies (`INPUT_IOC_GET_LAYOUT`):
  Alt+Shift+2 is the accelerator Solitaire knows in Spanish and in English.
  The guest's millisecond clock is the native one.

`winhost` itself is linked at `0x400000` and ends near `0x48A000`, well
below `0x01000000`, where `sol.exe` must load. A fixture that prefers
`0x400000` is relocated.

**Views.** The kernel gives a process 128 section views
([SECTIONS.md](../kernel/SECTIONS.md#the-budget)). `hello.exe` uses 14: three
images of about five protection runs each (headers, code, read-only data,
data, relocations), the stack, the TEB block and the trampoline.
`initorder.exe` uses 29. A window adds `windowd`'s two (surface and input
ring) and the region of its pixel buffer when it is larger than a heap
arena; an open dialog or menu adds the view of its surface. Solitaire's ten
images need about 50.

## Solitaire

The first profile's application: the user's own Windows XP `sol.exe` and
`cards.dll` (WIN32_SOLITAIRE_PROFILE.md), unchanged. They are installed by
copying them, by hand or with Files, to
`/system/win/drive_c/program files/solitaire/`, in lower case; the build
never touches that folder, so they survive `./build.sh build`. From there:

- **Launching.** `winhost.sxe` declares `ext_open=.exe` in its `.sxres`
  (`host/winhost.sxres`), so Files and the desktop open a `.exe` with it
  through `file_assoc`. The build installs `/system/start/Solitaire.sxlnk`,
  whose target is that `sol.exe`, as a default pin copied into the user's
  `Start Menu` folder; the Start menu lists it at the top, with
  the program's own icon read from its PE resources, once the file is there
  ([START_MENU.md](../desktop/START_MENU.md#shortcuts)). Under the window
  manager the window shows the same icon, from the class `sol.exe` registers.
- **Settings** are the registry's, `HKCU\Software\Microsoft\Solitaire`
  (`Atrás` for the deck, `Opciones` for the options), in the user's
  `win32.reg`, so they persist from one run, and one boot, to the next.
- **What runs** is everything Solitaire does: dealing, the stock, moves by
  click, drag and double click, the outline dragging of the options, Undo,
  both menus, Deck, Options, About, the end-of-game animation and its
  message box, Help's error message. Moving, covering, minimizing and
  restoring the window need no repaint, since `windowd` keeps its surface;
  resizing it repaints.

**The same game as on Windows XP.** Solitaire seeds every deal with
`srand(time() & 0x7fff)`. With the clock fixed at 998621760 (2001-08-24
02:56 UTC: `winhost --time`, the harness's `--time`, and on Windows XP a
launcher that sets the clock before starting `sol.exe`), the first deal is
the same everywhere. `fixtures/solitaire/input-1.txt` wins it in 98 moves by
pointer, found by a solver, with every click 600 ms after the last so that
no two make a double click on one system and not on the other; then it stops
the end-of-game animation, answers No, picks a deck and options and exits.
`input-2.txt` starts again on the same registry: the deck and options read
back, a card drawn alone, a dragged card as an outline, Undo, About, and the
frame's close. Played on Windows XP, every board those scripts dump (15)
equals the harness's outside the pointer and the menu bar's text, the Deck
dialog does outside its buttons' text, and the registry holds the same two
values. The images show Microsoft's card art, so only their SHA-256 are
kept (`expected/solitaire.sha256`), with the registry after run 1
(`expected/solitaire.reg`).

**Limits.** `MessageBoxW` lays its box out from a dialog template in dialog
units, so it is a pixel wider than Windows XP's here and its buttons are
labelled in English. `windowd` does not tell a program its window was
minimized: `IsIconic` stays false and no `WM_SIZE(SIZE_MINIMIZED)` arrives.
`SetCursorPos` fails, so keyboard play leaves the pointer where it is. Help
reports that it cannot load, as it does on a Windows without HTML Help.

## Tests

`win32-pe-test` (in `./build.sh test`) is built with the address and
undefined-behaviour sanitizers. It:

- builds a small DLL in memory, with imports by name and ordinal, exports with
  a forwarder, base relocations and a TLS directory with two callbacks, and
  checks every stage against it, including the relocated TLS addresses;
- runs the malformed corpus: one mutation of that image for each refusal above,
  each expecting its own status and a message;
- applies 20,000 random byte changes and truncations to the image and runs
  every stage on each, so a read outside a buffer fails the test.

`win32-hello` (`./build.sh smoke win32-hello`) boots SavanXP and runs
`win32test.sxe`, a SUN parent that starts `winhost` with each fixture,
captures its standard output through a pipe and compares the bytes and the
32-bit exit status with Windows XP's. Each case is a line in the system log.
Run it under `--accel kvm` as well: QEMU's TCG does not fault on a null data
selector in 32-bit code, so only KVM or hardware catches a lost one.

`win32-registry-test` checks the store alone, with the sanitizers on: every
error the registry services return, the file's round trip, malformed files,
and compaction when values are replaced again and again.

`win32-harness-test` runs the fixtures through the harness and
compares the output bytes and the full 32-bit status with what Windows XP
produced for the same files. A failed load must also name its cause and
release every mapping and file. `window.exe` and `dialog.exe` play their
input scripts, and what they dump must equal the golden images
(`expected/window.ppm.gz`, `dialog-menu.ppm.gz`, `dialog-settings.ppm.gz`);
a different one is kept in the build directory as `win32-NAME-actual.ppm`.

`win32-window-test` checks the pure parts of `user32` and `gdi32` with the
sanitizers on: regions against a brute-force set of pixels, including what
happens at the 32-rectangle bound; every ROP2 and ROP3 against its truth
table; 3,000 random lines against the rule above; clipped fills and glyphs;
DIBs in both header formats, the monochrome rule and every refusal; and
blits: both format conversions, a source outside its bitmap, overlapping
copies both ways, and the stretch rules on the cases Windows XP showed.

`win32-window` (`./build.sh smoke win32-window`) boots SavanXP with a USB
tablet and runs `win32test.sxe --window`, which starts `compositord` and
`windowd` with no desktop, then `window.exe` under `winhost`, and logs
`WIN32 WINDOW READY`. The host (`tools/win32_window_smoke.py`) then plays
the input script over QMP and compares the client area of its `dump` with
the golden image, while the guest compares the messages and the status
with Windows XP's. Run it under `--accel kvm` as well.

`win32-dialog` (`./build.sh smoke win32-dialog`) does the same with
`dialog.exe` (`win32test.sxe --dialog`, `tools/win32_dialog_smoke.py`): its
menus and dialogs as `windowd` owned windows, its messages against Windows
XP's, and the menu and the Settings dialog on the screen against their
golden images. When an output differs, `win32test` logs the first line that
does, both ways.

`win32-cards-test` runs `cards.exe` through the harness with the user's
`cards.dll`, from the folder named by `SAVANXP_WIN32_FIXTURES`, copied next to
it in a temporary folder, and compares the output with Windows XP's. Without
the file it is reported as skipped.

`win32-cards` (`./build.sh smoke win32-cards`) does the same on SavanXP. The
scenario names the user's files it needs (`win32_files` in
`tools/smoke_catalog.py`); the runner copies them from
`SAVANXP_WIN32_FIXTURES` into the disposable copy of the disk the scenario
boots, under `/system/win/drive_c/tests`, never into the build or
`build/disk.img`, and exits with 77, skipped, without them. `win32-hello`
runs `resource.exe` with the other fixtures.

`win32-solitaire-test` runs the user's `sol.exe` and `cards.dll` from
`SAVANXP_WIN32_FIXTURES` through the harness twice, on one fresh registry
and with the clock fixed, playing `fixtures/solitaire/input-1.txt` and
`input-2.txt`; the registry after run 1 and the SHA-256 of every image they
dump must be the expected ones. Without the files it is reported as skipped.

`win32-solitaire` (`./build.sh smoke win32-solitaire`) does the same on
SavanXP under `windowd`: the runner copies the two files to
`program files/solitaire` in the disposable disk (`NAME@DIR` in
`win32_files`), `win32test.sxe --solitaire` runs `sol.exe` twice from there
and checks the statuses and the registry, and the host
(`tools/win32_solitaire_smoke.py`) finds the board by its colour, plays both
scripts and compares the hashes.

`pe-icon-test` checks the shell's readers of a `.exe` with the sanitizers
on. The icon (`sxgfx/pe_icon.h`): `resource.exe`'s at 32 and 16 pixels
against known checksums. The version strings (`sxgfx/pe_version.h`): those
of a minimal PE the test writes itself, so no fixture changes and none needs
a new run on Windows XP, and, with `SAVANXP_WIN32_FIXTURES`, `sol.exe`'s
"Juego Solitario". Also a program with neither, and 20,000 random byte
changes and truncations of each file.

`win32-pe-fixtures-test` reads the user's Windows XP `sol.exe` and `cards.dll`
from the folder named by `SAVANXP_WIN32_FIXTURES`, as an environment or CMake
cache variable, and verifies the W0 hashes. It then checks what the profile
records: bases, sizes, entry points, `sol.exe`'s 122 imports from eight DLLs,
its refused move, `cards.dll`'s six exports by name and ordinal, its zero-filled
`.data` and a relocation moved by the right delta. Without the folder it is
reported as skipped. The files never enter the tree or the build.
