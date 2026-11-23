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

### OpenBSD re(4) Realtek gigabit driver

- Origin: OpenBSD `sys/dev/ic/re.c`, `sys/dev/ic/rtl81x9reg.h` and
  `sys/dev/mii/rgephy.c`, https://github.com/openbsd/src
- Pinned version: commit `287e5b695b75cb0c9c794575ed99e90305f680e8`
  (`re.c` rev 1.224)
- Versioned: adapted in `kernel/rtl8168.cpp`; no pristine copy is kept
- License reviewed: **BSD-4-Clause**, Copyright (c) 1997-2003 Bill Paul. Unlike
  most of OpenBSD it keeps the advertising clause: material that advertises the
  feature must say "This product includes software developed by Bill Paul".
  The notice is at the top of `kernel/rtl8168.cpp`, which carries this license
  rather than MIT.
- Decision: `Selective port`. Taken: the revision table read from `TxConfig`,
  the per-revision flags acted on, register offsets and bits, the reset,
  C+ and bring-up order, and the PHY wake and autonegotiation writes. Not
  taken: `bus_dma`, mbufs, `ifmedia`/`mii`, offloads, VLAN, jumbo frames,
  interrupt moderation, Wake-on-LAN.
- Distributed in: the kernel binary as the `rtl8168` NIC driver (HW-1 in
  [REAL_HARDWARE.md](../hardware/REAL_HARDWARE.md))

### OpenBSD nvme(4) NVMe driver

- Origin: OpenBSD `sys/dev/ic/nvme.c`, `sys/dev/ic/nvmereg.h` and
  `sys/dev/pci/nvme_pci.c`, https://github.com/openbsd/src
- Pinned version: commit `287e5b695b75cb0c9c794575ed99e90305f680e8`
  (`nvme.c` rev 1.132, `nvmereg.h` rev 1.16)
- Versioned: adapted in `kernel/nvme.cpp`; no pristine copy is kept
- License reviewed: ISC, Copyright (c) 2014 David Gwynne. The notice is at the
  top of `kernel/nvme.cpp`.
- Decision: `Selective port`. Taken: the PCI match, the register offsets and
  bits, the submission and completion entry layouts, the Identify field
  offsets, the disable/enable order with `CAP.TO`, queue creation, the
  phase-bit completion loop, the namespace size rule and the shutdown
  sequence. Not taken: the SCSI emulation layer, `bus_dma`, ccbs, interrupts,
  sensors, `bioctl`, passthrough, hibernation and the Apple quirks.
- Distributed in: the kernel binary as the `nvme` storage driver (HW-5 in
  [REAL_HARDWARE.md](../hardware/REAL_HARDWARE.md); [NVME.md](../kernel/NVME.md))

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

### dav1d

- Origin: https://code.videolan.org/videolan/dav1d, release `1.5.1`
- Versioned: no source vendored; `ports/ffmpeg/UPSTREAM` pins the archive
  (SHA-256 `401813f1f89fa8fd4295805aa5284d9aed9bc7fc1fdbe554af4292f64cbabe21`)
  and VideoLAN's release signing key
  `65F7C6B4206BD057A7EB73787180713BE58D1ADC`; no patches
- License reviewed: BSD-2-Clause. Text in
  `ports/ffmpeg/licenses/DAV1D-COPYING.txt`
- Decision: `Adopt`. FFmpeg's own `av1` decoder only drives hardware decoders,
  so software AV1 needs dav1d
- Distributed in: `/lib/libffmpeg.so.0.4`, linked in as a static archive by
  `ports/ffmpeg/link.sh`. Built by `ports/ffmpeg/dav1d.sh` with Meson

### NetSurf libraries

- Origin: https://download.netsurf-browser.org/libs/releases/, the release
  tarballs of libparserutils 0.2.5, libwapcaplet 0.4.3, libhubbub 0.3.8,
  libdom 0.4.2 and libcss 0.9.2
