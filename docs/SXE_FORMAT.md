# The SXE format — executables carrying their own resources

> **Status: all five phases COMPLETE, on master.** The canonical format lives in
> [include/sxe/sxe_format.h](../include/sxe/sxe_format.h), the SDK reader in
> [savanxp/sxe.h](../subsystems/posix/sdk/v1/include/savanxp/sxe.h) +
> [runtime/sxe.c](../subsystems/posix/sdk/v1/runtime/sxe.c), the stamping in
> [gen_sxe_resources.py](../tools/gen_sxe_resources.py) + `Add-SxeResources`,
> and the consumers are `progman_registry_apply_sxe()` (launcher),
> `windowd_presentation_load()` (window chrome and Task List) and `file_assoc`
> (file associations in filesapp). Validated by `sxe-smoke`, `progman-smoke`,
> `windowd-smoke` and `filesapp-smoke`.
>
> **Stamping by default: every program the build links comes out with
> `.sxmeta`, whether or not it has a `.sxres`.** It is no longer opt-in.
> `sxe-smoke` measures the whole image: **67/67 installed binaries stamped**,
> in-tree and external (Doom, busybox) alike. See
> [below](#default-stamping).
>
> **`icon=` in `progman.ini` points at a program, not at a catalog.** `icon=`
> resolves to a *path* (`icon=/bin/aboutapp` borrows that icon) instead of a
> baked id, with legacy aliases for the short names of before. See "`icon=` no
> longer picks from a catalog: it points at a program", below.
>
> **`desktop_icons.h` narrowed all the way down: a single baked id.** Doom,
> Shell, Notepad, Gfx Demo, Key Test and Mouse Test — the six the set came to
> hold — left one by one; only `DESKTOP_ICON_DESKTOP` remains, the universal
> safety net for when a binary cannot be read. See the callout in "`icon=` no
> longer picks from a catalog", below.
>
> Known open items, noted in place: **MIME resolution** (it needs a type
> detection layer that does not exist), the **cost of the association scan** —
> already measured — and the **rename to `.sxe`**, which was never done and
> which lost the argument in its favor once stamping became universal (see that
> section).
>
> Where this document and `sxe_format.h` disagree, **the header wins**.
>
> **Crux decision:** SXE is **not a container**. It is a convention over ELF:
> the file still starts with `7F 45 4C 46`, the same
> [kernel/elf.cpp](../kernel/elf.cpp) loads it without touching a line, and
> `llvm-readelf` opens it. All it adds is two **non-alloc** sections the kernel
> never maps. "SXE" names the convention and the extension, not a new binary
> format.

## Motivation

Today an executable cannot speak about itself, and that is why three tables
exist to compensate:

1. [progman_registry.h:33](../subsystems/posix/userland/progman_registry.h:33) —
   icons are referenced **by name** against a set baked at build time. Its own
   comment says: *"per-program icons would need an icon format + loader:
   separate work"*. This document is that work.
2. [windowd_appinfo.h:16](../subsystems/posix/userland/windowd_appinfo.h:16) —
   the WM **guesses** title/icon/accent from the path, and admits that *"the
   real fix is for each client to report its own title/icon"*.
3. `desktop_icons.h` — application icons baked inside the WM binary: adding a
   program to the menu requires **recompiling the system**.

With resources inside the executable itself, all three tables fall away and a
capability that does not exist today appears: copying a `.sxe` to `/disk/bin`
**is** installing a program.

## Why ELF sections and not a container of our own

- **The kernel loader is not duplicated.** `kernel/elf.cpp` is small and
  tested. A new format means a second parser on the most critical path of the
  system, or an unwrapper: double the bugs and attack surface in exchange for
  nothing.
- **Resources cost no RAM.** Not being `SHF_ALLOC`, they enter no `PT_LOAD`:
  the kernel does not see them, does not map them, and the process does not pay
  for them. With this system's memory budget (~133 MiB usable out of 256, arena
  in BSS mapped eagerly), putting tens of KiB of icons into a loadable segment
  **per process** would be an expensive mistake.
- **Tooling survives.** `readelf`, `objdump`, `nm` and gdb keep working.
- **Stamping is already possible with what is baked**:
  `toolchain/llvm/bin/llvm-objcopy.exe` is in the toolchain. Zero new tools.

Using standard `SHT_NOTE` (name/desc/type) was considered and dropped: our own
TLV is simpler to parse without malloc and gains nothing from sharing the note
mechanism.

## Anatomy of the file

```
notepad.sxe
├─ ELF header                        ← intact; EI_OSABI already carries the subsystem
├─ Program headers
├─ .text / .rodata / .data / .bss    → PT_LOAD: this gets mapped
├─ .sxmeta   (NOT alloc, ≤ 4 KiB)    → identity: name, version, mimes...
└─ .sxicon   (NOT alloc, ≤ 64 KiB)   → the icon pixels
```

