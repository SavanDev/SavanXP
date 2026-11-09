# Third-party provenance registry

This file records every third-party component that ends up inside a binary or
an image that SavanXP distributes. That is the entry rule: one entry per
distributed bit, not one per project that served as inspiration. The reasoning
about what is adopted and under which category lives in
`docs/third-party/THIRD_PARTY_ADOPTION.md`.

Each entry declares origin, pinned version, verified license, decision, and
where the distributed bit ends up. The `Versioned` field says whether the
repository contains the piece or whether it is downloaded or supplied
separately during the build: in both cases the built image redistributes it,
which is why both go into the registry.

## Third-party code compiled into the system

### uACPI

- Origin: https://github.com/uACPI/uACPI, v6.0.0
- Pinned version: commit `9c9b26d6291a1cdd9014cc5bb6b03e596697cbfd`
  (`vendor/uacpi/UACPI_COMMIT.txt`)
- Versioned: yes, `vendor/uacpi/`
- License reviewed: MIT (Copyright 2022-2026 Daniil Tatianin),
  `vendor/uacpi/LICENSE`
- Decision: `Adopt`
- Distributed in: the kernel binary; built with its own flags
  (`SAVANXP_UACPI_COMPILE_OPTIONS`) and integrated through
  `kernel/uacpi_glue.cpp`

### FatFs

- Origin: http://elm-chan.org/fsw/ff/, release R0.16 (`arc/ff16.zip`)
- Pinned version: R0.16, zip SHA-256
  `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e`
  (`vendor/fatfs/FATFS_RELEASE.txt`). FatFs has no public repository; the zip
  is the release.
- Versioned: yes, `vendor/fatfs/`, unmodified. Only the module sources are
  kept: `ff.c`, `ff.h`, `diskio.h`, `ffunicode.c` and the two text files.
  `ffconf.h`, `diskio.c` and `ffsystem.c` are left out: the configuration is
  ours (`kernel/fatfs/ffconf.h`), and the other two are examples the driver
  replaces.
- License reviewed: the FatFs license, one-clause BSD style (Copyright (C) 2025
  ChaN), `vendor/fatfs/LICENSE.txt`. Compatible with MIT; the notice stays in
  every source file.
