# Web Viewer: the rendering contract

> Web Viewer runs on SUN as `/system/webview.sxe` (N1), loads remote pages on a
> network worker with Stop (N3) through SUN's HTTP loader (N4), over validated
> HTTPS (N5), and saves what it does not show (D1). It parses HTML through
> libhubbub and libdom (E2), computes styles with libcss (E3) and lays pages
> out in CSS boxes (E4) with floats and positioning (E5). See the [roadmap](../roadmaps/open/WEBVIEWER_ROADMAP.md).
>
> It is built only when the OpenSSL port is (`ports/openssl/build.sh`, then
> `./build.sh build`); without it the image has no Web Viewer and the registry
> drops it.

`/system/webview.sxe` ("Web Viewer", also launched as `webview`) is the in-tree
browser prototype, in the spirit of IE 1.1, with an address bar, history, menus,
local files and HTTP fetching by name or IP. libhubbub builds a libdom tree.
libcss selects and composes the styles. The engine turns the styled tree
into CSS boxes and lays them out, and Web Viewer paints the result.

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
Refresh, Top and End of page, View Source), Favorites (below), Help. A right click on the page
opens its context menu (Back, Forward, Refresh, View Source). Home opens
`file:///system/samples/welcome.html`.

The window title names the page (`<title> - Web Viewer`), and so does its
taskbar button. With the pointer on a link the status bar shows where it
leads, as an absolute URL; the next status message takes the bar back, and
the link shows again when the pointer moves.

## Favorites

The Favorites menu, after View, as Internet Explorer 5 had it
(`system/webview/favorites.c`):

- **Add to Favorites** adds the page on screen, named by its `<title>` (its
  URL when it has none), and the status bar says `Added <name> to
  Favorites.` It is grey with no page, for a page already there, and when
  the list holds its 24. **Remove from Favorites** takes the page on screen
  out and is grey for one that is not a favorite.
- Under a separator, one row per favorite in the order they were added; the
  page on screen is checked. A row opens its URL as typing it would, into
  the history.
- Names collapse runs of spaces and are cut with `...` to fit a menu row
  (47 bytes, never inside a UTF-8 character).
- The list is `webview-favorites` in the person's Settings folder
  (`sxn_settings_path`), written whole on every change: a `#` header line,
  then a line per favorite, the URL, a tab and the name. Blank lines and
  other `#` lines are skipped when it is read, so it can be edited by hand.
- A profile without the file starts with two pages that measure the browser:
  `https://motherfuckingwebsite.com/` and `http://acid2.acidtests.org/`.
  Once a change is saved the file decides, so an emptied list stays empty.

`webview-favorites-test` checks the list and its text on the host; the
`webview` scenario's Favorites steps check the menu, adding, opening and
removing on screen.

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
- A failed or stopped navigation keeps the page on screen. The worker
  transfers the complete body through the mailbox. The UI parses the DOM
  and, if needed, submits a batch of stylesheet requests to the worker
  before committing the new document. The address bar goes back to the
  previous page after a failure, unless the person is typing in it.
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

libcss combines the browser's default stylesheet, the HTML color hints
(`body bgcolor/text/link` and `font color`), the author's `<style>` and
`<link rel="stylesheet">` sheets in their DOM order, and `style=""`. The
cascade accounts for selector specificity, `!important`, origins and
inheritance. Tag, class, ID, attribute, descendant, child and sibling
selectors and static pseudo-classes such as `:link` and `:first-child` can
affect the page.

Linked sheets resolve against the document's final URL. The network worker
loads HTTP and HTTPS sheets within the navigation's remaining budget.
Local documents can use relative CSS files on disk. A remote document can
load HTTP(S) sheets, with HTTPS documents restricted to HTTPS; the loader
validates certificates and redirects. A missing sheet, an attachment or a
response with a non-CSS media type is skipped. Cancel or navigate away to
drop the candidate document and its pending loads. View Source and the
return to the rendered page reuse the current document's stylesheet bytes.

Sheets honor their `media` attribute and libcss evaluates `@media` for a
screen at 800×600 CSS pixels. Alternate stylesheets, `@import`, the
interactive states (`:hover`, `:visited`, `:active`, `:focus`, `:target`)
and quirks mode are deferred.

## Layout and paint (E4, E5, E6)