**Invariant to verify at build time:** neither `.sxmeta` nor `.sxicon` may
carry the `A` flag in `llvm-readelf -S`. If they do, they get mapped, and the
whole memory argument collapses silently.

**Two sections and not one** because they have different read patterns:
`.sxmeta` is hundreds of bytes and is read whole; `.sxicon` is kilobytes and is
read only when something has to be painted. A file listing that only needs
names should not drag pixels along.

### Common conventions

- Little-endian, x86-64. No implicit padding: every field naturally aligned.
- Strings in UTF-8. **`length` rules**: no NUL terminator is guaranteed, and
  the reader must tolerate one being there.
- No malloc in userland: readers copy into fixed buffers and **truncate**.

## The `.sxmeta` section

### Header (16 bytes)

```c
struct sxe_meta_header {
    uint8_t  magic[4];      /* 'S','X','M','E' */
    uint16_t version;       /* 1 */
    uint16_t header_bytes;  /* sizeof(header); allows growth without breakage */
    uint32_t blob_bytes;    /* total blob size, header included */
    uint32_t record_count;
};
```

`header_bytes` exists so that a v2 can add fields to the header: a v1 reader
skips `header_bytes` instead of assuming `sizeof`.

### TLV record

```c
struct sxe_record {
    uint16_t tag;       /* see table */
    uint16_t flags;     /* SXE_RECORD_* */
    uint32_t length;    /* payload bytes, not counting padding */
    /* uint8_t payload[length]; */
    /* zero padding up to a multiple of 4 */
};
```

| flag | value | meaning |
|---|---|---|
| `SXE_RECORD_REQUIRED` | `0x0001` | If the reader does not know the `tag`, it **must discard the whole blob** and fall back to the defaults |

The general rule is "unknown tag is ignored" — that is how fields get added
without breaking old binaries. `REQUIRED` is the escape valve for the day
something is added that changes the meaning of the rest. No v1 tag uses it; it
is defined now because afterwards would be too late.

### Tag space

| range | use |
|---|---|
| `0x0001`–`0x00FF` | Identity |
| `0x0100`–`0x01FF` | Presentation |
| `0x0200`–`0x02FF` | Execution |
| `0x0300`–`0x03FF` | Capabilities |
| `0x0400`–`0x7FFF` | Reserved for the system |
| `0x8000`–`0xFFFF` | Private / experimental — the system never defines these |

### v1 tags

