# Win32 subsystem

> **Status: W1 in progress.** This document describes what exists under
> `subsystems/win32/`. The plan, the phases and the reasons behind them are in
> the [Win32 roadmap](../roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md); the API
> surface of the first profile is in
> [WIN32_SOLITAIRE_PROFILE.md](WIN32_SOLITAIRE_PROFILE.md).

So far the subsystem is one build-host library, the PE32 parser and mapper.
Nothing of it is installed in the image yet.

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

`win32-pe-fixtures-test` reads the user's Windows XP `sol.exe` and `cards.dll`
from the folder named by `SAVANXP_WIN32_FIXTURES`, as an environment or CMake
cache variable, and verifies the W0 hashes. It then checks what the profile
records: bases, sizes, entry points, `sol.exe`'s 122 imports from eight DLLs,
its refused move, `cards.dll`'s six exports by name and ordinal, its zero-filled
`.data` and a relocation moved by the right delta. Without the folder it is
reported as skipped. The files never enter the tree or the build.
