# Win32 profile: Windows XP WordPad

> **Status: inventory recorded (2026-11-13); nothing implemented.** This is
> the W0 record of the second Win32 profile, the one
> [W7](../roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md#w7--later-profiles) asks
> every later application to take before any code. It describes files the
> user owns, taken from their own Windows XP installation. None of them, nor
> any capture or log taken from them, is in this repository.

The goal is the original WordPad of Windows XP, unchanged, editing and saving
a formatted document on the SavanXP desktop. The API surface of the profile
is what the files below import and what a recorded session calls. A function
that is not in this document is not written.

Solitaire's profile ([WIN32_SOLITAIRE_PROFILE.md](WIN32_SOLITAIRE_PROFILE.md))
was 124 functions in seven DLLs. WordPad is a different kind of program: it
is written on MFC, its document is a RichEdit control, and it uses OLE for
the clipboard and embedded objects. Most of this document is about what that
brings with it.

## The files

Read from the user's Windows XP disk image without starting it (the qcow2
file exposed read-only through `qemu-storage-daemon` and read with
`ntfscat`), then parsed with `llvm-readobj` and a resource parser.

| File | Size | Version | Link time | Resources | Image base | Relocations |
| --- | --- | --- | --- | --- | --- | --- |
| `wordpad.exe` | 222,720 | 5.1.2600.7297 | 2017-06-22 | Spanish | `0x01000000` | **stripped** |
| `mfc42u.dll` | 974,848 | 6.2.8081.0 | 2011-02-08 | English | `0x727B0000` | present |
| `mfc42loc.dll` | 57,344 | 6.0.8665.0 | 2001-08-22 | Spanish | `0x61DF0000` | present |
| `msftedit.dll` (RichEdit 4.1) | 539,136 | 5.41.15.1515 | 2008-04-14 | | `0x4B460000` | present |
| `riched20.dll` (RichEdit 3.0) | 433,664 | 5.30.23.1230 | 2008-04-14 | | `0x74DC0000` | present |
| `riched32.dll` | 3,584 | 5.1.2600.0 | 2001-08-22 | | `0x73260000` | present |
| `msls31.dll` (Line Services) | 156,160 | 3.10.349.0 | 2009-03-08 | | `0x00400000` | present |
| `usp10.dll` (Uniscribe) | 406,528 | 1.420.2600.7334 | 2017-08-23 | | `0x74D20000` | present |
| `mswrd6.wpc`, `mswrd8.wpc`, `write.wpc` | 188,928; 281,088; 90,624 | | 2009, 2010, 2009 | | `0x00400000` | present |

| File | SHA-256 |
| --- | --- |
| `wordpad.exe` | `84d91fb2620f4e89372897638a5cf2a97a62ba9670b5e1d884c021b555d0ecd5` |
| `mfc42u.dll` | `1a5509517dffbea1d43d4f6527c7cb973a7b4f038525344aab51708f9a228a63` |
| `mfc42loc.dll` | `b6e9b183a98b2422abf97e5a3df57711078faa0a06f0734041f83ef05bcc22a6` |
| `msftedit.dll` | `39a9ebf7479acb36e54a742f5dceeae5733de637f2aa053eb8fe58862c15a146` |
| `riched20.dll` | `fe8280c9d7b598f7d2b694720357fb481001bf8d67b4b97125a09236d4dc64fd` |
| `riched32.dll` | `1604c80abcd5a9a286787e209e4f2e85ce88ab8a6cb26b0c83b40d8371eeff9e` |
| `msls31.dll` | `7effa9d4561674633b2ffb35f629947b061afe5da756da6d02e0584fce221445` |
| `usp10.dll` | `4f7ac3605e72884e17f305aa9a609bc6dc1bef708dcd40f3c5cd1d38ffe96705` |
| `mswrd6.wpc` | `146ac996642015b612e9ed87309b547ff76f18dc6a86831072d4a0f45cb39acd` |
| `mswrd8.wpc` | `e5173726401f3eb5b9bffa83fe04f18ee8595e125c1b78e0f3386eb63d8aa976` |
| `write.wpc` | `ecccf5e5aab5c8a2afb9c2f4c566c71aed342a09225f9564ecc3f205d0fa100f` |

