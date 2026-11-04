# NetSurf — the reference browser inside SavanXP

> **Status: not started.** This document is the order of work for porting
> [NetSurf](https://www.netsurf-browser.org/). The port is not meant to replace
> Web Viewer: it is the reference Web Viewer is measured against, page by page,
> inside the same system.

## Why port it

Web Viewer grows its own layout engine on top of NetSurf's MIT libraries
([WEBVIEWER_ROADMAP.md](../desktop/WEBVIEWER_ROADMAP.md)). Every phase of that
work needs an answer to "what should this page look like?". Answering it with
NetSurf running on SavanXP, with the same libraries and the same window size,
removes every difference except the layout itself.

It is also a usable browser while Web Viewer catches up.

## License

Verified against the upstream trees (NetSurf `d06cd9d`, 2026-10-28, and the
library heads of the same week), not remembered:

- **NetSurf itself: GPL-2.0-only**, with a special exception that allows linking
  with OpenSSL and distributing the result. `COPYING` says "version 2 of the
  License", without "or any later version". Visual artwork is MIT.
- **Every library it needs is MIT**: libcss, libdom, libhubbub, libparserutils,
  libwapcaplet, libnsfb, libnsgif, libnsbmp, libsvgtiny, librosprite, libnslog,
  libnspsl, libnsutils, nsgenbind and the shared `buildsystem`.

What that means for SavanXP (MIT):

- **NetSurf is a port, never part of the base tree.** It lives in
  `ports/netsurf/` under the [port layout](../../ports/README.md), as a separate
  program, like `doomgeneric` (GPL-2.0). A GPL program installed into the image
  does not relicense the system.
- **No code of NetSurf's core moves into the MIT tree**, and Web Viewer does
  not use it as a model (the clean-room rule in WEBVIEWER_ROADMAP.md).
- **Linking the SDK and the MIT libraries into it is fine.** MIT code may be
  part of a GPL-2.0 binary.
- **Shipping a `disk.img` with `/bin/netsurf` means the source must be
  available.** The pinned `UPSTREAM` plus `fetch.sh` pattern that `ports/ffmpeg`
  uses covers it.
- **Every library linked in must be GPL-2.0-compatible**, and "only" matters:
  Apache-2.0 is not compatible with GPL-2.0-only. libcurl (curl license),
  zlib, libpng and BearSSL (MIT) are fine. OpenSSL 3 (Apache-2.0) is allowed
  only through NetSurf's exception; mbedTLS would have to be taken under its
  GPL-2.0-or-later option. The default choice is BearSSL, which needs no
  exception at all.
- **Each piece gets an entry** in
  [THIRD_PARTY_PROVENANCE.md](../third-party/THIRD_PARTY_PROVENANCE.md) when
  the port lands, including data files such as a CA bundle.

## One copy of the libraries

The libraries Web Viewer adopts (libparserutils, libwapcaplet, libhubbub,
libdom, libcss, and later libnsgif and libnsbmp) live in `vendor/netsurf/`. The
port builds against those copies and pins only what Web Viewer does not use:
the core, libnsfb, libnsutils and whatever else the core asks for.

That way both browsers parse the same HTML into the same DOM and compute the
same styles, and a difference on screen is a layout difference. It also means
`vendor/netsurf/` and the port's pin of the core move together: updating one
means checking that the other still builds.

## What NetSurf needs from the platform

Measured against the upstream tree:

- **A framebuffer frontend exists, and it is the way in.**
  `frontends/framebuffer` draws its own chrome with `fbtk` over libnsfb.
  libnsfb has surfaces for `ram`, `sdl`, `x`, `vnc` and `wld`; a `savanxp`
  surface in `overlay/` presents into a window over `gfx_open` and
  `gfx_present_region`, the same contract `doomgeneric` uses. The main loop is
  `schedule_run()` followed by `fbtk_event(timeout)`, so the surface's input
  call must wait for window events with a timeout. The chrome does not look like
  the rest of the desktop; for a reference browser, that is acceptable.
- **No threads.** Nothing in the core, the framebuffer frontend or the fetchers
  calls `pthread_*`.
- **Only one fetcher touches the network: libcurl.** `content/fetchers/` has
  `file`, `data`, `resource` and `about` built in, and `curl.c` for HTTP and
  HTTPS. Local pages need no network at all.
- **Platform switches live in `utils/config.h`.** SavanXP needs its own block
  there: `HAVE_MMAP` off (the SDK maps anonymous memory only, and the file
  fetcher and the disk cache map files), `HAVE_REGEX` off (upstream ships a stub;
  regex is used only by "save complete"), plus `HAVE_STRPTIME`, `HAVE_SCANDIR`,
  `HAVE_DIRFD`, `HAVE_REALPATH`, `HAVE_UTSNAME`, `HAVE_INETATON` and
  `HAVE_SYS_SELECT` set to whatever the SDK really provides.
- **`iconv` is required by the core.** `utils/utf8.c` calls `iconv_open`; the
  libraries do not need it (see WEBVIEWER_ROADMAP.md). The SDK has no
  `<iconv.h>`; a small iconv covering UTF-8, UTF-16, Latin-1 and Windows-1252
  is enough to start.
- **Other SDK gaps:** `<locale.h>` (`setlocale` in `desktop/netsurf.c`),
  `<netdb.h>` (step 2) and `<regex.h>` if the stub is not used.
- **Defaults to turn off at first:** `NETSURF_USE_DUKTAPE` (JavaScript) and
  `NETSURF_USE_JPEG` default to `YES`. Both stay off; a reference for layout
  does not need them.

## The order

Each step names its acceptance condition. A step without one is a wish. Step 1
depends on Web Viewer's W1 (the vendored libraries).

### Step 1 — NetSurf on local pages

`ports/netsurf/`: the pinned core and the libraries Web Viewer does not use,
the `savanxp` libnsfb surface and the `utils/config.h` block in `overlay/`,
`build.sh` installing `/bin/netsurf` through the SxFS candidate flow. Built
with the internal bitmap font, no JavaScript, no network.

**Done when:** a `tools/shoot.sh` scenario opens a local page with `file://`,
follows a link, goes back, and the result is checked against the screen; and
the persistence check holds (`./ports/netsurf/build.sh`, then
`./build.sh build`, and `/bin/netsurf` is still there).

### Step 2 — the comparison scenario

The scenario described in
[WEBVIEWER_ROADMAP.md](../desktop/WEBVIEWER_ROADMAP.md#comparing): every page
of `tools/webview-test-www/` opened in both browsers, at the same window size,
both screenshots kept. Its Start menu entry points to `/bin/netsurf` and
disappears when the port is not installed, like Doom's
(`progman_registry_prune_missing`).

**Done when:** the scenario produces both screenshots for every page of the
corpus, and checks Web Viewer alone when the port is missing.

### Step 3 — HTTP

Prerequisites, each useful without the browser:

- **`getaddrinfo` in the SDK** (`<netdb.h>`), built from the DNS client that
  lives in Web Viewer today (see
  [NETWORKING.md](../net/NETWORKING.md#name-resolution-the-minimal-dns-client)).
  Web Viewer, `ntpsync` and every other numeric-only tool use it too.
- **libcurl**, built for HTTP only. What it asks of the socket layer —
  non-blocking `connect`, `poll` or `select` on sockets — is checked here, not
  assumed.

**Done when:** NetSurf loads the pages served by the HTTP test server Web
Viewer already uses, by name and by IP, and follows a 302. The time to load a
page is recorded: if the one-segment-in-flight TCP send path (see
NETWORKING.md) makes real pages slow, this port is the consumer that justifies
a sliding window.

### Step 4 — HTTPS

Prerequisites, shared with Web Viewer:

- **A random source.** There is none today; `arc4random` is already listed as a
  selective port from OpenBSD in THIRD_PARTY_ADOPTION.md. TLS without it is not
  TLS.
- **BearSSL** under libcurl.
- **A CA bundle** in the image, with its provenance recorded.
- **A correct clock**, since certificate validity depends on it (`ntpsync`,
  [TIME.md](../kernel/TIME.md)).

**Done when:** a page loads over HTTPS from a local test server with a test CA,
and a page with a bad certificate is refused.

## What not to do

- **Do not copy NetSurf's core into the MIT tree**, however small the piece.
- **Do not let the port pin its own copies of the vendored libraries.** Two
  versions of libcss would make the comparison measure the wrong thing.
- **Do not write a native SXGUI frontend for NetSurf.** The native browser is
  Web Viewer.
- **Do not turn on JavaScript.** Duktape adds `nsgenbind`, a large binding
  layer and its own bugs, and the comparison does not need it.
- **Do not pick a TLS library that needs an exception** when one that does not
  will do.
