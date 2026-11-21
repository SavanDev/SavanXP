# Web Viewer: the rendering contract

> Web Viewer runs on SUN as `/system/webview.sxe` (N1) and loads remote pages
> on a network worker with Stop (N3). Shared HTTP and validated HTTPS are the
> later network stages, before replacing this renderer with the pinned Ladybird
> engine. See the [roadmap](../roadmaps/open/WEBVIEWER_ROADMAP.md).

`/system/webview.sxe` ("Web Viewer", also launched as `webview`) is the in-tree
browser prototype, in the spirit of IE 1.1: chrome (address bar, Back/Forward
history, menus), local files and HTTP fetching by name or IP, and the rendering
pipeline `parse → cascade → wrap → paint` in `system/webview/webview.c`.

This document pins what it renders and what it openly ignores, so later
milestones extend a contract instead of rediscovering it.

## Native frontend and I/O

The frontend uses SUN's `sxngui_app` frame and compiled presentation resources;
its title, icon, menus and renderer stay the same. Local files use
`sxn_query_path`, `sxn_open_file` and partial `sxn_read` operations. TCP/UDP
handles use `sxn_socket_*`, `sxn_read`/`sxn_write` and `sxn_wait_io`; a pending
connection is checked with `sxn_socket_status` after readiness. These paths
make no POSIX calls.

The migration retains the optional initial path/URL, `--dns HOST:PORT`, the
`dns://` override, `file://`, history, links, redirects, Reload and View Source.
The build installs only `/system/webview.sxe` and retires its old `/bin` copies,
while preserving external applications and game assets.

### Loading in the background (N3)

Local files load on the UI thread; they are on the disk. An `http://` load runs
on the window's **network worker**, one thread that owns the window's DNS
resolver, its sockets and a body buffer of its own:

- The UI thread and the worker meet in single-slot mailboxes under one lock:
  the request, the latest progress line and the result. The worker wakes the
  window with `sxngui_app_wake`; `on_wake` takes the result on the UI thread,
  which alone touches widgets, the document and the history.
- Every navigation has an ID. A result or progress line for any other ID is
  dropped, so an obsolete page can never replace a newer one.
- A new navigation (typed, a link, Back/Forward, Reload), **Stop** and closing
  the window set the worker's cancel event, which every wait of the worker
  checks first: DNS, connect, write and read. Stop is the Go button (it reads
  `Stop` during a load), Esc, and Go > Stop.
- A failed or stopped load keeps the page on screen: the body lands in the
  worker's buffer and is copied into the document only on success. The address
  bar goes back to that page, unless the person is typing in it.
- The status line follows the worker: `Resolving <host>...`, `Connecting to
  <host>...`, `Waiting for <host>...`. History moves (a push, or Back/Forward's
  target) are applied only when the page arrives.

Budgets are monotonic: route setup six seconds, DNS replies two seconds per
attempt, connect and writes five seconds, reads ten seconds idle. A total
navigation budget arrives with shared HTTP (N4).

## Selectors and origins

- Tag selectors from `<style>` blocks (`h1`, `p`, `h1, h2`), later rules win.
- `style=""` on any element, winning over tag rules per property.
- `<body bgcolor text link>` attributes, winning over `body` rules.
- `body` rules set the page colours directly; there are no body paragraphs.
- Skipped, block-aware, never half parsed: classes, ids, pseudo-classes,
  `@`-rules, `<link rel=stylesheet>`.

## Properties

| Property | Renders as |
|---|---|
| `color`, `<font color>`, `<body text>` | Text colour: `#rgb`, `#rrggbb`, the 16 CSS1 names plus `grey` and `orange` |
| `background-color`, `background` (single colour), `<body bgcolor>` | Viewport background for `body`, block-width band behind other elements |
| `text-align: left/center/right` | Paragraph alignment (`justify` falls back to left) |
| `font-weight: bold` (or `>= 700`), `<b>`, `<strong>` | Double-strike bold, one pixel over, counted into wrap widths |
| `text-decoration: underline`, `<u>` | Underline in the run's own colour |
| `font-size` keywords | Bitmap faces quantize: `small` and below → UI, `large` and above → title; px/pt stay ignored |
| `<a href>` | Link blue (or `<body link>`), underlined, clickable |

Inline runs nest (`<b>a<span style="...">b</span></span>`) through a mark
system; explicit `normal`/`none` inside bold/underline undoes it. Links never
lose to run colours. `font-size` is paragraph-level only: runs do not change
faces. `font-style`, `line-through`, `blink`, `url()` and `rgb()` values are
parsed past and ignored.

## Names

Names resolve through SUN's shared resolver, `sxndns`, documented in
[`NETWORKING.md`](../net/NETWORKING.md#name-resolution-sxndns): numeric IPv4
never touches the network; anything else is one A/IN question to the window's
DNS server (10.0.2.3 under QEMU slirp), with CNAME chains, a TCP retry for a
truncated reply, replies believed only from the server's address and port, and
answers cached for their TTL. NXDOMAIN reads `No such host: <name>.`; every
other failure reads `Cannot resolve <name>:` and the resolver's own sentence
for it.

Two overrides exist for tests: `webview --dns 10.0.2.2:5353` at launch, and
`dns://10.0.2.2:5353` typed in the address bar (config, never history). A new
server empties the window's DNS cache.

## What it is not (yet)

No TLS, no images (an `[image]` marker), tables
degrade to stacked lines, fragments reload the page top, documents cap at 32 KiB / 256 paragraphs / 64 links / 256 runs / 32 rules /
4 KiB of CSS. Errors say which of these bit.

## Verification

`./tools/shoot.sh --scenario webview` boots the system, opens local files,
fetches from `tools/webview_http_server.py` over slirp, clicks a link, toggles
source, follows a 302, and asserts glyphs of `tools/webview-test-www/` against
the platform font tables. Glyph asserts use `solid_only` where anti-aliased edges
of neighbouring glyphs overlap: the harness blends against the background
while the painter blends in sequence (see the scenario comments).

The N3 steps load `/stall`, a download that never finishes, and check that the
window repaints the worker's progress and shows `Stop`, takes Ctrl+U during the
load, and keeps its page when Esc stops it; Stop during the resolution of a
name the DNS fixture never answers; a newer navigation replacing a stalled
one; and closing the window in the middle of a load.

The visual scenario also launches `webview --dns HOST:PORT URL`, checks F5,
launches `/system/webview.sxe /system/samples/welcome.html`, and opens a
`file://` URL.
The SUN socket contract is checked by `smoke native-network` and
`smoke native-network-offline`, including pending ARP, cancellation,
initialization without a NIC, route timeout and TCP refusal/reset/retry errors.