- Decision: `Adopt`, over a port of Microsoft's `fastfat` (written against the
  NT executive, MS-PL) and a driver of our own
  ([FAT_ROADMAP.md](../roadmaps/open/FAT_ROADMAP.md#decision-fatfs-not-fastfat-not-our-own))
- Distributed in: the kernel binary; built with the kernel's freestanding C
  flags (`savanxp_fatfs_objects`) and integrated through `kernel/fat.cpp` and
  `kernel/fat_diskio.cpp`

### OpenBSD HID report parser

- Origin: OpenBSD `sys/dev/hid/hid.c` (rev 1.10) and `sys/dev/hid/hid.h`,
  https://github.com/openbsd/src
- Pinned version: commit `4112720b9484adea19b5d2c5525b08be6066bf95`
- Versioned: yes, `kernel/usb/hid_report.cpp` and `kernel/usb/hid_report.h`
- License reviewed: BSD-2-Clause, Copyright (c) 1998 The NetBSD Foundation,
  Inc. The notice stays at the top of both files.
- Decision: `Selective port`. The parser is unchanged; the OpenBSD includes
  are replaced by `kernel/usb/hid_compat.hpp` and `hid_types.hpp` (malloc and
  free on the kernel heap, the `u_int` types), and `hid.c` gains the two casts
  from `void *` that C++ needs. The shims carry the same origin note.
- Distributed in: the kernel binary, used by `kernel/usb/hid.cpp` to read the
  report descriptors of USB pointers
  ([USB_ROADMAP.md](../roadmaps/done/USB_ROADMAP.md))

### OpenBSD Azalia HDA driver

- Origin: OpenBSD `sys/dev/pci/azalia.c`, `azalia_codec.c` and `azalia.h`,
  https://github.com/openbsd/src
- Pinned version: commit `3f02b625e68a509b55a83343a6be40565e836cea`;
  `vendor/openbsd/azalia/OPENBSD_COMMIT` and `SHA256SUMS`
- Versioned: pristine originals in `vendor/openbsd/azalia/`; selective port in
  `kernel/azalia/`, PCI/audio adapter in `kernel/azalia.cpp`
- License reviewed: BSD-2-Clause, Copyright (c) 2005 The NetBSD Foundation,
  Inc.; `vendor/openbsd/azalia/LICENSE`. Notices remain in derived sources.
- Decision: `Selective port`. Controller/command/stream routines, codec
  property handling and wire definitions are retained or adapted. The full
  audio(9) mixer, autoconf and interrupt infrastructure are not compiled.
  Analog ADC routing adapts discovery, amp and pin initialization in
  `kernel/azalia/input.cpp`; `capture.cpp` implements the local polled DMA cursor.
- Distributed in: the kernel binary as the analog HDA audio backend.
  [AZALIA.md](../kernel/AZALIA.md) maps the upstream routines to local code
  and records every adaptation boundary.

### Doom (DoomGeneric)

- Origin: https://github.com/ozkl/doomgeneric, commit
  `fc601639494e089702a1ada082eb51aaafc03722` (tree
  `4b8c10b05138583636e5425131242b7212023ef7`)
- Versioned: yes, `ports/doomgeneric/` (pristine source, reviewed patches, and
  SavanXP overlay)
- License reviewed: GPL-2.0. Copyright (C) 1993-1996 Id Software, Inc. and
  Copyright (C) 2005-2014 Simon Howard. Full text in
  `ports/doomgeneric/source/LICENSE`; the per-file notices stay in the headers
- Decision: `Adopt`
- Distributed in: `/bin/doomgeneric`. Built separately with
  `ports/doomgeneric/build.sh`, not by the main build
- Reproduction pin: archive SHA-256
  `6a5879c5f686199f0156ea8abcdaab820350c3a74aff211e81558f8675c8e2d5`
- Runtime dependency: the binary links the system's `libsxmidi.so.0.4` for
  music. That library is SavanXP code, not a third-party component, so it has
  no entry of its own; the MUS-to-MIDI step is `mus2mid.c`, already inside the
  pinned GPL-2.0 source tree above

### FFmpeg / Media Player

- Origin: https://github.com/FFmpeg/FFmpeg, release `n7.1.1`, commit
  `db69d06eeeab4f46da15030a80d539efb4503ca8` (tree
  `94e0dc384ffe4b562568294cf3846e47cb0260d6`)
- Versioned: no source vendored; `ports/ffmpeg/UPSTREAM` pins the release,
  archive SHA-256 `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`,
  and signature fingerprint
  `FCF986EA15E6E293A5644F10B4322F04D67658D8`
- License reviewed: LGPL-2.1-or-later for the selected configuration. Exact
  texts are in `ports/ffmpeg/LICENSE.md` and
  `ports/ffmpeg/COPYING.LGPLv2.1`; the build guard rejects GPL, LGPLv3, and
  nonfree components
- Decision: `Adopt`
- Distributed in: the system launcher `/bin/mediaplayer`, built by the tree, plus
  the optional shared library `/lib/libffmpeg.so.0.4` linked with the
  selected FFmpeg libraries. Corresponding source/relinking instructions are in
  `ports/ffmpeg/SOURCE.md`
- Local test media is generated under `build/media` and is not a third-party
  repository asset

### Celeste Classic (ccleste)

- Origin: https://github.com/lemon32767/ccleste, commit
  `261d96f15af430b8111abc7a5250229246654f52` (tree
  `8a5143facf7c8a6818817b7c33e4956e3a0caa8b`)
- Versioned: no source vendored. The repository is archived read-only since
  2023-11-07, so nothing is committed here; `ports/ccleste/UPSTREAM` pins the
  commit and the archive SHA-256
  `2f6c7f753d1c8d0ba13bbc144a2a3ab9d3942ed4333e4f7f56e058e3f3677069`, and
  `ports/ccleste/fetch.sh` verifies it before use
- License reviewed: **none exists**. Upstream has no `LICENSE`, `COPYING` or SPDX
  identifier, and the GitHub API reports `license: null`, which leaves the code
  under the default "all rights reserved". `UPSTREAM` records this as
  `license=Unlicensed` rather than guessing
- Decision: `Adopt`, with the redistribution question left open. The engine
  (`celeste.c`) is compiled unpatched and the port replaces the SDL frontend
  with an overlay, so no upstream code is committed. The archive is fetched and
  hash-verified at build time, and what the built image redistributes is the
  binary plus the game assets
- Distributed in: `/bin/ccleste` and `/games/celeste` (the 128x128
  tile sheet, the font and 23 sound effects, ~1.1 MB). Built separately with
  `ports/ccleste/build.sh`, not by the main build
- Known debt: because the license is unstated, this port cannot be
  redistributed as a binary image by anyone else without resolving it first.
  The music is also absent from the image: the tracks are OGG Vorbis and the
  SDK has no Vorbis decoder
- Credit for the original game: Maddy Thorson and Noel Berry, per upstream's
  README

## Bootloader

### Limine

- Origin: https://github.com/limine-bootloader/limine, `v10.x-binary` branch
- Versioned: no. `build.sh` clones it into `tools/limine`, which is in
  `.gitignore`
- License reviewed: BSD-2-Clause, `tools/limine/LICENSE`
- Decision: `Adopt`
- Distributed in: the ISO, as `BOOTX64.EFI`, `limine-bios-cd.bin`,
  `limine-bios.sys` and `limine-uefi-cd.bin`

### Limine protocol header

- Origin: derived from https://github.com/limine-bootloader/limine-protocol
- Versioned: yes, `vendor/limine.h`
- License reviewed: 0BSD, declared by SPDX in the file itself
- Decision: `Selective port`
- Distributed in: the kernel binary. It is a minimal version written for this
  repository, not a copy of the upstream header

## Content and assets in the image

### Freedoom

- Origin: https://freedoom.github.io
- Versioned: no. `.gitignore` excludes `ports/doomgeneric/wad/*.wad`; the file
  is supplied separately and the port prints the URL if it is missing
- License reviewed: BSD-3-Clause. Independent project, unrelated to id Software
- Decision: `Adopt`
- Distributed in: `/games/doom/freedoom1.wad` of the built image. It is
  the default IWAD so that the LiveCD is playable without proprietary or
  shareware content

### GNU UniFont (console and terminal)

- Origin: GNU Unifont 17.0.04, `unifont-17.0.04.hex`
  (https://unifoundry.com/unifont/)
- Versioned: yes, `assets/desktop/fonts/unifont.hex`
- License reviewed: SIL OFL-1.1 and, alternatively, GNU GPLv2+ with the GNU
  Font Embedding Exception (upstream dual license). Text in
  `assets/desktop/fonts/LICENSE-OFL-1.1.txt`
- Decision: `Adopt`
- Distributed in: the binary, as a C table baked offline by
  `tools/font/genfont.py`. SavanXP neither embeds nor parses TrueType at
  runtime: what stays compiled in is the derived bitmap
- Reason: a native 8x16 bitmap font with broad Unicode coverage. It is baked
  from the canonical `.hex` (ASCII, Latin-1, box drawing and blocks) because
  the TTF outline rasterizes off-grid

### Wine MS Sans Serif (desktop body text)

- Origin: the Wine project, `fonts/ms_sans_serif.sfd` at tag `wine-11.20`
  (commit `7298a818b79446bae153b8d5aa27235ab3703bf9`), file SHA-256
  `d8cc5e8e93d70478e810b9aa3272006da57e819a063c2b34f5cf92a233b0f3f3`.
  Copyright (C) 2004 Huw D M Davies, Dmitry Timoshkov
  (https://gitlab.winehq.org/wine/wine/-/blob/wine-11.20/fonts/ms_sans_serif.sfd)
- Versioned: yes, `assets/desktop/fonts/ms_sans_serif.sfd`, unmodified
- License reviewed: LGPL-2.1-or-later (Wine's `LICENSE`). Text in
  `assets/desktop/fonts/LICENSE-LGPL-2.1.txt`. It is the Wine project's own
  drawing, made as a free replacement for Microsoft's MS Sans Serif, which
  Wine does not ship
- Decision: `Adopt`
- Distributed in: `libsxgfx.so.0.4` and statically linked programs, as the
  13px bitmap strike baked to a C table by `tools/font/genfont.py sfd`
  (`gfx_font_noto.inc`). The source and the generator are both in the
  repository, so the table can be regenerated from a modified `.sfd` and the
  library rebuilt
- Reason: the Win9x/2000 UI face, drawn by hand at the pixel size the desktop
  uses; full Latin-1 including Spanish accents. See `docs/desktop/UI_FONTS.md`

### Noto Sans (boot screen wordmark)

- Origin: Noto Sans Regular v1.06 (Copyright 2012 Google Inc.), the Noto
  project (https://fonts.google.com/noto)
- Versioned: yes, `assets/desktop/fonts/NotoSans-Regular.ttf`
- License reviewed: SIL OFL-1.1,
  `assets/desktop/fonts/LICENSE-OFL-1.1.txt`
- Decision: `Adopt`
- Distributed in: the kernel, as the antialiased "SavanXP" wordmark that
  `tools/gen_boot_logo.py` rasterizes into the boot screen. It was the desktop
  body face until Wine's MS Sans Serif replaced it
- Reason: a smooth wordmark for the splash

### SpicyGame pixel fonts (captions and spares)

- Origin: Retro Sans, Pixel Sans and Impactful Bits by SpicyGame
  (https://spicygame.itch.io/fonts)
- Versioned: yes, `assets/desktop/fonts/RetroSans.ttf`,
  `assets/desktop/fonts/PixelSans.ttf`, `assets/desktop/fonts/ImpactfulBits.ttf`
  and `assets/desktop/fonts/readme.txt`
- License reviewed: CC0, per `readme.txt`
- Decision: `Adopt`
- Distributed in: the binary, as a coverage table baked by
  `tools/font/genfont.py`: RetroSans at 17px, its native pixel grid, for
  window captions (`gfx_font_title.inc`). PixelSans and ImpactfulBits are baked at 13px as
  committed spares (`gfx_font_pixelsans.inc`, `gfx_font_impactfulbits.inc`)
  and are not linked into any binary yet
- Reason: a pixel face for window captions; the spares cover future chrome
  without a new adoption. PixelSans lacks Latin-1 accents, so Spanish text
  would fall back to blank advances there

### Desktop icons and art

- Origin: the repository's own checked-in art, drawn for this project
- Decision: out of the third-party registry since its replacement
- Reason: the desktop no longer depends on icons copied from or derived from
  other projects
- Scope: `assets/desktop/icons/16x16` and `32x32`. It does **not** cover
  `assets/desktop/icons/mime/`, which is the file-type catalog and has the
  provenance below

### Tango Icon Theme (file-type catalog)

- Origin: Tango Icon Theme 0.8.90, `tango-icon-theme_0.8.90.orig.tar.gz`
  (Tango Desktop Project, http://tango.freedesktop.org/), `sha256`
  `6e98d8032d57d818acc907ec47e6a718851ff251ae7c29aafb868743eb65c88e`
- Versioned: yes, 22 PNGs (11 icons x 16x16 and 32x32) in
  `assets/desktop/icons/mime/`
- License reviewed: **public domain**. The tarball's `COPYING` states verbatim
  *"The icons in this repository are herefore released into the Public
  Domain"*. Versions before 0.8.90 were CC BY-SA 2.5; the move to the public
  domain was made precisely to remove the incompatibility with the GPL, so the
  version matters: **0.8.90 or later**
- Decision: `Adopt`
- Distributed in: the image, as standalone `.sxicon` blobs in `/icons`
  emitted by `tools/gen_mime_icons.py`. They are **not** baked into any binary,
  for the same reason `.sxicon` is a non-alloc section (see
  `docs/userland/SXE_FORMAT.md`)
- Reason: file-type icons are drawn over the WHITE field of a list, not over
  the gray face of the desktop. Measured against `C0C0C0` the Tango set loses
  to our own art (`input-keyboard` 27.1 against 52.2 mean luminance contrast),
  but over white it nearly doubles it, and white is exactly the background of
  this use case. It also brings the type coverage our own art does not have,
  and it follows freedesktop's Icon Naming Specification, which is what makes
  it possible to change source without rewriting `diskfs/mimeicon.ini`
- Note: Tango's `devices/` and `places/` icons were **not** adopted. Over the
  gray face they disappear, and those are exactly the ones the desktop would
  need

## Build only: not distributed

Host build tools (LLVM, QEMU, xorriso, Ninja) do not enter any image: they
produce the binaries and stay outside. They are resolved from `PATH` or CMake
cache overrides, and are not duplicated in this registry.

## Reference: no third-party bits in the repository

These adoptions are of design, not of code: no lines are copied, so there is
nothing to license or redistribute. The decision and the reason for each live
in `docs/third-party/THIRD_PARTY_ADOPTION.md`.

- SerenityOS (BSD-2-Clause): `WindowServer` and `Compositor` for the
  compositor architecture, the dirty regions and the occlusion; `LibGfx`
  (`Bitmap`, `Painter`, `DisjointRectSet`) for the toolkit and rect handling;
  `Kernel/Devices/GPU/DisplayConnector` for the `display` layer. SavanXP
  implements its own variant in C over its own ABI, with no dependency on `AK`
- OpenBSD (ISC and BSD-2): candidates evaluated and categorized in the adoption
  document. No piece has been incorporated yet, so there is no distributed
  entry to record

### Retrosmart cursor pack

- Origin: [useless-anvil/retrosmart-cursor](https://github.com/useless-anvil/retrosmart-cursor),
  tag `v2.0.3`, commit `1bb39e8681afae659fc46a47a9a938de147b85b9`.
- Artwork: Copyright 2019 Manuel Domínguez López; fork by useless-anvil.
  Upstream `AUTHORS`, `COPYING`, `LICENSE` (GPL-3.0) are included verbatim.
- Source release: `retrosmart-cursor-classic-v2.0.3-windows.zip`, Win-ish Classic,
  plain variant. [Release](https://github.com/useless-anvil/retrosmart-cursor/releases/tag/v2.0.3).
  SHA-256: `2021e6821b26f69a964a93a04e62cdc766ef4472bebb7a962857300972bed447`.
- Decision: `Adopt` as a separate selectable artwork pack, not linked code.
- Distributed in: `/system/cursors/retrosmart/`. Static CURs are unchanged
  release bytes, renamed by role. `wait.cur` is the first embedded CUR from
  `wait.ani`; animation is omitted. PNG payloads are decoded directly.
- Corresponding source: the complete pinned upstream archive is shipped as
  `source.tar.gz`, with `import_cursor_packs.py` describing the local import.
  Full credits, GPL terms and source locations appear in `NOTICE` and the
  Mouse Properties license dialog. No upstream build tools are added as OS
  build dependencies.

### Hackneyed cursor pack

- Origin: [Richard Ferreira / hackneyed-x11-cursors](https://gitlab.com/Enthymeme/hackneyed-x11-cursors),
  tag `0.9.3`, commit `6cb96737d5566e36fadf4526fb374778cbb114c7`.
- License: MIT/X11, Copyright (C) Richard Ferreira. Original `LICENSE` included.
- Source release: `Hackneyed-Windows-0.9.3.zip`, Standard (light, right-handed).
  [Release](https://gitlab.com/Enthymeme/hackneyed-x11-cursors/-/releases/0.9.3).
  SHA-256: `43980eb1660e1d9da5d20d755d2cf0d3ff120c2615cd9c17f00d7e8e467a4b6d`.
- Decision: `Adopt` as a separate selectable artwork pack.
- Distributed in: `/system/cursors/hackneyed/`. Static CURs are original bytes
  under role names; `wait.cur` is the first embedded CUR of `wait.ani`.
  Attribution and terms are available in `NOTICE` and Mouse Properties.

### Cursor imports and original artwork

SavanXP Classic is local artwork under the repository's MIT license. The eight
PNGs were recovered unchanged from the parent of commit `ab6a453` and encoded
as uncompressed 32-bit CUR with their previous hotspots. Two diagonal arrows
were added locally. The PNG sources and license are kept alongside the pack.
Wine's cursor resources have been removed; the Wine font entry above remains.

To reproduce the checked-in artwork from the original PNGs and the two release
ZIPs (requires the existing host Python/Pillow dependency):

```sh
python3 tools/import_cursor_packs.py --retrosmart /path/to/retrosmart-cursor-classic-v2.0.3-windows.zip --hackneyed /path/to/Hackneyed-Windows-0.9.3.zip
```

The role mapping is recorded in that script. No conversion or network access
runs during a normal build. Pack files have these SHA-256 hashes:

| Pack / file | SHA-256 |
|---|---|
| `classic/arrow.cur` | `310bb5ace5a6b54ffc324ce33cbcdfb2a21b79fe6b8b7907e0ee36f0f92b0d7c` |
| `classic/link.cur` | `f8c41383496cea46042fbdf695bfd744da29eb27cfbadcdfbc423328cdb32bcb` |
| `classic/move.cur` | `eb895f08f7f0403aa7575de2d78461088f827e22979b4847342faba76ddd49f8` |
| `classic/resize_h.cur` | `724e66fae024f4dacc69ddec6c71fc188cb42541c96794132a25a27760e152cd` |
| `classic/resize_nesw.cur` | `bb3d7b2cfd79718450989836dcfa820cf18cf6525cd68a749cf37d1c70ae479b` |
| `classic/resize_nwse.cur` | `1ad3e82d1b7b1a0524d05098241dd09976f55ccbbbeee079a32d922712f00001` |
| `classic/resize_v.cur` | `6bbff688956aa3ece2b3580bb2f748c5024ec7e0eac32146179346bebe8ca47b` |
| `classic/text.cur` | `e0a292a68a1a6fa337c56112e3aeadafba6932e1177828a8c4c0b9846309e356` |
| `classic/unavailable.cur` | `77414700fdebee2803cf1140281f04bd38f6c8daa46788b10ce41583dec22c52` |
| `classic/wait.cur` | `bed87ad58c57d77033bd977cdb6be10c98699eba01670677a88bf873a30a72d5` |
| `retrosmart/arrow.cur` | `778d3286326eb309ec56363904907eb753a86ad3c5bde0e2bf58dbdab573955b` |
| `retrosmart/link.cur` | `8a805df1c24c41742f87b03f8867fdb472dc9214f4571ee7132e31a053bb233b` |
| `retrosmart/move.cur` | `f3b0ccf949901df35c3038695fa4c3f5579a371d75be01f7a82f1bf419ec0384` |
| `retrosmart/resize_h.cur` | `4902c69b07b0bf1e0e486b6449f002e2f265e0c2eb6b161372b9bcecb4860331` |
| `retrosmart/resize_nesw.cur` | `f12916f6b8548b8d97492e95ecaa353bb85a725c07219c53230506aa7f7027ba` |
| `retrosmart/resize_nwse.cur` | `ba85f01b9293a3ebbdb2622a4edd955eeeec164fd74c778558fbabe705274d38` |
| `retrosmart/resize_v.cur` | `9c1a69d3d08ba2e982f42e835dcc7e43d1f03e772f6d4c195e9e52de82d7778e` |
| `retrosmart/text.cur` | `069341263aed4f47947e1902a0629c1227ff3eb1c37ff563b8d4d22b5e1b3ae8` |
| `retrosmart/unavailable.cur` | `f2b4acbd417fe06415d12c09b32c0857d187bdb71be07f2bc4c44f68767e2307` |
| `retrosmart/wait.cur` | `602306bb1862ef8e580052296a4e09ac1b2eb015e39f2455b944299df519034a` |
| `hackneyed/arrow.cur` | `8a72bf8a1a7a5890c2b243d227f0757d352694948b5f39c72c63e77fd351a4bf` |
| `hackneyed/link.cur` | `5f947bd53ee66482e8c7f74c999f8abe8241d921df0bf10a75cb01af33a87f29` |
| `hackneyed/move.cur` | `804adf18b02bd29345a4d8cb8a93e55ad187f245857c73aed1132d4abd0f91cc` |
| `hackneyed/resize_h.cur` | `eca6813f4f8a671e84b571969ef39fafe77c628f53bb2b250505752860be1095` |
| `hackneyed/resize_nesw.cur` | `78be535250acc60937aabe63a0252cc3d3f85ede524bc2c96b7a29d508ae8af3` |
| `hackneyed/resize_nwse.cur` | `bd5d78a5224f2068caab3264af39da6fb9b6589cce60e90504ab246c057d51a5` |
| `hackneyed/resize_v.cur` | `6fc15b259f36bc704d9609efb2ba1eb7a122513d2725f84365f93f47b2394905` |
| `hackneyed/text.cur` | `0ba47afb445158ab221b5801fc6016db174a38b17f3669d49a3261e0865c1192` |
| `hackneyed/unavailable.cur` | `fa685e87c60fad5a292b8c00c8eca335e50caf40fbed2615c5ef3b94519e8081` |
| `hackneyed/wait.cur` | `48729b8e5b8e5ce504cd94381373f66ebfd9b243525ebafe697e7c8b78edd06e` |

Retrosmart corresponding source archive SHA-256: `3b3ff5ea1d56a0ae461ccfe036495fe7c7f01c3ba90462033a61438e2b8cc0c8`.