Where they live on Windows XP: `wordpad.exe` and the three `.wpc` files in
`C:\Archivos de programa\Windows NT\Accesorios`, the DLLs in
`C:\WINDOWS\system32`. Unlike Solitaire's, most of these files were replaced
by updates: `wordpad.exe` by a 2017 security update, `mfc42u.dll` by a 2011
one. They are the versions on the user's patched Windows XP SP3, not the
release's.

What each one is, for this profile:

- **`wordpad.exe`** is the application: the frame, the views, the toolbars,
  the ruler, the dialogs and the file formats.
- **`mfc42u.dll`** is the Unicode build of MFC 4.2, Microsoft's C++ class
  library, and **`mfc42loc.dll`** holds its Spanish resources: the print
  preview bar and MFC's own dialogs and messages. They are application
  libraries, like `cards.dll`: they are used unchanged, never written here.
- **`msftedit.dll`** is the RichEdit control that holds and draws the
  document. WordPad asks for `MSFTEDIT.DLL` and its class `RICHEDIT50W`
  first; `RICHED20.DLL` is the fallback. `riched32.dll` forwards to
  `riched20.dll`. **`msls31.dll`** (line breaking) and **`usp10.dll`**
  (complex scripts) are delay-loaded by both RichEdit DLLs.
- **The `.wpc` files** convert Word 6, Word 97 and Write documents to RTF.
  WordPad loads them only to open or save those formats.

What the loader must honour:

- **`wordpad.exe` loads at `0x01000000` or not at all**, like `sol.exe`.
- **`msls31.dll` and the three converters prefer `0x00400000`**, which is
  where `winhost` itself is linked. They have relocations, so they move: the
  loader's rule for an occupied base already covers it.
- **`mfc42u.dll` has delay imports from thirteen DLLs** (below), resolved on
  first call through the delay-load helper inside MFC. A delay-loaded DLL
  that is never called need not exist. `msftedit.dll` and `riched20.dll`
  delay-load `msls31.dll` and `usp10.dll` the same way.
- **`mfc42u.dll` exports 6,386 functions by ordinal**, six of them also by
  name. `wordpad.exe` imports 719 of them, all by ordinal.
- **`wordpad.exe` has a load configuration with a SafeSEH table** of 123
  handlers. On Windows XP SP2 and later, `ntdll` refuses to call an
  exception handler of this image that is not in that table.
- **`usp10.dll` has a section named `Shared`**, read-only data.
- No image has a TLS directory. None is a .NET or 64-bit image.

A disassembly of every code section found no load of the FS register, no
`int 0x2e`, no `sysenter` and no `int 0x80`. (A byte scan finds such
patterns, but they are data inside `.text`.) Exception frames are another
matter: `wordpad.exe` writes `fs:[0]` 157 times and `mfc42u.dll` 1,240
times. That is the compiler's frame for every C++ function with an object to
destroy, and MFC uses C++ exceptions for its errors (`CException`,
`AfxThrowUserException` after a failed dialog validation). The SEH chain is
on the path, not a corner case.

## Imports

### The closure

The DLLs written here have to cover every import of the original binaries
that run: `wordpad.exe`, `mfc42u.dll`, `msftedit.dll`, `msls31.dll` and
`usp10.dll`. Counted without duplicates across them:

| DLL | Resolved at load | Delay-loaded only | Already exported by SSW |
| --- | --- | --- | --- |
| `user32` | 247 | | 57 |
| `kernel32` | 175 | | 8 |
| `gdi32` | 145 | | 25 |
| `msvcrt` | 101 | | 14 |
| `advapi32` | 13 | 8 | 6 |
| `ole32` | 11 | 68 | 0 |
| `shell32` | 6 | 3 | 1 |
| `comdlg32` | 4 | 6 | 0 |
| `shlwapi` | 2 | | 0 |
| `comctl32` | | 15 | 1 |
| `oleaut32` | | 38 (by ordinal) | 0 |
| `wininet` | | 38 | 0 |
| `odbc32` | | 32 (by ordinal) | 0 |
| `wsock32` | | 23 (by ordinal) | 0 |
| `oledlg` | | 10 | 0 |
| `olepro32` | | 5 (by ordinal) | 0 |
| `urlmon` | | 4 | 0 |
| `winspool.drv` | | 3 | 0 |
| **Total** | **704** | **253** | **112** |

