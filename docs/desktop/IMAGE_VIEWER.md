# Image Viewer

Image Viewer is the SUN program `/system/imgview.sxe` (`system/imgview`),
launched as `imgview` or from Accessories. It reads BMP, PNG and sequential
JPEG through the common, memory-only `libsximg` API. Its native SXGUI menus,
toolbar, file dialog, scrollbars and status panes share the desktop's fonts,
bevels and [icon rules](ICON_STYLE.md).

## Opening and browsing

`imgview /system/wallpapers/Mountains.bmp` opens an absolute path. With no
argument, the window offers **File > Open** (`Ctrl+O`), the common SUN
[Open dialog](FILE_DIALOG.md), filtered to `.bmp`, `.png`, `.jpg` and `.jpeg`
without regard to case. Cancelling keeps the current image. The argument must
be absolute; relative names can be entered in the dialog's current folder.
The file's content determines its format, independently of its name.

**Previous / Next**, Left/Right and PageUp/PageDown visit image names in the
same folder, stopping at each end. Files are ordered by ASCII case-folded
name, with raw name as the tie breaker. Directories are excluded. A new scan
on every open observes additions and removals; streaming the directory avoids
a fixed-size navigation list. The status index counts matching extensions,
including files that fail to decode, so navigation can continue past an error.

The `.sxres` declares `ext_open` and `mime_open` capabilities for these four
extensions and three media types. Shipped `/system/assoc.ini` makes Image
Viewer the default **Open** action (including double click and Enter) in
Files and on the desktop, and associates **Edit** with Paint. User choices
in `Settings/assoc.ini` override each action independently; existing `.ext=`
lines remain Open, while `edit:.ext=` selects an editor. See
[SXE_FORMAT.md](../userland/SXE_FORMAT.md#file-associations-phase-5).
JPEG Edit is associated too, but Paint still cannot decode JPEG; adding that
support is separate work. No user policy file is rewritten by the build.

## Viewing controls

| Control | Action |
| --- | --- |
| Fit, `F` | Fit inside the viewport, preserving aspect ratio and never enlarging; follow window resizes |
| Actual Size, `1` | One decoded image pixel per screen pixel |
| Zoom In/Out, `+`/`-` | Steps from 12.5% to 1600%, preserving the viewport's source centre where possible |
| Scrollbars, wheel | Scroll an image larger than the viewport; wheel moves vertically in 48-pixel increments |
| Left-button drag on the image | Pan, clamped to the image edges |
| Full Screen, `F11` | Toggle fullscreen and hide the menus and toolbar; keep the status |
| `Esc` in fullscreen | Restore the window |

Fit can go below the manual 12.5% minimum. Sampling is bilinear below 200%
and nearest-neighbour from 200% up. Opening another file returns to Fit.
Status panes show name, original dimensions, format, rounded-up KiB, zoom,
JPEG decode reduction when used, and position in the folder.

**Zoom is relative to decoded pixels.** For a JPEG automatically decoded at
1/2, `100.0% | Decode 1/2` means one reduced pixel per screen pixel; the
original dimensions still appear separately. The first delivery does not
re-decode at a finer scale when zooming. It does not add image editing,
saving, printing, rotation or wallpaper commands.

## Loading and errors

The viewer queries and reads files through SUN, into an owned anonymous
section, then plans and decodes into another section. The encoded section
and file handle are released after decoding; closing or replacing the image
releases its decoded section. A failed load clears old pixels and displays
the reason on the canvas. Late failures release partially written pixels.

Encoded input is limited to 64 MiB. Default common-API output limits are
4096 pixels per side and 16,777,216 pixels. JPEG chooses full, 1/2, 1/4 or
1/8 output to meet those bounds; PNG/BMP source dimensions must fit the
library limits. Alpha is composited onto white. Supported encodings and
named rejection errors, including progressive JPEG, are specified in
[IMAGE_FORMATS.md](../media/IMAGE_FORMATS.md).

## Verification

- `image-assoc-smoke` checks Open/Edit from Files, desktop images and image
  shortcuts; `filesapp-smoke` checks policy precedence against real binaries.
- `imgview-view-test` is the host test of fit, zoom anchoring, scroll clamps,
  edge dimensions and case-insensitive image names.
- `./build.sh smoke imgview-test` runs `imgview --selftest` in the guest.
  It uses the window's actual SUN load path to check exact BMP/PNG/JPEG
  pixels, alpha, reduced JPEG metadata, missing/invalid files, cleanup after
  late failures and folder navigation.
- `./build.sh smoke imgview-smoke` and `./tools/shoot.sh --scenario imgview`
  inspect rendered glyphs and image colours, navigate the fixtures, zoom,
  scroll and drag, open/cancel/select through the common dialog, and enter
  and leave fullscreen. Fixtures are generated only on disposable disks by
  `tools/imgview_fixture.py`; they do not enter the persistent build image.

The checked-in toolbar PNGs are the art sources; `icons.inc` embeds their
20x20 BGRA pixels. Previous/Next reuse Files' green arrows, and fullscreen
reuses the player's black glyph at its native size. The application icon
has separately drawn 16x16 and 32x32 versions.
