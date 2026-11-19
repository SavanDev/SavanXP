# Web Viewer: the rendering contract

> Web Viewer runs on SUN as `/system/webview.sxe` (N1). Remote loads still
> block the UI: the worker and shared DNS/HTTP/validated HTTPS are later network
> stages, before replacing this renderer with the pinned Ladybird engine.
> See the [roadmap checkpoint](../roadmaps/open/WEBVIEWER_ROADMAP.md#implementation-checkpoint--2026-11-15).

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
Socket retries use monotonic budgets (route setup six seconds, DNS replies two
seconds per attempt, TCP connect and writes five seconds, reads ten seconds idle). This is still synchronous
loading on the UI thread; cancellation/Stop and worker ownership belong to N3.
The build installs only `/system/webview.sxe` and retires its old `/bin` copies,
while preserving external applications and game assets.

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

Numeric IPv4 never touches the network. Anything else resolves through the
minimal DNS client documented in [`NETWORKING.md`](../net/NETWORKING.md#name-resolution-the-minimal-dns-client):
one A/IN question over UDP to the network's DNS (10.0.2.3 under QEMU slirp),
with CNAME chasing, NXDOMAIN told apart from other failures, and every
overrun a rejection. Transaction IDs mix uptime with a counter (the tree has
no kernel RNG yet), and answers are trusted past TXID plus the echoed
question — fine on a host bridge, not against an adversary. No cache, no
AAAA, no TCP fallback: each gap says so in the status line.

Two overrides exist for tests: `webview --dns 10.0.2.2:5353` at launch, and
`dns://10.0.2.2:5353` typed in the address bar (config, never history).

## What it is not (yet)

No TLS, no images (an `[image]` marker), tables
degrade to stacked lines, fragments reload the page top, fetches block the UI,
documents cap at 32 KiB / 256 paragraphs / 64 links / 256 runs / 32 rules /
4 KiB of CSS. Errors say which of these bit.

## Verification

`./tools/shoot.sh --scenario webview` boots the system, opens local files,
fetches from `tools/webview_http_server.py` over slirp, clicks a link, toggles
source, follows a 302, and asserts glyphs of `tools/webview-test-www/` against
the platform font tables. Glyph asserts use `solid_only` where anti-aliased edges
of neighbouring glyphs overlap: the harness blends against the background
while the painter blends in sequence (see the scenario comments).

The visual scenario also launches `webview --dns HOST:PORT URL`, checks F5,
launches `/system/webview.sxe /system/samples/welcome.html`, and opens a
`file://` URL.
The SUN socket contract is checked by `smoke native-network` and
`smoke native-network-offline`, including pending ARP, cancellation,
initialization without a NIC, route timeout and TCP refusal/reset/retry errors.