With RichEdit 3.0 (`riched20.dll`) instead of 4.1, load-time is 700. If the
RichEdit control were written here instead of run, the load-time closure
would drop to 599 (`wordpad.exe` and MFC alone), and the control's own
window messages would become the work.

What the table says:

- **The four core DLLs carry the load.** `user32`, `kernel32`, `gdi32` and
  `msvcrt` hold 668 of the 704. SSW exports 104 of those today.
- **`ole32` is a load-time import of `wordpad.exe` itself** (eleven
  functions: `OleInitialize` and structured storage on memory blocks), so
  `ole32.dll` must exist and export them, whatever OLE ends up doing.
- **Most of MFC's breadth is delay-loaded.** ODBC, WinINet, Winsock, URL
  monikers, OLE Automation and printing spooler calls are in MFC for other
  programs. The trace says which of them WordPad reaches (below).
- **Two new DLLs are on the path**: `comdlg32` (open, save, font, find,
  replace, print and page setup dialogs) and `shlwapi`.

### `wordpad.exe` by DLL

| DLL | Count | Notes |
| --- | --- | --- |
| `mfc42u` | 719 | By ordinal; the program's structure is MFC's document/view frame |
| `user32` | 92 | |
| `kernel32` | 61 | Files, atoms, moveable global memory, locale and date formats, events, `SetThreadPriority` and `ResumeThread` for the thread it starts through MFC, `LoadLibraryExW`, `SetUnhandledExceptionFilter` |
| `msvcrt` | 33 | Start-up of a Unicode program (`__wgetmainargs`, `_wcmdln`), wide-string functions, the C++ runtime (`__CxxFrameHandler`, `_purecall`) |
| `gdi32` | 31 | |
| `ole32` | 11 | `OleInitialize`, `OleUninitialize`, `CoRegisterActivationFilter`, `CLSIDFromString`, `StringFromCLSID`, `CoTaskMemFree`, `CreateILockBytesOnHGlobal`, `StgCreateDocfileOnILockBytes`, `ReadClassStg`, `OleRegGetUserType`, `ReleaseStgMedium` |
| `advapi32` | 8 | |
| `shell32` | 6 | `ShellAboutW`, `ShellExecuteExW`, `SHGetSettings`, `SHGetSpecialFolderPathW`, `DragQueryFileW`, `DragFinish` |
| `comdlg32` | 4 | `ChooseFontW`, `GetOpenFileNameW`, `GetSaveFileNameW`, `CommDlgExtendedError` |
| `shlwapi` | 2 | `PathFindFileNameW`, `SHRegGetValueW` |

## Resources

`wordpad.exe`, all in Spanish (80 resources):

| Type | Count | Notes |
| --- | --- | --- |
| Menus | 7 | The main menu (Archivo, Edición, Ver, Insertar, Formato, Ayuda), the menus MFC merges while an embedded object is active in place, the colour menu of the format bar, the text context menu, and a menu of pen commands |
| Accelerators | 4 | The main table (128): `Ctrl+U` new, `Ctrl+A` open, `Ctrl+G` save, `Ctrl+P` print, `Ctrl+T` select all, `Ctrl+B` find, `F3` find next, `Ctrl+H` replace, `Ctrl+N`, `Ctrl+K`, `Ctrl+S` bold, italic, underline, `Ctrl+L` and `Ctrl+R` left and right, `Ctrl+Z` and `Alt+Backspace` undo, `Alt+Enter` object properties. It does not match the Spanish menu text, which says `Ctrl+E` for select all and `Ctrl+R` for replace. `Ctrl+T` is listed twice, for select all and for centre; the first entry wins. The other three tables serve the in-place editing states |
| Dialogs | 9 | See below |
| Bitmaps | 15 | The toolbar and format bar strips (`BITMAPINFOHEADER`, 4 bpp, 16 and 24 px high) and the ruler's small marks |
| String tables | 25 blocks | 186 strings: messages, file type descriptions, status bar prompts and tooltips |
| Icons | 14 in 4 groups | |
| Manifest, version | 1 each | The manifest asks for version 6 of the common controls |

