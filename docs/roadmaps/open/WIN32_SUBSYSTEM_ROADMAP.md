# Win32 subsystem — Windows applications on Sx

> **Status: planned, not started (2026-11-08).** The first application goal is
> **Windows XP Solitaire (`sol.exe`)**. Plan initially for the XP x86/PE32
> profile and verify the exact executable at W0; 32-bit execution is required
> work on the x86-64 kernel. The installed runtime lives under **`/system/win`**.
> No PE loader, Windows DLL implementation or Wine test target exists yet.

## Objective and relationship to Linux

Run **the original Windows XP Solitaire binary** unchanged on the SavanXP
desktop, with real card drawing, input, menus and gameplay. Pin the exact XP
edition, service pack, language, executable and companion files at W0. A
native lookalike, Wine's own Solitaire application or a source port does not
satisfy this goal.

Small PE/console/DLL and drawing fixtures are intermediate engineering gates
on the way to Solitaire. Build those with an ordinary Windows-targeting
toolchain; they must also run on Windows without SavanXP headers, relinking
or binary patching. An unrelated external console application is optional,
not a prerequisite for the selected Solitaire application.

The preferred sequence is the bounded static milestone L4 in
[LINUX_SUBSYSTEM_ROADMAP.md](LINUX_SUBSYSTEM_ROADMAP.md), then Win32's first
integrated PE/DLL milestones. Linux exercises shared VM, ABI and process
mechanisms with fewer new userland components. It is not a dependency of
Win32: neither a Linux distribution nor running Wine on Linux compatibility
is required. W0 and the PE parser in W1 can begin independently. Audio and
Azalia completion are not prerequisites.

The planning profile is XP x86 **PE32**, including 32-bit execution, calling
conventions, TLS and exception handling as required by the pinned files.
An x86-64 PE32+ probe may help investigate the loader, but does not close
those gates. If W0 selects an XP x64 binary instead, record that explicit
profile decision and adjust every fixture/dependency accordingly; do not
silently substitute another Solitaire release to avoid architecture work.

## Baseline and architecture

SavanXP already has handles, events, timers, semaphores, waits, sections, IPC,
processes, SMP threads and per-thread FS/GS bases. It has a VFS, `windowd`,
composition and shared graphics libraries. These are reusable mechanisms,
not implementations of Windows object, file or window semantics.

The current loader handles ELF and chooses native or POSIX from `EI_OSABI`.
There is no PE recognition/loader, Windows process startup, PEB/TEB or Windows
DLL set. General VM reservation/protection and user exception delivery remain
shared groundwork; 32-bit user execution is not established by the existing
64-bit ELF path. SUN has no general file API. Recheck this inventory when
work begins rather than treating the roadmap as an implementation record.

The intended boundary is:

```text
Unmodified Windows PE application and its supported DLLs
    -> Windows API DLLs (kernel32, user32/gdi32 and measured dependencies)
    -> subsystem runtime / ntdll-compatible layer
    -> Sx objects, VM, processes, VFS and IPC
    -> existing platform services and graphics
```

- Follow [SYSTEM_LAYERING.md](../../userland/SYSTEM_LAYERING.md): Win32 is
  beside SUN and POSIX on Sx. SUN must not grow a replica of the Windows API.
- Put foreign ABI definitions, status translation and subsystem-private state
  in a proposed `subsystems/win32/` tree. Keep kernel glue minimal and neutral;
  reuse process/thread state registration instead of adding Windows fields
  to the kernel's process structures.
- General file/process operations missing from Sx become neutral mechanisms,
  not calls through the POSIX dispatcher. Map `NTSTATUS`, Win32 errors and
  access masks explicitly; do not return Sx's `-errno` directly to a PE caller.
- New SavanXP launchers/brokers are SUN programs. Missing SUN capabilities
  need their actual consumer and `nativetest` assertion. Windows test binaries
  are foreign compatibility fixtures, not SUN programs renamed to `.exe`.
- Reuse graphics algorithms where useful. A Windows `HWND`, message queue,
  device context or control is still a Windows contract to implement; mapping
  a Windows control to an SXGUI widget does not automatically preserve it.

