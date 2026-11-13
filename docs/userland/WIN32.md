# SSW — the Savan Subsystem for Windows

SSW is SavanXP's Win32 subsystem: it runs unmodified Windows programs. Win32
is the API it implements, as POSIX is the API of the POSIX subsystem; SSW is
the subsystem's name. Its source tree keeps the API's name
(`subsystems/win32/`), its code the `sxw_` prefix, and its host program is
`winhost`.

> **Status: W2 complete (2026-11-12).** This document describes what exists
> under `subsystems/win32/`. The plan, the phases and the reasons behind them
> are in the [Win32 roadmap](../roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md); the
> API surface of the first profile is in
> [WIN32_SOLITAIRE_PROFILE.md](WIN32_SOLITAIRE_PROFILE.md).

A PE32 console program runs on SavanXP: `winhost.sxe` loads it and its DLLs
below 4 GiB and runs it as 32-bit code. The same loader and DLLs run on the
build host in a harness, and the fixtures that check both give the bytes and
exit statuses Windows XP gives. Nothing of the Windows API beyond what those
fixtures import exists yet, and nothing starts a `.exe` but a command line
naming `winhost`.

```text
subsystems/win32/
  include/       windows.h (the subsystem's own), sxw_abi.h (host <-> DLLs)
  pe/            the PE32 parser and mapper, portable C
  loader/        the process loader, portable C
  dlls/          ntdll.dll and kernel32.dll, PE32
  fixtures/      hello, initorder (+ alpha.dll, beta.dll), missing
  harness/       win32-harness: the loader and gate on Linux x86-64
  host/          winhost.sxe: the loader and gate on SavanXP
  tests/         win32test.sxe: the win32-hello smoke runner
  Win32.cmake
```

On the disk image, under `/system/win/drive_c` (the guest's `C:`):

```text
windows/system32/   ntdll.dll, kernel32.dll
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
  2 has fourteen: version, exit, standard handle, write, debug output; the
  clock; a new region for the heap; find, load and resolve a module; and
  open, close, query and set a registry key or value.
  `loader/services.c` answers them for both hosts.
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
`0x7C800000`, `msvcrt.dll` at `0x77C10000`, `advapi32.dll` at `0x77DD0000`),
so none of them relocates. `include/windows.h` is the subsystem's own header;
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
  blocks, free neighbours merged. `LocalFree` of a pointer that is not a live
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

A different compiler version may change the bytes, not the behaviour; when
a fixture changes, it is run on Windows XP again and this table follows. The
files went to the machine on a USB disk image the batch file wrote its
output and `%errorlevel%` back to. The first run showed the one difference
the harness had: Windows XP detaches on exit, which `RtlExitUserProcess` now
does. `startup.exe` was run the same way; the batch file puts its redirection
first (`>s1.out startup.exe ...`), because `cmd` keeps the blank before a
trailing `> file` in the program's command line.

## The host harness — `subsystems/win32/harness/`

`savanxp-win32-harness [--dlls DIR] [--registry FILE] [--time SECONDS]
program.exe [arguments...]` runs a fixture on the
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
fixes where the guest's clock starts, so a program that seeds `srand` from
`time` repeats its run; the clock still advances from there. DLLs are looked up next to the executable, then in `DIR`, by
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

`winhost` itself is linked at `0x400000` and ends near `0x470000`, well
below `0x01000000`, where `sol.exe` must load. A fixture that prefers
`0x400000` is relocated.

**Views.** The kernel gives a process 64 section views. `hello.exe` uses 14:
three images of about five protection runs each (headers, code, read-only
data, data, relocations), the stack, the TEB block and the trampoline.
`initorder.exe` uses 29. Solitaire's ten images would need about 50 before
W3 adds two per window surface; [the roadmap](../roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md#w1--the-loader-the-gate-and-the-first-executable)
records the options.

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
release every mapping and file.

`win32-pe-fixtures-test` reads the user's Windows XP `sol.exe` and `cards.dll`
from the folder named by `SAVANXP_WIN32_FIXTURES`, as an environment or CMake
cache variable, and verifies the W0 hashes. It then checks what the profile
records: bases, sizes, entry points, `sol.exe`'s 122 imports from eight DLLs,
its refused move, `cards.dll`'s six exports by name and ordinal, its zero-filled
`.data` and a relocation moved by the right delta. Without the folder it is
reported as skipped. The files never enter the tree or the build.
