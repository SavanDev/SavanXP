# Web Viewer: the native browser

> **Status as of 2026-11-24: N1–N5, D1 and E1–E3 are implemented.** Web Viewer runs as
> `/system/webview.sxe`, resolves through SUN's `sxndns`, loads on a
> per-window network worker through SUN's `sxnhttp`, opens validated
> `https://`, and saves what it does not show in a download window of its
> own. It reads HTML through libhubbub and libdom
> ([WEBVIEWER_ENGINE.md](../../desktop/WEBVIEWER_ENGINE.md)) and builds its
> paragraphs and runs from the DOM using computed CSS. The next engine
> stage is E4.

## Direction

Decided 2026-11-21. Web Viewer is the system's **native browser**, written
against SUN, and it grows towards the modern web at its own pace. Two roles,
in this order:

1. **The browser that fetches another one.** Like Internet Explorer on a
   fresh Windows, its first job is to download installers and archives
   reliably over HTTP and validated HTTPS. That needs downloads (D1), not a
   modern renderer.
2. **A document browser that keeps improving.** Its engine grows by stages
   (E1–E7): NetSurf's **MIT libraries** read HTML into a DOM and resolve CSS
   into computed styles, and Web Viewer writes its own layout, paint and,
   later, scripting on top.

**The modern web is a compatibility goal, not Web Viewer's.** Pages that need
a current engine are for an unmodified Firefox or Chromium running under SSL
([LINUX_SUBSYSTEM_ROADMAP.md](LINUX_SUBSYSTEM_ROADMAP.md#l7--later-profiles))
or SSW ([WIN32_SUBSYSTEM_ROADMAP.md](WIN32_SUBSYSTEM_ROADMAP.md#w7--later-profiles)).
That is a long road -- a modern browser is among the most demanding programs
there are -- but every system call and API it pulls in serves every other
program of its subsystem, where an engine port serves one.

### How this was decided

The plan changed twice; the reasons matter for the next time it is weighed.

- **2026-11-03:** Web Viewer adopts NetSurf's MIT libraries and writes its own
  layout, with a clean-room rule for NetSurf's GPL core (commit `f4db268b`).
- **2026-11-09:** that plan is retired for integrating the Ladybird engine at a
  pinned C++ commit (`37ec6a08`), so that the native browser itself would reach
  the modern web (commit `5a0c626d`). The network stages N1–N5 were built
  under this plan and serve any engine.
- **2026-11-21:** Ladybird is retired, before W0 measured anything. Its cost,
  weighed against the system as it is: no C++ runtime in userland at all, Skia, ICU, HarfBuzz/FreeType,
  curl and media codecs to port, and a multi-process design whose IPC, shared
  memory and isolation SUN would have to grow first -- several times all the
  work before it, for one program. With the modern web assigned to the
  compatibility subsystems instead, the native browser does not need it, and
  the November 3 plan applies again, updated: Web Viewer is now SUN, its
  loading is asynchronous, and HTTPS goes through the OpenSSL port rather than
  BearSSL. The NetSurf browser is no longer a reference port; the comparison
  basis is below.

Nothing of N1–N5 is lost: DNS, HTTP, TLS, the worker and the CSPRNG are
engine-independent. OpenSSL 3.5.3 was pinned to match the Ladybird plan; it
stays, as a long-term-support release.

## Target architecture

The SUN frontend keeps Web Viewer's chrome, history, local files and identity.
Loading is `sxnhttp` on one network worker per window, with at most one active
navigation; the worker publishes results through synchronized mailboxes and
SUN events and never touches widgets or renderer data. Navigation IDs discard
obsolete results; Back, Forward, Reload, Stop and closing the window cancel;
a failed load keeps the page on screen.

The engine, from E2 on: HTML through libhubbub into a libdom document;
libcss, with Web Viewer's own selection handler and user-agent stylesheet,
computing styles; Web Viewer's box tree, layout and paint over SXGFX, in the
UI process. The libraries are built neutral, as the OpenSSL port is: they ask
the program only for C functions that both runtimes define, so the same
objects serve SUN and the host tests.

Scripting (E7) does not run remote code in the UI process without a decision
about isolation recorded in this document first.

## Network delivery (N1–N5, complete)

The record of what was delivered; the checkpoints below hold the evidence.

1. SUN IPv4 TCP/UDP handles, pending connection/error queries and cancellable
   readiness without changing `sxn_wait_many`; Web Viewer migrated to
   `/system/webview.sxe`, retiring only `/bin/webview`.
2. TCP with bounded independent servicing, correct asynchronous errors, FIN
   retransmission and TIME_WAIT, POSIX on the same core.
3. A shared SUN DNS resolver (numeric IPv4, A/CNAME, bounded TTL cache, source
   validation, TCP fallback for truncated UDP), `--dns` and `dns://` kept, and
   one network worker per window with cancellation and stale-result handling.
4. URL and HTTP/1.1 loading out of the renderer: explicit ports, relative
   redirects, Content-Length, chunked and close-delimited bodies, refusal of
   malformed, contradictory or incomplete responses; `Host`, `Connection:
   close`, `Accept-Encoding: identity`, a 32 KiB body cap, 16 KiB head cap,
   five redirects, no remote-to-file or HTTPS-to-HTTP redirect; monotonic,
   cancellable budgets (DNS 2×2 s, connect 5 s, TLS 10 s, idle 10 s, total
   30 s).
5. A kernel CSPRNG from real entropy, clock validity, the pinned and audited
   OpenSSL with the Mozilla CA store, and validated TLS 1.2/1.3 (chain,
   name/IP, dates, SNI, `http/1.1` only) with distinct trust, name, date,
   clock, entropy and transport errors, no bypass, and test CAs never
   installed.

| Stage | Completion gate |
|---|---|
| N1 — Native contracts and application migration | Existing local-file, argument, DNS-option, history and link workflows run as `/system/webview.sxe`; the migrated application makes no POSIX calls. |
| N2 — Transport progress and correctness | Pending transmissions progress without their consumer reentering the stack; losses, simultaneous operations, cancellation and close cause neither hangs nor leaks. |
| N3 — Shared DNS and worker | Slow DNS or stalled downloads leave repainting, movement, input and cancellation responsive; stale results are discarded. |
| N4 — Shared HTTP | Correct responses render independently of header, chunk and TCP fragmentation; contradictory framing and truncated bodies fail explicitly. |
| N5 — Validated HTTPS | Valid certificates work; expired, self-signed, wrong-name and invalid-chain certificates are rejected, without a bypass. |

## D1 — downloads

The IE role. Anything Web Viewer does not show, and anything the server marks
as an attachment, is saved to disk instead of failing or being cut at 32 KiB.

- **`sxnhttp` streams.** A body sink in the request (a callback taking each
  decoded piece), beside today's buffer, so a response of any length passes
  through without being held; the progress callback learns the expected
  length when `Content-Length` gives one. The framing rules do not change: a
  download cut short is `TRUNCATED`, never a short file.
- **Show or save.** `text/html` and `text/plain` render (images after E6);
  anything else, or `Content-Disposition: attachment`, is a download. The
  name comes from `Content-Disposition`'s `filename`, else the URL's last
  segment, sanitized (no separators, no control characters, no leading dot);
  the person confirms it in the Save As dialog (`sxnfiledlg`).
- **Writing.** Into `<name>.part` beside the destination, renamed on
  completion; deleted on failure, cancellation or window close. An existing
  file is never replaced without confirming it in the dialog.
- **The window.** A download dialog owned by the browser, after IE5's: the
  name, the source host, received and total size, the rate, and Cancel; on
  completion, Open Folder (Files) and Close. The page window stays usable,
  and its worker can load pages while a download runs.
- **Budgets.** The idle budget applies between reads; the 30-second
  navigation total does not apply to a download's body.

**Slices.** D1 lands in two:

- **D1a, the loader** -- landed 2026-11-22. `sxnhttp`'s head callback and
  body sink, the expected length in the progress callback, `Content-
  Disposition` and `sxnhttp_file_name`, and the total budget off a download's
  body ([HTTP.md](../../net/HTTP.md#downloads-the-head-and-the-sink)). The
  fixtures serve a 3 MiB blob five ways with its SHA-256; `native-network` and
  `https-smoke` check it byte-exact.
- **D1b, Web Viewer** -- landed 2026-11-22. Show or save, Save As, the
  `.part` file, the download worker and the download window
  ([WEBVIEWER.md](../../desktop/WEBVIEWER.md#downloads)). Two gaps closed on
  the way: **modeless windows** (`SAVANXP_WM_WINDOW_MODELESS` in windowd,
  `struct sxngui_window` in the SUN frame;
  [OWNED_WINDOWS.md](../../desktop/OWNED_WINDOWS.md#modeless-windows)), and
  SUN's `sxn_delete_path`. The visual scenario checks it, the disk read back
  on the host.

**External download check, 2026-11-23.** QEMU user networking (slirp), the
default in `./build.sh run`, let Web Viewer save Firefox 128.0 for Linux i686
over HTTPS. The 87,805,303-byte archive extracted from the guest disk matched
[Mozilla's SHA-256 manifest](https://archive.mozilla.org/pub/firefox/releases/128.0/SHA256SUMS).
The POSIX frame's modeless windows and the POSIX HTTP library's streaming are
pending parity work.

**Done when:** a multi-megabyte binary, with and without `Content-Length`,
chunked, and as an attachment, arrives byte-exact over HTTP and validated
HTTPS (`native-network`, `https-smoke` and a host-checked SHA-256);
cancellation, a truncated body and a closed window leave no partial file; the
visual scenario downloads a file through the dialog while the page window
keeps repainting; and, with a network, Web Viewer downloads a real
browser's Linux or Windows package. POSIX parity: the pending POSIX HTTP
library (HTTP.md) gains the same streaming when it exists.

## The engine stages

Each stage names its acceptance condition; each ends with the page corpus
(below) opened in Web Viewer.

### E1 — the libraries build

`vendor/netsurf/` with libparserutils, libwapcaplet, libhubbub, libdom (with
its hubbub binding, not the XML one) and libcss, pinned to releases chosen in
this stage, unmodified, each in the provenance registry. CMake builds them as
static libraries for SUN programs and for the host. A host test parses every
page of the corpus into a DOM and computes the style of every element.

Two facts measured in the upstream trees on 2026-11-03, confirmed at the
pinned releases on 2026-11-22: libparserutils builds without `iconv`
(`WITHOUT_ICONV_FILTER`: its own ASCII, ISO-8859-x, Windows-125x, UTF-8 and
UTF-16 codecs), and libcss generates part of its parser at build time from a
small host program (`css_property_parser_gen.c`), which CMake builds and runs.
The libraries must ask the program only for C functions both runtimes define,
checked the way `ports/openssl/link.sh` checks OpenSSL's.

**Done when:** `./build.sh test` runs the host test, and the same libraries
link into a SUN program that `nativetest` or its own scenario exercises.

**Landed 2026-11-22.** The checkpoint below has the details.

### E2 — HTML through hubbub and libdom

Web Viewer builds its paragraphs and runs from the DOM instead of its own
parser. The rendering does not change yet. The 32 KiB, 256-paragraph and
64-link caps go: the limit becomes memory.

**Done when:** the `webview` shoot scenario passes unchanged, and a page
larger than 32 KiB opens whole.

**Implemented 2026-11-23.** Local and remote pages grow in memory, and the
visual scenario exercises a page larger than 32 KiB and follows its last
link after 1100 linked paragraphs.

### E3 — styles through libcss

Web Viewer's own selection handler over libdom (libcss's
`css_select_handler`), its own user-agent stylesheet written from CSS 2.1's
Appendix D and HTML's rendering section, and stylesheets from `<style>`,
`style=""` and `<link rel=stylesheet>` fetched through `sxnhttp`. The current
renderer reads computed styles instead of its own cascade.

**Done when:** classes, ids, descendant and child selectors, pseudo-classes
such as `:link` and `:first-child`, and external stylesheets render in the
corpus, and WEBVIEWER.md describes the new contract.

**Implemented 2026-11-24.** The renderer uses computed styles from the
browser defaults, HTML color hints and author CSS, including external sheets
fetched by the navigation worker. The visual corpus covers the cascade and
cancellation while waiting for CSS. See [WEBVIEWER.md](../../desktop/WEBVIEWER.md#selectors-and-origins).

### E4 — the box tree and normal flow

Boxes from the DOM and computed `display`, then block and inline layout:
widths, margins with collapsing, padding, borders, line boxes,
`vertical-align` for text, `white-space`, list items. The largest stage: it
replaces "paragraphs that wrap" with a layout engine.

**Done when:** a page of nested blocks with margins, padding, borders and
lists lays out as its corpus expectation says, box for box, up to font
metrics.

### E5 — floats and positioning

`float` and `clear`, then `position: relative` and `absolute`, then `overflow`
on blocks.

**Done when:** a float-based two-column page and a page of absolutely
positioned boxes match their expectations.

### E6 — tables and images

Automatic table layout, replacing "tables degrade to stacked lines". Images
through the system's decoders (`libsximg`), with libnsgif and libnsbmp where
they add formats, replacing the `[image]` marker and taking part in layout.

**Done when:** a table-based page with images matches its expectation, and
images arrive over HTTP and HTTPS without blocking the window.

### E7 — scripting

A JavaScript engine in C under the MIT license -- QuickJS, or the QuickJS-ng
fork, chosen and pinned in this stage -- with bindings to libdom for the DOM
subset the corpus needs (document and element access, events, timers), run on
the UI thread's loop. Before it runs remote scripts, this document records the
isolation decision: a separate content process, or scripts limited to local
and explicitly trusted pages until there is one.

**Done when:** corpus pages that change the DOM from a script, on load and on
an event, render the result, and a script that loops forever is stopped
without hanging the window.

### Later

Forms and POST, cookies, keep-alive and caching, fragment navigation inside a
page, `flex` and `grid`, web fonts, printing. Each one gets its own stage here
when it is next.

## The clean-room rule

NetSurf's core (the browser itself, its box construction, layout, redraw, the
libcss select handler and its default stylesheet) is GPL-2.0-only; Web Viewer
is MIT. That code does not enter Web Viewer and is not its model either. Each
piece is written from its specification: CSS 2.1's visual formatting model
and Appendix D, HTML's rendering section, and the libraries' own MIT headers
and documentation.

## Acceptance and comparing

The corpus is `tools/webview-test-www/`, one page or more per stage. Each
layout page carries its expected box geometry as data, written from the
specification (with a reference browser's rendering on the host as a check
when one helps); Web Viewer dumps its box tree in a test mode, and the
comparison tolerates the difference in font metrics. The visual scenario
keeps asserting glyphs on screen. Run `./build.sh smoke sxgui-smoke` when a
stage touches a drawing program, the loader or SXGUI, and
`./tools/verify_doom_persistence.sh` for build, SDK or image-flow changes.

## What not to do

- **Do not read NetSurf's core to write Web Viewer.** See the clean-room rule.
- **Do not patch the vendored libraries in place.** A needed change goes
  upstream or into a documented glue file beside them.
- **Layout uses the computed styles from E3.** Keep selector matching and
  the cascade in the engine when replacing paragraph layout in E4.
- **Do not chase the modern web here.** A page that needs a current engine is
  the compatibility subsystems' target, not a reason to widen a stage.

## Adoption and maintenance

The NetSurf libraries are `Adopt`: pristine pinned sources under
`vendor/netsurf/<library>/`, each in the
[provenance registry](../../third-party/THIRD_PARTY_PROVENANCE.md) in the commit
that vendors it, with its notice in the image. The JavaScript engine, when E7
chooses it, follows the same rule. Updates are explicit changes with their own
corpus run; security fixes in the parsers are taken when published.

## Implementation checkpoints

The stages as they landed, newest first. Their "next step" lists and
mentions of the Ladybird plan are history; the stages above are current.

## E1 implementation checkpoint — 2026-11-22

**E1 is complete.** The details are in
[WEBVIEWER_ENGINE.md](../../desktop/WEBVIEWER_ENGINE.md).

- **Vendored:** libparserutils 0.2.5, libwapcaplet 0.4.3, libhubbub 0.3.8,
  libdom 0.4.2 and libcss 0.9.2 under `vendor/netsurf/`, unmodified and
  pinned by tarball SHA-256, with their entry in the provenance registry and
  notices under `/system/licenses`.
- **Built:** `cmake/NetSurf.cmake` builds them as static archives for SUN
  programs and for the host. It generates upstream's files with Perl, gperf
  and libcss' own generator. gperf is a new build requirement.
- **Checked:** the SUN archives ask the program only for C functions SUN's
  runtime answers without a POSIX call; `tools/check_archive_symbols.py`
  enforces it.
- **The glue:** `system/webview/engine/` is the parse, the stylesheets, the
  style walk with composition, and the selection handler over libdom,
  written from the libraries' headers.
- **Tests:** `webengine-test` on the host (the self-test and the corpus) and
  the `webengine-test` scenario (`/system/enginetest.sxe`, a SUN PIE).

Two gaps the stage found and closed:
- **The SDK:** `<string.h>` now declares `strncasecmp` under
  `_DEFAULT_SOURCE`, as glibc and musl do.
- **The C runtime's allocator:** it walked every block on each `malloc`, which
  made a DOM of 4000 elements take 100 s to style in the guest. It now walks
  free blocks only, and the same page takes under a second.

One cost left for E3: libcss' style sharing is quadratic in the number of
same-named siblings once a sibling combinator applies to them
([WEBVIEWER_ENGINE.md](../../desktop/WEBVIEWER_ENGINE.md#known-costs)).

## N5 implementation checkpoint — 2026-11-21

**N5 is complete, and with it the network-first stages.** Web Viewer opens
validated `https://`; the next stage is D1, downloads (above). N5
landed in four slices, each with its own commit:

- **N5a**, the kernel CSPRNG ([RANDOM.md](../../kernel/RANDOM.md)): seeded only
  from virtio-rng or RDSEED, refusing while unseeded, with SUN, POSIX and Linux
  interfaces.
- **N5b**, clock validity (`sxn_clock_status`, `clock_status`;
  [TIME.md](../../kernel/TIME.md#can-the-date-be-believed)): whether the date
  can be believed, its source, and why not. A prerequisite in the SDK put the
  C library's clocks, randomness and identity on native calls in SUN programs
  and added `atexit`.
- **N5c**, the OpenSSL 3.5.3 port and the Mozilla CA store
  ([ports/openssl/README.md](../../../ports/openssl/README.md)), pinned and in
  the provenance registry: `libcrypto.so.0.4` and `libssl.so.0.4`, with no file
  system below them, and `/system/ssl/cert.pem`.
- **N5d**, TLS in `sxnhttp` ([HTTP.md](../../net/HTTP.md#https)) through
  `system/runtime/sxnhttp_tls.c`, its fixture `tools/tls_fixture.py`, and Web
  Viewer's lock. Web Viewer is now built only with the OpenSSL port.

### N5 acceptance evidence

| Gate | Evidence |
|---|---|
| Valid certificates work | `https-smoke`: a leaf with its intermediate loads by name (TLS 1.3, SNI and `http/1.1` as the server saw them) and by IPv4 address, against the fixture's root handed over in memory. |
| Expired, self-signed, wrong-name and invalid-chain certificates are rejected, without a bypass | `https-smoke`: `TLS_DATE` for expired and not yet valid, `TLS_UNTRUSTED` for self-signed, a missing intermediate and an untrusted CA, `TLS_NAME` for another site's certificate, `TLS_HANDSHAKE` for a TLS 1.1 server; the system store rejects the test root. With verification turned off in the backend, all seven fail the scenario. Web Viewer's scenario shows the refusal and keeps the page; there is no override anywhere. |
| Entropy and clock failures are explicit | `random-unseeded` and `https-unseeded` (`TLS_ENTROPY`, and OpenSSL itself refuses: `openssl-sun` fails without entropy); `https-smoke` sets 2005 and gets `TLS_CLOCK`; `date-smoke` checks the clock's status as it moves. |
| Test CAs only in test images | The fixture's root is generated per run on the host and handed to the test in memory (`trust_pem`); it is never written to an image. |

## N4 implementation checkpoint — 2026-11-21

**N4 is complete.** URL handling and HTTP/1.1 left Web Viewer for a SUN
library, `include/sxn/sxnhttp.h`, that the worker calls. Its prerequisite,
reclaiming `TIME_WAIT` slots when the socket table is full, landed first.

- Requests carry `Host`, `Accept-Encoding: identity` and `Connection: close`.
  Content-Length, chunked and close-delimited bodies are read from any
  fragmentation; contradictory framing, malformed heads, truncated bodies,
  heads past 16 KiB and non-identity encodings fail, each with its own error.
  The body cap is the caller's buffer (32 KiB in Web Viewer), kept and marked
  as cut rather than failed.
- Up to five redirects, resolved by RFC 3986; never to `file://`, never from
  https down to http, `https://` refused until N5.
- Budgets are monotonic and cancellable: connect 5 s, idle 10 s, total 30 s.
  TLS's 10 s belong to N5.
- Contracts are in [HTTP.md](../../net/HTTP.md) and
  [WEBVIEWER.md](../../desktop/WEBVIEWER.md#loading-in-the-background-n3).

### N4 acceptance evidence

| Gate | Evidence |
|---|---|
| Correct responses render independently of header, chunk and TCP fragmentation | `sxnhttp-message-test` feeds every response whole, split at every byte and byte by byte; `native-network` loads each framing from `tools/http_fixture.py`, which sends every response in small pieces; the webview scenario renders a chunked page sent 11 bytes at a time. |
| Contradictory framing and truncated bodies fail explicitly | The host test covers each framing error and truncation at every cut of a good response (two parser mutations each make it fail); `native-network` gets `BAD_FRAMING`, `TRUNCATED` (close and reset), `BAD_RESPONSE`, `HEADERS_TOO_LARGE` and `UNSUPPORTED_ENCODING` from the fixture; the webview scenario shows the message and keeps the page. |
| Redirects, timeout, cancellation | `native-network`: a relative redirect, five allowed and the sixth refused, file and https targets refused, the total and idle budgets, cancellation from a thread mid-body. |

## N3 implementation checkpoint — 2026-11-21

**N3 is complete.** Slow DNS and stalled downloads no longer hold the window:
remote loading runs on one network worker per window, with Stop, cancellation
and stale results discarded, over the shared SUN resolver. The next stage is
N4; the TIME_WAIT reclamation planned before it has landed (2026-11-21,
[NETWORKING.md](../../net/NETWORKING.md#closing-keeps-protocol-state-alive)).

### The worker

- `sxngui_app_wake` and the `on_wake` hook (`include/sxn/sxngui.h`) let any
  thread wake the SUN frame; the hook runs on the UI thread. POSIX's
  `sxgui_app.c` has no counterpart yet, recorded as pending in
  [PLATFORM_GRAPHICS.md](../../desktop/PLATFORM_GRAPHICS.md).
- Web Viewer's worker owns the resolver, the sockets and its own body buffer.
  Request, progress and result cross in single-slot mailboxes under a futex
  lock; navigation IDs drop anything obsolete. A new navigation, Back/Forward,
  Reload, Stop (the Go button, Esc, Go > Stop) and window close set the
  worker's cancel event, checked first in every wait. Failed and stopped loads
  keep the displayed document; history moves apply only when a page arrives.
- Contracts are in [WEBVIEWER.md](../../desktop/WEBVIEWER.md#loading-in-the-background-n3).

### N3 acceptance evidence

| Gate | Evidence |
|---|---|
| Slow DNS and stalled downloads leave the window responsive | `./tools/shoot.sh --scenario webview`: during `/stall` (a download that never ends) the status follows the worker, Go reads `Stop`, and Ctrl+U toggles the source of the page on screen; Esc stops and keeps the page. |
| Cancellation | Stop during resolution of a name the fixture never answers returns `Stopped.` at once; closing the window mid-load ends the process. |
| Stale results discarded | A typed navigation replaces a stalled one and only the newer page appears. |
| Resolver | `sxndns-message-test` on the host; `native-network` against `tools/dns_fixture.py`. |

### The shared resolver

- `include/sxn/sxndns.h` (`system/runtime/sxndns.c`, in every native
  program's runtime): numeric IPv4, A/CNAME with chains in one reply and
  across up to four further questions, a 16-entry cache bounded by TTL and
  five minutes, replies believed only from the server's address and port for
  the TXID and question asked, a TCP retry for truncated replies assembled
  from any number of reads, and a manual-reset cancel event checked first in
  every wait. Budgets: six seconds for the route, two attempts of two seconds.
- A resolver is caller-owned and single-threaded; changing its server empties
  its cache. Web Viewer keeps one per window and keeps `--dns` and `dns://`.
- The wire format is a pure module with its own host test
  (`sxndns-message-test`); `native-network` checks the resolver against
  `tools/dns_fixture.py`, which the echo server starts on one port number for
  UDP and TCP.

Contracts and limits are in
[NETWORKING.md](../../net/NETWORKING.md#name-resolution-sxndns). POSIX has no
resolver yet; that gap is recorded there as pending.

### Next implementation step — N4

Extract URL and HTTP/1.1 loading from Web Viewer into a reusable SUN library
that the worker calls, with the framing, redirect, limit and budget rules of
step 4 above, including the 30-second total navigation budget. Its
prerequisite, reclaiming a `TIME_WAIT` slot when the socket table is full, has
landed.

## N2 implementation checkpoint — 2026-11-16

**N2 is complete.** The shared TCP core has independent progress, bounded NIC
servicing, retained asynchronous errors and detached close state. N1 remains
complete; N3–N5 and Ladybird remain pending. The next implementation stage is N3.

- The BSP heartbeat services routes, RX, queued TX, retransmissions and close
  before waking readers/waiters, even when no consumer reenters the stack.
  Monotonic elapsed time supplies deadlines. The existing big kernel lock
  serializes SUN/POSIX calls, timer entries and NIC IRQs; callbacks do not sleep.
- RX handles at most 16 frames per pass; TX drains at most 16 before and after
  RX through a bounded 64-frame queue. Drivers submit DMA without waiting for
  completion. Queued TCP frames carry an owner token for cancellation before
  socket reuse. The single outstanding 1 KiB TCP segment is retained.
- Final release drains accepted stream data before queuing FIN. FIN retries,
  active/passive/crossed close, 60-second TIME_WAIT with duplicate-FIN replies,
  and bounded orphan expiry retain protocol state independently of handles.
  Attached and detached sockets share the 32-slot budget. The read-only
  `NET_IOC_GET_TCP_LIFETIMES` query observes reclamation without driving it.
- Retry/ACK failures remain available through status, readiness and I/O. ACKs
  beyond sent sequence space cannot clear pending TX; blocking writes check
  transport failure after the pending slot is disarmed. Failed I/O remains
  ready; POSIX `poll` reports `POLLERR` even without requested interests.

The maintained contracts and limits are in
[NETWORKING.md](../../net/NETWORKING.md#independent-bounded-and-synchronized-progress)
and [SUN.md](../../userland/SUN.md#ipv4-socket-foundation).

### N2 acceptance evidence

| Gate | Evidence |
|---|---|
| Build and host tests | Build succeeded with `check_shared_libs: OK`; `100% tests passed out of 54`, with three Win32 fixture tests skipped because `SAVANXP_WIN32_FIXTURES` is unset. |
| Independent progress and close | `tcp-smoke`: simultaneous unpolled writes retry; closing preserves the exact bytes saved by the peer; dropped FIN retries; exhausted FIN and a half-open peer reclaim state; TIME_WAIT reserves the port and expires after its real interval. |
| Native concurrency and errors | `native-network`: distinct threaded streams under loss, independent SYN retries and accepted-data timeout, cancellation/close during setup and a retained stream wait; existing refusal/reset/route/EOF assertions pass. |
| General regression | `native-network-offline`, `native-test`, `net-smoke`, `smoke` and `sxgui-smoke` passed. |
| Additional driver | `native-network`, `tcp-smoke` and `net-smoke` also passed with `--virtio`. |
| Visual regression | `webview: OK`, with 25 captures; local files, history, links, CSS, DNS/HTTP, redirects, source, arguments and Reload. |
| Persistence | `./tools/verify_doom_persistence.sh --jobs 8`: `Doom persistence: PASS`, using the deterministic WAD fixture. |

The tests run under QEMU/TCG with two CPUs. The visual check's extracted SUN
executable matches the staged binary by SHA-256 and has `EI_OSABI=0x53`; both
retired POSIX Web Viewer copies are absent. Physical RTL8168 hardware was not
available for verification.

### Next implementation step — N3

Extract the shared SUN resolver and move remote loading to one worker per
window. Preserve DNS overrides, use synchronized queues/events and navigation
IDs, and make Stop, navigation and close cancel work while the UI remains
responsive. Loading still runs synchronously in Web Viewer; N2 does not claim
background loading, shared DNS/HTTP, HTTPS or a new renderer.

**Carried limit: TIME_WAIT shares the 32 socket slots.** Rapid active closes
can fill the table for up to 60 seconds, and a new socket then fails with
`ENOMEM`. Before N4 opens many short HTTP connections, a full table should
reclaim a `TIME_WAIT` slot, the one closest to expiry, instead of refusing,
with a `tcp-smoke` assertion that opens more sockets than the table holds.
See [NETWORKING.md](../../net/NETWORKING.md#closing-keeps-protocol-state-alive).

## N1 implementation checkpoint — 2026-11-15

**N1 is complete.** This records N1's acceptance; the N2 checkpoint above
supersedes its next-step list. The remaining setup/route contract and the application
migration were delivered as separate changes. The historical checkpoints below
record earlier partial states; this checkpoint supersedes their next-step lists.

| Stage | Current state | Remaining work |
|---|---|---|
| N1 | Complete: cancelable native setup/ARP waits, retained transport errors, and `/system/webview.sxe` with its existing workflows. | None for this gate. |
| N2 | Pending; progress still depends on consumers or parked waiters. | Independent bounded and synchronized servicing, complete asynchronous error handling, FIN retransmission and TIME_WAIT. |
| N3 | Pending; native DNS and loading still run synchronously in Web Viewer. | Shared resolver/cache/validation/TCP fallback, worker, cancellation, Stop and stale-result handling. |
| N4 | Pending; URL/HTTP handling remains in the application. | Reusable result contract, strict framing, redirects, limits, deadlines and protocol tests. |
| N5 | Pending at this checkpoint; complete on 2026-11-21 (see the N5 checkpoint above). | Entropy and valid civil clock, audited shared OpenSSL, CA store, certificate validation and TLS fixtures. |

### Delivered for N1

- Native sockets advance NIC reset/link negotiation and ARP without sleeping
  inside socket operations. Write/error readiness carries route completion;
  events cancel waits and last close releases pending route state. The shared
  TCP pump also avoids blocking on a POSIX socket's ARP. The six-second route
  budget and UDP retry contract are maintained in
  [SUN.md](../../userland/SUN.md#ipv4-socket-foundation).
- `native-network` checks pending setup/ARP, cancellation, route timeout,
  pending read/write, invalid user buffers, TCP success/refusal/reset/retry
  timeout, partial reads, EOF and handle lifetime. `native-network-offline`
  checks retained `ENODEV` through native status, readiness and I/O.
- Web Viewer uses SUN files, sockets, waits, clock and the `sxngui_app` frame,
  with compiled resources and no POSIX calls in its application paths. Its
  interface, arguments, local files/URLs, DNS overrides, HTTP, history, links,
  redirects, Reload, CSS and View Source remain covered by the visual scenario.
  Only the old Web Viewer executables are retired by this migration.

### Acceptance evidence

| Command | Observed result |
|---|---|
| `./build.sh build` | Passed; `check_shared_libs: OK`, valid persistent image synced. |
| `./build.sh test` | `100% tests passed out of 53`; three Win32 fixture tests skipped because `SAVANXP_WIN32_FIXTURES` is unset. |
| `./build.sh smoke native-network` (RTL8139 and `--virtio`) | `NATIVE NETWORK PASS`. |
| `./build.sh smoke native-network-offline` | `NATIVE NETWORK OFFLINE PASS`. |
| `./build.sh smoke native-test`, `net-smoke`, `tcp-smoke`, `smoke` | Their respective `PASS` tokens. |
| `./build.sh smoke windowd-smoke`, `taskbar-smoke`, `sxgui-smoke` | Their respective `PASS` tokens, including editor glyph/scroll and taskbar checks. |
| `./tools/shoot.sh --scenario webview` | `webview: OK`; existing flows plus native argument/DNS, Reload and file-URL checks. |
| `./tools/verify_doom_persistence.sh --jobs 8` | `Doom persistence: PASS`, with the deterministic WAD fixture. |

The visual check's extracted executable matches the built SUN image by SHA-256
and carries `EI_OSABI=0x53`, with both old `/bin` copies absent. Network tests
also run with `native-network --virtio`; no physical RTL8168 hardware was
available for verification.

### Next implementation step — N2

Give the shared TCP core bounded independent and synchronized servicing, so
pending traffic progresses even without a consumer calling or waiting in the
stack. Extend `tcp-smoke` for progress without consumer polling, simultaneous
connections, close during operations and lost FIN; implement FIN retransmission
and TIME_WAIT. Keep POSIX on that same core and initially retain one outstanding
segment. Do not mark N2 complete from N1's waiter-driven network checks.

## Historical checkpoint — 2026-11-09

**Only the socket foundation portion of N1 is implemented. No network stage is
complete, and Web Viewer itself has not been migrated or changed by this slice.**
This checkpoint records development work; it does not identify a release or
claim that the complete network delivery is available.

| Stage | Current state | Remaining work |
|---|---|---|
| N1 | Partial: SUN socket handles and interest-based waits exist; success, refusal and retry timeout are tested. | Finish asynchronous initialization/ARP and the remaining transport contract, then migrate Web Viewer separately and verify all existing workflows. |
| N2 | Pending; POSIX continues using the existing shared network core. | Independent bounded and synchronized servicing, complete asynchronous error handling, FIN retransmission and TIME_WAIT. |
| N3 | Pending; Web Viewer still uses its own synchronous DNS/loading code. | Shared resolver, TTL cache, sender validation, TCP fallback, worker, cancellation, Stop and stale-result handling. |
| N4 | Pending; the existing application still owns URL/HTTP handling. | Reusable loading result contract, URL/HTTP extraction, strict framing, redirects, limits, deadlines and protocol tests. |
| N5 | Pending; HTTPS is not enabled. | CSPRNG/virtio-rng, SUN entropy and valid civil clock, audited shared OpenSSL, versioned CA store, certificate validation and TLS fixtures. |

### Implemented foundation

- `sxn_socket_create`, `sxn_socket_bind`, `sxn_socket_connect`,
  `sxn_socket_status`, `sxn_socket_sendto` and `sxn_socket_recvfrom` call the
  network core directly, without POSIX process state. TCP streams use SUN
  read/write, including partial operations; duplicate/close use native handles.
- Connection initiation can return `-EAGAIN` while the SYN exchange is pending.
  Status distinguishes pending, established and retained transport failure;
  callers must query it after readiness rather than assuming success.
- `sxn_wait_io` combines read/write/error interests with waitable objects such
  as a cancellation event. Existing `sxn_wait_many` semantics are unchanged.
  The new wait retains object references through completion, including when
  another thread closes a handle. Socket calls also hold a reference across
  network-core operations that may sleep.
- `nativetest network` and `smoke native-network` exercise UDP loopback/source
  addresses, duplicate/close, timeouts, event cancellation, closing a handle
  during a wait, TCP establishment, fragmented reads and orderly EOF.

The maintained API contract and limits are in
[SUN.md](../../userland/SUN.md#ipv4-socket-foundation). Implementation entry
points are the [native ABI](../../../include/abi/savanxp_native_abi.h),
[SUN wrappers](../../../include/sxn/sxn.h),
[native dispatcher](../../../kernel/native_dispatch.cpp),
[wait core](../../../kernel/sx.cpp),
[network core](../../../kernel/net.cpp) and
[nativetest](../../../sdk/nativetest/main.c).

### Known limits and next implementation step

NIC initialization and ARP resolution may still wait inside the network core:
these APIs do not yet provide fully asynchronous transport. Retransmission
progress still depends on network servicing, including servicing for parked
waiters. Independent periodic progress, bounded concurrent servicing, FIN
retransmission and TIME_WAIT have not been delivered. Pending-connect success,
refusal and retry timeout are covered by the intermediate step below; this is
not complete coverage of every transport error or concurrent operation.

Resume at N1's remaining transport contract: make initialization/address
resolution cancelable without blocking the window's worker protocol and cover
the remaining transport/I/O failure paths with `nativetest` assertions. Then
migrate Web Viewer in a separate change, preserving its visible identity,
arguments, local files, history, links and DNS overrides. Do not mark N1 done
based solely on the socket smoke or retire the current executable early.

### Verification recorded for this foundation

The implementation session completed these checks successfully:

| Command | Result and coverage |
|---|---|
| `./build.sh build` | Passed; the existing valid persistent image was synced. |
| `./build.sh smoke native-network` | Passed with the local TCP fixture, including cancellation and concurrent handle close. |
| `./build.sh smoke native-test` | Passed the existing native ABI regression suite. |
| `./build.sh smoke net-smoke` | Passed the existing network regression scenario. |
| `./build.sh smoke tcp-smoke` | Passed the existing POSIX TCP loss/reordering scenario. |
| `./tools/verify_doom_persistence.sh --jobs 8` | Passed the isolated external executable and persistent-asset check. |
| `git diff --check` | Passed. |

These checks do not establish independent TCP progress, FIN/TIME_WAIT behavior,
a responsive browser, shared DNS/HTTP correctness or TLS validation. Browser
visual checks, `sxgui-smoke`, the expanded transport scenarios and DNS/HTTP/TLS
fixtures remain acceptance work for their corresponding changes. No browser or
renderer binary was changed in this slice.

### Intermediate step — pending TCP connection contract

**Completed and verified on 2026-11-09. Work stops at this intermediate gate;
N1 remains partial.**

This step is limited to completion of a pending connection: success, refusal
and timeout. It does not migrate Web Viewer or add independent TCP progress.

- A completed handshake reports zero through `sxn_socket_status`; a pending
  handshake reports `-EAGAIN`. Starting another connect while pending or
  established returns `-EBUSY`.
- A rejected handshake reports `-ECONNREFUSED`, distinct from the
  `-ECONNRESET` of an established connection reset. Exhausting the SYN retry
  budget reports `-ETIMEDOUT`. Failures wake write/error interests and remain
  available through repeated status queries and I/O; readiness alone is never
  evidence of a successful connection.
- Expiring `sxn_wait_io` leaves the socket pending. A deliberate new connect
  attempt replaces a retained failure. The existing blocking `connect` also
  stops retransmitting after its own monotonic deadline expires.
- Pending handshakes only accept a SYN-ACK or reset acknowledging the SYN;
  unrelated ACKs no longer disarm its retransmission slot.

`smoke native-network` tests the SUN success path, error-only polling on an
established connection, a host port reserved without a listener for refusal,
and the normal retry budget with every outgoing SYN dropped by the existing
fault injector. It checks wait expiry versus socket timeout, repeated status,
read/write errors, write/error readiness and a new attempt after timeout. The
fault injector is restored on exit. `tcp-smoke` additionally checks that a
short blocking-connect timeout leaves no SYN retransmission after servicing
the stack beyond the initial RTO.

The host fixture's `REFUSE` command returns a reserved, non-listening TCP port
and keeps it reserved for the lifetime of the control connection. Its host
protocol test checks that this port refuses connections, along with the
existing echo/bulk/delay commands.

Verification for this intermediate step:

| Command | Result |
|---|---|
| `./build.sh build` | Passed. |
| `python3 tests/host/tcp_echo_server_test.py --server tools/tcp_echo_server.py` | Passed, including the refusal-port fixture. |
| `./build.sh smoke native-network` | Passed, including success, refusal, wait expiry and SYN retry timeout. |
| `./build.sh smoke native-test` | Passed. |
| `./build.sh smoke net-smoke` | Passed. |
| `./build.sh smoke tcp-smoke` | Passed, including blocking-connect timeout cleanup. |
| `./build.sh smoke sxgui-smoke` | Passed the editor glyph/scroll and taskbar visual checks. |
| `./tools/verify_doom_persistence.sh --jobs 8` | Passed. |
| `git diff --check` | Passed. |

The remaining transport work listed above is not part of this completed step.
No asynchronous ARP, independent TCP servicing, Web Viewer migration or browser
loading changes have been started by this step.