| Dialog | Controls |
| --- | --- |
| 143, Párrafo | A group box, three edit controls, a drop-down list (alignment), static texts, two buttons |
| 145, Tabulaciones | A group box, a simple combo box (the stops), five buttons |
| 151, Fecha y hora | A list box of formats, two buttons |
| 158, 159, Opciones | Property sheet pages: radio buttons, check boxes and group boxes; the sheet has six tabs (Opciones, Texto, Texto enriquecido, Word, Write, Incrustado) |
| 160, Nuevo | A list box of document types, two buttons |
| 150, Configurar página | Four edit controls. Used only where `PageSetupDlg` is missing; Windows XP shows the common dialog |
| `DEFAULTFORMATDIALOG` | A check box appended to the Save As dialog ("Establecer este formato como predeterminado") |
| `UNSUPPORTEDSAVEFORMATDIALOG` | A message with a check box |

`mfc42loc.dll` adds MFC's own dialogs in Spanish (the print preview bar, the
"printing" dialog, the New document chooser, and three stock property pages
for OLE controls) and its 348 messages.

The control classes these templates and the program create:

| Class | Where | Owner |
| --- | --- | --- |
| Button (push, default, check, radio, group), Static, Edit, ListBox, ComboBox (simple, drop-down, drop-down list, owner-drawn) | Dialogs and the format bar | `user32` |
| `RICHEDIT50W` | The document | `msftedit.dll`, original |
| `ToolbarWindow32` (two), `msctls_statusbar32`, `tooltips_class32` | The toolbar, the format bar, the status bar, the tooltips | `comctl32` |
| `#32770` property sheet with `SysTabControl32` | Options | `comctl32` |
| `AfxControlBar42u`, `AfxFrameOrView42u`, `Afx:...` | Docking frames, the view, the ruler | registered by MFC, drawn by MFC |

The common controls are not in `comctl32`'s import list at all: MFC creates
them by class name, and `comctl32` registers them when it loads. So
`comctl32.dll` stops being one call; it becomes four window classes, the
property sheet and image lists.

## Run-time trace

Taken on 2026-11-13 on the build host with Wine 11.21 (WOW64, X11 driver,
a 1024x768 virtual desktop) in a fresh prefix under the `es_AR` locale. The
original `mfc42u.dll`, `msftedit.dll`, `riched20.dll`, `msls31.dll` and
`usp10.dll` sat next to `wordpad.exe` and were forced native with
`WINEDLLOVERRIDES`, so the trace records what the Microsoft binaries ask of
the system DLLs, which is what SSW would have to provide.