Web Viewer lays pages out with the engine's box tree and normal flow
([WEBVIEWER_ENGINE.md](WEBVIEWER_ENGINE.md#the-box-tree)): blocks with
margins (collapsing), borders, padding, widths and heights, and line boxes
with wrapping, `white-space`, `line-height`, `vertical-align`, `text-align`,
`text-indent` and list markers. Floats and `clear`
([WEBVIEWER_ENGINE.md](WEBVIEWER_ENGINE.md#floats)) take boxes out of that
flow and wrap the text beside them, and `position`
([WEBVIEWER_ENGINE.md](WEBVIEWER_ENGINE.md#positioning)) moves boxes relative to
it or out of it. `overflow` other than `visible`
([WEBVIEWER_ENGINE.md](WEBVIEWER_ENGINE.md#overflow)) clips a box's content,
and on the root or `body` keeps the page from scrolling when `hidden`.
Tables ([WEBVIEWER_ENGINE.md](WEBVIEWER_ENGINE.md#tables)) lay their cells
out in rows and columns, with HTML's table attributes (`border`,
`cellspacing`, `cellpadding`, `width`, `height`, `bgcolor`, `align`,
`valign`, `nowrap`, `colspan`, `rowspan`) and `align=""` on blocks and
`<center>` aligning the blocks inside. Images take part in layout at their
sizes ([Images](#images)). The user-agent sheet is the engine's
`webengine_ua_css`, CSS 2.1's Appendix D with HTML's heading sizes, so
`body` has its 8 pixels of margin and paragraphs and headings their em
margins.

- **The viewport.** The layout viewport is the page's area, the window less
  its frame and the scroll bar. The page is laid out again only when its
  size changes, so a resize reflows; scrolling repaints from the same
  layout.
- **Faces.** Text under 18 CSS pixels uses the UI face and larger text the
  title face; `monospace` (`pre`, `code`, `tt`, `kbd`, `samp`) uses the
  console's. Bold is the bitmap double strike, one pixel more per character,
  counted in the layout. Italic has no face yet and draws upright. The
  painter gives no ascent, so baselines use the baked faces' proportions.
- **Paint.** The canvas takes the root's background, or body's (CSS 2.1
  14.2). Then the engine's painting order (`webengine_tree.paint`): each
  block's background and borders in tree order, the floats whole, then the
  line fragments, then positioned boxes by `z-index`
  ([WEBVIEWER_ENGINE.md](WEBVIEWER_ENGINE.md#painting-order)). A table
  paints its background and borders around its rows, not its captions; a
  row's or row group's background shows under each of its cells, not in
  the spacing between them, and `empty-cells: hide` leaves an empty cell
  unpainted. Fixed boxes
  are painted where they are on the screen, whatever the scroll. The line
  fragments are: an inline's background and borders on each line it
  spans, text, its underline, overline or line-through, and markers. Disc,
  circle and square are drawn shapes; numbers are text. Border styles
  `dotted`, `dashed` and `double` are drawn as such; `inset`, `outset`,
  `groove` and `ridge` light one pair of sides. Colours with an alpha
  channel blend over the backgrounds behind them. `visibility: hidden`
  keeps the space and paints nothing.
- **Images** are painted scaled to their content box, smoothly when the
  size differs from the picture's, with the transparency of PNG and BMP
  over what is behind them. A picture still loading paints nothing; a
  broken one a sunken frame.
- **Links.** A link is the text, inline boxes and images of an `<a href>`, hit
  tested against the last paint's fragments, topmost first; a link clipped
  away by overflow cannot be clicked.
- **View Source and text/plain** are the engine's plain-text tree
  (`webengine_tree_text`): the bytes as they came, newlines kept, tabs to the
  next eighth column, wrapped at spaces, with 8 pixels of padding.
- **The welcome page** is a small HTML document of its own, laid out like
  any page.

## Images

A page's pictures load after the page is shown
([WEBVIEWER_ENGINE.md](WEBVIEWER_ENGINE.md#images) has how they are laid out).
Each `<img src>` resolves against the page: an `http://` or `https://` URL,
or a local path from a local page. A page loaded over `https://` takes only
`https://` pictures, as it takes only `https://` stylesheets; `data:` and
other schemes fail. A local picture is read and decoded at once. Remote ones
go, in the order of the page, to a queue that three picture threads of the
window take from, apart from the network worker that loads pages: each
thread fetches a picture through `sxnhttp` with its own resolver, decodes it
there and hands it back as soon as it is done, and the page is laid out
again where it was scrolled to. A picture that stalls holds one thread, not
the pictures after it nor the next navigation. Stop and Esc are enabled
while pictures are on their way, as for a load: they make the rest fail and
keep the page. A new page cancels them too, and a picture that comes back
for a page no longer on screen is dropped.

- **Formats.** PNG, BMP and JPEG through libsximg
  ([IMAGE_FORMATS.md](../media/IMAGE_FORMATS.md)), PNG and BMP with their
  alpha, and GIF through NetSurf's libnsgif
  ([WEBVIEWER_ENGINE.md](WEBVIEWER_ENGINE.md#what-is-vendored)) with its
  transparency; an animated GIF shows its first frame. A JPEG over 2048
  pixels a side is reduced while it decodes and keeps its own size on the
  page; a PNG, BMP or GIF that big fails.
- **Budgets.** A picture may take 8 MiB to arrive, and a page's decoded
  pictures 64 MiB; past that a picture fails.
- **The same picture** used twice on a page is fetched once. The pictures
  belong to the page on screen: View Source and back finds them, a new page
  starts again.

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

Animated GIFs do not animate. `inline-block` and the other inline-level displays are laid out as inline
boxes, and `inline-table` as a table at block level. `border-collapse:
collapse` lays out as `separate`, and `table-layout: fixed` as `auto`. `overflow: auto` and `scroll` clip without scroll bars of their own.
Fragments reload the page top. A word wider than the page is cut
rather than overflowing (the engine's emergency break), and there is no
horizontal scroll.

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
The E3 page asserts glyph colors from linked CSS, selector matches,
cascade order, media and inline declarations. The scenario follows a
styled link, checks the rendered page after View Source, and stops a
navigation waiting for a stylesheet. The E5 step opens `e5-float-text.html`
and asserts the text inside a float and the text wrapping beside one, then
`e5-position-text.html` for a relative inline and an absolute box, and
`e5-overflow.html`, whose clipped lines must not show. The E6 step opens
`e6-table-cells.html` and asserts text on a cell's background, on a row's
background under its cell, and in a cell spanning two columns; then
`e6-image-stall.html`, whose page and arrived pictures (a PNG, and the
first frame of an animated GIF and no other) must show while its first
picture, ahead of them, never arrives, and `e6-images.html`, loaded while that
picture is still pending, whose pictures must be painted.
The SUN socket contract is checked by `smoke native-network` and
`smoke native-network-offline`, including pending ARP, cancellation,
initialization without a NIC, route timeout and TCP refusal/reset/retry errors.
