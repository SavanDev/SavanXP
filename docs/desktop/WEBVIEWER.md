# Web Viewer: the rendering contract

> Web Viewer runs on SUN as `/system/webview.sxe` (N1), loads remote pages on a
> network worker with Stop (N3) through SUN's HTTP loader (N4), over validated
> HTTPS (N5), and saves what it does not show (D1). It parses HTML through
> libhubbub and libdom (E2). Computed CSS styles are next (E3). See the
> [roadmap](../roadmaps/open/WEBVIEWER_ROADMAP.md).
>
> It is built only when the OpenSSL port is (`ports/openssl/build.sh`, then
> `./build.sh build`); without it the image has no Web Viewer and the registry
> drops it.

`/system/webview.sxe` ("Web Viewer", also launched as `webview`) is the in-tree
browser prototype, in the spirit of IE 1.1, with an address bar, history, menus,
local files and HTTP fetching by name or IP. libhubbub builds a libdom tree.
Web Viewer walks it into paragraphs and inline runs, then applies its tag-rule
cascade, wraps the text and paints it.

This document pins what it renders and what it openly ignores, so later
milestones extend a contract instead of rediscovering it.

## The window

Laid out after Internet Explorer 5 on Windows 2000, in bands ruled off by
etched lines as in Files:

- **Toolbar**: Back, Forward | Stop, Refresh, Home | Open, Source, each a
  20x20 glyph over its caption. Back and Forward are the shared glyphs of
  `system/gfx/toolbar_glyphs/`; the rest are Web Viewer's own, drawn by
  `system/webview/gen_icons.py` ([ICON_STYLE.md](ICON_STYLE.md)). Buttons that
  do not fit a narrow window are hidden, not squeezed.
- **Address bar**: the `Address` label, the field and Go.
- **The page**, and under it the **status bar**: the status line and a
  progress pane the worker fills while it resolves, connects and waits. When
  the response has a Content-Length, the progress bar tracks the bytes received.

One function (`webview_refresh_commands`) enables the toolbar and the menus,
so a grey button and a grey menu item never disagree: Stop only while a page
loads, Refresh and Source once there is a page, Back and Forward with the
history. Menus: File (Open, Exit), Go (Back, Forward, Home Page), View (Stop,
Refresh, Top and End of page, View Source), Help. A right click on the page
opens its context menu (Back, Forward, Refresh, View Source). Home opens
`file:///system/samples/welcome.html`.

The window title names the page (`<title> - Web Viewer`), and so does its
taskbar button. With the pointer on a link the status bar shows where it
leads, as an absolute URL; the next status message takes the bar back, and
the link shows again when the pointer moves.

## Native frontend and I/O

The frontend uses SUN's `sxngui_app` frame and compiled presentation resources;
its icon and renderer stay the same. Local files use
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
  checks first: DNS, connect, write and read. Stop is the toolbar's Stop,
  Esc, and View > Stop.
- A failed or stopped load keeps the page on screen. The worker transfers
  ownership of the complete body through the result mailbox. The address
  bar goes back to that page, unless the person is typing in it.
- The status line follows the worker: `Resolving <host>...`, `Connecting to
  <host>...`, `Waiting for <host>...`. History moves (a push, or Back/Forward's
  target) are applied only when the page arrives.

The worker loads through `sxnhttp_get` ([HTTP.md](../net/HTTP.md)), which
owns the request, the framing, redirects and the budgets: DNS as `sxndns`
sets them, connect and writes five seconds, ten seconds idle between reads,
thirty for the whole navigation. What Web Viewer adds:

- A failure shows the loader's sentence in the status bar, `No such host:
  <name>.` for NXDOMAIN, and keeps the page on screen. A response with
  contradictory framing, a body cut short or a malformed head is a failure,
  never a short page.
- A status other than 200 renders its page with `(HTTP <status>)` after the
  title in the status bar. Web Viewer accumulates the response through
  `sxnhttp`'s body sink and keeps the 30-second navigation budget.
- `text/plain` is shown line by line, as the source view shows HTML.
- Redirects go only to other `http://` or `https://` URLs: one to `file://` is
  refused (an http page could otherwise open local files), and so is one from
  https down to http.
