# Web Viewer: the rendering contract

`/bin/webview` ("Web Viewer") is the in-tree browser prototype, in the spirit
of IE 1.1: chrome (address bar, Back/Forward history, menus), local files and
numeric-IP HTTP fetching, and the rendering pipeline
`parse → cascade → wrap → paint` in `subsystems/posix/userland/webview.c`.

This document pins what it renders and what it openly ignores, so later
milestones extend a contract instead of rediscovering it.

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

## What it is not (yet)

No DNS (numeric IPv4 only), no TLS, no images (an `[image]` marker), tables
degrade to stacked lines, fragments reload the page top, fetches block the UI,
documents cap at 32 KiB / 256 paragraphs / 64 links / 256 runs / 32 rules /
4 KiB of CSS. Errors say which of these bit.

## Verification

`./tools/shoot.sh --scenario webview` boots the system, opens local files,
fetches from `tools/webview_http_server.py` over slirp, clicks a link, toggles
source, follows a 302, and asserts glyphs of `tools/webview-test-www/` against
the `libsxgfx` tables. Glyph asserts use `solid_only` where anti-aliased edges
of neighbouring glyphs overlap: the harness blends against the background
while the painter blends in sequence (see the scenario comments).
