# Web Viewer on the NetSurf libraries

> **Status: not started.** This is the order of work for turning Web Viewer
> from a CSS1 subset into a CSS 2.1 browser, keeping it MIT, native (SXGUI) and
> in the base image. What it renders today is pinned in
> [WEBVIEWER.md](../../desktop/WEBVIEWER.md); that contract changes phase by phase.

## The decision

A browser has three large parts: reading HTML into a DOM, resolving CSS into
computed styles, and laying boxes out on the page. NetSurf ships the first two
as **MIT libraries**, separate from its GPL-2.0-only core. The third, layout,
lives in the core.

So Web Viewer adopts the libraries and writes its own layout. It stays in the
tree, MIT, with the system's own chrome. The NetSurf port
([NETSURF_ROADMAP.md](NETSURF_ROADMAP.md)) runs next to it as the
reference: the same page, opened in both, inside SavanXP.

The price is explicit: NetSurf's layout, box construction, redraw and forms
are about 29,000 lines (`content/handlers/html/`, NetSurf `d06cd9d`). Web Viewer
grows its equivalent one phase at a time, and renders less than NetSurf for a
long while.

## What is adopted

Whole libraries, unmodified, under `vendor/netsurf/<library>/`, as uACPI is
under `vendor/uacpi/`. Each one gets its entry in
[THIRD_PARTY_PROVENANCE.md](../../third-party/THIRD_PARTY_PROVENANCE.md) in the
commit that vendors it.

| Library | Role | Phase |
|---|---|---|
| libparserutils | Input streams and charset codecs | W1 |
| libwapcaplet | Interned strings shared by the others | W1 |
| libhubbub | HTML5 tokenizer and tree builder | W1 |
| libdom | DOM, with its hubbub binding (not the XML one) | W1 |
| libcss | CSS 2.1 parser, selection, cascade, computed styles | W1 |
| libnsgif, libnsbmp | GIF and BMP decoding | W6 |
| libsvgtiny | SVG, if ever | later |

Not adopted: libnsfb (Web Viewer paints with SXGFX), libnslog, libnsutils and
libnspsl unless a library above requires them.

Two build facts measured in the upstream trees:

- **libparserutils needs no `iconv`.** Built with `WITHOUT_ICONV_FILTER`, it
  uses its own codecs: ASCII, ISO-8859-x, Windows-125x, UTF-8 and UTF-16. That
  is enough for the web Web Viewer will meet, and keeps `<iconv.h>` off this
  path.
- **libcss generates part of its parser at build time.** A small host program
  (`css_property_parser_gen.c`) reads `properties.gen` and writes one C file per
  property. Upstream drives it with Perl from Make; here CMake builds the host
  tool and runs it, so no new host dependency appears (rule 5 in `AGENTS.md`).

## The clean-room rule

NetSurf's core is GPL-2.0-only. Web Viewer is MIT. Code of the core does not
enter Web Viewer, and is not used as the model for it either: not the libcss
select handler (`content/handlers/css/select.c`), not box construction, not
`layout.c`, not the default stylesheet (`resources/default.css`).

Each of those is written from its specification: the CSS 2.1 recommendation
(its visual formatting model, and the sample stylesheet in its Appendix D),
HTML's rendering section, and the libraries' own MIT headers and documentation.
Where behaviour is unclear, the answer comes from opening the same page in the
NetSurf port and looking at the result, not from reading its source.

## The phases

Each phase names its acceptance condition, and each one ends with the page
corpus opened in both browsers (see [Comparing](#comparing)).

### W1 — the libraries build

`vendor/netsurf/` with the five libraries, CMake targets that build them as
static libraries for the POSIX SDK and for the host, and a host test that parses
every page in `tools/webview-test-www/` into a DOM and computes the style of
every element.

**Done when:** `./build.sh test` runs that host test, and the same libraries
link into a guest program.

### W2 — HTML through hubbub and libdom

Web Viewer builds its paragraphs and runs from the DOM instead of its own
parser. The rendering does not change yet. The 32 KiB, 256-paragraph and
64-link caps go away: the limit becomes memory.

**Done when:** the `webview` shoot scenario passes unchanged, and a page larger
than 32 KiB opens whole.

### W3 — styles through libcss

Web Viewer's own selection handler over libdom (about 36 callbacks in
`css_select_handler`), its own user-agent stylesheet, and stylesheets from
`<style>`, `style=""` and `<link rel=stylesheet>`. The existing paragraph
renderer reads computed styles instead of its own cascade.

**Done when:** classes, ids, descendant and child selectors, pseudo-classes
such as `:link` and `:first-child`, and external stylesheets all render in the
test corpus, and WEBVIEWER.md describes the new contract.

### W4 — the box tree and normal flow

Boxes built from the DOM and computed `display`, then block and inline layout:
widths, margins (with collapsing), padding, borders, line boxes, `vertical-align`
for text, `white-space`, list items. Text uses the bitmap faces, as today.

This is the phase that replaces "paragraphs that wrap" with a layout engine,
and the largest one.

**Done when:** a page of nested blocks with margins, padding, borders and
lists matches NetSurf's layout of the same page, box for box, up to font
metrics.

### W5 — floats and positioning

`float` and `clear`, then `position: relative` and `absolute`, then
`overflow` on blocks.

**Done when:** a float-based two-column page and a page with absolutely
positioned boxes match NetSurf's layout.

### W6 — tables and images

Automatic table layout, replacing "tables degrade to stacked lines". Images
through libnsgif, libnsbmp and PNG from `libsximg`, replacing the `[image]`
marker; their sizes take part in layout.

**Done when:** a table-based page with images matches NetSurf's layout.

### Later

Forms, fetches that do not block the UI, fragment navigation, and HTTPS. HTTPS
shares its prerequisites with the NetSurf port: a random source (`arc4random`)
and BearSSL. Web Viewer has its own HTTP client, so it does not need libcurl.

## Comparing

The page corpus is `tools/webview-test-www/`, grown with one page per phase.
A shoot scenario opens each page in Web Viewer and in NetSurf, in windows of
the same size, and keeps both screenshots. They will not match pixel for pixel:
the fonts differ. They are compared box by box, and what each phase's "done
when" asks is that the layout agrees.

The comparison needs the NetSurf port installed; without it, the scenario
checks Web Viewer alone.

## What not to do

- **Do not read NetSurf's core to write Web Viewer.** See the clean-room rule.
- **Do not patch the vendored libraries.** If one needs a change, it goes
  upstream or into a documented glue file next to it, never edited in place.
- **Do not start layout before W3.** A layout engine without computed styles
  ends up with a second cascade inside it.
- **Do not grow the old parser or cascade** while W2 and W3 are pending; they
  are about to be replaced.