- `WINEDEBUG=+relay` with `RelayFromInclude` naming the native modules
  records every call they make into Wine's DLLs and every message delivered
  to their window procedures. Calls from one native module to another
  (`wordpad.exe` into MFC's ordinals) are not relayed; which of the 719 MFC
  ordinals run is not measured, and need not be while MFC runs unchanged.
- A driver program in the same prefix plays a script with `SendInput` and
  marks each phase by posting `WM_NULL` with `wParam` `0x573000nn` to the
  main window. Under the relay WordPad runs far slower than the input, so the
  driver waits for the program's thread to pump messages
  (`SendMessageTimeout`) before every step, and for each dialog to come to
  the front.
- The phases: start-up; typing three lines with accents, `Backspace` and
  caret movement; bold, italic and underline from the keyboard; every
  button of the toolbar and the format bar, the colour menu, the font and
  size boxes; clicks, a double click, a selection drag (which became an OLE
  drag of the selected text) and the context menu in the text; a tab stop
  set and dragged on the ruler; Fuente, Párrafo, Tabulaciones, bullets, Fecha
  y hora and Opciones; select all, copy, paste, undo, cut, Buscar, `F3` and
  Reemplazar todo; Guardar como RTF, Nuevo as a text document, Guardar as
  `.txt`, Abrir the RTF again; Configurar página, Vista preliminar and
  Imprimir; the four view toggles off and on; Insertar objeto and Pegado
  especial; Acerca de and `F1`; Salir with "No" to saving.
- The driver, the script, the analysis scripts, the summary
  (`relay.json`, 58 million relay lines reduced to the first twenty calls
  of each function) and Wine's screenshots of each step are kept with the
  user's files under `WordPadXP/w0/`, not in the repository.

### What was reached

**385 system functions in twelve DLLs.** Wine's internal names are mapped to
Windows XP's: `combase` to `ole32`; `ntdll`'s heap and critical section
functions to the `kernel32` ones that forward to them; `NtdllDefWindowProc_W`
to `DefWindowProcW`.

| DLL | Called | Already exported by SSW |
| --- | --- | --- |
| `user32` | 147 | 48 |
| `kernel32` | 97 | 8 |
| `gdi32` | 54 | 20 |
| `msvcrt` | 33 | 6 |
| `ole32` | 23 | 0 |
| `comdlg32` | 10 | 0 |
| `advapi32` | 8 | 6 |
| `shell32` | 5 | 1 |
| `comctl32` | 3 | 1 |
| `oledlg` | 3 | 0 |
| `shlwapi` | 1 | 0 |
| `hhctrl.ocx` | 1 (`HtmlHelpA`, Help only) | 0 |

So of 704 load-time imports, 385 ran: the rest still have to resolve, as an
entry point that fails cleanly, but need no behaviour. None of the delay-loaded
DLLs for databases, the Internet, sockets, OLE Automation or printing
spoolers was loaded. **`msls31.dll` and `usp10.dll` were not loaded either**:
RichEdit 4.1 laid out this Latin text without Line Services or Uniscribe.

By the phase that first needed them:

| Phase | New | Functions |
| --- | --- | --- |
| Start-up | 246 | The frame, the control bars, the RichEdit control, MFC's start-up: windows and classes, `SetWindowsHookExW`, `CallWindowProcW`, `SetWindowLongW`, properties, deferred positioning, carets, timers, `OleInitialize` and `RegisterDragDrop`, `InitCommonControlsEx`, atoms, the heap, critical sections, TLS, `_beginthreadex`, `SuspendThread`, `ResumeThread`, events and waits, locale queries, resources and the registry. The full list is in the trace summary |
| Typing | 5 | `CreateBitmap`, `DestroyCaret`, `GetCharWidthA`, `ScrollWindowEx`, `TranslateAcceleratorW` |
| Keyboard formatting | 1 | `PatBlt` |
| Toolbar and format bar | 20 | `CreatePopupMenu`, `AppendMenuW`, `TrackPopupMenu`, `DestroyMenu`, `GetSubMenu`, `GetMenuItemCount`, `GetMenuItemID`, `EnableMenuItem`, `GetClassNameW`, `RemovePropW`, `GlobalFindAtomW`, `GlobalDeleteAtom`, `GetBkColor`, `GetTextColor`, `GetPaletteEntries`, `Rectangle`, `TextOutW`, `MessageBeep`, `lstrcmpiW`, `wcslen` |
| Mouse in the text | 32 | OLE drag and drop and the data it carries: `DoDragDrop`, `CreateILockBytesOnHGlobal`, `StgCreateDocfileOnILockBytes`, `WriteClassStg`, `ReleaseStgMedium`, `OleRegGetUserType`, `OleRegGetMiscStatus`, `CoTaskMemFree`, metafiles (`CreateMetaFileW`, `CloseMetaFile`, `SetWindowOrgEx`, `SetWindowExtEx`), moveable global memory (`GlobalReAlloc`, `GlobalSize`, `GlobalFlags`, `GlobalUnlock`, `GlobalFree`); the context menu (`InsertMenuW`, `DeleteMenu`, `RemoveMenu`, `CheckMenuItem`, `GetMenuStringW`, `OleUIAddVerbMenuW`); `SetCapture`, `ReleaseCapture`, `GetAsyncKeyState`, `InvertRect`, `CreateDCW`, `GetLayout`, `SetLayout`, `WindowFromDC`, `GetProfileSectionW` |
| Ruler | 1 | `SetROP2` |
| Format dialogs | 20 | `ChooseFontW`, `PropertySheetW`, `CreateDialogIndirectParamW`, `EndDialog`, `DestroyWindow`, `EnableWindow`, `MapDialogRect`, `GetActiveWindow`, `SetActiveWindow`, `MonitorFromWindow`, `GetMonitorInfoW`, the date and time formats (`EnumDateFormatsW`, `EnumTimeFormatsW`, `GetDateFormatW`, `GetTimeFormatW`, `GetLocalTime`), `GetUserDefaultUILanguage`, `GetModuleHandleW`, `CoFreeUnusedLibraries`, `iswspace` |
| Editing, find, replace | 9 | `FindTextW`, `ReplaceTextW`, `OleSetClipboard`, `OleGetClipboard`, `CompareStringW`, `MessageBoxW`, `MoveWindow`, `IsBadWritePtr`, `wcspbrk` |
| Files | 21 | `GetSaveFileNameW`, `GetOpenFileNameW`, `GetFileTitleW`, `CreateFileW`, `ReadFile`, `WriteFile`, `SetFilePointer`, `GetFileSize`, `GetFileTime`, `FindFirstFileW`, `FindClose`, `GetFullPathNameW`, `GetVolumeInformationW`, `FileTimeToLocalFileTime`, `FileTimeToSystemTime`, `mktime`, `SHGetFileInfoW`, `SHGetSettings`, `CreateFileMoniker`, `GetRunningObjectTable`, `InSendMessage` |
| Page setup, preview, print | 6 | `PageSetupDlgW`, `PrintDlgExW`, `CommDlgExtendedError`, `SetMenu`, `GetMessagePos`, `GetMessageTime` |
| View toggles | 0 | |
| OLE dialogs | 3 | `OleUIInsertObjectW`, `OleUIPasteSpecialW`, `realloc` |
| About, Help | 4 | `ShellAboutW`; Help's `RegOpenKeyExA`, `RegQueryValueExA`, `HtmlHelpA` |
| Exit | 17 | `OleFlushClipboard`, `OleIsCurrentClipboard`, `RevokeDragDrop`, `CoDisconnectObject`, `OleUninitialize`, `CopyMetaFileW`, `RegSetValueExW`, `RegDeleteKeyW`, `UnregisterClassW`, `ShowOwnedPopups`, `PostQuitMessage`, `WinHelpW`, `HeapDestroy`, `TlsFree`, `DeleteCriticalSection`, `GlobalHandle`, `exit` |

### Run-time lists

- **Window classes created:** `WordPadClass` (the frame), `RICHEDIT50W` (the
  document), two `ToolbarWindow32`, three `COMBOBOX` (font, size, script),
  `msctls_statusbar32`, `tooltips_class32`, four `AfxControlBar42u` (the
  docking areas), `AfxFrameOrView42u`, and one `Afx:...` class MFC registers
  for the ruler. MFC subclasses all of them: its window procedure received
  153 different messages, among them the combo boxes' (`CB_ADDSTRING` 1,334
  times while filling the font list, with 2,713 `WM_DELETEITEM`), the
  tooltips' (`TTM_*`) and the RichEdit's (`EM_*`). `SetWindowLongW`,
  `CallWindowProcW` and the window properties MFC keeps on each window must
  be right for every class.