- Pinned version: those releases, each tarball's SHA-256 in
  `vendor/netsurf/NETSURF_RELEASES.txt` (NetSurf publishes no checksums; the
  hashes are of the archives as downloaded on 2026-11-22)
- Versioned: yes, `vendor/netsurf/<library>/`, unmodified, without each
  release's `test/` (and libhubbub's `perf/`, the `.github/` directories);
  libdom's XML binding stays in the tree but is not built
- License reviewed: MIT, each library's `COPYING` (J-M Bell, 2007-8; The
  NetSurf Browser Project, 2009). NetSurf's GPL-2.0-only core is not
  vendored and not used ([THIRD_PARTY_ADOPTION.md](THIRD_PARTY_ADOPTION.md#netsurf))
- Decision: `Adopt`
- Distributed in: static archives built by `cmake/NetSurf.cmake`, linked into
  `/system/enginetest.sxe`. Web Viewer links the HTML libraries from E2
  ([WEBVIEWER_ENGINE.md](../desktop/WEBVIEWER_ENGINE.md)); notices
  `assets/licenses/netsurf-*.notice`

### OpenSSL

- Origin: https://github.com/openssl/openssl, release `openssl-3.5.3`, a 3.5
  long-term-support release; chosen to match the Ladybird engine planned at
  the time (commit `37ec6a08`, `vcpkg.json` override `openssl` `3.5.3#0`),
  a plan since retired
- Versioned: no source vendored; `ports/openssl/UPSTREAM` pins the release
  archive, SHA-256
  `c9489d2abcf943cdc8329a57092331c598a402938054dc3a22218aea8a8ec3bf`
  (the same in the GitHub release's `.sha256` and openssl.org's), 53183370
  bytes; `SAVANXP_VERIFY_SIGNATURE=1` also checks the release signature
- Patches: one, `ports/openssl/patches/0001-no-file-system.patch`: directory
  listing, the random seed file and the `file:` store answer "unsupported",
  so the library reaches no file system
- License reviewed: Apache-2.0, `ports/openssl/LICENSE.txt` (the release's
  `LICENSE.txt`)
- Decision: `Adopt`
- Distributed in: `/lib/libcrypto.so.0.4` and `/lib/libssl.so.0.4`, linked by
  `ports/openssl/link.sh` from the configuration in `ports/openssl/configure.sh`;
  built only when the port is installed

### Mozilla CA certificate store

- Origin: Mozilla NSS `certdata.txt`, as extracted to PEM by curl,
  https://curl.se/docs/caextract.html
- Pinned version: the 2026-11-05 extract, `cacert-2026-11-05.pem`, SHA-256
  `427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285`
  (curl's published `cacert.pem.sha256` on that day), 200116 bytes, 118
  roots; `ports/openssl/UPSTREAM`, checked by `ports/openssl/fetch.sh`
- Versioned: no; downloaded by the port
- License reviewed: MPL-2.0 (the certificate data),
  `ports/openssl/licenses/MPL-2.0.txt`
- Decision: `Adopt`, unchanged. Test CAs are never added to it
- Distributed in: `/system/ssl/cert.pem`, OpenSSL's default file under
  `--openssldir=/system/ssl`, installed with the port

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

### TinyCC (Tiny C Compiler)

- Origin: https://repo.or.cz/tinycc.git, branch `mob`, commit
  `0f004a0883c640528d69a006706f867d56c809e5`
- Versioned: no source vendored. `ports/tinycc/UPSTREAM` pins the commit, and
  `ports/tinycc/build.sh` clones it, checks it out and applies
  `patches/0001-savanxp-target.patch` and `overlay/`
- License reviewed: LGPL-2.1-or-later for the compiler (`COPYING`, copied as
  `ports/tinycc/licenses/TINYCC-COPYING.txt`). The runtime in `lib/` that
  becomes `libtcc1.a` and `runmain.o` is GPL with a linking exception per
  file, so programs `tcc` builds carry no GPL obligation from it
- Decision: `Adopt`. The patch is the SavanXP target, the `-run` memory and
  symbol table, and nothing else
- Distributed in: `/bin/tcc` and `/system/tcc` (headers, `libtcc1.a`,
  `runmain.o`, and copies of the SDK's `crt0.o`, `libc.a` and headers). Built
  separately with `ports/tinycc/build.sh`, not by the main build

### HLSDK Portable

- Origin: [FWGS/hlsdk-portable](https://github.com/FWGS/hlsdk-portable), commit
  `b562490665c48d273f352d62228577cd003c4d92`.
- Versioned: the archive checksum and the SavanXP target patch are tracked under
  `ports/xash3d/`. The source archive is fetched during the standalone port build.
- License reviewed: Half-Life 1 SDK License, with free redistribution terms.
  The upstream license accompanies the module.
- Decision: `Adopt` for the optional Xash3D port.
- Distributed in: `/games/xash3d/valve/dlls/hl_savanxp_amd64.so` when the
  standalone port is installed. Half-Life game data is supplied separately.

### Alpine Xlib profile libraries

- Origin: X.Org's [individual library releases](https://www.x.org/releases/individual/lib/)
  and [libbsd/libmd releases](https://libbsd.freedesktop.org/releases/), as
  original Alpine 3.24.4 Linux x86-64 binaries. Package versions and archive
  hashes are pinned in `subsystems/linux/profile/alpine.lock`; installed
  binary hashes are in [LINUX_XEYES_PROFILE.md](../userland/LINUX_XEYES_PROFILE.md)
  and checked by CMake before staging.
- Versioned: no binaries or source archives. The full upstream `COPYING`
  files are versioned in `assets/licenses/linux-x11/`.
- License reviewed: X11/MIT for X11, xcb, Xau and Xdmcp; libbsd and libmd
  contain BSD variants, ISC, Beerware and public-domain components. Their
  complete copyright-format documents preserve the per-file notices and
  all applicable license texts; these are not relicensed as SavanXP MIT.
- Decision: `Adopt` as unchanged compatibility-profile binaries, not code
  linked into the kernel or SUN server.
- Distributed in: optional test-image and installed Xeyes runtime
  `/system/linux/usr/lib/`.
  `linux-x11.notice` stages the notices under `/system/licenses`.

The reviewed source releases and SHA-256 hashes (all `.tar.xz`):

| Release | SHA-256 |
| --- | --- |
| libX11 1.8.14 | `bbc78c46217174655c2b32b138ad119f3ac15f50808b4d0b3a2677a4b1afc950` |
| libxcb 1.17.0 | `599ebf9996710fea71622e6e184f3a8ad5b43d0e5fa8c4e407123c88a59a6d55` |
| libXau 1.0.12 | `74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb` |
| libXdmcp 1.1.5 | `d8a5222828c3adab70adf69a5583f1d32eb5ece04304f7f8392b6a353aa2228c` |
| libbsd 0.12.2 | `b88cc9163d0c652aaf39a99991d974ddba1c3a9711db8f1b5838af2a14731014` |
| libmd 1.2.0 | `ac15ffb8430502fbaccdec66c5a82ee0eab0b0f36220df56710feadfeb13d0a0` |

### Alpine Xeyes compatibility runtime

- Origin: original Alpine Linux 3.24.4 packages pinned in
  `subsystems/linux/profile/alpine.lock`. Exact installed paths and SHA-256
  hashes (Xeyes, musl, 18 shared libraries and three locale files) are in
  `subsystems/linux/profile/xeyes-runtime.json`.
- Versioned: no binaries or source archives. Complete upstream notices are
  preserved in `assets/licenses/linux-xeyes/`; the six libraries shared with
  L5 retain the `linux-x11` notice above.
- License reviewed: MIT/X11 for Xeyes, libXi, libXmu, libXt, libXext,
  libXrender, libICE and libSM; musl's complete COPYRIGHT preserves its MIT
  and per-file exceptions; libuuid is Modified BSD (BSD-3-Clause), with
  original source headers preserved alongside the license text.
- Decision: `Adopt` as unchanged compatibility-profile binaries, never linked
  into the kernel, SUN launcher or X11 engine.
- Distributed in: `/system/linux/`, explicitly installed by
  `subsystems/linux/install_xeyes.py`. All notices accompany the runtime
  under `/system/licenses`.

Reviewed official source archives and SHA-256 hashes:

| Release | SHA-256 |
| --- | --- |
| libICE-1.1.2.tar.xz | `974e4ed414225eb3c716985df9709f4da8d22a67a2890066bc6dfc89ad298625` |
| libSM-1.2.6.tar.xz | `be7c0abdb15cbfd29ac62573c1c82e877f9d4047ad15321e7ea97d1e43d835be` |
| libXext-1.3.7.tar.xz | `6c643c7035cdacf67afd68f25d01b90ef889d546c9fcd7c0adf7c2cf91e3a32d` |
| libXi-1.8.4.tar.xz | `ddd6057b89a91903bc2f884d55c8467440906945e482c36547260dff28661afc` |
| libXmu-1.3.1.tar.xz | `81a99e94c4501e81c427cbaa4a11748b584933e94b7a156830c3621256857bc4` |
| libXrender-0.9.13.tar.xz | `3fe88c609df6782e1e7af2e4de36b2563d70cd70ff7dcb1cda561ad64912bb64` |
| libXt-1.3.1.tar.xz | `e0a774b33324f4d4c05b199ea45050f87206586d81655f8bef4dba434d931288` |
| musl-1.2.6.tar.gz | `d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a` |
| util-linux-2.42.3.tar.xz | `66ac7c0e725278eb2b039e3104f2c91119341d941b41bac7a285c695f940bd57` |
| xeyes-1.3.1.tar.xz | `5608d76b7b1aac5ed7f22f1b6b5ad74ef98c8693220f32b4b87dccee4a956eaa` |

X.Org releases come from `https://www.x.org/releases/individual/{app,lib}/`,
musl from `https://musl.libc.org/releases/`, and util-linux (libuuid) from
`https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.42/`.

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
  it possible to change source without rewriting `diskfs/system/mimeicon.ini`
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

- [ITU-T T.81 (1992)](https://www.w3.org/Graphics/JPEG/itu-t81.pdf) and
  [JFIF 1.02](https://www.w3.org/Graphics/JPEG/jfif3.pdf): sequential JPEG
  framing, entropy coding, reconstruction and YCbCr conversion in `libsximg`.
  [libjpeg-turbo 3.2.1 colour conventions](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/3.2.1/src/jdapimin.c)
  inform JFIF/Adobe/markerless interpretation; its [reduced-output design](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/3.2.1/src/jidctred.c)
  confirms spatial averaging for scaled IDCT references. Its encoder/decoder produced
  host fixtures from our own samples. `Reference`: implemented here to share
  the memory-only SUN/POSIX API, with no upstream decoder code copied or
  linked. Specifications and upstream code are not redistributed. See
  [IMAGE_FORMATS.md](../media/IMAGE_FORMATS.md#sequential-jpeg).
- X Consortium's [X11R7.7 core protocol](https://www.x.org/releases/X11R7.7/doc/xproto/x11protocol.html):
  the wire encoding, resource errors, events and ellipse edge convention of
  SSL's portable X11 engine. `Reference`: implemented here, with no X server
  code copied and no third-party engine linked into `x11d`.
- SerenityOS (BSD-2-Clause): `WindowServer` and `Compositor` for the
  compositor architecture, the dirty regions and the occlusion; `LibGfx`
  (`Bitmap`, `Painter`, `DisjointRectSet`) for the toolkit and rect handling;
  `Kernel/Devices/GPU/DisplayConnector` for the `display` layer. SavanXP
  implements its own variant in C over its own ABI, with no dependency on `AK`
- OpenBSD (mostly ISC and BSD-2): candidates evaluated and categorized in the
  adoption document. The pieces that were incorporated (the HID parser, Azalia,
  re(4), nvme(4)) have their own entries above

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