| tag | name | payload | notes |
|---|---|---|---|
| `0x0001` | `NAME` | utf8 | Display name. Without it the program has no identity: the reader falls back to the basename. Recommended ≤ 31 bytes (`PROGMAN_NAME_CAPACITY`) |
| `0x0002` | `DESCRIPTION` | utf8 | Recommended ≤ 63 bytes (`PROGMAN_DESC_CAPACITY`) |
| `0x0003` | `VERSION` | `uint16[4]` | major, minor, patch, build. Binary and **comparable** |
| `0x0004` | `VERSION_STRING` | utf8 | What gets displayed ("1.2.0-rc3"). Separate from `VERSION` for the same reason Windows separates `FILEVERSION` from `StringFileInfo`: sorting and displaying are different things |
| `0x0005` | `VENDOR` | utf8 | |
| `0x0006` | `COPYRIGHT` | utf8 | |
| `0x0007` | `BUILD_ID` | utf8 | Short git commit. **Always** set by the generator ([default stamping](#default-stamping)); a `.sxres` can pin another value by hand if needed |
| `0x0101` | `ACCENT` | `uint32` | `0x00RRGGBB`, the format `gfx_rgb` already returns. Replaces the `accent` field of `windowd_appinfo` |
| `0x0102` | `LAUNCH_FLAGS` | `uint32` | `SAVANXP_DESKTOP_LAUNCH_FLAG_*` **by default**. The launcher can override them |
| `0x0201` | `INTERPRETER` | utf8 | Absolute path of the program that executes this image. **Absent or empty = the kernel executes it directly** |
| `0x0202` | `SUBSYSTEM` | `uint8` | An **informational** mirror of `EI_OSABI`. The authority is still the ELF byte ([elf.hpp:14](../include/kernel/elf.hpp:14)); this exists so a userland reader does not have to parse the ELF header |
| `0x0301` | `MIME_OPEN` | utf8, NUL-separated entries | Types the program **declares it can** open |
| `0x0302` | `EXT_OPEN` | utf8, NUL-separated entries | Extensions, with the dot: `.txt` |

**`INTERPRETER` is in from v1 even though there is no VM yet.** It is the field
that resolves the case that is coming: when a Haxe app is HashLink bytecode and
not an x86-64 ELF, the launcher has to know the image is executed not by the
kernel but by `/bin/hlvm`. It is the equivalent of a shebang /
`binfmt_misc`, and it does not fit in the `EI_OSABI` byte.

**`MIME_OPEN` declares capability, not association.** A program saying it can
open `text/plain` does not make it the one that opens `.txt` files: that is
user policy, and it is resolved in the registry (see below).

### Size cap

`.sxmeta` must not exceed **4 KiB**. The reader uses a fixed buffer of that
size and **rejects** the blob if `blob_bytes` exceeds it — without malloc there
is no other honest option, and 4 KiB is plenty for text.

## The `.sxicon` section

```c
struct sxe_icon_header {
    uint8_t  magic[4];      /* 'S','X','I','C' */
    uint16_t version;       /* 1 */
    uint16_t header_bytes;
    uint32_t blob_bytes;
    uint32_t image_count;
    /* struct sxe_icon_entry entries[image_count]; */
    /* pixels */
};

struct sxe_icon_entry {
    uint16_t width;
    uint16_t height;
    uint32_t format;    /* SXE_ICON_FORMAT_* */
    uint32_t offset;    /* from the start of the blob */
    uint32_t length;    /* = width * height * 4 in BGRA8888 */
};
```

| format | value | description |
|---|---|---|
| `SXE_ICON_FORMAT_BGRA8888` | `2` | `uint32` per pixel, `0xAARRGGBB` (in memory: B,G,R,A), rows top to bottom, no row padding |

The **value 2 is not arbitrary**: it deliberately matches
`SX_PIXEL_FORMAT_BGRA8888` from
[gfx2d.h:18](../subsystems/posix/sdk/v1/include/savanxp/gfx2d.h:18), so the
pixels of a `.sxicon` can be handed to a `struct sx_bitmap` without translating
anything. `sxe.c` has a `_Static_assert` that breaks the build if someone
renumbers the gfx2d formats — the alternative failure mode would be swapped
colors at runtime.

That is **exactly** what
[gen_desktop_icon_assets.py](../tools/gen_desktop_icon_assets.py) already
produces (`(a << 24) | (r << 16) | (g << 8) | b`) and what
`struct desktop_embedded_bitmap` consumes. The blob's pixels are handed to the
existing blitter **without conversion**.

**Sizes:** 16×16 and 32×32 are the ones the system uses today
(`desktop_icon_small` / `desktop_icon_large`) and every `.sxe` should carry
both. Other sizes (48×48) are valid and optional. Reader selection rule: exact
match, otherwise the smallest one ≥ the request, otherwise the largest
available.

**Cap:** 64 KiB. With 16+32+48 in BGRA8888 about 16 KiB is used, so there is
plenty of headroom.

## Backward compatibility

Windows EXE style: **an executable without resources is a first-class
executable, forever.**

- An ELF without `.sxmeta` launches all the same. With [default
  stamping](#default-stamping) that no longer describes any binary coming out
  of *this* build, but it is still the real contract: an executable copied from
  elsewhere, built with another toolchain, or brought in by a third party must
  never fail to start for lacking a section the kernel does not even look at.
  There will never be a "SXE only" `/disk/bin`.
- **The kernel takes no part in any of this.** It does not read, does not
  validate, does not care. The whole mechanism lives in userland.
- A `.sxmeta` that is corrupt, truncated, from a future version, or carrying an
  unknown `REQUIRED` is treated **as absent**. It never prevents launching.
- Defaults when there is no metadata:

  | datum | fallback |
  |---|---|
  | name | basename of the path |
  | icon | generic from the system set |
  | accent | `gfx_rgb(59, 95, 156)` — the one [windowd_render.c:356](../subsystems/posix/userland/windowd_render.c:356) already uses |
  | flags | `SAVANXP_DESKTOP_LAUNCH_FLAG_NONE` |

### The extension is a hint, not the authority

`.sxe` signals "there are probably resources here"; `.elf` signals "do not
look". That saves the launcher from opening N files per scan, which is real I/O
over SxFS.

But **the blob is the source of truth**. A `.sxe` may have no valid `.sxmeta`
(old build, truncated file, someone renamed it) and an `.elf` may have one. If
the reader treats the extension as a guarantee, that case breaks it. As a hint,
the fallback is the same one above: silent and already written.

**Default stamping pulls the floor out from under the rename.** The extension
hint was worth something for *pruning* a scan — skipping, without opening, the
files that almost certainly have no resources. With `.sxmeta` on 100% of the
binaries there is nothing left to prune: every file you opened would have
resources, so `.sxe` would stop meaning "open this" and `.elf` would stop
meaning "skip this" — they would be the same thing under two names.
`sxe_path_has_extension()` is still alive in the reader because the
backward-compatible contract requires it (a third party can still use it as a
file convention), but it is no longer the lever for `file_assoc`'s cost. That
lever has to be found somewhere else — an index, or bounding which directories
get scanned — not in the name.

## No cache — and now with the measurement

There is no persistent index. The decision was to **measure first**, because a
cache invalidated by mtime is the kind of thing that adds hard-to-see bugs.

There are numbers now, and they are of two very different orders:

| consumer | what it opens | cost |
|---|---|---|
| progman | the binaries the catalog lists | ~9 files, imperceptible |
| windowd | the binary of each window it creates | 1 per window, imperceptible |
| **`file_assoc`** | **every installed executable** | **141 files, ~220 ms** (TCG) |

The first two are bounded by something small and need nothing. The third does
hurt, and that is why filesapp does it **lazily**: a session that only browses
directories pays nothing, and the cost is charged once, when a file actually
has to be opened.

**The 141 has a concrete reading, and it changed with default stamping.**
`/disk/bin` is a copy of `/bin`, so every program is examined twice — that did
not move. What did move is the other half of the argument: with the previous
scan, "of the 131, only a handful carry resources" suggested the extension hint
could prune away nearly all the cost. Now **every** binary has `.sxmeta` —
opening them is no longer avoidable by name, because they all "have something"
even though almost none declares `EXT_OPEN`. The extension hint stopped being a
cost lever (see the note in the previous section); if this number ever really
hurts, the way out is an index or bounding which directories `file_assoc`
scans, not renaming binaries.

`file_assoc_scan_examined()` and the `ms=` of `filesapp-smoke` exist so this
can be measured again when something changes.

## Who reads what

```
  .sxe on disk
      │
      │ shared SDK reader (savanxp/sxe.h) — no malloc, fixed buffers
      ▼
  progman ──► paints the launcher (name, icon, description)
      │
      │ fd 9 (SAVANXP_WM_FD_LAUNCH): path + flags, exactly as today
      ▼
  windowd ──► re-reads the .sxe at that path when creating the window
              title, window icon, accent, Task List
```

The WM stops guessing from the path and starts reading; `windowd_appinfo.c`
remains as a fallback for orphans, or disappears.

This does **not** contradict the decision the WM already made in
[wm_protocol.h:64](../subsystems/posix/sdk/v1/include/savanxp/wm_protocol.h:64)
(*"the requester declares the launch flags: the WM knows no app catalog"*).
What was forbidden there is the WM having a **catalog**: a table of known apps
maintained by hand. Reading the self-description of the binary it was just
asked to launch is exactly the opposite of a catalog — no table, no prior
knowledge, and a program the WM has never seen introduces itself.

`desktop_icons.h` **does not die: it narrows.** Folders, the generic file,
dialog icons and WM chrome belong to no app and stay baked. What leaves the set
are the *application* icons, which is what should never have been there.

**The first to go was Doom.** `DESKTOP_ICON_DOOM` / `app-spider.png` no longer
exist: the art moved, pixel by pixel, to
[sdk/doomgeneric/icon.png](../sdk/doomgeneric/icon.png), and
`doomgeneric.sxres` declares it with `icon_file=` instead of `icon=`. It is the
same logic that already governed `ports/ccleste` — the icon travels *inside*
the executable, not in the system tree — applied for the first time to a
program that **is** versioned in the repository. The `windowd_appinfo` fallback
table and the `progman_registry` baked default now point at
`DESKTOP_ICON_DESKTOP` (generic): they only come into play if the Doom binary
cannot be read at all, which is exactly what a fallback should cover.

### `icon=` no longer picks from a catalog: it points at a program

This was the blocker noted above, and it no longer is. `icon=` in an `[item]`
of `progman.ini` stops resolving against a baked id (`icon_id_from_name()`,
which no longer exists) and instead stores a **path** —
`progman_item.icon_borrow_path` — that `progman_registry_apply_sxe()` tries to
read with the same mechanism it already uses for the item's own icon, only
pointed at *another* binary:

```ini
[item]
name=My Notepad
path=/bin/notepad
icon=/bin/aboutapp
```

That item launches Notepad but shows `aboutapp`'s icon — the real use case is a
second entry for the same executable (different argument, different name) that
still wants to look distinct. Three shapes of the value:

| value | resolves to |
|---|---|
| `/bin/aboutapp` (starts with `/`) | that path as-is — the main form |
| `shell`, `notepad`, `gfxdemo`, `keytest`, `mousetest` | legacy alias: the path of the program that draws that icon today (`/bin/shellapp`, ...) |
| `desktop`, or any name without an alias | nothing — `icon_borrow_path` stays empty and the item keeps the generic `icon_id` |

**The aliases are a bridge, not the new mechanism.** Before this change,
`icon=shell` picked a baked pixel array because there was no other way for
Shell to have an icon. Today `/bin/shellapp` already carries its own `.sxicon`
(stamped from `icon=app-terminal` in `shellapp.sxres`), so the alias resolves to
the *same* path the item would get anyway with no `icon=` at all — it is purely
backward compatibility for a `.ini` written before this redesign, not something
a new `.ini` needs to write.

**An explicit override beats the binary, even when the binary has its own
icon.** `icon=desktop` on an item that launches `/bin/notepad` forces the
generic one and does **not** try to read Notepad's real `.sxicon` — the user
explicitly said "no", and that outweighs whatever the executable declares. It
is the same hierarchy as always (`.ini` > `.sxmeta` > default > generic),
applied inside the `icon=` field too.

`ports/ccleste/progman.ini` had an `icon=doom` that this change left silently
resolving to the generic instead of Doom's real icon — that line was removed,
with the same comment the Celeste item already carried ("no `icon=` on purpose:
the binary already brings its own").

> **Narrowing finished.** `desktop_icons.h` is down to a single value:
> `DESKTOP_ICON_DESKTOP`. Shell, Notepad, Gfx Demo, Key Test and Mouse Test
> left through the same operation that removed Doom, repeated five times — one
> row of `progman_registry`'s `k_default_items` and one of `windowd_appinfo`'s
> `k_window_items` per program, all ten now pointing at the generic. The only
> baked id left is the universal safety net: what gets shown when a binary
> cannot be read at all, not one option among several.
>
> The legacy aliases (`shell`, `notepad`, `gfxdemo`, `keytest`, `mousetest`)
> were not touched and **did not depend on this table to begin with** — they
> resolve to *paths* (`/bin/shellapp`, ...), never to a `desktop_icons.h` id.
> That is why emptying the enum breaks nothing for them: an old `.ini` with
> `icon=shell` still shows Shell's real icon, now read from its `.sxicon`
> through the same path as if the `.ini` had said nothing.
>
> The five source PNGs (`app-terminal.png`, `app-libgfx-demo.png`,
> `app-keyboard-settings.png`, `app-mouse.png`, `app-notepad.png`) **are still
> in `assets/desktop/icons/`** — unlike Doom's, they did not move anywhere.
> They remain the source each `.sxres` references with `icon=<name>`
> (`shellapp.sxres` says `icon=app-terminal`, etc.) and
> `tools/gen_desktop_source_art.py` still regenerates them on every build. The
> only thing retired was the step that *also* baked them into a second C array
> inside `desktop_icons.c` — `tools/gen_desktop_icon_assets.py` no longer lists
> them. Two catalogs that shared PNGs by coincidence, not one catalog with two
> names.

### Decision: the WM reads the resources, they do not travel over the protocol

**The WM opens the `.sxe` at the path it launched and reads `.sxmeta` +
`.sxicon` once, when creating the window.**
`savanxp_desktop_launch_request` does not grow.

The alternative was sending the presentation inline in the request. It was
dropped:

- `savanxp_desktop_launch_request` is 388 bytes today (flags + path +
  argument). A 16×16 icon in BGRA8888 is another 1 KiB — the message grows ~4×
  and enters **partial pipe read** territory, a class of bug that has bitten
  this system before and is not worth reopening over an icon.
- There would be **two sources** for the same datum (what progman parsed and
  what the binary says), and therefore a way for them to drift apart.

With the WM reading, the cost is **bounded by the number of open windows** — a
handful — and not by the size of a directory, which was the only weighty reason
not to do I/O here. And since the WM already has the path and is going to open
the file anyway, reading the ~4 KiB of `.sxmeta` in the same pass is
essentially free: same `open`, same inode. That is why it reads **both**
sections and not just the icon.

Consequences:

- **Zero protocol changes.** `savanxp_desktop_launch_request` stays exactly as
  it is.
- It is read **once** per window and kept in the client's session structure; the
  rest of the window's life touches no disk.
- If the file does not open, has no `.sxmeta`, or the blob is invalid: fall back
  to the defaults in the table above. **It never blocks window creation.**

The honest downside being accepted is disk I/O in the WM loop, which is
latency-sensitive. It is paid at window creation time, already the most
expensive moment of the cycle, so it is the right place. If it is ever
noticeable, the way out is painting the generic icon on the first frame and
completing later — **not** moving pixels into the protocol.

## Division of responsibilities with `progman.ini`

The registry does not disappear: **it changes role**, and the Windows model
remains.

| | `.sxmeta` | `progman.ini` |
|---|---|---|
| what it is | the program's identity | the user's arrangement |
| what it holds | name, version, icon, accent, default flags, declared mimes | which groups exist, what goes in them, in what order, targeted overrides |
| who sets it | whoever compiles | whoever uses |

It stops being a catalog with hardcoded icons and becomes what a start menu
really is. And it resolves the mime question: the binary **declares
capability**, the registry **resolves the association**.

### File associations (phase 5)

The association does not live in `progman.ini` but in its own registry,
`/disk/assoc.ini`, because they are different things: one is the start menu
arrangement, the other is system-wide policy. Minimal format, one line per
association:

```ini
# /disk/assoc.ini — who opens each extension
.txt=/bin/notepad
.log=/bin/shellapp
```

Precedence: **user policy > first binary declaring the extension > nothing**.
The third case is not an error: filesapp falls back to its default editor, and
another caller can decide otherwise.

"First binary declaring it" is not theoretical: `/disk/bin` is a copy of
`/bin`, so **every program shows up twice in the scan**. Without the rule that
one scan entry does not overwrite another, the second pass rewrote every
association to the `/disk/bin` path. A stable directory order is more
predictable than any tie-breaking heuristic.

### Precedence (implemented in phase 3)

Highest to lowest, field by field:

1. **The key written in the `.ini`** — what the user decided
2. **The binary's `.sxmeta`/`.sxicon`** — what the program declares about
   itself
3. **The baked default** — a safety net for what carries no resources
4. **Generic** — basename and desktop icon

Step 1 needs to distinguish *"the user chose this name"* from *"the default
value stayed"*, and that cannot be deduced from the value: that is why `struct
progman_item` carries an `overrides` mask that the parser sets for each key
present. Without it, the `.sxe` would overwrite user decisions or the other way
round, depending on how the steps were ordered.

`progman_registry_apply_sxe()` runs **after** pruning: there is no point
opening the binary of an item that is going to be discarded, and pruning
reorders the items — which would invalidate the icon slots already assigned.

The icons that are read are copied into a fixed pool of `PROGMAN_MAX_ITEMS`
32×32 slots (**192 KiB of BSS**). It is a conscious choice: without malloc you
have to reserve the worst case, and the alternative — re-reading the `.sxicon`
while painting — would put disk I/O inside the repaint cycle.

Step 1 over the `icon=` field in particular — including the way it can point at
*another* program's `.sxicon` — is described below, in the section "`icon=` no
longer picks from a catalog: it points at a program".

## Build integration

### The `.sxres` manifest

Each program declares its resources in a `<name>.sxres` **next to its source**.
The convention is all you need to know: if the file exists it gets stamped, and
if it does not the binary comes out exactly as before. Nothing to register in
`build.ps1`.

```ini
# subsystems/posix/userland/notepad.sxres
name=Notepad
description=Edit text files
version=system
vendor=SavanXP
accent=78643c
icon=app-notepad
mime_open=text/plain
ext_open=.txt,.ini,.cfg,.md
```

| key | value |
|---|---|
| `name`, `description`, `vendor`, `copyright`, `build_id`, `version_string`, `interpreter` | text, as-is |
| `version` | `1.2.3`, up to 4 components — or `system`, which resolves it against `include/shared/version.h` |
| `accent` | `RRGGBB` in hex (accepts a leading `#` or `0x`) |
| `launch_flags` | comma-separated list; the names come from `SAVANXP_DESKTOP_LAUNCH_FLAG_*` |
| `subsystem` | `posix` or `native` |
| `icon` | asset name under `assets/desktop/icons/{16x16,32x32}/<icon>.png` — the same way `progman.ini` references it today |
| `icon_file` | the program's own PNG, resolved **relative to the `.sxres`**. The two sizes the runtime requires are derived from that file |
| `mime_open`, `ext_open` | comma-separated lists |

**`icon` and `icon_file` are mutually exclusive, and the difference matters.**
`icon` references the system catalog: it is for the programs shipped with
SavanXP that share its look. `icon_file` takes a PNG living next to the
manifest, and it is the right one when the program brings its own art — above
all a third-party port, whose icon has no business entering
`assets/desktop/icons/`, which is versioned and baked into the image. With
`icon_file` the art ends up **only inside the executable**, which is exactly
what this format exists for.

`icon_file` accepts **a single PNG** of any size and derives the two that are
needed. If it is an integer multiple of the target in either direction —
shrinking (48→16) or enlarging (16→32, the most common case: a pixel-art icon
is usually drawn once, at the small size) — it uses `NEAREST` so as not to
touch a single pixel of the original. If there is no clean multiple, it uses
`LANCZOS`. The enlarging branch was missing until Doom's icon exercised it for
the first time: a 16×16 PNG reached `LANCZOS` for the 32×32 and came out blurry
instead of crisp — the bug was found by comparing the stamped blob, byte by
byte, against the original.

**`version=system` exists so that system programs do not go stale.**
Hardcoding `0.3.3` in nine manifests would be the same duplication this design
avoids everywhere else.

### Default stamping

When phase 2 was implemented, a `.sxres` was the **condition** for a binary to
receive sections: with no manifest, `Add-SxeResources` did not touch the file.
With 67 programs in the tree and 9 manifests written, that means "every program
compiled is already in SXE format" was not true — the default had to be
inverted.

Now **the generator always stamps**. Besides the directories to search for
`.sxres`, `gen_sxe_resources.py` receives the full list of programs the build
is going to link (`--program`, repeatable) — the same list, filtered by
`-NoTestApps`, that the compilation phase already uses. Every name on that list
gets a `.sxmeta`, manifest or not:

| tag | where it comes from without a `.sxres` |
|---|---|
| `NAME` | the program's own name |
| `VERSION` / `VERSION_STRING` | the system version (`include/shared/version.h`) |
| `SUBSYSTEM` | `posix` — the only subsystem going through this generator today |
| `BUILD_ID` | the short git commit, resolved once per build and cached |

It is the same role played by the `VERSIONINFO` block the Windows *linker* adds
even when the programmer never wrote a `.rc`: minimum identity coming from the
build, not from the programmer. And it is not a cosmetic default — the
automatic `NAME` is **exactly** the text progman and windowd already showed as
the basename fallback when there was no `.sxmeta`: for a binary without a
`.sxres`, default stamping changes not a single existing window, it just
stopped *inferring* something that is now *declared*.

**What is still not invented**: icon, accent, description, `launch_flags`,
mimes. That is enrichment — nobody can derive it from the build — and
fabricating it would be worse than leaving it absent. The `.sxres` did not lose
its place: it became exactly that, optional enrichment over an identity that
already exists.

Coverage verified with `llvm-readelf` over the complete image: **67/67**
installed binaries with `.sxmeta`, in-tree and external (Doom, busybox) alike —
no build path was left out except the native subsystem
(`subsystems/native/build.ps1`), which is paused and out of scope for now.

### The generator

[tools/gen_sxe_resources.py](../tools/gen_sxe_resources.py) turns the manifests
into blobs. **It does not duplicate a single number**: magics, tags, versions,
sizes and caps come from `include/sxe/sxe_format.h`; the launch flags from
`savanxp/syscall.h`; the native OSABI from `savanxp_native.h`. It is the
criterion of `Assert-SxfsFormatMatchesHeader` — a format copied by hand between
reader and generator drifts apart, and the failure is silent.

Unlike the runtime parser, which ignores what it does not understand so it can
read newer binaries, **the generator is strict** about what does come in a
`.sxres`: an unknown key, a missing icon or a nonexistent flag breaks the
build. A typo in a manifest has to fail, not silently leave the app without an
icon. A `.sxres` whose program is not on the `--program` list breaks nothing —
it may be an app excluded by `-NoTestApps` — but it is reported on the console,
because the most likely error there is a `.c` renamed without renaming its
manifest.

### The stamping

```bash
llvm-objcopy --add-section .sxmeta=app.sxmeta --add-section .sxicon=app.sxicon app app
```

`--add-section` creates sections without `SHF_ALLOC`, which is what we want —
but it is **verified** anyway, reading the raw `sh_flags` value with
`llvm-readelf --section-details` and checking the `0x2` bit. If it ever shows
up, the cost is paid in RAM per process and **with no visible symptom**: that
is exactly the class of regression that needs an automatic guard.

`Add-SxeResources` and `Invoke-SxeResourceGenerator` live in
`tools/UserAppCommon.ps1`, not in `build.ps1`, so that both build paths — the
in-tree one and the external app one (`build-user.ps1`) — stamp with the same
implementation. Duplicating the step would mean the non-alloc check applies to
one and not the other.

> **PowerShell gotcha:** anything a command writes without being captured is
> added to the return value of the enclosing function. A stray `sxe: N
> manifests` turned the path returned by `Build-ExternalUserProgram` into a
> two-element array, and the build broke far away from there (busybox copying
> to a "drive" called `sxe`). That is why the python and objcopy invocations
> capture their output and re-emit it with `Write-Host`.

## Suggested phases

1. ~~**Format + reader.**~~ **DONE.** The canonical format in
   `include/sxe/sxe_format.h` (freestanding, C/C++, with layout static
   asserts), the reader in `savanxp/sxe.h` + `runtime/sxe.c`, and the
   `build.ps1 sxe-smoke` harness (`/disk/bin/sxetest`). Pure parsing is
   exercised against blobs fabricated on the stack — well formed and every
   degraded case no correct generator would produce — and the disk path against
   the image's real binaries.
2. ~~**Stamping in the build.**~~ **DONE.** Per-app `.sxres` manifests, a
   header-driven host-side generator, stamping with `llvm-objcopy` and a
   non-alloc guard by `sh_flags` bit — shared between the in-tree build and the
   external app build. Nine system programs stamped; nobody reads them yet, so
   the only change is binaries growing by ~5 KiB.
3. ~~**progman consumes.**~~ **DONE.** `progman_registry_apply_sxe()` resolves
   name, description, launch flags and icon from each item's binary, respecting
   whatever the `.ini` declared explicitly. **Nothing changes visually**, and
   that is correct: the manifests reproduce the presentation that used to live
   in the tables. What changed is where the data comes from.
4. ~~**windowd consumes.**~~ **DONE.** `windowd_presentation_load()` resolves
   title, 16×16 icon and accent by reading the `.sxe` of the binary it just
   launched, once, in `start_client_process()`. **Zero protocol changes**, as
   decided. `windowd_appinfo` remains as a fallback step.
5. ~~**Associations.**~~ **DONE** for `EXT_OPEN`. `file_assoc` resolves
   extension → program by combining user policy (`/disk/assoc.ini`) with what
   the installed binaries declare, and filesapp opens each file with the
   associated program through the `argument` field that already existed — the
   `#define FILESAPP_EDITOR_PATH "/bin/notepad"` stopped being the only answer
   and stayed as a fallback. **`MIME_OPEN` is still stamped but does not
   resolve yet**: doing so needs a type detection layer (extension → mime, or
   sniffing) the system does not have, and baking a "`.txt` is `text/plain`"
   table would reintroduce exactly the kind of central table this design came to
   remove.

   > **What does exist since then: per-type icons, and it is NOT the same
   > thing.** `subsystems/posix/userland/mime_icon.c` resolves *extension →
   > icon* and filesapp's list draws them. **It does not resolve `MIME_OPEN`,
   > does not detect types and does not touch associations**: a file still has
   > no `text/plain` associated with it, it just has an icon. The catalog's
   > names are those of freedesktop's *Icon Naming Specification*, which is an
   > icon naming convention and not a type table.
   >
   > It meets the constraint above in two ways, and both matter:
   >
   > - **The mapping is not baked.** It lives in `diskfs/mimeicon.ini`, image
   >   data with the same format and precedence as `assoc.ini`. Adding a type is
   >   one line, not a recompile.
   > - **Neither are the pixels.** The catalog is a set of standalone `.sxicon`
   >   blobs in `/disk/icons`, emitted by `tools/gen_mime_icons.py` and read
   >   with `sxe_load_icon_file()` — the variant of `sxe_load_icons()` for a
   >   file that *is* the blob, with no ELF around it. Not a single KiB of icon
   >   enters a loadable segment, which is the same decision that made `.sxicon`
   >   a non-alloc section.
   >
   > When type detection exists, this is the layer it will hang off: the change
   > would be resolving *mime → icon* instead of *extension → icon*, with the
   > same catalog and the same mapping file.

6. ~~**Default stamping.**~~ **DONE.** Phases 1 to 5 left stamping *opt-in*:
   with no `.sxres`, `Add-SxeResources` did not touch the binary. With 67
   programs in the tree and 9 manifests, that was the real gap between "the
   format exists" and *"every program compiled for the OS is already in that
   format"*. Now `gen_sxe_resources.py` receives the build's complete program
   list (`--program`) and stamps a minimal `.sxmeta` — name, version,
   subsystem, commit — for all of them, manifest or not. See [Default
   stamping](#default-stamping). Coverage measured over the complete image:
   **67/67**.
7. ~~**Doom leaves the baked set.**~~ **DONE.** `DESKTOP_ICON_DOOM` /
   `app-spider.png` are retired; the art moves pixel by pixel to
   `sdk/doomgeneric/icon.png`, declared with `icon_file=`. The first program
   *versioned in the repository* to use `icon_file=` (previously only
   `ports/ccleste`, which is gitignored). Along the way a real bug in
   `collect_icons_from_file()` was fixed: it did not detect an integer multiple
   when *enlarging* (only when shrinking), so a 16×16 source — the typical
   pixel-art case — fell to `LANCZOS` instead of `NEAREST` for the 32×32. See
   "The first to go was Doom", above.
8. ~~**`icon=` points at a program, and the set narrows all the way.**~~
   **DONE.** `icon_id_from_name()` is deleted: `icon=` stores a *path*
   (`progman_item.icon_borrow_path`) and `progman_registry_apply_sxe()` reads it
   with the same mechanism it uses for the item's own icon, pointed at another
   binary. With that, the five remaining baked ids
   (`shell`/`notepad`/`gfxdemo`/`keytest`/`mousetest`) lose their only consumer
   and are retired from `desktop_icons.h` just like Doom —
   `DESKTOP_ICON_DESKTOP` is today the enum's only value. See "`icon=` no
   longer picks from a catalog", above.

`INTERPRETER` has no phase: the field has been there since v1 and starts being
honored the day the VM exists.

## Out of scope for now: the native subsystem

`subsystems/native/build.ps1` links with its own `ld.lld` and never goes
through `Add-SxeResources`, so the native subsystem's apps (Haxe → C++) come
out without resources **even if a `.sxres` is written for them**. It is a
coverage gap, not a design decision — but the native subsystem is paused, so it
is documented and left alone until it is picked up again.