- **Hooks:** MFC installs `WH_CBT` (to attach its objects to windows as they
  are created) and `WH_MSGFILTER` on both threads, and `wordpad.exe`
  installs `WH_CALLWNDPROC` seven times. `CallNextHookEx` is on the path.
- **A second thread.** At start-up a second thread is created through MFC
  (`_beginthreadex` with `CREATE_SUSPENDED`, then `SetThreadPriority` and
  `ResumeThread`); it made 44 calls in the session, and the main thread
  waits for it on events. SSW runs one thread today.
- **No C++ exception was thrown** in a session that ends normally. An
  earlier session that left a dialog open when the script exited threw two,
  from MFC (`_CxxThrowException`), on the way out. The frames are still
  installed everywhere, and MFC's error paths need the exception dispatch.
- **Raster operations:** `BitBlt` with `SRCCOPY`, `SRCAND` and `SRCINVERT`;
  `PatBlt` with `PATCOPY`; `StretchDIBits` with `SRCCOPY`;
  `SetROP2(R2_XORPEN)` while dragging on the ruler.
  `SetBkMode(TRANSPARENT)`.
- **Text:** `ExtTextOutW` 1,659 calls and `ExtTextOutA` 151, from the
  RichEdit control; `CreateFontIndirectW`; `GetOutlineTextMetricsA`,
  `GetCharWidthW`, `GetTextCharsetInfo` and `EnumFontFamiliesExW` five times
  (the font box and the font dialog). The RichEdit control measures and
  draws with real font metrics, so the bounded font mapper of the first
  profile is not enough here.
