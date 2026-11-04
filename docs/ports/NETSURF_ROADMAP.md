# NetSurf — the browser that replaces Web Viewer

> **Status: not started. Web Viewer is frozen.** This document is the order of
> work for porting [NetSurf](https://www.netsurf-browser.org/) and, once the
> port has caught up, for deleting Web Viewer from the tree.

## Why NetSurf instead of growing Web Viewer

Web Viewer (`subsystems/posix/userland/webview.c`, ~4100 lines) renders a
subset of CSS1: tag selectors only, no classes or ids, no tables, no images, a
32 KiB document cap. [WEBVIEWER.md](../desktop/WEBVIEWER.md) lists the gaps.
Taking a hand-written engine from there to CSS 2.1 with a real DOM is years of
work that NetSurf has already done, in portable C, for machines smaller than the
one SavanXP targets.

So Web Viewer gets no new features. It stays in the tree as the working browser
until NetSurf matches it, and then it is removed (step 5). The pieces of it
worth keeping — the DNS client — move into the SDK as a prerequisite of the
port (step 2), not as Web Viewer features.

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
- **No NetSurf code moves into the MIT tree.** Not into `webview.c`, not into the
  SDK, not "just this helper". The MIT libraries may be reused elsewhere under
  [THIRD_PARTY_ADOPTION.md](../third-party/THIRD_PARTY_ADOPTION.md), with their
  notices.
- **Linking the SDK into it is fine.** The SDK runtime is MIT, and MIT code may
  be part of a GPL-2.0 binary.
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

## What NetSurf needs from the platform

Measured against the upstream tree:

- **A framebuffer frontend exists, and it is the way in.**
  `frontends/framebuffer` draws its own chrome with `fbtk` over libnsfb.
  libnsfb has surfaces for `ram`, `sdl`, `x`, `vnc` and `wld`; a `savanxp`
  surface in `overlay/` presents into a window over `gfx_open` and
  `gfx_present_region`, the same contract `doomgeneric` uses. The main loop is
  `schedule_run()` followed by `fbtk_event(timeout)`, so the surface's input
  call must wait for window events with a timeout.
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
- **`iconv` is required.** `utils/utf8.c` calls `iconv_open`. libparserutils can
  be built with `WITHOUT_ICONV_FILTER`, but the core cannot. The SDK has no
  `<iconv.h>`; a small iconv covering UTF-8, UTF-16, Latin-1 and Windows-1252
  is enough to start.
- **Other SDK gaps:** `<locale.h>` (`setlocale` in `desktop/netsurf.c`),
  `<netdb.h>` (step 2) and `<regex.h>` if the stub is not used.
- **Defaults to turn off at first:** `NETSURF_USE_DUKTAPE` (JavaScript) and
  `NETSURF_USE_JPEG` default to `YES`. Both stay off until step 4. PNG, GIF and
  BMP come from libpng and the MIT libnsgif/libnsbmp, and can come early.

## The order

Each step names its acceptance condition. A step without one is a wish.

### Step 0 — freeze Web Viewer

Bug fixes only. No new CSS, no new tags, no new limits raised. Every hour that
would go into `webview.c` goes into the steps below instead.

**Done when:** [WEBVIEWER.md](../desktop/WEBVIEWER.md) says it is frozen and
points here. It does.

### Step 1 — NetSurf on local pages

`ports/netsurf/`: pinned NetSurf and libraries, the `savanxp` libnsfb surface
and the `utils/config.h` block in `overlay/`, `build.sh` installing
`/bin/netsurf` through the SxFS candidate flow. Built with the internal bitmap
font, no JavaScript, no network.

**Done when:** a `tools/shoot.sh` scenario opens a local page with `file://`,
follows a link, goes back, and the result is checked against the screen; and
the persistence check holds (`./ports/netsurf/build.sh`, then
`./build.sh build`, and `/bin/netsurf` is still there).

### Step 2 — HTTP

Prerequisites, each useful without the browser:

- **`getaddrinfo` in the SDK** (`<netdb.h>`), built from the DNS client that
  lives in Web Viewer today (see
  [NETWORKING.md](../net/NETWORKING.md#name-resolution-the-minimal-dns-client)).
  `ntpsync` and every other numeric-only tool get names for free.
- **libcurl**, built for HTTP only. It brings redirects, chunked transfers,
  cookies and keep-alive, which a hand-written fetcher would have to grow one by
  one. What curl asks of the socket layer — non-blocking `connect`, `poll` or
  `select` on sockets — is checked here, not assumed.

**Done when:** NetSurf loads the pages served by the HTTP test server Web Viewer
already uses, by name and by IP, and follows a 302. The time to load a page is
recorded: if the one-segment-in-flight TCP send path (see NETWORKING.md) makes
real pages slow, this port is the consumer that justifies a sliding window.

### Step 3 — HTTPS

Prerequisites:

- **A random source.** There is none today; `arc4random` is already listed as a
  selective port from OpenBSD in THIRD_PARTY_ADOPTION.md. TLS without it is not
  TLS.
- **BearSSL** under libcurl.
- **A CA bundle** in the image, with its provenance recorded.
- **A correct clock**, since certificate validity depends on it (`ntpsync`,
  [TIME.md](../kernel/TIME.md)).

**Done when:** a page loads over HTTPS from a local test server with a test CA,
and a page with a bad certificate is refused.

### Step 4 — parity, then the rest

NetSurf takes Web Viewer's place in the system:

- The Start menu entry points to `/bin/netsurf`, and disappears when the port is
  not installed, like Doom's (`progman_registry_prune_missing`).
- Web Viewer's shoot scenario gets a NetSurf twin covering the same ground:
  local file, HTTP fetch, link click, view source, redirect.
- Images (JPEG) and JavaScript (Duktape) are turned on, each with its own check.

**Done when:** everything Web Viewer's scenario asserts, the NetSurf scenario
asserts too, and passes.

### Step 5 — remove Web Viewer

Only after step 4. Everything goes in one commit, with a `Removed` entry in the
changelog:

- `subsystems/posix/userland/webview.c` and `webview.sxres`;
- its target in `cmake/UserPrograms.cmake` and its line in
  `progman_registry.c`;
- `diskfs/welcome.html`, rewritten for NetSurf or deleted;
- the `webview` scenario and its helpers in `tools/shoot.sh` and
  `tools/shoot_session.py`;
- `tools/webview_http_server.py`, `tools/webview_dns_server.py` and
  `tools/webview-test-www/`, renamed for the NetSurf scenario if it uses them,
  and their `.gitignore` lines;
- `docs/desktop/WEBVIEWER.md` and its entry in `docs/README.md`, plus the
  references to it in `NETWORKING.md`.

One consequence is accepted on purpose: a base image built without the port has
no browser. NetSurf comes with the port, like Doom.

**Done when:** `grep -ri webview` over the tree finds nothing outside the
changelog's history.

## What not to do

- **Do not copy NetSurf code into the MIT tree**, however small the piece.
- **Do not keep growing Web Viewer** in parallel. Two browsers is twice the work
  for one result.
- **Do not write a native SXGUI frontend before step 4.** The `fbtk` chrome does
  not look like the rest of the desktop, and that is acceptable until the port
  works; a native frontend is its own project, started from a browser that
  already works.
- **Do not turn on JavaScript early.** Duktape adds `nsgenbind`, a large binding
  layer and its own bugs to every step before it.
- **Do not pick a TLS library that needs an exception** when one that does not
  will do.