## Runtime layout

The installed subsystem lives in **`/system/win`**, alongside the other
subsystem runtimes under `/system`, as specified in
[SYSTEM_LAYERING.md](../../userland/SYSTEM_LAYERING.md#planned-subsystem-runtime-layout).
This is a runtime path inside the OS; the proposed source tree remains
`subsystems/win32/`.

Use `/system/win/drive_c` as the initial backing tree for the guest `C:`
drive. The profile's Windows directory and system DLLs live beneath
`drive_c/windows/`, with the first PE32 DLL set in `windows/system32/`.
Install Solitaire and its application-local dependencies beneath
`drive_c/Program Files/Solitaire/`. W0 pins actual filenames, DLL search order,
case handling and any separate per-user settings store. Windows path rules
are implemented by Win32; do not change native SxFS path semantics globally.

Native launchers/brokers and compatibility-runtime metadata also belong to
the subsystem's installation manifest under `/system/win`. Keep SUN/POSIX
libraries separate and retain SavanXP credentials/handle checks when exposing
native files. A guest drive mapping is not an authorization boundary. Normal
builds and updates preserve externally supplied executables, companion files
and persistent data; do not reset this tree or a valid disk image to install it.

## Shared prerequisites and implementation choice

Use the shared S1-S3 inventory in
[the Linux roadmap](LINUX_SUBSYSTEM_ROADMAP.md#shared-sx-groundwork): neutral
launch selection/rollback, general VM regions and user exception delivery.
Each kernel slice lands with a native consumer and assertions, independent of
whether Linux or Win32 first needs it. W1's parser and a restricted PE probe
can precede complete VM and exception coverage; ordinary runtimes will need
more than that probe.

The XP x86 goal additionally needs a neutral 32-bit user-execution slice in
the architecture/process layer, with a real SUN launcher consumer and
`nativetest` assertions for launch, transitions, scheduling and cleanup.
Keep PE/TEB/Windows layouts in Win32. Linux x86-64 does not depend on this
slice, and it does not require implementing a general Windows WoW64 stack.

The default direction is **Wine-style user-mode API compatibility**, with
selective reuse evaluated before large APIs are reimplemented. W0 must record
whether the first DLL subset is written locally, adapted from Wine, or mixed,
and what the boundary to Sx is. Porting Wine wholesale is not a small build
task: its host integration, server and graphics backend need explicit work.

A separate experiment may evaluate **real Windows DLLs over NT syscall
translation**, as [sogen](https://github.com/momo5502/sogen) does in an emulator.
That path requires a pinned Windows/DLL profile, the actual NT entry mechanism
used by those DLLs, Windows register/argument conventions and the corresponding
NT services. For an x64 experiment this includes the CPU's `syscall` entry;
XP x86 uses its own 32-bit path. Linux syscall numbers/conventions cannot be
used for Windows.
Emulator support does not prove that native execution on SavanXP will work.
DLL acquisition and distribution rights must be resolved separately; sogen's
license does not license Microsoft's DLLs. Do not make this experiment a
dependency of the default route without evidence and an updated decision.

## Phases and acceptance

All phases below are pending. Complete one observable execution chain before
expanding API coverage; a successful stub or a window-shaped screenshot is
not a compatibility milestone.

### W0 — freeze the first Windows profile

Freeze the Windows XP Solitaire profile, initially XP x86/PE32. Inspect the
actual `sol.exe` Machine field and optional-header format; record the XP
edition/service pack/language and hashes. Inventory imports by name/ordinal,
dynamically loaded DLLs, resources, registry/settings access and expected
interactive behavior. Include `cards.dll` if required by the selected binary,
and measure its imports too; a static import list alone is not the full
dependency closure. No proprietary files are acquired or bundled by this plan.

Pin an x86 Windows-targeting toolchain and small PE32 fixtures without a CRT
for the first chain, then include the application's actual runtime needs.
Define rejected image variants, the initial DLL set, DLL search policy,
license boundary and install mappings beneath `/system/win`. Keep any x64
investigation explicitly separate from the PE32 acceptance profile.

**Done when:** the implementation choice above is recorded, fixtures run on
the reference Windows installation with retained hashes/results, and an
API/dependency matrix distinguishes required Solitaire work from deferred
features, including a recorded gameplay/reference input sequence. No claim
of general "XP compatibility" follows from running this one application.

### W1 — PE recognition, validation and mapping

Use S1 to route a validated PE image to Win32 without changing ELF/SXE launch
behavior. Build an isolated parser first, then map PE headers/sections with
checked sizes, RVAs, alignment, zero-filled tails, relocations and final page
permissions. Reject unsupported architectures, malformed directories,
overlaps, truncated data and address arithmetic overflow. Preserve platform
memory policy; do not silently allow RWX just because a guest asks for it.

For the initial XP x86 profile, implement PE32 mappings/relocations and CPU
compatibility-mode execution on the x86-64 kernel: user segments, saved
register state, address/stack limits, faults, interrupts and kernel entry/
return. Preserve that execution mode across scheduling on several vCPUs.
Define a checked 32-bit bridge to Sx, including pointer/length widening,
overflow checks and handle translation; a 64-bit C call cannot execute an
arbitrary 32-bit application function.

Implement x86 Windows `stdcall` API/callback and `cdecl` CRT conventions as
needed, with correct stack cleanup, nonvolatile registers and 32-bit layouts.
An optional PE32+ profile needs a separate x64 ABI bridge, including shadow
space and its register convention. Do not confuse those two execution paths.

**Done when:** host parser/corpus checks reject malformed images, and a tiny
PE32 fixture executes in user mode and returns an observed result through a
controlled bootstrap bridge. Its hash matches the Windows reference. Loading
failure releases all mappings and process resources; ELF/SXE still works.
Tests cover 32-bit pointer boundaries, faults, scheduling and mode restoration;
64-bit SUN/POSIX/Linux execution remains correct. This restricted bootstrap is
not yet a normal Windows application environment.

### W2 — process startup and DLL loading

Build the required 32-bit PEB/TEB subset, FS-based TEB access, command line and
environment, module bookkeeping and per-thread last-error state. Implement
imports by name/ordinal, exports/forwarders as required, static TLS and its
callbacks, DLL dependencies and initialization order. Define loader-lock,
failure rollback and thread attach/detach behavior as real consumers arrive.
Unsupported API sets, delay imports or loader features must be reported, not
silently resolved to success stubs.

All in-process DLLs must match the PE32 application's architecture. Preserve
32-bit FS/TLS state across callbacks and scheduling; a future x64 profile uses
its own 64-bit layouts and GS-based TEB path. Do not treat the existing native
FS/GS base mechanism alone as proof of working compatibility-mode TLS.

**Done when:** fixtures call an imported function from a separate DLL, run
initialization and TLS callbacks in the expected order, and verify relocation
at a different base, per-thread TLS isolation and observable DLL state. Missing
DLLs/symbols and failed initialization produce useful diagnostics with no leaks.

### W3 — the first Windows console executable

Supply the minimal `ntdll`/base DLL path for `GetStdHandle`, `WriteFile` and
`ExitProcess`. Connect Windows standard handles to real Sx streams, including
invalid-handle and partial-write behavior. Add the required startup helpers
without pulling in a full CRT or GUI dependency for this fixture.

**Done when:** the same unmodified `hello.exe` writes exact bytes through a
real imported Windows API and exits with the expected status on Windows and
SavanXP. A SUN parent captures its output and observes termination. No injected
replacement application entry point or patched import table on disk counts.
This is the first integrated milestone, not a claim of general console support.

### W4 — Solitaire's base runtime and file services

Use the W0 Solitaire/dependency inventory to drive heap, virtual memory,
ANSI/UTF-16 conversion, file/directory operations, paths/drives, clocks and
synchronization. Include registry/settings and additional base DLLs only
where this target actually requires them. Exercise these operations with
small PE32 fixtures before bringing up the GUI. Use S2 for memory and S3 for
exceptions as needed, with executable tests for the selected x86 SEH/VEH and
runtime behavior. A future x64 profile needs its own unwind implementation;
`.pdata`/`.xdata` support does not establish x86 exception handling.

Preserve Windows-specific file-sharing, creation disposition, handle rights,
inheritance, case behavior and namespace rules in the subsystem. Do not change
SxFS globally to imitate Windows. A guest's apparent drive root is not an
authorization boundary; SavanXP credentials and handle grants still apply.

**Done when:** the measured non-GUI dependency subset has executable tests
matching Windows, including invalid handles/paths, permissions, failures and
the thread/exception features actually required. Installed executables, DLLs
and settings/data survive a normal build and guest restart. A small external
console tool may provide extra coverage, but is not a separate required target.

### W5 — USER32/GDI over the existing desktop

Implement a Win32 window client over `windowd`/composition: classes, window
procedures, per-thread queues, posted versus synchronous messages, input,
focus, close/destruction and the paint/invalidation lifecycle. Add device
contexts, clipping, bitmap/text drawing and resources using platform rendering
where suitable. Controls, menus and dialogs come from named consumers;
`comctl32` is a later DLL contract, not a rename of SXGUI.

**Done when:** an unchanged PE creates a window, paints known content, receives
real keyboard/pointer input, updates it and closes cleanly. Assert message and
callback behavior as well as rendered glyphs/pixels; native/POSIX windows and
the taskbar remain correct. Implement the resource, menu, dialog, timer, mouse
capture and drawing operations identified for Solitaire rather than selecting
an unrelated GUI application to close this gate.

### W6 — Windows XP Solitaire

Launch the pinned, unchanged `sol.exe` from
`/system/win/drive_c/Program Files/Solitaire/`, with its recorded companion
files and DLL set. Connect its USER32/GDI calls to the real SavanXP desktop.
Implement any remaining dependency revealed by startup and gameplay; a
successful stub or replacement card renderer inside the application does not
count as implementing the required Windows API behavior.

**Done when:** Solitaire displays a correctly drawn playable board, deals a
new game, draws from the stock and supports legal card moves with mouse
clicks/dragging. Verify menus/options/dialogs, focus, repaint after occlusion,
moving the window and closing/relaunching, plus a completed game and the
reference victory behavior. Record a repeatable input sequence
and compare visible/state transitions with the pinned XP reference, controlling
deal/timer variability where possible. Verify persistent options if the
reference stores them. Retain hashes, logs and visual/input assertions; merely
opening a window or rendering a static board does not close the application
goal. Native/POSIX windows and the taskbar remain correct, and the installed
game, companion files and settings survive a normal build and guest restart.

### W7 — later compatibility profiles

Expand only with application-driven gates: registry and `advapi32`, additional
CRT/Unicode/locale support, networking, `waveOut`/other audio APIs through the
platform service, graphics libraries and installation behavior. Each extension
records its tested profile and dependencies. The bounded XP x86 support needed
by Solitaire belongs to W1-W6. Broader x86 coverage, x86-64 PE32+, a general
WoW64 environment, 16-bit/DOS, Windows kernel drivers, .NET, COM coverage,
DirectX, arbitrary installers and general Windows desktop compatibility need
separate decisions and acceptance criteria before becoming promised scope.

**Done when:** every selected extension has a named application, conformance
cases and retained results. Freeze a finite extension profile before starting
this phase; there is no universal "all Windows applications" completion gate.
The roadmap stays open while any selected phase is pending. Excluded future
coverage stays documented separately and must not be reported as completed.

## Wine tests and sogen as development tools

Use three distinct sources of evidence:

| Source | Role | Limit |
| --- | --- | --- |
| Pinned Windows environment | Behavioral reference for the chosen profile | Version, architecture and installed DLLs affect results |
| Wine conformance tests | Executable assertions, edge cases and regressions | Their harness/imports can require much more API surface than the tested operation |
| sogen on the development host | Inspect startup, syscalls, memory, TLS and exception handling with hooks/snapshots | Partial emulator behavior is not authoritative Windows behavior or proof of native execution |

Pin upstream commits and start with small PE tests. Then select Wine cases
from `dlls/ntdll/tests/{virtual,sync,thread,exception,unwind,file}.c` and
`dlls/kernel32/tests/{loader,module,virtual,heap,file,process,thread,sync}.c`.
USER32/GDI cases follow W5. Compile for the selected Windows x86/PE32 profile,
not the SavanXP POSIX SDK, and select architecture/version-appropriate cases.
An x64 test pass does not establish x86 coverage. Keep imported test code/
notices and local adaptations identifiable. A reduced test is reported as
reduced, not as a pass of the upstream suite.

The upstream `ntdll` and `kernel32` test executables also import `user32` and
`advapi32`. Inventory dependencies and start with standalone/reduced fixtures
rather than inventing successful DLL stubs to launch the whole suite. Preserve
Wine's expected-failure/version logic, but report SavanXP pass, fail, skip,
expected failure, unexpected pass, crash and timeout separately. Missing setup
or a skipped assertion is not coverage; the full `winetest` runner is not an
early dependency and test results are kept locally by default.

Compare the same executable across environments. With different DLL
implementations, compare statuses, outputs and effects; identical syscall
traces are not required. Normalize unstable addresses, handles and timings
without hiding semantic differences. If sogen differs, reduce the case and
check Windows before changing the implementation or expected result.

## Build, regression and source policy

- Keep fixture hashes, upstream/toolchain pins, DLL/import inventories,
  reference Windows build, serial logs and visual artifacts with each result.
  Proposed PE/Wine smoke scenarios become real commands only when registered;
  none exists today. Verify the rebuilt binary is the image payload being run.
- Discover Windows-targeting tools through CMake and document them in
  [BUILD_CMAKE.md](../../build/BUILD_CMAKE.md), without machine-specific paths.
  Keep the foreign fixture toolchain optional until it is an explicit dependency.
- Run appropriate host/parser tests, native and POSIX/thread regressions,
  and `./build.sh smoke sxgui-smoke` whenever the loader or drawing is touched.
  Follow [AGENTS.md](../../../AGENTS.md) for Doom executable/WAD persistence
  after a normal build, and verify the Win32 payload and persistent state under
  `/system/win` survive too. Never reset a valid image to install the subsystem.
- Apply [THIRD_PARTY_ADOPTION.md](../../third-party/THIRD_PARTY_ADOPTION.md).
  Wine's inspected test files/framework use LGPL-2.1-or-later; sogen declares
  GPL-2.0. Verify each selected file and pin before reuse, preserve notices and
  register distributed components in
  [THIRD_PARTY_PROVENANCE.md](../../third-party/THIRD_PARTY_PROVENANCE.md).
  Copyleft adapters stay on the user side of the syscall boundary under the
  layering policy; this plan does not authorize copyleft code in the kernel.
- External tracing and separate test executables do not require adopting an
  emulator into SavanXP. Microsoft DLLs/assets are not covered by the licenses
  of Wine or sogen. This roadmap vendors nothing. Every later behavior change
  gets its own `[Unreleased]` entry in the implementation commit.

Primary references, to pin or record at W0:

- [Microsoft PE/COFF format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
- [Microsoft x86 stdcall convention](https://learn.microsoft.com/en-us/cpp/cpp/stdcall)
  and [Windows data types](https://learn.microsoft.com/en-us/windows/win32/winprog/windows-data-types)
- [Microsoft x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention)
  and [x64 type layout](https://learn.microsoft.com/en-us/cpp/build/x64-software-conventions)
- [VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)
- [Wine source](https://github.com/wine-mirror/wine),
  [test framework](https://github.com/wine-mirror/wine/blob/master/include/wine/test.h),
  [ntdll tests](https://github.com/wine-mirror/wine/tree/master/dlls/ntdll/tests)
  and [kernel32 tests](https://github.com/wine-mirror/wine/tree/master/dlls/kernel32/tests)
- [sogen source and capabilities](https://github.com/momo5502/sogen)
- [Thread groundwork](../done/THREADS_ROADMAP.md),
  [Sx API](../../kernel/KERNEL_NATIVE_API.md) and
  [platform graphics](../../desktop/PLATFORM_GRAPHICS.md)