- **System metrics:** `SM_CXSCREEN`, `SM_CYSCREEN`, `SM_CXVSCROLL`,
  `SM_CYHSCROLL`, `SM_CXBORDER`, `SM_CYBORDER`, `SM_CXICON`, `SM_CYICON`,
  `SM_SWAPBUTTON`, `SM_CXDOUBLECLK`, `SM_CYDOUBLECLK`, `SM_PENWINDOWS`,
  `SM_DBCSENABLED`, `SM_CYMENUCHECK`; through `GetProcAddress`, the
  multi-monitor functions. Colours: `COLOR_WINDOW`, `COLOR_WINDOWFRAME`,
  `COLOR_WINDOWTEXT`, `COLOR_HIGHLIGHT`, `COLOR_HIGHLIGHTTEXT`,
  `COLOR_BTNFACE`, `COLOR_BTNSHADOW`, `COLOR_BTNTEXT`, `COLOR_BTNHIGHLIGHT`.
  Stock objects: `SYSTEM_FONT`, `DEFAULT_PALETTE`, `DEFAULT_GUI_FONT`.
  Device capabilities: `TECHNOLOGY`, `HORZRES`, `VERTRES`, `LOGPIXELSX`,
  `LOGPIXELSY`, `SIZEPALETTE`.
- **Memory:** a private heap (`HeapCreate`), 655 `HeapAlloc`, 4,462
  `malloc`; `GlobalAlloc` with `GMEM_MOVEABLE` and `GMEM_DDESHARE` for the
  clipboard and OLE data; `LocalAlloc` with `LPTR` and `LMEM_FIXED`;
  `LocalReAlloc`. SSW's heap refuses moveable blocks today.
- **Files:** `CreateFileW` for writing (`GENERIC_READ | GENERIC_WRITE`,
  `CREATE_ALWAYS`) and reading (`GENERIC_READ`, `FILE_SHARE_READ`,
  `OPEN_EXISTING`), then `ReadFile`, `WriteFile`, `SetFilePointer`,
  `GetFileSize`, `GetFileTime`, `FindFirstFileW` on the saved name.
