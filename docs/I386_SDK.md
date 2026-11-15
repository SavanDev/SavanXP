# Native-width integers in the i386 SDK

This is the SDK prerequisite between memory/ELF32 milestone 3 and the graphics
port in milestone 4. It corrects inherited LP64 assumptions in integer limits,
conversion functions and integer formatted I/O. The branch still targets the
pinned v0.4.0 behavior; it does not turn every native type into a 64-bit type.

## The contract

| Type or API | i386 representation |
| --- | --- |
| `int`, `long`, pointers, `size_t`, `ptrdiff_t` | 32 bits |
| `long long`, `int64_t`, `uint64_t`, `intmax_t`, `uintmax_t` | 64 bits |
| `imaxdiv_t` | two native `intmax_t` fields, 16 bytes, remainder at offset 8 |
| `LONG_MIN` / `LONG_MAX` | -2147483648 / 2147483647 |
| `ULONG_MAX` | 4294967295 |
| `off_t`, time and clock types | their existing native definitions; no large-file/time ABI extension |

The limits header derives `long` bounds from the compiler target rather than
copying a 64-bit literal. `<stdint.h>` remains the compiler-provided header.
`<inttypes.h>` uses Clang's format definitions for the actual fixed, least, fast,
maximum and pointer integer families. These macros are strings, not a way of
changing the argument's type. For example, i386 `PRId64` is `lld`, while
`PRIdPTR` is `d`; scanning requires the corresponding destination size too.
Clang is already the SDK's documented compiler.

The runtime includes the public `imaxdiv_t` definition and uses `intmax_t` in
both declarations and implementations. The previous private duplicate used
`long`: on i386 it returned an 8-byte struct where the caller expected 16 bytes,
with incompatible argument widths as well. Keeping that duplicate and silencing
alias warnings would preserve an ABI error. Using the public type removes it.

## Parsing without narrowing

`strtol`, `strtoul`, `strtoll`, `strtoull`, `strtoimax` and `strtoumax` share a
`uintmax_t` accumulator with a limit supplied by the requested result type.
A wide accumulator alone is insufficient: a 32-bit `long` must still saturate at
its own boundary, and a negative signed value may have one extra magnitude bit.

The parser computes a cutoff and last-digit limit before accumulating. Overflow
saturates and sets `ERANGE`, but continues consuming valid digits so `endptr`
still identifies the suffix. Signed minimum values are formed without negating
an unrepresentable positive signed number. Unsigned negation wraps at the
requested width only when the magnitude itself did not overflow.

The reference ASCII/base model remains: whitespace and an optional sign, bases
2 through 36, and decimal/octal/hexadecimal detection for base zero. Zero remains
a digit; the old octal-prefix code skipped it and could report no conversion for
`"0"`. A hexadecimal prefix requires a following hex digit, so `"0x"` converts
its leading zero and leaves the `x`. Invalid bases return zero with `EINVAL` and
an unchanged input position. The SDK retains its existing `EINVAL` convention
for no digits; successful conversions leave errno unchanged. `atoll` now uses
the wide conversion rather than casting a truncated `long` afterward.

## Formatting and scanning

The existing formatter keeps its emission callbacks, width/precision handling
and standard/SDK entry points. Integer argument extraction now distinguishes
`hh`, `h`, `l`, `ll`, `j`, `z` and `t`: `ll` reads a real `long long`, `j` an
`intmax_t`, and size/difference lengths use the compiler's corresponding native
types. Small integers are read after default promotions and then narrowed.
Unsigned decimal, hexadecimal and octal share this dispatch. Octal and integer
`%n` complete the integer-format surface needed by these macros.

The scanner parses wide values before storing them in the type requested by the
length modifier. `j` no longer shares the size_t destination path. Signed size
and pointer-difference destinations stay 32 bits; `long long` and `intmax_t`
destinations receive eight bytes. `%p` stores through a pointer destination.
As in the reference, the public `printf`/`snprintf`/SDK logging entry points use
one formatter, and `sscanf`/`vsscanf` use one scanner.

The relevant language contracts are C11 draft N1570 sections 7.8, 7.21.6 and
7.22.1.4 ([committee draft](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf),
[searchable copy](https://www.sigbus.info/n1570)). This port does not claim full
C/POSIX formatted-I/O conformance: the reference's other flags, floating-point,
locale and stream limitations remain. Scanning a value outside the destination
type's representable range is not a supported range-validation mechanism; use a
`strto*` function and check its range/error when that matters.

## Verification

```sh
./build.sh build
./build.sh smoke shared
./build.sh smoke shared-memory
./build.sh smoke
./build.sh test
python3 tools/verify_posix_persistence.py
./tools/verify_doom_persistence.sh
```

The shared-library scenarios and full smoke run two new clients:

- `sdkint32` uses the SDK static runtime and only public standard headers.
- `sdkintpie` imports `libsdkint.so.0.4`. The library runs the same test matrix,
  calling the executable's exported PIC runtime, and returns 64-bit values and
  an `imaxdiv_t` across real PLT calls. The loader must identify those functions
  as shared; a static substitute cannot satisfy the test.

Compile-time assertions check native sizes, bounds and struct layout. Runtime
vectors cover signed/unsigned extrema and overflow, every base from 2 to 36,
signs/prefixes/zero, invalid bases, no conversion, errno and exact suffix
positions. Mixed varargs include an ordinary integer between two 64-bit values,
so consuming only half an argument shifts later fields and fails the test.
Formatting/scanning tests cover integer families, octal/hex, field width,
precision, suppression, `%n`, pointer conversion and truncated snprintf output.
Each round-trip compares against the original input and checks adjacent guard
bytes, catching both silent truncation and an oversized destination write.

The tests do not certify the entire SDK or the full graphical session. The
remaining graphics, signals, object services and device work stays in the port
roadmap. No kernel calling convention, filesystem format or disk reset is needed
for these corrections; statically linked external apps gain them when rebuilt.
The persistence regression keeps already-installed external binaries and assets.