- `https://` is validated against the system's trust store
  ([HTTP.md](../net/HTTP.md#https)). A page that arrived over it shows a lock
  in the status bar's pane between the text and the progress bar, and
  `(secure, TLSv1.3)` after its title. A certificate or handshake failure
  says which in the status bar -- untrusted, another site's, expired, the
  clock, no randomness -- with the host and the verifier's words, and the
  page on screen stays. There is nothing to click through.
- Links resolve against an http page by RFC 3986 through `sxnhttp_resolve`,
  dot segments and ports included; local paths keep Web Viewer's own rules.

## Downloads

Web Viewer's first role is the browser that downloads another one (D1 in the
[roadmap](../roadmaps/open/WEBVIEWER_ROADMAP.md)). `system/webview/download.c`.

- **Show or save.** The page worker's `sxnhttp` request carries a head
  callback ([HTTP.md](../net/HTTP.md#downloads-the-head-and-the-sink)): a 2xx
  that is an attachment (`Content-Disposition`), or whose type is anything
  but `text/html` or `text/plain`, is a file. A response without a type is
  shown, as before; an error page is always shown. A file stops the load at
  its head; the page on screen stays.
- **Where.** Save As (`sxnfiledlg`) asks, in the person's profile folder,
  with the name `sxnhttp_file_name` makes from `Content-Disposition` or the
  URL. It asks before replacing a file.
- **The transfer** runs on a **download worker**, a second thread with its
  own resolver and HTTP client, so the page worker keeps loading pages. It
  asks for the URL again -- the page worker stopped at the head -- and
  streams the body through the loader's sink into `<name>.part`, written in
  64 KiB pieces. Only a whole body is flushed and renamed over `<name>`; a
  failure, a body cut short, Cancel, the download window's X or the browser
  closing deletes the `.part` (`sxn_delete_path`). A server answering
  anything but 2xx the second time saves nothing. The navigation's 30
  seconds do not apply to the body; ten seconds of silence still do.
- **The window** is a modeless window
  ([OWNED_WINDOWS.md](OWNED_WINDOWS.md#modeless-windows)) after IE5's File
  Download: `Saving <name> from <host>`, a bar when the length is known,
  `<received> of <total> copied`, the transfer rate, where it goes, and
  Cancel (or Esc). When it is done: `Download complete`, Open Folder (Files
  on the folder) and Close; when it fails, why, and `Nothing was saved.` The
  page window stays usable throughout.
- **One download at a time.** A file asked for while one runs is not saved,
  and the status bar says so. A download is a second request for the same
  URL: a link that works only once would not survive it.

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

Images use an `[image]` marker, tables stack their cells, and fragments reload
the page top. The tag-rule cascade holds up to 32 rules until E3. The raw
document, paragraphs, runs and links grow with the page. View Source keeps the
original bytes, and long lines wrap in both views.

## Verification

`./tools/shoot.sh --scenario webview` boots the system, opens local files,
fetches from `tools/webview_http_server.py` over slirp, clicks a link, toggles
source, follows a 302, and asserts glyphs of `tools/webview-test-www/` against
the platform font tables. Glyph asserts use `solid_only` where anti-aliased edges
of neighbouring glyphs overlap: the harness blends against the background
while the painter blends in sequence (see the scenario comments).

The window's own steps check the toolbar captions, the absolute URL of a
hovered link in the status bar, the page's context menu, and the Home button
clicked on the toolbar. The N5 step loads `https://` from
`tools/tls_fixture.py`, whose test root the system does not trust, and checks
that it fails as untrusted and keeps the page. The N4 steps check that a page sent chunked in pieces of 11 bytes renders, and
that a response with contradictory framing and one cut short each fail with
their message while the previous page stays. The N3 steps load `/stall`, a download that never
finishes, and check that the window repaints the worker's progress and enables
`Stop`, takes Ctrl+U during the
load, and keeps its page when Esc stops it; Stop during the resolution of a
name the DNS fixture never answers; a newer navigation replacing a stalled
one; and closing the window in the middle of a load.

The D1 steps download from `tools/webview_http_server.py`: a slow file in
the download window while the page window, clicked, loads another page;
the 3 MiB attachment from `tools/http_fixture.py` under its server-given name;
one cancelled with Esc; one cut short, which fails; and one abandoned by
closing the browser. At the end the scenario extracts the guest's disk
(`sxfs-cli extract`, the disposable copy `tools/shoot.sh` passes in
`SAVANXP_SHOOT_DISK`) and checks the two downloads byte-exact against the
blob, the 3 MiB one by its SHA-256, and that the other three left neither a
file nor a `.part`.

The visual scenario also launches `webview --dns HOST:PORT URL`, checks F5,
launches `/system/webview.sxe /system/samples/welcome.html`, and opens a
`file://` URL. Its E2 page exceeds 32 KiB and 1100 paragraphs. The scenario
scrolls to the end, checks a decoded HTML entity and follows the last link.
The SUN socket contract is checked by `smoke native-network` and
`smoke native-network-offline`, including pending ARP, cancellation,
initialization without a NIC, route timeout and TCP refusal/reset/retry errors.
