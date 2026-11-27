# SSW — the Savan Subsystem for Windows

SSW is SavanXP's Win32 subsystem: it runs unmodified Windows programs. Win32
is the API it implements, as POSIX is the API of the POSIX subsystem; SSW is
the subsystem's name. Its source tree keeps the API's name
(`subsystems/win32/`), its code the `sxw_` prefix, and its host program is
`winhost`.

> **Status: W6 complete (2026-11-13). WordPad's WP6 complete (2026-11-26);
> WP7a, [RichEdit](#richedit)'s text, written (2026-11-27).**
> The user's Windows XP [WordPad](#wordpad) and its MFC load and start up
> to their first window. SSW handles [software exceptions](#software-exceptions),
> runs [threads](#threads), and has [files](#files), heaps, atoms and
> [the locale](#the-locale) as Windows XP has them.
> This document describes what exists
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
`sol.exe` and the fixtures use is written; what Windows XP WordPad and its MFC
import beyond that is [declared](#declared-entry-points), so their imports
resolve.

```text
subsystems/win32/
  include/       windows.h (the subsystem's own), sxw_abi.h (host <-> DLLs)
  pe/            the PE32 parser and mapper, portable C
  loader/        the process loader, portable C
  dlls/          ntdll, kernel32, msvcrt, advapi32, gdi32, user32, shell32,
                 comctl32, ole32, comdlg32, shlwapi, msftedit (PE32); common/ holds the
                 code page, regions, memcpy and declared entry points they
                 share; declared.py generates those from each declared.txt
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
windows/system32/   ntdll, kernel32, msvcrt, advapi32, gdi32, user32, shell32,
                    comctl32, ole32, comdlg32, shlwapi and msftedit .dll
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
   - `pe_for_each_delay_imported_dll()` checks the delay-load descriptors
     and their name tables, and lists their DLLs;
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
- Delay-load imports with RVAs, what every linker since Visual C++ 7 writes
  (`mfc42u.dll` has thirteen). They are checked, not bound: the image's own
  helper loads each DLL and fills its slots on the first call, through
  `LoadLibrary` and `GetProcAddress`, so a DLL that is never called need not
  exist.

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
- A data directory outside the image. **CLR images** are refused outright,
  not ignored.
- Delay-load descriptors with VAs instead of RVAs, and descriptors whose
  name, module handle slot, IAT or name table lies outside the image.
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
   free 64 KiB step from `0x10000000`, and is relocated, and its delay-load
   descriptors are checked. It is mapped as one
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
  8 has forty-nine services covering process exit, standard I/O, debug
  output, time, heap regions given and taken back, module lookup and
  loading, symbol lookup, `GetModuleFileName`, exception handler validation,
  the registry, [handles and files](#files), and [threads](#threads) with
  the objects they wait on.
  The window services provide a millisecond clock and screen dimensions.
  They create, destroy, title and present top-level windows and wait for
  pointer, keyboard, resize and close events.
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
`0x7C9C0000`, `comctl32.dll` at `0x5D090000`, `ole32.dll` at `0x774E0000`,
`comdlg32.dll` at `0x763B0000`, `shlwapi.dll` at `0x77F60000`,
`msftedit.dll` at `0x4B460000`), so none of them relocates. `include/windows.h` is the subsystem's own header;
it declares what the DLLs and fixtures use and grows with the W0 matrix.
`dlls/common/codepage.c`, compiled into the DLLs that need it, converts
between UTF-16 and the "ANSI" code page, Windows-1252, the one Windows XP uses
in Western European languages.

| DLL | Exports |
| --- | --- |
| `ntdll.dll` | `RtlExitUserProcess`; private: `SxwProcessStart`, `SxwService`, `SxwLoadLibrary`, `SxwGetProcAddress`, `SxwFindModule`, `SxwTime`, `SxwHeapAllocate`, `SxwHeapFree`, `SxwHeapSize`, `SxwHeapResize`, `SxwHeapIsBlock` |
| `kernel32.dll` | `ExitProcess`, `GetStdHandle`, `WriteFile`; Solitaire's nine (`GetModuleHandleA`, `GetStartupInfoA`, `GetCommandLineW`, `LocalAlloc`, `LocalFree`, `MulDiv`, `lstrlenW`, `LoadLibraryA`, `GetProcAddress`); and for WordPad's start-up: the last error, the version (`GetVersion`, `GetVersionExA`, `GetProcessVersion`), `SetErrorMode`, the current process and thread, `GetStartupInfoW`, the clocks (`GetTickCount`, `GetSystemTimeAsFileTime`, `QueryPerformanceCounter` and `Frequency`), TLS, critical sections, the three `Interlocked` functions, global and local memory with moveable blocks, the `lstr` family, `MultiByteToWideChar`, `WideCharToMultiByte`, modules (`GetModuleHandleW`, `LoadLibraryW`, `LoadLibraryExA` and `W`, `FreeLibrary`, `DisableThreadLibraryCalls`, `GetModuleFileNameW` and `A`, `GetSystemDirectoryA` and `W`), resources (`FindResourceW`, `FindResourceExW`, `LoadResource`, `LockResource`, `SizeofResource`, `FreeResource`) and the locale (`GetUserDefaultLCID`, `GetSystemDefaultLCID`, `GetThreadLocale`, `GetLocaleInfoW`); for WP3 and WP4, threads and the objects they wait on, private heaps, files, searches and paths, atoms, the calendar, the date and time formats, `CompareStringW` and win.ini; the full list is `dlls/kernel32/kernel32.def` |
| `msvcrt.dll` | Solitaire's eighteen; the Unicode start-up (`__wgetmainargs`, `_wcmdln`, `__p___argc`, `__p___wargv`), `_onexit`, `__dllonexit`, `setlocale`, `abort`, `_purecall`, `std::terminate`, `type_info`'s destructor; the heap (`malloc`, `calloc`, `realloc`, `free`, `_msize`, `_expand`) and the `mem` functions; wide strings, `wcstol`, `wcstoul`, `wcstod`, `_wtoi`, `_wtol`, `_itow`, `_ltow`, `_ultow`, `iswdigit`, `iswspace`, `strcmp`, `strcpy`; `abs`, `labs`, `fabs`, `floor`, `ceil`, `modf`, `_ftol`; `_vsnwprintf`, `_snwprintf`, `swprintf`, `vswprintf`; the full list is `dlls/msvcrt/msvcrt.def` |
| `advapi32.dll` | Solitaire's six (`RegCreateKeyExW`, `RegOpenKeyExA`, `RegQueryValueExW`, `RegQueryValueExA`, `RegSetValueExW`, `RegCloseKey`) and `RegOpenKeyExW` |
| `gdi32.dll` | The matrix's twenty-seven: `GetStockObject`, `SelectObject`, `DeleteObject`, `CreateSolidBrush`, `SetBrushOrgEx`, `PatBlt`, `SetROP2`, `MoveToEx`, `LineTo`, `LineDDA`, `SetPixel`, `GetPixel`, `PtVisible`, `GetDeviceCaps`, `CreateFontW`, `TextOutW`, `SetTextColor`, `SetBkColor`, `GetTextExtentPoint32W`, `GetTextMetricsW`, `CreateCompatibleDC`, `DeleteDC`, `CreateCompatibleBitmap`, `BitBlt`, `StretchBlt`, `GetObjectA`, `GetDCOrgEx`; for WordPad, `GetObjectW` and `CreateFontIndirectW`; private: `SxwCreateSurfaceDC`, `SxwSurfaceOfDC`, `SxwCreateBitmapFromDIB`, `SxwSetBkMode` |
| `user32.dll` | The matrix's sixty-two: windows and messages (classes, creation and destruction, showing, moving, enabling, focus, the message calls, painting, capture, keys, timers, the rectangle helpers, `DrawTextW`, `FrameRect`, `InvertRect`, `GetSysColor`, `SetCursorPos`, `ShowCursor`), resources (`LoadBitmapA`, `LoadStringW`, `LoadStringA`, `LoadIconW`, `LoadCursorW`, `LoadImageW`, `LoadAcceleratorsW`, `TranslateAcceleratorW`), menus (`GetMenu`, `EnableMenuItem`), dialogs (`DialogBoxParamW`, `EndDialog`, `GetDlgItem`, `CheckDlgButton`, `IsDlgButtonChecked`, `CheckRadioButton`, `MessageBoxW`) and `WinHelpW`; for WordPad, `GetSystemMetrics`, `GetSysColorBrush`, `RegisterClipboardFormatW`, `FindWindowW`, `GetActiveWindow`, `GetCursorPos` and the rest of the rectangle helpers; the full list is `dlls/user32/user32.def`. Private: `SxwDialogBoxIndirectParamW` |
| `shell32.dll` | `ShellAboutW` |
| `comctl32.dll` | `InitCommonControlsEx`, which registers nothing: Solitaire uses no common control |
| `ole32.dll` | `OleInitialize`, `OleUninitialize`, `CoRegisterMessageFilter`, `CoTaskMemAlloc`, `CoTaskMemFree`: the minimum WordPad and MFC need ([WIN32_WORDPAD_ROADMAP.md](../roadmaps/open/WIN32_WORDPAD_ROADMAP.md#ole-is-the-minimum-wordpad-and-mfc-need)) |
| `shlwapi.dll` | `PathFindFileNameW` |
| `comdlg32.dll` | Declared entry points only (below) |
| `msftedit.dll` | The `RICHEDIT50W` class, registered when it attaches, and `RichEditWndProc` ([RichEdit](#richedit)); the `IID_IRichEditOle`, `IID_IRichEditOleCallback`, `IID_ITextServices`, `IID_ITextHost` and `IID_ITextHost2` GUIDs |

The table lists what is written. Each DLL also exports the entry points of
its `declared.txt`, below.

### Declared entry points

A program's imports must all resolve before it runs, including the many it
never calls in a given session: of the 599 functions `wordpad.exe` and
`mfc42u.dll` import at load time, a recorded session reached 385. A function
that is imported but not yet written is **declared**: it is a line in its
DLL's `declared.txt` (name, the argument bytes a `stdcall` function pops or
`cdecl`, and the value it returns), and `dlls/declared.py` generates an
entry point for it and appends it to the DLL's exports. Called, a declared
entry point:

- writes `<dll>!<name> is only declared` on the debug output, the first time
  only (`win32: debug:` on the host's error stream);
- sets the last error to `ERROR_CALL_NOT_IMPLEMENTED` (120) when it is
  `stdcall`; the C runtime's functions have no last error;
- returns its table's value, which is the function's documented failure: 0
  for most, -1 where the failure is `INVALID_HANDLE_VALUE`, `WAIT_FAILED`,
  `CLR_INVALID` and the like, `E_NOTIMPL` for an `HRESULT`, 120 for a
  function that returns its error (the registry's).

A `data` line exports four zero bytes instead (`msvcrt.dll`'s `_wcmdln`).
A name may not be both written and declared: the generator refuses it, and
writing a function means removing its line. So `declared.txt` is, per DLL,
the list of what a program may import and still finds unimplemented, and the
debug output says which of those a run reached.

### What they do, and what they refuse

- **The C runtime's start-up.** `_acmdln` is the command line in the ANSI
  code page, made when `msvcrt.dll` attaches. `__getmainargs` splits it the
  way `msvcrt.dll` does (backslashes before a quote, `""` inside quotes) and
  gives an empty environment: there is no environment yet. `_initterm` calls
  every non-null pointer in its range. `_controlfp` maps its bits onto the
  x87 control word and back. `_adjust_fdiv` is 0.
- **Exceptions.** [`RaiseException`](#software-exceptions) walks the guest's
  x86 frame chain. `RtlUnwind`, `_except_handler3` and `__CxxFrameHandler`
  run the termination and cleanup code compiled into those frames.
- **The Unicode start-up.** `_wcmdln` is the command line as UTF-16, made
  with `_acmdln`; `__wgetmainargs` splits it by the same rules as
  `__getmainargs` and keeps argc and argv for `__p___argc` and `__p___wargv`,
  which MFC reads.
- **Exit.** `_onexit` registers functions (a C++ program's static
  destructors among them) that `exit` and `_cexit` run last first;
  `__dllonexit` grows a DLL's own table, which the DLL runs itself. `_exit`
  and `_c_exit` run nothing. There is no stdio to flush. `abort` ends the
  process with status 3 and `_purecall` with 255, each with a line on the
  debug output instead of Windows' message box.
- **The C locale only.** `setlocale` answers `"C"` to any query or request.
  The case-insensitive wide functions fold ASCII letters alone, as
  `msvcrt.dll` does in that locale (`L"Ñ"` and `L"ñ"` differ).
- **The wide printf family** (`dlls/common/wformat.c`, shared with
  `user32`) is `msvcrt.dll`'s, as Windows XP showed it to `wstartup.exe`:
  floating point keeps 17 significant digits and writes zeros after them
  (1e300 with `%.0f` ends in zeros), rounds half up on the decimal digits of
  the exact value (2.675 at two decimals is 2.67), prints a zero with no
  sign, and gives three-digit exponents; `ll` is `l`, the argument 32 bits,
  as `msvcrt.dll` of Windows XP reads it (`I64` is the 64-bit length);
  `_vsnwprintf` returns -1 and writes no terminator when the result does not
  fit.
- **`rand` and `srand`** are Microsoft's generator, `x * 214013 + 2531011`,
  bits 16 to 30: a seed deals the same cards here as on Windows. **`time`**
  returns a 32-bit `time_t` from the host's clock.
- **Global and local memory** (`dlls/kernel32/memory.c`) are one heap, as
  on Win32. A fixed block's handle is its address. A moveable block's handle
  is the address of an entry in a table of 8,192 whose first word is the
  block's address, as on Windows (`*(void **)handle` is the data); the block
  carries its handle in an 8-byte header, so `GlobalHandle` finds it from the
  data. `GlobalLock` counts locks, `GlobalFlags` reports them and
  `GMEM_DISCARDED`, `GlobalReAlloc` moves a moveable block and keeps its
  handle, a size of 0 discards it, and a fixed block grows in place only
  within the space it has unless `GMEM_MOVEABLE` lets it move. `LocalFree`
  of a pointer that is not a live block fails with `ERROR_INVALID_HANDLE`.
  The heap behind them is `ntdll`'s: first fit over 256 KiB arenas the host
  maps, 8-byte aligned blocks with a 16-byte header that keeps the size asked
  for (what `GlobalSize`, `LocalSize` and `_msize` answer, 10 for a request of
  10, as on Windows XP), free neighbours merged when they are in the same
  arena (two arenas the host maps side by side are still two regions, and a
  block across them would fail the host's range checks).
- **Threads** are [below](#threads). Thread-local storage has Windows XP's
  64 slots in each TEB (`TEB + 0xE10`); critical sections are `ntdll`'s,
  with Windows XP's fields and algorithm.
- **Heaps.** `HeapCreate` makes a heap with arenas of its own, which
  `HeapDestroy` gives back to the host (`SXW_SVC_FREE`); with a maximum size
  it holds no more than that. `HeapAlloc`, `HeapFree`, `HeapReAlloc` and
  `HeapSize` are `ntdll`'s `RtlAllocateHeap` and the rest, and like Windows
  XP's only a failed `HeapAlloc` sets the last error (8).
- **Atoms.** A local table and a global one, both this process's: Windows'
  global table is the session's, and nothing here uses DDE. Names of 1 to
  255 characters without case, reference counts, "#123" and `MAKEINTATOM`
  as integer atoms below `0xC000`, and Windows XP's errors.
- **The version** is Windows XP Professional SP3: `GetVersion` is
  `0x0A280105`, `GetVersionExA` 5.1.2600 "Service Pack 3".
- **Strings and code pages.** The ANSI and OEM code pages are both
  Windows-1252 (Windows XP in Spanish has 850 for OEM, which only console
  programs see); UTF-8 converts exactly. `lstrcmp` and `lstrcmpi` compare by
  code unit, `lstrcmpi` folding ASCII and Latin-1, where Windows uses the
  locale's collation ("a" sorts before "B" there, after it here).
- **The locale** is [below](#the-locale).
- **Modules.** `GetModuleFileNameW` asks the host for the module's path, so
  it is `C:\...` under the guest's drive and `Z:\...` anywhere else; cut
  short, it is not terminated and the last error is left alone, as on
  Windows XP. `LoadLibrary` of a Windows path (MFC loads
  `C:\WINDOWS\system32\MFC42LOC.DLL`) looks the file name up in the usual
  order. `LoadLibraryEx`'s flags are not honoured: a DLL asked for as a data
  file is loaded as a module. `FreeLibrary` succeeds and unloads nothing.
- **Resources** are found by `dlls/common/resource.c`, shared with `user32`:
  ids, names in any case, `"#123"` as an id, and a language (the first when
  the one asked for is not there). An `HRSRC` is the address of the
  resource's data entry, and the data never moves.
- **`GetObjectW`** answers a font's `LOGFONTW` as it was made. Every stock
  font is the System font here, so `DEFAULT_GUI_FONT` answers System's, not
  Windows XP's MS Shell Dlg at -11 (WP6 brings the fonts).
- **`RegisterClipboardFormatW`** shares `RegisterWindowMessageW`'s table, as
  Windows' atom table: the same name gives the same number from either.
- **`GetSystemMetrics`** answers Windows XP's classic style at 96 dpi, and the
  screen's size from the host.
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
window tree, the message queues, invalid regions, timers, focus, capture and
activation. A window belongs to the thread that made it: each thread has its
own queue for what is posted to it and its windows and for their input, its
windows' timers and paint come to it, and a message sent from another
thread waits in its queue until it looks there, as on Windows (see
[Threads](#threads)). Focus, capture and the active window are one for the
process.
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

**Text.** A font is drawn one of two ways (WP6).

- **The interface's faces** (System, MS Shell Dlg, MS Shell Dlg 2, MS Sans
  Serif, Microsoft Sans Serif, Tahoma) keep
  the platform's 13 px UI strike, MS Sans Serif
  ([UI_FONTS.md](../desktop/UI_FONTS.md)), compiled into `gdi32` from the
  same table the desktop uses. A weight of 600 or more, and the stock
  System font, are drawn twice one pixel apart with one more pixel of
  advance, as Windows emboldens a bitmap font. Their vertical metrics are
  the requested font's on Windows XP, because programs size windows from
  them: System is 16 pixels high with an ascent of 13; the others have
  Tahoma's proportions (2049 and 423 units of a 2048-unit em, 13 pixels at
  -11). They do not follow the Appearance tab, a gap kept on purpose: a
  Win32 dialog is laid out in those pixels.
- **Any other face** is a TrueType file the host opens with `libsxfont`
  ([FONT_LIBRARY.md](../desktop/FONT_LIBRARY.md#win32-through-winhost-wp6)):
  `winhost` lists `/system/fonts` and `C:\windows\fonts`, the harness
  `assets/fonts/liberation` and `assets/desktop/fonts` (or each `--fonts
  DIR`). Services 58 to 62 (`SXW_SVC_FONT_OPEN`, `_CLOSE`, `_ADVANCES`,
  `_GLYPH`, `_FACE`) open a font for a `LOGFONTW`, give its metrics and
  advances, a glyph's coverage, and the faces to enumerate. A family the
  host lacks takes its metric-compatible Liberation face (Arial, Times New
  Roman, Courier New), else the default for its pitch and family, as does
  a font made with no face.

The rules the host follows, each measured on Windows XP RTM with
`text.exe`:

- **Size.** An `lfHeight` of 0 is -16; a negative one is the em in pixels;
  a positive one is the cell, and the em is the one whose ascent and
  descent add up nearest to it, or, in a face with a `VDMX` table, the
  first record that equals it, else the last below it (Arial at 12 is
  9 ppem).
- **Metrics.** Ascent and descent from `VDMX` at that size, else from the
  scaled `usWinAscent` and `usWinDescent`. The rest of `TEXTMETRICW` and
  `OUTLINETEXTMETRICW` scale from font units by the em, or by the cell for
  a positive height in a face without `VDMX`, through a 16.16 factor
  rounded first; the enumeration's numbers are rounded once. The cap and x
  heights are half and a quarter of the em whatever OS/2 says, the caret
  slope is `hhea`'s, the italic angle is in tenths rounded down, and an
  underline or strike-out thinner than half a pixel reports 0 and is drawn
  a pixel thick. Weights 600 to 699 take the regular file with synthetic
  bold and report 700.
- **Advances** are the hinted ones (FreeType's v35 interpreter takes them
  from `hdmx` where the face has one); a fixed-pitch face has one advance,
  its `advanceWidthMax` scaled.
- **Drawing.** `ExtTextOutW` fills its rectangle (`ETO_OPAQUE`), clips to
  it (`ETO_CLIPPED`), takes per-character advances, the alignment
  (`TA_CENTER` leaves an odd width's spare pixel on the left),
  `TA_UPDATECP`, the background mode, and underline and strike-out bands.
  Grey edges are drawn where the face's `gasp` asks for them, in sixteenths
  of coverage blended in linear light with a gamma of 2.3, XP's standard
  smoothing; `SystemParametersInfoW` answers `SPI_GETFONTSMOOTHING` 1 and
  `SPI_GETFONTSMOOTHINGTYPE` `FE_FONTSMOOTHINGSTANDARD`.
- **Enumeration.** `EnumFontFamiliesExW` gives one entry per charset the
  face's code-page ranges hold, in XP's order (Western, Hebrew, Arabic,
  Greek, Turkish, Baltic, Central European, Cyrillic, Vietnamese, ...),
  with English script names; its metrics are at an em of 32 pixels. A
  family's faces come by full name, as XP's Fonts key lists them; families
  by name. A signed file (`DSIG`) adds `NTM_DSIG` and `NTM_TT_OPENTYPE`.

What still parts from Windows XP: the ink of a glyph by a pixel or a few
(FreeType's rasterizer is not XP's), the advances of a few glyphs of
Microsoft's own faces by one pixel (Arial and Times New Roman at some
sizes; their hinting is not reproduced glyph for glyph), the order of the
families in a whole enumeration (XP lists them as they were installed), and
`AddFontResourceW`, which answers 0: fonts come from the directories only.
`TextOutW` is `ExtTextOutW` with no rectangle. `DrawTextW` breaks lines,
aligns them, draws `&` prefixes and computes rectangles, but does not clip
to its rectangle.

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
- `StretchDIBits` draws 1, 4, 8, 24 and 32-bit DIBs, bottom-up or
  top-down, at their size, grown, shrunk or in part, under any ROP3;
  `CreateBitmap` makes a monochrome or 32-bit bitmap from the caller's bits
  (another depth, a colour one left black). `CreatePen` gives a pen of any
  colour, drawn a pixel wide whatever its width, or `PS_NULL`; `LineTo` and
  `Rectangle` draw with it under any ROP2, `R2_XORPEN` the ruler's
  (`text.exe`, against Windows XP's CRCs).
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

**At run time (WP5).** Menus are also built and changed by program:
`CreateMenu`, `CreatePopupMenu`, `AppendMenuW`, `InsertMenuW`,
`ModifyMenuW`, `DeleteMenu`, `RemoveMenu`, `CheckMenuItem`,
`GetMenuState` (a popup's item count in the high byte), `GetMenuStringW`,
`GetSubMenu`, `SetMenu` and `DrawMenuBar`; a menu keeps its own copy of
each item's text. Owner-drawn items are measured and drawn through the
owner (`WM_MEASUREITEM`, `WM_DRAWITEM` with `ODT_MENU`), a checked item
shows Windows XP's 7-pixel tick in its 13-pixel check column, and
`TrackPopupMenu` runs the same loop for a popup on its own
(`TPM_RETURNCMD` answers the command instead of posting it).
`CreateDialogParamW`, `CreateDialogIndirectParamW` and
`DialogBoxIndirectParamW` take templates in both formats, the classic and
the extended (`DLGTEMPLATEEX`); a modeless dialog is a host window of kind
`SXW_WINDOW_MODELESS` (the owner stays enabled) and is shown with
`UpdateWindow` at once, as Windows XP paints it. `IsDialogMessageW` asks
the message filter hook first (`MSGF_DIALOGBOX`). The dialog class keeps
the 30 bytes of `DWL_MSGRESULT`, `DWL_DLGPROC` and `DWL_USER`, and
`DefDlgProc` answers what the procedure left in `DWL_MSGRESULT`.
`GetDialogBaseUnits` is 8 by 16, the system font's.

What is left out: the system menu's own items, submenus of a popup menu
and menu bars that wrap.

## Hooks and subclassing

`dlls/user32/hook.c` and `winpos.c`, checked by `hooks.exe` against
Windows XP (see [Fixtures](#fixtures)). MFC attaches its objects to windows
from a `WH_CBT` hook and subclasses every window it makes, so the order of
what these calls send is what matters.

- **Hooks.** `SetWindowsHookExW` installs `WH_CBT`, `WH_CALLWNDPROC`,
  `WH_CALLWNDPROCRET`, `WH_GETMESSAGE` and `WH_MSGFILTER` for one thread, or
  for every thread of the process when it names none and gives a module;
  others fail with error 1426, and a thread hook without a thread 1428. The
  newest hook of a kind runs first and `CallNextHookEx` passes on to the
  next older one; `UnhookWindowsHookEx` of a hook that is gone fails with
  1404.
- **Where they are called.** The CBT hook sees `HCBT_CREATEWND` before the
  window gets any message (with `hwndInsertAfter` `HWND_TOP`; a nonzero
  answer makes `CreateWindowEx` fail), `HCBT_ACTIVATE`, `HCBT_SETFOCUS`
  before the activation that a focus change brings, `HCBT_SYSCOMMAND`, and
  `HCBT_DESTROYWND` for a window given to `DestroyWindow` and the windows
  it owns, never for its children. The window procedure hooks see sent
  messages only, with wParam 0 from the same thread. The message hook may
  change the message it is given. The filter hook sees the dialog manager's
  messages (`MSGF_DIALOGBOX`), the menu loop's (`MSGF_MENU`) and a scroll
  bar's tracking (`MSGF_SCROLLBAR`).
- **Window longs.** `GetWindowLongW` and `SetWindowLongW` read and write
  `GWL_WNDPROC` (subclassing), `GWL_STYLE` and `GWL_EXSTYLE` (with
  `WM_STYLECHANGING` and `WM_STYLECHANGED`), `GWL_ID`, `GWL_USERDATA`,
  `GWL_HINSTANCE`, a dialog's `DWL_` values and the class's extra bytes;
  `CallWindowProcW` calls the procedure a subclass saved. The system classes
  keep Windows XP's class styles and extra bytes (Button 4, Static 4, Edit
  6, ListBox 4, ComboBox 4, ScrollBar 72, the dialog class 30).
- **Properties.** `SetPropW`, `GetPropW` and `RemovePropW`, by name or atom,
  over global atoms; a window's are freed with it.

## Positions

`SetWindowPos` sends what Windows XP sent `hooks.exe`: `WM_WINDOWPOSCHANGING`
always; `WM_NCCALCSIZE` when the size or the frame changes; then, when
anything changed at all, `WM_WINDOWPOSCHANGED` with `SWP_NOMOVE`,
`SWP_NOSIZE` and `SWP_NOZORDER` added for what did not change,
`SWP_NOCLIENTSIZE` (`0x800`) and `SWP_NOCLIENTMOVE` (`0x1000`) for the client
area, and `SWP_NOREDRAW` when the parent is hidden. `DefWindowProc` turns
`WM_WINDOWPOSCHANGED` into `WM_MOVE` and then `WM_SIZE`. `MoveWindow` and
`ShowWindow` go through it. A deferred set (`BeginDeferWindowPos`,
`DeferWindowPos`, `EndDeferWindowPos`) sends every `CHANGING` first and then
every `CHANGED`. A child shown or moved without `SWP_NOACTIVATE` gets
`WM_CHILDACTIVATE`; hiding a visible child erases its parent at once.

Activating or showing a top-level window brings up its whole owner group,
the top first: the window itself with its own flags, the others with
`0x13`. `SetParent` moves a child under another, hidden and shown again.
`CreateWindowEx` follows Windows XP's rules too: a visible child of a hidden
parent only gets `WM_SHOWWINDOW`, a popup gets `WM_SIZE` and `WM_MOVE` when
it is made, `WM_GETMINMAXINFO` goes only to overlapped and sizable windows,
a framed window gains `WS_EX_WINDOWEDGE`, a top-level one keeps
`WS_CLIPSIBLINGS`, and `WM_PARENTNOTIFY` goes to the immediate parent only.

The window tree (`GetWindow`, `GetTopWindow`, `IsChild`, `EnumChildWindows`,
`EnumWindows`), screen coordinates (`GetWindowRect`, `ScreenToClient`,
`MapWindowPoints`, `WindowFromPoint`, `ChildWindowFromPoint`) and
`GetWindowPlacement` and `SetWindowPlacement` are there for MFC's frames. A
popup with no caption, owned by another window, is drawn whole on its own
surface (a host popup), and the pointer is given to it across top-level
windows in screen coordinates while it has the capture: that is how a
combo box's list and a menu work.

## Carets

`dlls/user32/caret.c`: one caret for the process, owned by a window, with a
hide count. It is drawn by inverting its rectangle, so drawing it twice
takes it away; `BeginPaint` takes it off, and `EndPaint` puts it back at
once and starts its blink again, as Windows XP does, so the caret is on the
window after every paint. It blinks every `GetCaretBlinkTime` milliseconds,
530 unless the program sets another. The calls fail as Windows XP's did: no
caret is `ERROR_ACCESS_DENIED`, a window that does not own it fails without
an error, and a new caret keeps the old one's position. `controls.exe` sets
a blink of ten seconds while it runs, so the caret is on the screen at
every dump wherever its blink had come to on each system.

## Scroll bars

`dlls/user32/scroll.c`. A window with `WS_HSCROLL` or `WS_VSCROLL` has its
bars in its non-client area (`WM_NCCALCSIZE` makes room, `WM_NCHITTEST`
answers `HTVSCROLL` and `HTHSCROLL`), and the `ScrollBar` class is one as a
control. Both keep a range, a page and a position as Windows XP does: the
page is cut to the range, the position to what the page leaves, and a
window's bar is hidden when its page covers its range and shown again when
a range is set (`SetScrollInfo`, `SetScrollRange`, `SetScrollPos`,
`ShowScrollBar`, `EnableScrollBar`, the `SBM_` messages).

They look as Windows XP's classic bars do, read back pixel by pixel from
`controls.exe`: raised arrow buttons with a triangle 4 rows deep and 7
pixels wide in a 16-pixel button (the one pointing left a pixel further
left), a track of the highlight and face colours on alternate pixels with
the face colour where x + y is even, and a raised thumb as long as the page
takes of the track, placed and followed by rounding to the nearest, as
`MulDiv` does. A press on an arrow or the track repeats after 400 ms and
then every 50 ms; a press on the thumb sends `SB_THUMBTRACK` with the
position at once and on every move, then `SB_THUMBPOSITION` and
`SB_ENDSCROLL`. A scroll bar control takes the focus on a click only with
`WS_TABSTOP`. `ScrollWindowEx` moves the pixels on the surface and
invalidates what had no source.

## Edit, list box and combo box

`dlls/user32/edit.c`, `listbox.c` and `combo.c`, with the messages, the
notifications, their order and the pixels `controls.exe` got from Windows
XP, driven by its input script.

- **Edit** is single-line: typing and deleting, a selection by keyboard
  and pointer, a caret, horizontal scrolling, Cut, Copy and Paste, one
  level of undo, a limit (30,000 by default; 0 means `0x7FFFFFFE`) and the
  `EM_` messages. A control made with text tells its parent `EN_UPDATE` and
  `EN_CHANGE` from `WM_CREATE`. A replacement the limit cuts sends
  `EN_MAXTEXT` before `EN_UPDATE` and `EN_CHANGE`. Windows XP takes
  `WS_BORDER` out of the style and draws the line itself inside the client
  area, so the style a program reads back lacks it. Inside a combo box the
  control carries `ES_COMBO` (`0x200`), draws its text from its corner and
  hides its selection without the focus even with `ES_NOHIDESEL`.
- **ListBox** holds items with text and a value, sorted with
  `CompareStringW` ignoring case (a new item goes ahead of one that
  compares equal) or in the order given, one selection, a caret item, a top
  item, integral height and owner-drawn items, fixed or variable.
  `LB_INSERTSTRING` past the end and `LB_SETCURSEL` past the end fail and
  change nothing. Deleting an item sends `WM_DELETEITEM` whatever it holds;
  emptying or destroying the list sends it, from the last item, only for an
  owner-drawn one. `LBN_SELCHANGE` goes out only when the selection moved.
  Multiple selection is not written.
- **ComboBox**, simple, drop-down, drop-down list and owner-drawn, is an
  Edit control (id 1001) and a list box (id 1000, class `ComboLBox`): a
  child of a simple one, an owned popup with `WS_BORDER` for the others.
  Windows XP moves `WS_VSCROLL` from the combo box to its list and adds
  `CBS_HASSTRINGS` when it is not owner-drawn. The field is as high as the
  font and two pixels, or what `WM_MEASUREITEM` answers for item -1 (it is
  offered the system font's height and two, since the measure comes before
  `WM_SETFONT`); the box is the field and six. A simple one's list is a
  whole number of items high under the box. The dropped list is as high as
  the height given at creation less the box at creation, and that is what
  `CB_GETDROPPEDCONTROLRECT` adds to the box. The button is 16 pixels wide
  inside a sunken edge, with the arrow of the scroll bars.
- **The combo box's order** is Windows XP's: a choice made in the list
  sends `CBN_SELENDOK`, then `CBN_CLOSEUP` when the list was down, then
  `CBN_SELCHANGE`; a move in the dropped list by keyboard sends only
  `CBN_SELCHANGE`; Enter sends `CBN_SELENDOK` and `CBN_CLOSEUP`; Escape and
  losing the focus send `CBN_SELENDCANCEL` (a simple one sends nothing
  there) and `CBN_CLOSEUP` when it was down. The field is drawn before the
  parent hears `CBN_SETFOCUS`, and again when the button comes up. An
  owner-drawn list draws `ODA_FOCUS` off and on around its items while it
  is down, for item -1 when nothing is chosen. What is typed in the edit
  control finds its item: chosen in a dropped list, only scrolled to in a
  closed one, whose selection goes. `WM_SETTEXT` reaches the edit control
  without a `CBN_EDIT` notification, and a drop-down list ignores it.
  `CB_SETCURSEL` scrolls the list to the item and leaves everything as it
  was for an index past the end. A simple combo box's list is always down
  (`CB_GETDROPPEDSTATE` is TRUE).

## The clipboard

`dlls/user32/clipboard.c` puts Win32's clipboard over SavanXP's
([CLIPBOARD.md](../desktop/CLIPBOARD.md)), through the `CLIPBOARD_SET`,
`INFO`, `FORMAT` and `GET` services (`loader/clipboard.c`): in `winhost`
over `/dev/clipboard` with SUN's typed calls (`sxn/sxnclip.h`), in the
harness over a set in memory that `--clipboard FILE` keeps between runs.

- **While a program is the owner**, its own handles are what
  `GetClipboardData` gives back. When it closes the clipboard after
  changing it, every format that is memory goes to the system's set:
  `CF_UNICODETEXT` (or the text made from `CF_TEXT` or `CF_OEMTEXT`) as
  UTF-8 with LF line ends, `Rich Text Format` as `text/rtf` without its
  NUL, `CF_HDROP` as `text/uri-list`, another registered format as
  `application/x-win32-NAME` and a numbered one as `application/x-win32-cfN`,
  in the program's order with RTF and file lists first after the text.
  GDI formats stay with the program.
- **Delayed rendering** (`SetClipboardData` with no handle) is rendered
  when the clipboard closes, and `WM_RENDERALLFORMATS` when its owner goes:
  the system's set is a value with no owner.
- **Made formats.** From any text format Windows XP makes `CF_LOCALE` and
  the other two text formats when the clipboard closes, listed after what
  was set: `CF_LOCALE`, then the others by number. `CF_OEMTEXT` is code page
  437, as an English Windows XP's (kernel32 converts `CP_OEMCP` with it).
- **Another program's set** is read when the clipboard is opened or asked
  about, each format only when it is asked for.
- **Sequence.** `GetClipboardSequenceNumber` is the system's sequence plus
  what this program counted: `EmptyClipboard`, every `SetClipboardData` and
  every format made at `CloseClipboard`, as Windows XP counts them, the
  system's own step at the close counting once. Programs compare
  differences, never values.
- `OpenClipboard` while another window has it open fails without an error
  code, as on Windows XP.

`clip.exe` checks all of it in one program and, with `put` and `get`, across
two: on SavanXP `win32test` reads what `put` left with SUN's calls, writes
the same set itself, and `get` must print what Windows XP printed.

## Drag and drop

`dlls/shell32/drop.c` reads a file list: the `DROPFILES` block a
`WM_DROPFILES` or the clipboard's `CF_HDROP` carries, wide or ANSI, with
`DragQueryFileW` (a name cut to the buffer still answers its whole length,
as Windows XP does), `DragQueryPoint` and `DragFinish`.
`DragAcceptFiles` sets or clears `WS_EX_ACCEPTFILES`. User32 makes such a
block from a `text/uri-list` (`make_drop`).

A drag over a window of the program is SavanXP's
([DRAG_AND_DROP.md](../desktop/DRAG_AND_DROP.md)): `winhost` turns
`windowd`'s drag records into `SXW_EVENT_DRAG` (phase, effects and effect in
`c`) and reads `/dev/dragdata` for `SXW_SVC_DRAG_FORMAT` and `_GET`.
User32 finds the window under the point, then the nearest one with
`WS_EX_ACCEPTFILES` from it up through its parents, as Windows looks; for an
enter or a move it answers copy through `SXW_SVC_DRAG_REPLY` when there is
one and the drag offers `text/uri-list`, and 0 otherwise. On the drop it
reads the list and posts that window `WM_DROPFILES` with the block, the
point in its client coordinates (outside the client area, the top-level
window with the non-client flag). The harness plays the drag with the input
script's `dragover X Y URI...` and `drop X Y`; `clip.exe drop` is the
fixture, in `win32-harness-test` and in the `win32-drop` scenario. A Win32
program as a drag source (`DoDragDrop`) and OLE's `RegisterDragDrop` targets
are not there yet.

## RichEdit

`dlls/msftedit/` is SSW's `msftedit.dll`, at Windows XP's base
(`0x4B460000`), with the `RICHEDIT50W` class WordPad's document is made of
([the plan's decision](../roadmaps/open/WIN32_WORDPAD_ROADMAP.md#richedit-is-written-here-2026-11-21)).
WP7a, the text, is written: `text.c` keeps it and converts it in and out,
`layout.c` breaks it into lines, `undo.c` keeps undo and redo, and
`msftedit.c` is the window. Every rule below is what Microsoft's
`msftedit.dll` answered `richedit.exe` on Windows XP RTM, and the fixture's
two outputs are byte for byte that control's.

- **The class.** Registered when the DLL attaches: `CS_GLOBALCLASS |
  CS_PARENTDC | CS_DBLCLKS`, four bytes of window extra, no cursor (the
  control sets the I-beam on `WM_SETCURSOR`), `COLOR_WINDOW` behind. Only
  `RICHEDIT50W`; `RichEdit20W` is `riched20.dll`'s, which nothing loads.
- **The text** is paragraphs ending in one CR, and the document ends in one
  more paragraph mark that is not part of the text: `WM_GETTEXTLENGTH` of
  "abc" is 3, but selecting everything gives 0 to 4, and `EM_GETLINE`
  returns each paragraph's last line with its CR, the last one included.
  Coming in (`WM_SETTEXT`, `EM_REPLACESEL`, a paste), CR LF and a lone LF
  are a CR and CR CR LF is a space; a single-line control keeps what comes
  before the first line break. Going out, `WM_GETTEXT` writes CR LF;
  `EM_GETTEXTEX` and `EM_GETTEXTRANGE` write the CR alone unless asked
  (`GT_USECRLF`). `EM_GETTEXTLENGTHEX` is exact with `GTL_NUMCHARS`,
  `GTL_PRECISE` or no flag, twice the length otherwise, and
  `E_INVALIDARG` for characters and bytes together.
- **Lines** are as wide as the formatting rectangle, which is the client
  area less a pixel left and right (`EM_SETRECT` moves it and keeps it as
  insets from the client area's edges, so it follows a scroll bar that
  comes; `EM_SETRECTNP` only stores it for the next layout), less the
  selection bar's 8 pixels with `ECO_SELECTIONBAR`. A line breaks after a
  run of spaces, which stays at its end however wide, or after a hyphen; a
  word wider than the line breaks where it stops fitting. `EM_SETTARGETDEVICE`
  with a line width of 1 stops wrapping. Widths are `gdi32`'s
  (`GetCharWidthW`), tabs stop every 48 pixels, every line is the font's
  height, and `EM_CHARFROMPOS` gives the boundary nearest the point.
- **Selections** a program sets (`EM_EXSETSEL`, `EM_SETSEL`) return the
  new end: a negative start puts the caret at the end, a negative end runs
  past the last paragraph mark, which a collapsed selection cannot sit
  after; a range given backwards keeps its anchor at the higher end. The
  old highlight and the new one are each painted at once, and the view
  follows the caret. A selection the user makes is painted once. Shift+End
  takes the paragraph mark; a drag past the end of a paragraph's line takes
  it too. A double click selects the word and the spaces after it, and a
  press on the selection drags it: dropped elsewhere in the control, the
  text moves, as one undo, and stays selected.
- **Notifications.** `EN_CHANGE`, `EN_SELCHANGE` and `EN_VSCROLL` go out as
  `EM_SETEVENTMASK` asks; `EN_UPDATE` goes out with every paint whatever the
  mask, and `EN_MAXTEXT`, `EN_SETFOCUS`, `EN_KILLFOCUS` always. Their order
  is Windows XP's: a paint, then `EN_SELCHANGE`, then `EN_CHANGE`.
  `WM_SETTEXT` of empty text over empty text sends nothing; a paste sends
  `EN_SELCHANGE` even when the caret stays, and so do Delete and
  Backspace. A control destroyed while the clipboard holds its copy sends
  `EN_SAVECLIPBOARD`.
- **Undo** keeps 100 actions (`EM_SETUNDOLIMIT`). Typing grows one action
  until something else happens or `EM_STOPGROUPTYPING`; `WM_SETTEXT` and an
  `EM_REPLACESEL` that is not undoable empty both lists. Undoing selects
  what comes back, except after Backspace, where the caret goes after it;
  redoing leaves the caret after what goes in again. `EM_GETMODIFY` answers
  -1 when modified.
- **The limit** is 32,767 characters, 65,536 for a limit of 0. Typing and
  pasting stop at it with `EN_MAXTEXT`; `EM_REPLACESEL` does not, and
  `WM_SETTEXT` raises it to the text's length.
- **Scrolling.** A window made with `WS_VSCROLL` loses it until its text is
  taller than the view; then the bar comes with a range of the text's
  height and a page of the view's, and goes when the text fits again,
  keeping its range. `EM_SETOPTIONS` writes the style back from the options
  with the `WS_VSCROLL` the window was made with, as a bit only, without
  recalculating the frame. Page Up and Page Down move the view a page less
  a line, Page Down as far as the last line at the top, and the caret as
  many lines as the view moved. `SB_THUMBPOSITION` scrolls to the pixel
  without `EN_VSCROLL`.
- **Words** (`EM_FINDWORDBREAK`, Ctrl with the arrows, Backspace and
  Delete): letters and digits, punctuation, the hyphen (which a line may
  break after), spaces, and the paragraph mark are each a class, and a word
  is a run of one class followed by its spaces.
- **The clipboard** is `user32`'s plain one: a copy puts `CF_UNICODETEXT`
  with CR LF, and a paste takes it, dropping the text's last paragraph mark.
  Windows XP's control also offers OLE's formats and RTF, and pastes its own
  copy back as RTF, which keeps that mark; until WP7c's RTF, the control
  recognizes its own copy by the clipboard's sequence number.

Not there yet: formats (WP7b), streams and RTF (WP7c), finding text and the
OLE interfaces (WP7d), horizontal scrolling, and the I-beam pointer, which
waits for `SetCursor` (only declared). Inside WordPad's process the DLL does
not load yet: see [WordPad](#wordpad).

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
| `exception.exe` | `RaiseException` visits two SafeSEH-listed registrations from inner to outer, passes the exception arguments and x86 context, and resumes when the outer handler asks to continue | `expected/exception.out`, status 0 |
| `sehfail.exe` | An uncaught noncontinuable software exception | No output, status `0xE0421003` |
| `sehscope.exe` | x86 `__except` filters that search, continue a nested exception, or select a handler, `__finally` in two frames, and explicit `RtlUnwind` to a target registration | `expected/sehscope.out`, status 0 |
| `cxxthrow.exe` | A throw across three frames with destructors, catch by reference and by value, a rethrow, `catch (...)` inside another catch, and destruction of the caught copy and original exception object | `expected/cxxthrow.out`, status 0 |
| `cxxfail.exe` | An uncaught C++ exception | No output, CRT termination status 3 |
| `initorder.exe` | Imports from `alpha.dll` and `beta.dll`, which share a preferred base so one is relocated; `alpha.dll` imports from `beta.dll`; static TLS with two callbacks and no C runtime | The attach and detach order below, status 7 |
| `missingdll.exe` | An import from a DLL that is not installed | No output, status `0xC0000135` |
| `missingsym.exe` | An import `kernel32.dll` does not export | No output, status `0xC0000139` |
| `startup.exe` | It starts the way `sol.exe` does: the same runtime imports, an SEH frame at `fs:[0]` with `_except_handler3`, C and C++ initializers through `_initterm`, `__getmainargs`, the tail of `_acmdln`, a `WinMain`. Then every non-GUI function of the matrix, failure paths included: `rand` after two seeds, `MulDiv`'s rounding and overflow, a missing module and procedure, `beta.dll` loaded at run time and found by name and ordinal, 115 `LPTR` blocks, and the registry (a counter, `ERROR_MORE_DATA`, a size query, an ANSI read, the value name `Atrás`, a missing value and key) | Run twice as `startup.exe alpha "two words" 3`: `expected/startup-1.out` then `startup-2.out`, where the counter reads back the first run's 1 as 2; status 5 |
| `window.exe` | A top-level window with a child, painted with the matrix's drawing calls (`PatBlt` with `PATCOPY`, `BLACKNESS` and `DSTINVERT`, `FrameRect`, `InvertRect`, lines in every octant under `R2_COPYPEN` and `R2_NOT`, `SetPixel`, `TextOutW`, a bold `CreateFontW`, `DrawTextW`), two chained timers (one with a timer procedure), and its input script: clicks, a double click, a right click, a click on the child, a captured drag, keys with Shift, an invalidated rectangle, and the frame's close. It writes the messages it gets (the list it keeps is in `logged()`) and, on the key P, its client area outside the text box as runs of colour read back with `GetPixel` | `expected/window.out`, status 3 |
| `cards.exe` | Loads the user's own `cards.dll` from its folder with `LoadLibraryA` and draws, with `cdtInit`, `cdtDrawExt` and `cdtTerm`, the 52 faces and 12 backs at 71x96 on Solitaire's green, the other modes (inverted, the empty pile's marks, X, O) on a few cards, and the 12 backs at 33x62 as the Deck dialog does. It writes a CRC-32 of each drawing's pixels read back with `GetPixel`, or, with the argument `pixels`, the pixels themselves | `expected/cards.out`, status 0. The pixel listing describes Microsoft's art and is kept with the user's files, never here |
| `wstartup.exe` | It starts as `wordpad.exe` does, a Unicode program over `msvcrt.dll` (`__wgetmainargs`, `_wcmdln`, `_onexit`), then checks what WordPad's and MFC's start-up need, failure paths included: moveable and fixed global memory, local memory, the C heap, TLS, critical sections, the interlocked functions, the version, `lstr` functions, code pages (1252 and UTF-8, a default character), the wide C strings, number conversions both ways, `floor`, `ceil` and `modf`, the wide printf family (integers, strings, floating point with its edges, lengths, truncation), module paths cut short, resources by id, `"#100"` and name, rectangles, registered clipboard formats, a font's `LOGFONTW`, the registry, `OleInitialize` twice and `__dllonexit`. It prints no path, handle, TLS index, locale or screen size | Run as `wstartup.exe alpha "two words" 3`: `expected/wstartup.out`, status 5 |
| `files.exe` | WP4: in its own folder, `CreateFileW` with every disposition, reads, writes, `SetFilePointer` past the end and before the start, `SetEndOfFile`, `GetFileTime` against the clock, share modes, another spelling of a name, folders, `FindFirstFileW` with masks, deletes and moves, full paths, the current directory, the volume, and the file-time conversions at their limits. It prints no handle, time, drive letter or serial | Run from a folder on C:: `expected/files.out`, status 0 |
| `memory.exe` | WP4: the process heap, private heaps (growing, zeroing, in place, a fixed maximum), moveable memory shared for DDE, and local and global atoms (integer atoms, case, references, the name length limits) | `expected/memory.out`, status 0 |
| `locale.exe` | WP4: every value `GetLocaleInfoW` has for English (United States), dates and times from the locale's pictures and others with every flag, their enumerations, `CompareStringW` on pairs with each flag, and win.ini's defaults | `expected/locale.out`, status 0 |
| `threads.exe` | WP3, with `tlog.dll` and `tquiet.dll`: a thread started suspended, suspend counts, TLS slots and static TLS with its callbacks, thread notifications in order and `DisableThreadLibraryCalls`, events, mutexes, semaphores and waits, a contended critical section, `_beginthreadex` with a priority, a message-only window on the second thread taking sent, posted and thread messages, and a thread left waiting as the process ends | `expected/threads.out`, status 5 |
| `resource.exe` | Its own resources (`resource.rc`): strings, bitmaps in both header formats at 1, 4 and 24 bits, an icon, a cursor and an accelerator table. It loads them as `sol.exe` does, then copies, converts and stretches the bitmaps (grown by 2, grown to odd sizes, shrunk), and drives the accelerators with posted keys | `expected/resource.out`, status 0 |
| `dialog.exe` | A menu of two popups with separators and an accelerator shown after a tab, an accelerator table, and two dialogs built from the same resource types and styles as `sol.exe`'s (`dialog.rc`): one of owner-drawn buttons with a default and a cancel button, like Deck, and one of group boxes, radio buttons the program checks itself, automatic check boxes and static text left and right, like Options; then `MessageBoxW`, `ShellAboutW` and `InitCommonControlsEx`. Its input script uses the menus by pointer and by keyboard alone, a grayed item, both dialogs by pointer and by keyboard (Tab, arrows, Space, mnemonics, Enter, Escape), an accelerator for a menu command and one for a command no menu has. It writes the messages its procedures get (the list is in `logged()`), where each control is, the buttons' states, and pixels of the controls outside their text | `expected/dialog.out`, status 5 |
| `hooks.exe` | WP5, what MFC does to every window: the CBT, window procedure, message and filter hooks with their codes and order, `CallNextHookEx` and unhooking; subclassing with `SetWindowLongW` and `CallWindowProcW`, window longs, the system classes and properties; the window tree; positions, deferred positions, `SetParent`, non-client edges and scroll bars, show and focus; carets; `ScrollWindowEx`; menus built and changed at run time; a modeless and a modal dialog built in memory, driven through `IsDialogMessageW`. It needs no input, writes the messages its procedures and hooks get (the list is in `logged()`) and prints no handle or screen position | `expected/hooks.out`, status 0 |
| `controls.exe` | WP5: Edit, ListBox, ComboBox (simple, drop-down, drop-down list, owner-drawn) and ScrollBar controls asked every message the profile uses, then driven by its input script (typing, selections, clicks, double clicks, keys, the lists dropped and chosen from, the scroll bars' arrows, track and thumb, a context menu); it writes their notifications, owner-drawn measures and drawings, styles, sizes and final state, and on F2 the controls' pixels outside their text. It dumps the window with a drop-down list open and with the context menu open | `expected/controls.out`, status 0 |
| `clip.exe` | WP5: the clipboard calls refused while it is closed; text, Rich Text Format and a format of its own set and read back; the formats Windows makes and their order, the owner, the sequence, a format rendered when asked for, `WM_DESTROYCLIPBOARD`; a file list read with `DragQueryFileW` and `DragQueryPoint`, wide and ANSI, and `DragAcceptFiles`. With `put` it leaves text, RTF and its own format; with `get` it writes what another program left. With `drop` it shows a window that takes files, with a child that does not, and writes the first `WM_DROPFILES` it gets: the names with `DragQueryFileW` and the point with `DragQueryPoint` | `expected/clip.out`; run as `clip.exe put` then `clip.exe get`: `clip-put.out` and `clip-get.out`; status 0. `clip-drop.out` is the harness's: an input script cannot drag from another program on Windows XP |
| `text.exe` | WP6: with `liberation`, the three Liberation families added with `AddFontResourceW`; without it, Arial, Times New Roman and Courier New. Per face, style and size (-8 to -72, 0, 12, 20): `GetTextMetricsW`, `GetCharWidthW` over ASCII, an extent, `GetOutlineTextMetricsW`, `GetTextCharsetInfo`; the faces substituted for missing families, weights, ANSI and wide widths, three enumerations; then pictures read back with `GetPixel`: the ink's box and count for opaque, clipped, aligned, positioned, underlined and transparent text, the grey levels of a smoothed line, and CRCs of `StretchDIBits` (4, 8 and 24 bits), `R2_XORPEN` lines, `Rectangle`, `CreateBitmap` blits and `PATCOPY`. It prints the screen's depth and `SPI_GETFONTSMOOTHING` first | `expected/text-liberation.out`, Windows XP RTM's, compared through `tests/host/win32_text_compare.py` (ink counts within 3%, grey levels within one, family lines in any order, the faces substituted for missing families skipped: XP has Arial there); `text-savanxp.out`, the harness's own, byte for byte, which `win32test` holds `winhost` to outside the substituted faces. Plain `text.exe` against XP's output is `win32-text-xp-test`, with the user's own XP fonts (`SAVANXP_WIN32_TEXT`), a pixel per glyph of slack |

| `richedit.exe` | WP7a: `RICHEDIT50W` from `msftedit.dll` next to it, at Liberation Sans -13 (given to Windows XP from the files beside it). Without arguments: the class, the control's defaults, then text set and read back with every length and text message, selections, replacements, undo and redo, `WM_CHAR`, the limit, lines and positions both ways, the formatting rectangle, wrapping, the options, scrolling, every `EM_FINDWORDBREAK` code at every position, the clipboard, read-only and a single-line control, with every `WM_COMMAND` and `WM_NOTIFY` its parent gets. With `input`, driven by its input script: typing, the caret and selections by keyboard, deleting, undo, the clipboard, clicks, a double click, drags that select and one that moves text, scrolling by typing and by page; F2 writes the state, and a dump holds the control with a selection, where no caret blinks | Microsoft's own `msftedit.dll` on Windows XP RTM: `expected/richedit.out` and, run as `richedit.exe input`, `richedit-input.out`; status 0 |

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
| `wstartup.exe` | `af71162775ebce60f5e4d270ef63586e51f75e027c9ada29c63061a78771b5b9` (2026-11-21) |
| `files.exe` | `893a956e8b18a9f809136feba5d97ede2121317a420004c0ebc65fd2b1dd2554` (2026-11-25) |
| `memory.exe` | `8be7fbf78b8ede6d847b70234e9c9645a831fad0918fc892f5d576e18aca06d1` (2026-11-25) |
| `locale.exe` | `ca1fc79771bd4d8f5476107f7dc377170d7c8cbfee89dbad978e70c21722c493` (2026-11-26, on the English Windows XP) |
| `threads.exe` | `4d55ea8c3684da784e93499a1fc51e5a60f525969f9f46bea7178f2988e9235a` (2026-11-25) |
| `tlog.dll` | `e3dba5a8e2d46e32b345029bb6fe844a0169ea279e113414588892a6188d541b` (2026-11-25) |
| `tquiet.dll` | `bacb05d74ff972ee18001a7925c70cfeb76d11a5cc91c84cd8598a0fc89e8697` (2026-11-25) |
| `hooks.exe` | `44c41cea36ae717d97af9e482120ae967274c8bd0f129d23d0a44f9518ef724f` (2026-11-25) |
| `controls.exe` | `d3479f319704dcfdfa1e2b8fe8e7f509705eebe46f21dbb02284423cfd2212ca` (2026-11-25) |
| `clip.exe` | `a38ab7dcfa1c567eaedadd5fbf4698fc7f45c31b30cd48c8ee466d4b28506e9a` (2026-11-25) |
| `richedit.exe` | `e35e5b1f41a250dc17208cf33c666fb495461589f5d8de8a940e267cce327569` (2026-11-27, on the English Windows XP, with the user's `msftedit.dll` beside it) |

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

`hooks.exe`, `controls.exe` and `clip.exe` ran on the same machine, one
after the other from one batch file (`clip.exe`, `clip.exe put`, `clip.exe
get`, `hooks.exe`, `controls.exe`), the last driven by its input script.
`controls.exe` writes, besides the messages, the pixels of each control
outside its text; what Windows XP showed decided the controls' look, from
the scroll bars' arrows to the combo box's button. Its measure of a fixed
list's height prints no item: Windows XP leaves `itemID` as it finds it.
`expected/controls-dropdown.ppm.gz` (the drop-down list open) and
`expected/controls-menu.ppm.gz` (the context menu open) are the harness's
images; outside the text, the caret's blink and the menu's drop shadow they
are Windows XP's screen.

Since 2026-11-26 the locale is the English one (see [The locale](#the-locale)),
and what depends on it comes from a second machine, an English Windows XP
(RTM) set to English (United States): `expected/locale.out`, and
`expected/clip.out`, whose only change was the clipboard's `CF_LOCALE`
(`0x0409`). `clip.exe put` and `clip.exe get` wrote there byte for byte what
they had written on the Spanish machine.

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
[--input SCRIPT] [--clipboard FILE] program.exe [arguments...]` runs a
fixture on the
Linux x86-64 build host through the same loader and DLLs as `winhost`. The
guest's standard output is the harness's; the guest's 32-bit exit status, or
the load failure's NT status, is printed as `win32: exit status 0x...` and
its low byte is the harness's exit status.

The gate uses Linux's own selectors: `0x23` for 32-bit code, `0x2b` for data,
`0x33` for 64-bit code. FS holds an LDT entry (`modify_ldt`), one per guest
thread, whose base is the thread's TEB while guest code runs; glibc's FS base is put back with `arch_prctl`
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
  selector into DS, ES and FS once per thread, then sets FS's base to that
  thread's TEB with `sxn_set_thread_base`, and never switches them again:
  the native runtime does not use FS, and the kernel keeps all three per
  thread. Each guest thread is a SUN thread with a 256 KiB native stack per
  thread slot, reused once the slot's last thread has ended. The 32-bit
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
  manager's own shortcuts (Alt+F4, Alt+Tab) never reaching the guest. A
  modeless dialog is an owned window with `SAVANXP_WM_WINDOW_MODELESS`. A key
  arrives as the character it typed, so Shift and a digit are mapped back to
  the digit's key by the layout the kernel applies (`INPUT_IOC_GET_LAYOUT`):
  Alt+Shift+2 is the accelerator Solitaire knows in Spanish and in English.
  The guest's millisecond clock is the native one.
- **The clipboard** is `/dev/clipboard`, opened for each call with SUN's
  typed calls (`sxn/sxnclip.h`), whose first consumer `winhost` is.

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

## Threads

WP3 of [WIN32_WORDPAD_ROADMAP.md](../roadmaps/open/WIN32_WORDPAD_ROADMAP.md).
A guest thread is a host thread: under `winhost` a SUN thread with its own
selectors and FS base and a native stack, in the harness a POSIX thread with
its own LDT entry. Its guest memory is one region: the stack (`CreateThread`'s
size, or the executable's reserve), the TEB, and its static TLS copied from
each image's template (`sxw_new_thread`). The gate finds a thread's state by
the index the host keeps in its TEB at `0xFF0`, past every field of Windows
XP's TEB.

**They take turns.** One thread holds the turn and runs guest code; it gives
the turn up while it waits on an object, sleeps or waits for input, and at a
call to the host while another thread is ready (a ticket lock, so each gets
its turn in order). SSW's DLLs need no locks of their own, and a run repeats
itself at any number of processors; two guest threads never use two
processors at once, and a thread that spins in guest code without calling
the host keeps the others waiting. The decision and its reasons are in the
roadmap ([Guest threads take turns](../roadmaps/open/WIN32_WORDPAD_ROADMAP.md#guest-threads-take-turns-2026-11-25)).

- **Start and end.** A new thread enters the guest at
  `ntdll!SxwThreadStart`, which runs `DLL_THREAD_ATTACH`: each DLL in the
  order it attached, its TLS callbacks then `DllMain`, skipping those that
  called `DisableThreadLibraryCalls`, and the executable's TLS callbacks
  last. `ExitThread`, or the thread function returning, runs
  `DLL_THREAD_DETACH` in reverse order, the executable last, under the loader
  lock as the attach. The host then gives back the thread's region. The last
  thread to end ends the process.
- **ExitProcess** stops every other thread where it is before any DLL
  detaches, and they get no detach, as Windows XP does
  (`SXW_SVC_END_OTHER_THREADS`).
- **Suspending.** `CreateThread` with `CREATE_SUSPENDED` holds the thread
  before its first instruction; `SuspendThread` on a running thread stops
  it at its next call to the host (Windows stops it at once). Priorities
  are kept and answered; the host does not schedule by them.
- **Objects.** Events, mutexes (with abandonment), semaphores and threads,
  named or not within the process, are handles of the host's table, with
  files. `WaitForSingleObject` and `WaitForMultipleObjects` (any or all, up
  to 64) take what they wait for atomically. Handles are `0x40` and up.
- **Critical sections** are Windows XP's: `LockCount` -1 when free and
  counting the waiters, an auto-reset event in `LockSemaphore` made by the
  first thread that has to wait.
- **The C runtime** keeps no data per thread: `_beginthreadex` starts a
  kernel32 thread and `_endthreadex` ends it; `rand` has one seed for the
  whole process, where msvcrt.dll has one per thread.
- **user32** keeps a queue per thread (above). Another thread's
  `SendMessageW` waits in the window's thread's queue, and that thread calls
  the window procedure when it next looks at its messages; meanwhile the
  sender takes the messages sent to it. A posted or sent message wakes the
  thread from its wait for input (`SXW_SVC_WAKE_THREAD`). `DestroyWindow`
  refuses another thread's window, and an `HWND_MESSAGE` window is never
  shown. Input from the host goes to the thread that made the host window.

## Files

WP4. `kernel32` makes every path full before the host sees it, as
`GetFullPathNameW` does (the current directory, "." and "..", '/' as '\',
and the dots and blanks at the end of the last name dropped); the current
directory starts as the executable's folder, as Explorer starts a program.
The host maps `C:\` to its `drive_c` folder and `Z:\` to its root (in the
harness, `Z:` alone), finds every component without case as NTFS does, and
keeps each file's position in its handle.

- `CreateFileW` with every disposition, `ReadFile`, `WriteFile`,
  `SetFilePointer` (past the end, and a gap written later reads as zeros),
  `GetFileSize`, `SetEndOfFile`, `FlushFileBuffers`, `GetFileTime`,
  `CloseHandle`; share modes between the opens of the process, a file open
  without `FILE_SHARE_DELETE` refused to `DeleteFileW` and `MoveFileW`.
- `GetFileAttributesW` answers `FILE_ATTRIBUTE_DIRECTORY`, or
  `FILE_ATTRIBUTE_ARCHIVE` with `READONLY` for a file that cannot be written.
- `FindFirstFileW` lists a folder in NTFS's order, "." and ".." first in any
  folder but a drive's root, with '*' and '?' without case and "*.*" for
  everything.
- `CreateDirectoryW`, `RemoveDirectoryW`, `DeleteFileW`, `MoveFileW` (onto
  nothing that exists), `GetCurrentDirectoryW`, `SetCurrentDirectoryW`,
  `GetVolumeInformationW` (no label, a fixed serial, 255-character names
  whose case is kept, "SxFS").
- **One time per file.** The hosts keep a file's last write, in seconds; it
  answers for its creation and last access too. **A file's time cannot be
  set**: neither SUN nor POSIX has a way to on SavanXP, so `SetFileTime`
  stays [declared](#declared-entry-points).
- `FileTimeToSystemTime`, `SystemTimeToFileTime` and `GetSystemTime`, with
  Windows XP's limits (1601 to 30827). **Local time is UTC** until
  [TIMEZONE_ROADMAP.md](../roadmaps/open/TIMEZONE_ROADMAP.md) T6:
  `GetLocalTime` and the local file-time conversions change nothing.

Under `winhost` they stand on SUN's file calls, `sxn_seek_file`,
`sxn_set_file_size`, `sxn_query_file` and `sxn_delete_directory` among them
([SUN.md](SUN.md#files-and-small-profile-saves)); in the harness on POSIX's.

## The locale

One locale, English (United States), LCID `0x0409`, the language the rest
of SavanXP speaks: SavanXP has no regional setting yet. Its values are
Windows XP's own, taken from what `locale.exe` printed on an English Windows
XP (RTM) test machine. Until 2026-11-26 it was Spanish (Argentina), the
locale of the machine the profiles were taken from; it was switched so the
whole system shows one language.

- `GetLocaleInfoW` answers every value Windows XP has for it, in text or
  with `LOCALE_RETURN_NUMBER`, but `LOCALE_FONTSIGNATURE`; another locale
  is `ERROR_INVALID_PARAMETER`, another value `ERROR_INVALID_FLAGS`.
- `GetDateFormatW` and `GetTimeFormatW` format from the locale's pictures or
  the caller's, with Windows' letters and quoting and the time flags, which
  drop a field with the text before it. `EnumDateFormatsW` and
  `EnumTimeFormatsW` list Windows XP's pictures, the default first.
- `CompareStringW` sorts by letters (ñ is n with a tilde), then accents,
  then case (lower first); symbols sort before digits and letters,
  and in a word sort the hyphen and the apostrophe count only last.
  `NORM_IGNORECASE`, `NORM_IGNORENONSPACE`, `NORM_IGNORESYMBOLS` and
  `SORT_STRINGSORT` as on Windows.
- **win.ini** is not there: `GetProfileIntW` and `A`, `GetProfileStringW` and
  `GetProfileSectionW` answer as Windows XP answers for a name its win.ini
  lacks, with the default given.

## Software exceptions

`kernel32!RaiseException` builds an `EXCEPTION_RECORD` and calls
`ntdll!RtlRaiseException`. The dispatcher captures the x86 integer and
control context and visits the current thread's registrations at `fs:[0]`,
youngest first. It checks that each record fits in the thread's stack and
that the chain advances toward its top. The host checks that the handler
points into executable guest image code, honours `NO_SEH`, and checks a
SafeSEH table when the image supplies one. The guest calls the handler.

`ExceptionContinueSearch` advances to the next frame, and
`ExceptionContinueExecution` resumes from the saved integer registers,
flags, instruction pointer, and stack pointer. A request to continue a
noncontinuable exception raises `EXCEPTION_NONCONTINUABLE_EXCEPTION`.
An uncaught software exception ends the guest with its full 32-bit status.
The dispatcher registers a guard for each handler call. An exception raised
inside a handler can search the enclosing registrations with the nested
call flag set. The unwind walk uses the same guards to identify an unwind
interrupted by another unwind.

`RtlUnwind` runs each younger handler with `EXCEPTION_UNWINDING` and
removes its registration. The target registration stays at `fs:[0]`.
The requested return value is in the unwind context's EAX. `_except_handler3`
uses compiler scope tables to run filters and `__finally` blocks before
entering a selected `__except` body. `_XcptFilter` continues the search.

`_CxxThrowException` raises a noncontinuable software exception with the
thrown object and its type descriptor. `__CxxFrameHandler` reads the classic
Microsoft x86 exception tables used by WordPad and MFC. It finds typed
catches, adjusts or copies their arguments, and calls cleanup funclets on
the way to a catch. The active catch's registration keeps the exception
object alive for `throw;` and destroys it when the last catch finishes.
The CRT terminates an uncaught C++ exception with status 3, as on XP.
Hardware faults stay in the kernel (S3).

## WordPad

The second profile ([WIN32_WORDPAD_PROFILE.md](WIN32_WORDPAD_PROFILE.md),
planned in [WIN32_WORDPAD_ROADMAP.md](../roadmaps/open/WIN32_WORDPAD_ROADMAP.md)).
After WP1 the user's `wordpad.exe`, with their `mfc42u.dll` and
`mfc42loc.dll` in the system folder, loads with every import resolved (599
at load time, most of them [declared](#declared-entry-points)), runs MFC's
initialization and the C runtime's Unicode start-up, and runs WordPad's own
start-up through OLE's initialization and its settings in the registry. It
stops where the plan expects: WordPad cannot load `MSFTEDIT.DLL`, the
RichEdit control of WP7, and says so in its own message box ("No se puede
cargar la DLL del control RichEdit..."), its first window. Closing the box
reaches MFC's delay-load lookup for `ole32!CoFreeUnusedLibraries` (WP9). The
missing export raises a software exception, and the guest exits with
`0xc06d007f`.

Since WP7a the DLL is there, and loads in any program, but not yet in
WordPad's process: the loader keeps at most 64 mapped regions
(`SXW_MAX_REGIONS`), one per protection run of each image plus stacks and
heaps, and WordPad and MFC have used them all by then, so `LoadLibraryW`
fails with `ERROR_NOT_ENOUGH_MEMORY` and WordPad shows the same box. With
more regions the control loads and WordPad goes on to `wsprintfW` and
`LoadBitmapW`, both only declared, and a missing delay-loaded export. On
SavanXP each region is a section handle of `winhost`'s, and the kernel gives
a process 64 handles (`kMaxFileHandles`) and 128 views
(`kMaxSectionViews`), so the limit cannot simply be raised.

What the run reaches and is only declared is on the debug output; since WP5
the start-up up to that first window reaches none (MFC's message-filter
hook is installed). WordPad's look for its Word converters next to it
(`GetFileAttributesW`) finds none, and it goes on without them.

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
selector in 32-bit code, so only KVM or hardware catches a lost one. With
`threads.exe` among its fixtures, run it at `--smp 1` and `--smp 4` too:
the threads' output must not depend on the number of processors.

`win32-registry-test` checks the store alone, with the sanitizers on: every
error the registry services return, the file's round trip, malformed files,
and compaction when values are replaced again and again.

`win32-harness-test` runs the fixtures through the harness and
compares the output bytes and the full 32-bit status with what Windows XP
produced for the same files. A failed load must also name its cause and
release every mapping and file. `window.exe`, `dialog.exe` and
`controls.exe` play their input scripts, and what they dump must equal the
golden images (`expected/window.ppm.gz`, `dialog-menu.ppm.gz`,
`dialog-settings.ppm.gz`, `controls-dropdown.ppm.gz`, `controls-menu.ppm.gz`),
as must `richedit.exe input`'s (`richedit-selection.ppm.gz`);
`clip.exe put` and `clip.exe get` share one `--clipboard` file;
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

After the console fixtures `win32test.sxe` starts `windowd` and runs the
windowed ones, `hooks.exe`, `clip.exe` and `richedit.exe`, then the clipboard across
subsystems: `clip.exe put` leaves its formats, `win32test` checks the set on
`/dev/clipboard` with SUN's calls and writes the same set itself, and
`clip.exe get` must print what Windows XP printed after `put`.

`win32-dialog` (`./build.sh smoke win32-dialog`) does the same with
`dialog.exe` (`win32test.sxe --dialog`, `tools/win32_dialog_smoke.py`): its
menus and dialogs as `windowd` owned windows, its messages against Windows
XP's, and the menu and the Settings dialog on the screen against their
golden images. When an output differs, `win32test` logs the first line that
does, both ways. `win32-controls` does it with `controls.exe`
(`win32test.sxe --controls`, `tools/win32_controls_smoke.py`): its edit,
list and combo boxes and scroll bars driven by its script, a drop-down list
and a context menu open against their golden images. `win32-drop`
(`win32test.sxe --drop`, `tools/win32_drop_smoke.py`) runs `clip.exe drop`
and, once its window is up, a small SUN window the host presses on to start
a drag of two files, then drags onto `clip.exe`'s window and releases:
`clip.exe` must write `clip-drop.out` and the source hear that the drop
took a copy ([DRAG_AND_DROP.md](../desktop/DRAG_AND_DROP.md)).
`win32-richedit` (`win32test.sxe --richedit`, `tools/win32_richedit_smoke.py`)
runs `richedit.exe input`: the control driven by its script, what its parent
hears against Windows XP's, and its dump against the golden image.

`win32-wordpad-test` starts the user's `wordpad.exe` through the harness,
with `mfc42u.dll` and `mfc42loc.dll` from `SAVANXP_WIN32_WORDPAD/system32`
beside the subsystem's DLLs, and checks that every import resolves, nothing
faults, and the program waits for input with a window up, which it dumps and
does not compare: the window shows Microsoft's text. Without the files it is
reported as skipped.

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