- **Registry:** everything under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Applets\Wordpad`, opened
  for each access: `Options` (`WordSel`, `Units`, `Maximized`, `FrameRect`,
  `PageMargin`, `DefaultFormat`, `AlwaysConvertToRTF`), one subkey per
  format (`Text`, `RTF`, `Word6`, `Write`, `IP`) with `Wrap`, `BarState0`,
  `BarState1`, `Layout2` and `LayoutAux2` (MFC's saved toolbar layout,
  binary), and `Recent File List` (`File1` to `File4`). Read-only:
  `HKLM\...\Wordpad\COMChecks`, `HKCR\.rtf`, and RichEdit's language pack
  fallback key. Written on exit, with `RegDeleteKeyW` for MFC's bar state.
- **`win.ini`:** RichEdit reads `[windows]` scrolling and drag settings
  with `GetProfileIntA`, and `[FontSubstitutes]` with `GetProfileSectionW`.
- **Libraries loaded at run time:** `MSFTEDIT.DLL` by WordPad; MFC's delay
  loads of `comctl32`, `comdlg32`, `ole32`, `oledlg`, `shell32` and
  `advapi32`; `MFC42LOC.DLL` from the system folder by full path; `hhctrl.ocx`
  for Help.
- **Clipboard formats registered:** `Rich Text Format`, `RichEdit Text and
  Objects`, `Embedded Object`, `Embed Source`, `Link Source`, `Object
  Descriptor`, `Link Source Descriptor`, `FileName`, `FileNameW`, `Native`,
  `OwnerLink`, `ObjectLink`.
- **Timers:** one every second on the frame (WordPad's), MFC's idle and
  tooltip timers.

### Where Wine differed

- Without `mfc42loc.dll` in the prefix's system folder, MFC fell back to its
  English resources: the exit question read "Save changes to doc1.rtf?".
  On Windows XP it is Spanish. The file belongs in the profile.
- Wine has no printer, so Configurar página reported an error and Imprimir
  showed an empty list; Windows XP has the XPS Document Writer.
- Wine's `OleUIInsertObjectW` is a stub and shows nothing; Windows XP lists
  the registered object types.
- `F1` reached `HtmlHelpA` and showed nothing under Wine; Windows XP opens
  `wordpad.chm` in the HTML Help viewer.
- Native `msftedit.dll` asked Wine's text services (`msctf`) for a text
  store; Wine logged its callbacks as stubs and the control worked.

### Decisions this inventory leaves open

These are for the plan, not answered here:

- **Run the original RichEdit, or write one.** Running `msftedit.dll`
  unchanged costs 105 more load-time imports than WordPad and MFC alone, and
  real font metrics. Writing a RichEdit means the 74 messages its window
  received, 32 of them `EM_*` (selection, character and paragraph formats,
  streaming RTF in and out, find, undo, printing with `EM_FORMATRANGE`, and
  the OLE interface and callback), and an RTF reader and writer.
- **How much OLE.** `OleInitialize` must succeed: WordPad's string 100 is
  "La inicialización de OLE ha fallado. WordPad no puede continuar." The
  clipboard, drag and drop and embedded objects go through OLE
  (`OleSetClipboard`, `DoDragDrop`, structured storage on memory). Which OLE
  calls may fail without stopping WordPad was not measured.
- **Threads.** MFC's second thread is on the start-up path.
- **Exception dispatch.** Not reached in a normal session, but the path
  every MFC error takes.
- **The common controls and dialogs.** `comctl32` becomes four classes and a
  property sheet; `comdlg32` six dialogs.

## Reference captures

Taken on 2026-11-13 from Windows XP Professional SP3 in Spanish, in the QEMU
virtual machine run with `-snapshot`, classic theme, 1024x768 at 32 bits,
driven over QMP: WordPad started from Run, a three-line document typed with
the first word in bold, then every menu and dialog in turn.

They are kept with the user's files under
`$SAVANXP_WIN32_FIXTURES/../WordPadXP/reference/`, never in the repository.
Its `SHA256SUMS` lists 65 images (full screens and the changed region of
each) and has SHA-256
`937520daa5750a4d0b17af4c1816eeac26f6f9726b88a3d0aaa2c1a7b4dc17d8`.

| Captures | Contents |
| --- | --- |
| `01`, `02` | The empty window, then the typed document |
| `03` to `08` | The six menus open |
| `10` to `14` | Fuente (the common font dialog), Párrafo, Tabulaciones, Fecha y hora, Insertar objeto |
| `15` to `15-6` | Opciones, each of its six tabs |
| `16` to `18` | Buscar, Reemplazar, Pegado especial |
| `19` to `23` | Guardar como (with WordPad's format check box), Abrir, Nuevo, Configurar página, Imprimir |
| `24`, `25` | Print preview, a bulleted paragraph |
| `26`, `27` | About (it shows the machine's registered owner and stays local), Help in the HTML Help viewer |
| `28`, `29` | The "save changes" question on exit, and the desktop after it |

What they establish:

- The window has the toolbar, the format bar (font, size and script boxes,
  bold, italic, underline, colour, three alignments, bullets), the ruler and
  the status bar, laid out as under Wine.
- The script box shows "Occidental"; under Wine it is empty.
- The print preview bar is MFC's, in Spanish from `mfc42loc.dll`.
- Help opens `wordpad.chm` in the HTML Help viewer, a separate program.
