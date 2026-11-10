# Win32 subsystem — Windows applications on Sx

> **Status: design settled, implementation not started (2026-11-10).** The
> first application goal is unchanged: the original **Windows XP Solitaire
> (`sol.exe`)**, x86/PE32, playable on the SavanXP desktop. This revision
> replaces the plan of 2026-11-08. It chooses the architecture, reduces the
> kernel work to two small neutral slices, and orders the phases by what
> Solitaire needs. The static inventory of the user's two files is recorded in
> [WIN32_SOLITAIRE_PROFILE.md](../../userland/WIN32_SOLITAIRE_PROFILE.md).
> Nothing under `subsystems/win32/` exists yet, and no Windows file is part of
> this repository.

## Objective

Run **the original Windows XP Solitaire binary** unchanged on the SavanXP
desktop, with real card drawing, input, menus, dialogs and gameplay. The
profile is Windows XP x86: `sol.exe` and its companion `cards.dll` as PE32
images, supplied by the user from their own Windows XP installation. A native
lookalike, Wine's or ReactOS's own Solitaire, a source port or a 64-bit build
of the game does not satisfy this goal.

Small PE32 fixtures are the engineering gates on the way. They are built with
the project's toolchain, and the same files must also run on Windows XP
without relinking or patching.

Running this one application is not "Windows XP compatibility". Every later
application is its own named profile ([W7](#w7--later-profiles)).

## The decision

```text
One process, started as a native SUN program:

  sol.exe, cards.dll            unmodified, supplied by the user      32-bit
  ntdll, kernel32, msvcrt,      written here, built as real PE32      32-bit
  user32, gdi32, advapi32,      DLLs; all Windows semantics live
  shell32, comctl32             in them
  ---------------------------- the gate: a far transfer, no kernel ----------
  winhost                       SUN program: PE loader, guest memory,  64-bit
                                threads, files, clock, window surfaces
                                and input
  ---------------------------- int 0x80, the existing native ABI ------------
  Sx                            objects, sections, threads, VFS
  windowd, compositord          the existing desktop
```

1. **The Windows API is implemented here, as PE32 DLLs.** They are compiled
   from C with the clang and lld the build already requires, and they export
   the documented Win32 functions by name and ordinal. Wine and ReactOS are
   references to read, not code to import.
2. **The process is a 64-bit SUN program, `winhost`, that carries 32-bit
   code.** It loads the images below 4 GiB, builds the TEB and the stack, and
   transfers to the guest through a far return into a 32-bit code segment. The
   guest reaches the host the same way in the other direction. This is the
   shape of Windows' own WoW64 and of Wine's current 32-bit mode.
3. **The kernel learns nothing about Windows.** No PE parser, no Windows
   process state, no 32-bit system call ABI, no new subsystem id. It gains two
   neutral capabilities with native consumers: 32-bit user segments
   ([K1](#k1--32-bit-user-segments)) and a view at a chosen address
   ([K2](#k2--a-native-view-at-a-chosen-address)).
4. **Windows semantics stay on the 32-bit side.** Window procedures, message
   queues, device contexts, dialogs and menus are guest code. The host offers
   a small, private, pointer-free service table. It never calls back into the
   guest, so no Windows structure is translated between 32 and 64 bits.
5. **The scope is measured, not guessed.** The API surface is the import
   closure of `sol.exe` and `cards.dll`, recorded at W0. A function is
   implemented when a named consumer calls it. An unimplemented import fails
   the load with its name; it is never resolved to a stub that returns
   success.

### It is a subsystem, and it does not live inside SUN

Win32 keeps the place [SYSTEM_LAYERING.md](../../userland/SYSTEM_LAYERING.md#planned-binary-compatibility-subsystems)
gives it: a foreign contract translated onto Sx, with its own source tree
(`subsystems/win32/`), its own runtime root (`/system/win`) and its own
licence boundary, beside POSIX and SUN.

- **The Windows API is not part of SUN.** A SUN program cannot call it, no
  SUN header describes it, and no SUN call is added to resemble a Windows
  one. K1 and K2 are neutral mechanisms that any native program may use.
- **`winhost` uses SUN as its binding to Sx.** It is a native process that
  includes `sxn.h` and `sxnwm.h` the way any native service does. That is the
  "may reuse SavanXP pieces internally" clause of the layering document, not
  a dependency of the Windows API on SUN's design.
- **It has no kernel half, so it has no `subsystem::Id`.** In the kernel an id
  selects a system call dispatcher and sizes private per-process state
  (`include/kernel/subsystem.hpp`). POSIX needs both. Win32 needs neither: its
  programs never enter the kernel with an ABI of their own, and its process
  state lives in `winhost`. The kernel therefore sees a Windows program as a
  native process named `winhost`, and Task Manager reports it that way.

This is the arrangement of Windows NT's own environment subsystems, which are
user-mode code over the native API. It is closer to the declared model than
POSIX is today, since POSIX still has a translation layer compiled into the
kernel image. The Linux subsystem is the opposite case and does get an id:
its contract is a system call ABI, which only the kernel can receive
([Linux roadmap](LINUX_SUBSYSTEM_ROADMAP.md)).

## What was measured

Read from the tree on 2026-11-10. Recheck before relying on a row.

| Fact | Where | Consequence |
| --- | --- | --- |
| The GDT has five fixed entries: null, kernel code and data, user data `0x1b`, user code `0x23`. There is no 32-bit code descriptor, and the user data descriptor has D/B clear | `arch/x86_64/cpu_init.cpp` | 32-bit user code cannot run today; the data descriptor cannot be a 32-bit stack segment. K1 |
| Every kernel entry returns with `iretq` from a saved frame, and "from user mode" is tested as `cs & 3`. There is no `syscall`/`sysret` path | `arch/x86_64/context.S`, `kernel/process.cpp` | A thread interrupted in 32-bit code resumes correctly once the segments exist. No entry path has to change |
| A thread switch saves and restores the FPU state (`fxsave64`) and the FS and GS bases through their MSRs. The data segment selectors are not per-thread state | `kernel/process.cpp`, `arch/x86_64/cpu_init.cpp` | DS, ES, FS and GS selectors must become per-thread. K1 |
| A native program's segment bases are its own: the shared runtime installs no thread block at `%fs` for it (`sx_tls_init` returns early), `errno` is a global, and the stack guard is the global `__stack_chk_guard` | `subsystems/posix/sdk/v1/runtime/pthread.c`; the disassembly of `nativetest` | `%fs` is free in `winhost` and can hold the 32-bit TEB |
| Program images load at `0x400000`; kernel-chosen views start at `0x1000000000`; the stack top is `0x7000000000` | `include/kernel/vmm.hpp` | The range a PE32 image wants is free in a native process |
| `vm::map_section_view` honours a caller-chosen base, and POSIX exposes it as `map_view_at`. The native call `SXN_SYS_MAP_VIEW` takes a handle and flags only | `kernel/vmm.cpp`, `include/abi/savanxp_native_abi.h` | A native loader cannot place an image. K2 |
| W^X is enforced per view; a section may grant read, write and execute. There is no `mprotect`. A process has 64 views | [SECTIONS.md](../../kernel/SECTIONS.md) | Images are filled through a writable view and remapped executable. The view budget must be counted |
| The native file API opens existing regular files for sequential reading and saves whole files of up to 4096 bytes. No seek, no size query, no directory enumeration | [SUN.md](../../userland/SUN.md#files-and-small-profile-saves) | Enough to load images and keep Solitaire's settings. Not enough for case-insensitive lookup |
| A process has one connection to `windowd`, a main window framed by the window manager and up to four owned windows: framed dialogs, or frameless popups placed in the owner's client area. Pointer capture, cursor shape and resize exist | [OWNED_WINDOWS.md](../../desktop/OWNED_WINDOWS.md), `include/sxn/sxnwm.h` | A top-level window, a modal dialog and a popup menu each map to something that exists |
| The desktop's body face is MS Sans Serif 8pt at 96 DPI as a 13 px bitmap strike | [UI_FONTS.md](../../desktop/UI_FONTS.md) | The font Windows XP dialogs and menus are laid out for is already in the system |
| The build requires clang, `ld.lld`, `llvm-objcopy` and `llvm-readelf`. `subsystems/` holds only `posix` | `build.sh`, `CMakeLists.txt` | The PE toolchain is the LLVM that is already there |

Two experiments on the build host, 2026-11-10 (clang 23.1.4):

- **The DLLs can be built with the project's compiler.**
  `clang --target=i686-pc-windows-msvc -ffreestanding` and
  `lld-link /nodefaultlib` with a `.def` file produced a `kernel32.dll` that
  exports `GetStdHandle`, `WriteFile` and `ExitProcess` undecorated, with a
  base relocation table, and a `hello.exe` that imports them by name. Both are
  `IMAGE_FILE_MACHINE_I386` PE32 files. No Windows SDK, no MinGW and no C
  runtime were involved.
- **The gate needs nothing from a kernel beyond the segments.** A 64-bit
  Linux process mapped such a DLL at its preferred base below 4 GiB, entered
  an exported `stdcall` function through a far return to a 32-bit code
  segment, and came back through a far jump. The function read and wrote its
  own data and returned the expected values on two consecutive calls. The
  same loader and gate code can therefore run on the build host, which is
  what makes the [host harness](#evidence-and-tests) possible.

The user's `sol.exe` and `cards.dll`, read without running them
([profile](../../userland/WIN32_SOLITAIRE_PROFILE.md)):

- **The surface is 124 functions in seven DLLs**: `USER32` 62, `GDI32` 27,
  `msvcrt` 18, `KERNEL32` 9, `ADVAPI32` 6, `SHELL32` 1 and `COMCTL32` 1. The
  first plan's guess was about two hundred.
- **`sol.exe` has no relocations** and must load at `0x01000000`.
  `cards.dll` prefers `0x70140000` and can move.
- **`cards.dll` imports only drawing calls and `LoadBitmapA`**: nothing from
  `KERNEL32` or the C runtime.
- **There is no file I/O, thread, `VirtualAlloc` or TLS directory**, and the
  code never reloads FS or enters the kernel on its own.
- **`COMCTL32` is one call**, `InitCommonControlsEx`. The status bar is the
  program's own window class.
- **The card bitmaps use two header formats**, the 12-byte and the 40-byte
  one, at 1, 4 and 24 bits per pixel.
- **The Deck dialog is twelve owner-drawn buttons.**

Not measured yet: what the program calls at run time, and in which order.

## Paths considered

| Path | What it needs | Verdict |
| --- | --- | --- |
| **A. Port Wine to the POSIX subsystem** | Wine is its PE DLLs plus a Unix side: `wineserver`, the Unix half of `ntdll` and `win32u`, and a display driver. That side needs delivered signals with machine contexts, general `mmap`/`mprotect`, Unix sockets with descriptor passing, `poll`, threads and `dlopen`. SavanXP's POSIX layer stores signal handlers without delivering them and has a restricted `mmap` | Not for the first goal. It turns the project into a Wine host, after a POSIX layer close to Linux's exists. Revisit with a named application that a local implementation cannot reach |
| **B. Unmodified Linux Wine on the Linux subsystem** | The [Linux roadmap](LINUX_SUBSYSTEM_ROADMAP.md) in its later profiles: dynamic glibc, futexes, delivered signals, sockets and a far larger X11 surface | The furthest path. It also makes Linux a prerequisite, which both roadmaps reject |
| **C. Microsoft's own DLLs over an emulated NT system call layer** | Windows XP's `ntdll`, `kernel32`, `user32` and `gdi32`, a few hundred NT services and several hundred `win32k` services with XP's numbers and undocumented behaviour, and the CSRSS connection `kernel32` opens at start | Rejected. The interface is larger than the API above it and undocumented, and the user would have to supply all of `system32` |
| **D. Own DLLs, PE loader and 32-bit system calls in the kernel** | A PE parser in the kernel, Windows process start-up there, a 32-bit entry with pointer widening, a new subsystem id | Rejected. It is what the first plan implied. It grows the kernel's attack surface, and every loader defect becomes a kernel defect |
| **E. Own PE32 DLLs over an in-process 64-bit host** | K1, K2 and user-mode code | **Chosen.** The smallest kernel change, and the only path whose core can be tested on the build host |
| **F. API implemented in 64-bit code, one thunk per function** | Every function, structure and callback converted between 32 and 64 bits, including window procedures called from 64-bit code | Rejected. It is the expensive half of WoW64, paid for every `USER32` call. E avoids it by keeping the API DLLs 32-bit |
| **G. A 64-bit Solitaire** (from Windows XP x64 Edition, if its `sol.exe` is PE32+) | No 32-bit execution at all | Not taken. It is a different, rarer binary whose format is unverified, and K1 turned out to be small |

Emulating the 32-bit processor was never a candidate: every CPU `master`
supports runs 32-bit code natively in compatibility mode.

## What changed from the first plan

- **The architecture is decided.** The first plan left "written locally,
  adapted from Wine, or mixed" and "the boundary to Sx" to W0. They are above.
- **The PE loader moved out of the kernel.** The old W1 routed a validated PE
  through the kernel's launch path and the shared slice S1. The loader is now
  user-mode code, shared between `winhost` and the host tests.
- **The 32-bit work is specific and small.** "CPU compatibility-mode
  execution" and "a checked 32-bit bridge to Sx" become K1: two descriptors
  and four selectors per thread. There is no 32-bit entry into the kernel.
- **The shared slices S1 to S3 are not prerequisites.** Launch selection is a
  file association. General reserve/commit memory and user exception delivery
  are not on Solitaire's path. They return in W7, each with its consumer.
- **No ordering with the Linux subsystem.** The first plan preferred Linux L4
  before Win32. Nothing in W0 to W6 consumes Linux work, in either direction.
- **The phases follow the application.** The console executable and the file
  services of the old W3 and W4 shrank to what `sol.exe` imports. The old W5,
  which held most of the work in one step, is split into windows, bitmaps and
  resources, and menus and dialogs. Running the original `cards.dll` from a
  fixture is a new gate before the game itself.
- **The tools changed.** A relay trace from Wine on the build host is the
  practical way to list what `sol.exe` calls at run time. sogen emulates
  current Windows with its own DLL sets and adds little for an XP x86 binary.

## Architecture

### The gate

Entering the guest (process start and thread start only): `winhost` loads the
32-bit data selector into SS, DS, ES and FS, sets the FS base to the thread's
TEB with `sxn_set_thread_base`, switches to the guest stack and far-returns to
the guest entry point in the 32-bit code segment.

Calling a service: the guest's `ntdll` loads a service number and the address
of a flat argument block, then calls through a pointer kept in the TEB. It
leads to a host-provided thunk that far-jumps to the 64-bit code segment. The
host switches to the thread's own host stack, validates the block, performs
the service and returns the same way.

The service table is **private and versioned**. `winhost` and the DLLs are
built and installed together and refuse a mismatched version. It is not a
public ABI and it is not the NT system call table. Its rules:

- arguments are fixed-width integers and guest addresses below 4 GiB, checked
  by the host before use;
- no service calls guest code; window procedures, timer procedures,
  `DllMain` and TLS callbacks are called by `user32` and `ntdll`;
- a service that blocks (waiting for input, a timer or a handle) blocks in the
  host on the existing native waits.

The expected groups, about forty calls for the first profile: process and
thread, guest memory, files, clock and sleep, synchronization, debug output,
window surfaces, input, cursor and fonts.

### Guest address space

`winhost` owns an allocator for the range below 2 GiB and places every guest
mapping itself with K2: images at their preferred base when it is free,
relocated otherwise, then the stack, the TEB and PEB, and the heap arenas.
Kernel-chosen views land above 4 GiB and never collide with the guest.

An image is loaded as one section per run of pages with the same protection.
Each is mapped writable, filled, relocated and bound, then unmapped and
mapped again with its final access. A view is never writable and executable
at once, and an image that asks for such a section is rejected with a
diagnostic. This uses the view rules that exist; it does not need `mprotect`.

`winhost` itself links at `0x400000`. It must end below the lowest preferred
base in the profile, which is asserted at load. For Solitaire that base is
`0x01000000` and cannot move, which leaves `winhost` 12 MiB.

### The DLL set and its build

Each DLL is a directory under `subsystems/win32/dlls/` with C sources and a
`.def` file, compiled for `i686-pc-windows-msvc` as freestanding code and
linked by `lld-link` without default libraries. Windows types and constants
come from a local header set that covers what is used; neither the Windows
SDK nor MinGW headers are a dependency. `lld-link`, and `llvm-rc` when the
first fixture with resources needs it, join CMake's tool discovery and
[BUILD_CMAKE.md](../../build/BUILD_CMAKE.md).

The same sources also compile for the build host where they hold pure logic
(regions, raster operations, the message queue, the dialog template parser),
so those parts have ordinary host tests.

### Windows, input and drawing

- A top-level `HWND` is a `windowd` window. The frame and caption are the
  window manager's; `user32` reports system metrics that agree with them. The
  menu bar is drawn by `user32` at the top of the surface and the client
  origin is offset below it.
- A modal dialog is an owned window; a popup menu is an owned popup. Child
  windows and controls exist only inside `user32` and share their top-level
  window's surface.
- The host maps each surface a second time below 4 GiB. `gdi32` rasterizes
  into it and the host presents the damaged rectangles. Device contexts,
  bitmaps, brushes, pens and clip regions are guest objects in guest memory.
- Text uses the platform's UI strikes, which the host exposes read-only. The
  font mapper is bounded: `MS Shell Dlg`, `MS Sans Serif` and `System`
  resolve to the 13 px face. There is no TrueType in the first profile.
- Input arrives through the host as raw pointer and key records. Translating
  them into Windows messages, capture, focus and virtual keys is `user32`'s
  work.

### Files, paths and settings

The guest's `C:` is `/system/win/drive_c`. Our DLLs install to
`drive_c/windows/system32/`; the user's files install to
`drive_c/Program Files/Solitaire/`. The build syncs the files it owns and
never touches the rest, by the same rule that protects `/bin` and `/games`.

Names are lower-cased on lookup and installed lower-case, because the native
API cannot enumerate a directory. Real case-insensitive lookup waits for a
native enumeration call with its own consumer.

`HKEY_CURRENT_USER` is one small text file per user, written whole with
`sxn_write_file`. Its path is pinned in W2. Windows path rules, sharing modes
and error codes are translated in the subsystem; SavanXP credentials and
handle checks still apply underneath, and a drive mapping is not an
authorization boundary.

### Launch

`winhost <path to the .exe>` is the only entry. Files and the desktop reach it
through the existing `file_assoc` mapping for `.exe`, and the Start menu
through a shortcut. The kernel's `spawn` of a PE file fails as it does today.
A neutral "run this image with that program" mechanism is not planned until
something other than the shell needs it.

## Kernel slices

Both are neutral, both follow the consumer rule of
[SUN.md](../../userland/SUN.md), and neither mentions Windows.

### K1 — 32-bit user segments

Size S. The whole of "32-bit execution on the x86-64 kernel".

- Add a ring-3 32-bit code descriptor, and a ring-3 data descriptor usable as
  a 32-bit stack and data segment (4 GiB limit, D/B set). The TSS descriptors
  move; `kTssSelectorBase` and its assertion follow.
- Make the DS, ES, FS and GS selectors per-thread state, restored before the
  FS and GS bases on a thread switch. Without this, a thread that migrates to
  a core whose data selectors are null faults on its first memory access.
- A thread that enters the kernel with `int 0x80` from a 32-bit code segment
  gets `SX_ENOSYS`. The native ABI is defined for 64-bit callers only, and the
  kernel must not interpret registers whose upper halves are undefined.
- Still absent on purpose: an LDT, 16-bit segments, call gates,
  `syscall`/`sysenter`.

**Done when:** `nativetest` enters 32-bit code from a 64-bit SUN thread,
computes, reads FS-relative data and returns, repeatedly, under preemption on
1, 2 and 4 vCPUs with 64-bit threads running beside it. A fault in 32-bit
code ends that process alone. The refused `int 0x80` is asserted.
[ARCH.md](../../kernel/ARCH.md) and
[KERNEL_SECURITY.md](../../kernel/KERNEL_SECURITY.md) record the new contract.

### K2 — a native view at a chosen address

Size S. `SXN_SYS_MAP_VIEW_AT`: what `vm::map_section_view` already does and
POSIX already reaches. The base is page-aligned, the range must be free, and
a view is still never writable and executable.

Count the views of the Solitaire process at W1: protection runs per image,
stack, TEB, heap arenas, and two views per surface. If the count nears
`kMaxSectionViews`, raising it is its own change with its `sectiontest`
update, as [SECTIONS.md](../../kernel/SECTIONS.md#the-budget) describes.

**Done when:** `nativetest` maps a section at a chosen low address, is
refused on a misaligned base and on an occupied range, and remaps the same
section with a different access after unmapping it.

### Deferred: S1, S2 and S3

The [shared groundwork](LINUX_SUBSYSTEM_ROADMAP.md#shared-sx-groundwork) of
the Linux roadmap stays where it is. For Win32:

| Slice | Needed by | Until then |
| --- | --- | --- |
| S1, launch contract | A program that must start a PE through `spawn` | The shell's file association |
| S2, reserve/commit/protect/query | `VirtualAlloc` and `VirtualProtect` with their real semantics; guard pages; large heaps | The heap sits on section-backed arenas. A fixture that needs `VirtualProtect` is out of the first profile |
| S3, user exception delivery | Structured exception handling: `__try`/`__except`, `RaiseException`, C++ exceptions | `sol.exe` only registers a handler frame, which is a write to the TEB. A fault ends the process with the kernel's `user: exception` line |

## Phases

W0 and the build-host half of W1 need no kernel change and can start now.

```text
W0 ─┬─────────────► W1 ─► W2 ─► W3 ─► W4 ─► W5 ─► W6
K1 ─┤ (guest half of W1 onward)
K2 ─┘
```

### W0 — profile and inventory

Size S. Nothing here is committed except hashes and lists.

**Done (2026-11-10):** the static inventory, in
[WIN32_SOLITAIRE_PROFILE.md](../../userland/WIN32_SOLITAIRE_PROFILE.md). The
user's files are the Spanish Windows XP ones, version 5.1.2600.0. The record
holds their hashes, headers and section protections, every import by DLL,
the exports of `cards.dll`, the resources with each dialog's controls and
each bitmap format, and the scan for what the design assumes is absent:
loads of FS, `int 0x2e`, `sysenter`, a TLS directory, delay imports.

**Remaining:**

- A relay trace of the same files under Wine on a build host, from start-up
  through a game, the Deck and Options dialogs, About and exit. It marks each
  function as needed at start-up, needed in play, or reachable only from a
  feature declared out of scope, and lists the raster operations, window
  messages, system metrics, stock objects and registry values actually used.
- Reference captures from Windows XP in a virtual machine: the main window,
  each menu and dialog, all card faces and backs, and the victory animation.
- Three decisions the trace settles: whether mouse play reaches
  `SetCursorPos` (the desktop has no request to move the pointer), whether
  the edit control of the hidden game-number dialog is in scope, and how the
  currency format for Vegas scoring is read.

Help is out of scope. `LoadLibraryA` of `hhctrl.ocx` fails and `WinHelpW`
reports failure, which the program handles with its own message.

**Done when:** the profile also carries that classification, the run-time
lists and the reference captures.

### W1 — the loader, the gate and the first executable

Size L. Replaces the old W1, W2 and W3.

- `subsystems/win32/pe/`: a PE32 parser and mapper as a portable C library.
  Checked sizes, RVAs, alignment and arithmetic; zero-filled tails; base
  relocations; imports by name and ordinal; exports and forwarders; the TLS
  directory and callbacks; initialization order. Rejections are explicit:
  other machines, PE32+, malformed directories, overlaps, truncation.
- `subsystems/win32/host/`: `winhost` with the gate, the guest allocator, a
  minimal TEB and PEB (exception list head, self pointer, PEB pointer, last
  error, TLS slots) and the first services: write to a standard handle, exit,
  debug output.
- `ntdll.dll` and `kernel32.dll` with what `hello.exe` imports.
- A build-host backend for the same loader and gate, so fixtures run under
  CTest on a Linux x86-64 host without booting the guest.

**Done when:** host tests reject a corpus of malformed images. The same
`hello.exe`, identified by hash, writes the same bytes through an imported
`WriteFile` and exits with the same status on Windows XP, in the host harness
and on SavanXP, where a SUN parent captures its output. A second fixture
imports from a separate DLL, is relocated to a different base and runs its
initialization and TLS callbacks in order. A missing DLL or symbol is named
in the diagnostic, and a failed load leaves no view or handle behind.
`win32-hello` is a registered smoke scenario; ELF and SXE launches are
unaffected.

### W2 — base runtime for the measured closure

Size S. Only the non-GUI functions of the W0 matrix: 18 in `msvcrt`, 9 in
`KERNEL32`, 6 in `ADVAPI32`.

The C runtime start-up and exit `sol.exe` links against, with `rand`,
`srand` and `time`; `LocalAlloc` and `LocalFree` over the process heap;
`GetModuleHandleA`, `GetStartupInfoA`, `GetCommandLineW`, `lstrlenW` and
`MulDiv`; `LoadLibraryA` and `GetProcAddress` over the loader; last-error
state; and the six registry calls over the per-user store.

The clock is a service, and `winhost` accepts a fixed start time in test
runs, so a deal that depends on it can be repeated.

**Done when:** a fixture that starts the way `sol.exe` does (the same runtime
imports, a `WinMain`) runs on all three environments with identical output,
and a registry value written by a fixture is read back after a guest restart.
Each failure path the matrix marks as reachable has an assertion.

### W3 — windows and messages

Size L. `user32` core and the part of `gdi32` a window needs to paint.

Window classes and procedures, creation and destruction, the window tree,
the message queue with posted and sent messages, paint synthesis from the
invalid region, timers, quit, focus and activation, pointer and keyboard
translation, capture, cursors, `DefWindowProc`, moving and resizing, and
closing from the caption. In `gdi32`: window and paint device contexts, solid
fills, lines and rectangles, text with the platform strike, stock objects.

**Done when:** an unchanged PE32 fixture creates a window, paints known
content, responds to pointer and key input, updates and closes. Its message
log matches the Windows XP reference for a recorded input sequence, and its
pixels match a golden image. Native and POSIX windows and the taskbar behave
as before. `win32-window` is a smoke scenario.

### W4 — bitmaps, resources and `cards.dll`

Size L.

The resource loader (find, load and size; bitmaps, strings, icons, cursors,
accelerators), device-independent to device bitmaps in both header formats
and at the three depths `cards.dll` contains, memory device contexts, `BitBlt`, `StretchBlt` and `PatBlt` with the
raster operations the trace lists, brushes and pens, clipping, saved device
context state, and pixel access.

**Done when:** a fixture written here loads the user's **original
`cards.dll`** and draws every face, every back and the special cards through
`cdtInit`, `cdtDrawExt` and `cdtTerm`, and the result matches the Windows XP
captures. The raster operations have host tests against computed truth
tables. `win32-cards` is a smoke scenario that is skipped, not failed, when
the user's files are absent.

### W5 — menus, dialogs and controls

Size L.

The menu bar and popup menus from menu resources, with keyboard navigation,
accelerators, checked and disabled items. Dialogs from dialog templates as
owned windows, the modal loop, dialog units from the font, and the controls
the templates contain. The inventory lists them: push and default buttons,
automatic check boxes, radio buttons that the program checks itself, group
boxes, left- and right-aligned static text, and **owner-drawn buttons**, which
the Deck dialog uses for its twelve card backs. The edit control only if the
trace puts the game-number dialog in scope. `MessageBoxW`, `ShellAboutW` in
`shell32`, and `InitCommonControlsEx` in a `comctl32` that registers no
class. There is no status bar control: Solitaire draws its own.

**Done when:** a fixture with a menu and two dialogs built from the same
resource types as `sol.exe` matches the reference in layout and in behaviour
for a recorded sequence, including keyboard-only operation and cancelling.

### W6 — Windows XP Solitaire

Size M, if the earlier gates were honest.

Launch the pinned, unchanged `sol.exe` with the user's `cards.dll` from
`/system/win/drive_c/Program Files/Solitaire/`, from Files and from the Start
menu, with its own icon.

**Done when:** Solitaire shows a correctly drawn, playable board. It deals a
new game, draws from the stock, and supports legal moves by click, by drag
and by double click. Menus, Options, Deck and About work. The window
repaints after occlusion, after being moved, resized, minimized and restored,
and it closes and relaunches. A game is played to the end with a fixed clock
and a recorded input sequence, and the victory animation runs. Options
persist across a guest restart. Hashes, logs and visual assertions are kept;
a window that opens or a board that renders once does not close the goal.
Native and POSIX windows and the taskbar stay correct, and the game, its
files and its settings survive a normal build. `win32-solitaire` is a smoke
scenario, skipped when the files are absent.

### W7 — later profiles

Not planned in detail. Each extension names an application and records its
own inventory before any code:

- S2 and S3 with their Windows consumers: `VirtualAlloc` semantics and
  structured exception handling.
- Threads beyond the first, with the loader lock and attach and detach calls.
- Further DLLs by consumer: more of `comctl32`, common dialogs, `winmm`
  through the audio service, networking.
- Other applications of the same era. Those that also use `cards.dll` are
  the cheapest next step.
- PE32+ on the same host, where the gate becomes an ordinary call.

Not promised without a separate decision: general Windows compatibility,
16-bit and DOS programs, kernel drivers, .NET, DirectX, COM at large,
installers.

## Starting the implementation

For whoever picks this up, human or agent. Read this document, then
[SUN.md](../../userland/SUN.md), [NATIVE_APPS.md](../../userland/NATIVE_APPS.md),
[SECTIONS.md](../../kernel/SECTIONS.md) and
[OWNED_WINDOWS.md](../../desktop/OWNED_WINDOWS.md). `AGENTS.md` governs every
commit.

**Order of work.** Each line is one or more commits that leave the tree
green; none needs the one after it.

1. W0, the rest of the inventory: the run-time trace and the reference
   captures. No code in the tree; it needs the user's files and Wine on a
   build host.
2. W1, host half: `subsystems/win32/pe/` with its host tests, the fixture
   build (`lld-link` in CMake discovery), and the host harness that runs
   `hello.exe` on the build host. No kernel change.
3. K2, then K1, each with its `nativetest` assertions.
4. W1, guest half: `winhost` on SavanXP and the `win32-hello` smoke scenario.
5. W2 onward, in order.

**What is already proven on the build host** (2026-11-10, clang 23 and lld;
nothing of it is in the tree yet). The fixture build needs only these two
tools:

```sh
clang --target=i686-pc-windows-msvc -ffreestanding -fno-stack-protector \
      -fno-builtin -O1 -c k32.c -o k32.obj
lld-link /dll /nodefaultlib /machine:x86 /entry:DllMain /def:kernel32.def \
         /out:kernel32.dll /implib:kernel32.lib k32.obj
lld-link /nodefaultlib /machine:x86 /subsystem:console /entry:start \
         /out:hello.exe hello.obj kernel32.lib
```

The `.def` file is `LIBRARY kernel32.dll`, then `EXPORTS` and one name per
line; exported functions are `__stdcall`. The gate below called a `stdcall`
function of such a DLL from a 64-bit Linux process and returned its result.
On Linux the selectors are `0x23` (32-bit code), `0x2b` (data) and `0x33`
(64-bit code); on SavanXP they are the ones K1 adds.

```asm
call32:                       # (entry, guest stack top, a, b), SysV ABI
    push %rbx; push %rbp; push %r12; push %r13; push %r14; push %r15
    mov  %rsp, %r12           # host stack, in a register stdcall preserves
    mov  %ds, %r13w
    mov  %es, %r14w
    mov  %esi, %esp           # guest stack, below 4 GiB
    sub  $20, %rsp
    mov  %edx, 12(%rsp)       # stdcall arguments
    mov  %ecx, 16(%rsp)
    lea  back32(%rip), %eax
    mov  %eax, 8(%rsp)        # the guest's return address
    mov  %edi, 0(%rsp)        # far pointer: entry point ...
    movl $0x23, 4(%rsp)       # ... in the 32-bit code segment
    mov  $0x2b, %eax
    mov  %ax, %ds
    mov  %ax, %es
    lret                      # 64 -> 32
    .code32
back32:
    ljmp $0x33, $back64       # 32 -> 64
    .code64
back64:
    mov  %r13w, %ds
    mov  %r14w, %es
    mov  %r12, %rsp
    pop  %r15; pop %r14; pop %r13; pop %r12; pop %rbp; pop %rbx
    ret
```

Two things the experiment did not cover and the real gate must: `back32` and
every host thunk the guest jumps to must sit below 4 GiB, and SS and FS need
the 32-bit data selector as well, with the FS base set after the selector is
loaded (loading a selector resets the base).

**Rules that are easy to break.**

- A function exists because the W0 matrix names it. An import that is not
  implemented fails the load by name; it never becomes a stub.
- No Windows concept enters the kernel, and no call is added to SUN because
  Windows has one.
- Microsoft's files stay out of the repository and out of the build. Tests
  that need them read `SAVANXP_WIN32_FIXTURES` and are skipped without it.
- A phase closes on its "Done when", with hashes and logs, not on a window
  that opened once.

## Evidence and tests

| Source | Role | Limit |
| --- | --- | --- |
| Windows XP in a virtual machine, the edition the files come from | The behavioural reference | One version and language; the deal cannot be fixed there |
| Host tests | Pure logic: the PE parser against a corpus, raster operations, regions, the message queue, template parsers | No Windows semantics beyond what the test encodes |
| The host harness | The real PE32 fixtures and DLLs, run on the build host through the same loader and gate, with an offscreen surface and scripted input | Linux x86-64 only; it is skipped elsewhere. It does not test K1, K2 or the desktop |
| Smoke scenarios in QEMU | The acceptance gate of every phase, with serial logs and screenshots through QMP | Slower; run after the harness passes |
| Wine on the build host | Relay traces: what the unchanged binary calls, with arguments. It also shows this binary runs over a DLL set that is not Microsoft's | Wine's behaviour is not Windows' behaviour. Check the reference before changing an expectation |
| Wine's conformance tests | Assertions and edge cases to reduce into fixtures | The test executables import far more than the operation under test. A reduced case is reported as reduced |
| ReactOS and Wine sources | Reading | Reference only, under the licence rule below |

Compare the same executable across environments by its outputs and effects.
Normalize addresses, handles and timings without hiding a difference in
meaning. Report pass, fail, skip, crash and timeout separately; a test that
could not run is not coverage.

Tests that need the user's files take their location from the
`SAVANXP_WIN32_FIXTURES` environment variable or the CMake cache variable of
the same name, verify the hashes recorded at W0, and are skipped without
them.

## Build, source and licence rules

- New code under `subsystems/win32/` is the project's own, under its licence.
  Wine (LGPL-2.1-or-later) and ReactOS (GPL and LGPL) are `Reference` in the
  terms of [THIRD_PARTY_ADOPTION.md](../../third-party/THIRD_PARTY_ADOPTION.md).
  Adopting a file from either is a decision per DLL, recorded in
  [THIRD_PARTY_PROVENANCE.md](../../third-party/THIRD_PARTY_PROVENANCE.md);
  each DLL is a separate library, so it would not change the licence of the
  others. None is planned.
- Microsoft's files are never committed, bundled into an image by the build,
  or downloaded by a script. The user installs their own copies with a
  documented command that goes through the image update path the ports use.
- `winhost` is a SUN program. Each native capability it lacks arrives as a
  slice with its `nativetest` assertion. SUN does not grow toward the Windows
  API ([SYSTEM_LAYERING.md](../../userland/SYSTEM_LAYERING.md)).
- Host tools are found by CMake and listed in
  [BUILD_CMAKE.md](../../build/BUILD_CMAKE.md); no machine-specific paths.
- Loader, kernel and drawing changes run the host tests, `native-test`, the
  POSIX and thread regressions and `./build.sh smoke sxgui-smoke`, then the
  persistence check of [AGENTS.md](../../../AGENTS.md), extended to the files
  under `/system/win`. A valid image is never reset to install the subsystem.
- Each phase that changes behaviour gets its `[Unreleased]` entry in the
  commit that lands it, and what was built is described in a document under
  `docs/userland/`. This roadmap stays a plan.

## Risks

| Risk | Effect | Answer |
| --- | --- | --- |
| The run-time trace shows behaviour the import list hides: a message, a raster operation or a metric that is costly | A phase grows | The static surface is fixed at 124 functions; the trace is taken before W3 starts |
| `winhost` grows past `0x01000000`, where `sol.exe` must load | The load fails | Asserted at load and in the build. If it ever does not fit, `winhost` is linked above the guest range instead |
| A later program reloads FS | Its TEB access faults | Solitaire does not. The fix is a per-thread descriptor, a known extension of K1 |
| Keyboard play needs `SetCursorPos` and the desktop cannot move the pointer | Playing without a mouse is incomplete | Decided from the trace: a neutral `windowd` request with its own consumer, or a stated limit of the first profile |
| 64 views are not enough | The load fails cleanly | Count at W1; coalesce protection runs; raise the limit with its test |
| Pixel comparison is too strict across font or metric differences | False failures | Compare card art and geometry exactly, text regions by layout; record each tolerance |
| The work turns into an API catalogue | The goal recedes | The matrix is the scope. A function without a consumer in it is not written |

## References

- [Microsoft PE/COFF format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
- [x86 `stdcall`](https://learn.microsoft.com/en-us/cpp/cpp/stdcall) and
  [Windows data types](https://learn.microsoft.com/en-us/windows/win32/winprog/windows-data-types)
- [Wine source](https://gitlab.winehq.org/wine/wine): its `cards` DLL
  documents the `cdt*` contract, and `dlls/user32/tests` and
  `dlls/gdi32/tests` are the conformance tests to reduce
- [ReactOS source](https://github.com/reactos/reactos), for reading
- [Thread groundwork](../done/THREADS_ROADMAP.md),
  [the Sx API](../../kernel/KERNEL_NATIVE_API.md),
  [sections and views](../../kernel/SECTIONS.md),
  [owned windows](../../desktop/OWNED_WINDOWS.md) and
  [platform graphics](../../desktop/PLATFORM_GRAPHICS.md)
