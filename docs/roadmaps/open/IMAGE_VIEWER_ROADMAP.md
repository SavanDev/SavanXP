# Image Viewer and the image formats

Status: **phases I1–I4 landed** (shared BMP decoding, PNG wallpapers, extended BMP and PNG formats).
I5 has landed its common interface and full-resolution greyscale JPEG; colour and reduction are pending.

This page is the plan for an image viewer that opens BMP, JPEG and PNG, and
for the image library under it: what the system has today, the decisions
taken, and the order of the work. The viewer is the consumer; most of the
work is in `libsximg`, so that Paint, the wallpaper and the cursors read the
same formats it reads.

## The short version

- **One image library: `libsximg.so.0.4`** (`savanxp/img.h`,
  `subsystems/posix/sdk/v1/runtime/sximg.c`). Every decoder lives there,
  works on a buffer in memory, makes no system call, and has its host test
  (`savanxp-sximg-test`). Programs do not carry decoders of their own.
- **The viewer is a SUN program** (`system/imgview`, "Image Viewer"),
  drawn with SXGUI, linking `libsximg` with `DEPENDS` like any neutral
  library.
- **JPEG is written in the tree from the specification** (ITU-T T.81), the
  `Reference` category of
  [THIRD_PARTY_ADOPTION.md](../../third-party/THIRD_PARTY_ADOPTION.md), as the
  PNG decoder was. Baseline first, progressive after the viewer exists.
- **The format is told by its first bytes, not by the extension.**
- **PNG wallpapers come almost for free** once the wallpaper reads through
  `libsximg` (phase I2).

## What exists today

| Piece | Where | What it gives the viewer |
|---|---|---|
| PNG decoder | `sx_png_decode`, `sx_png_decode_argb`, `sx_png_info`, `sx_png_decode_into` (I4) | All base colour types and their legal 1/2/4/8/16-bit depths, `tRNS`, Adam7, all filters, full inflate. Up to 4096 on a side |
| BMP decoder | `sx_bmp_info`, `sx_bmp_decode_into`, `sx_bmp_decode_argb_into` (I3) | `BI_RGB` 1/4/8/16/24/32 bpp, 16/32-bit masks and alpha, RLE4/RLE8. Up to 4096 on a side |
| JPEG decoder | `sx_jpeg_info`, `sx_jpeg_decode_into` (I5 greyscale delivery) | SOF0/SOF1, 8-bit one-component JPEG, restart markers, full resolution. Up to 4096 on a side; colour and reduction pending |
| Window, painting, keys, resize | `sxngui.h`: `on_paint`, `on_key`, `on_resize` | the canvas is painted in `on_paint`, as Paint paints its own |
| Scaling | `sx_painter_draw_scaled_bitmap` (`gfx2d.h`), `SX_SCALE_NEAREST` / `BILINEAR` | zoom and fit-to-window |
| Scrolling, toolbar, status bar | `SXGUI_SCROLLBAR`, `SXGUI_FLAG_TOOLBAR`, the status label | the frame around the image |
| Open dialog | `sxnfiledlg.h` ([FILE_DIALOG.md](../../desktop/FILE_DIALOG.md)) | File > Open |
| Fullscreen | SUN windows can ask for it (Media Player's F11) | F11 |
| Capability to open a type | `.sxe` `ext_open` / `mime_open` ([SXE_FORMAT.md](../../userland/SXE_FORMAT.md)) | declaring `.bmp,.png,.jpg,.jpeg` |
| Icons by type | `diskfs/mimeicon.ini` already maps `.png`, `.bmp`, `.jpg`, `.jpeg` to `image-x-generic` | nothing to add |

## Decisions

### Decoders work on memory and know nothing about files

A decoder takes `(data, size)` and returns pixels. It does not open files,
map sections or log. That keeps it the same code in POSIX, SUN, the
compositor and the host test, and it is what makes the host test the real
proof of the decoder: the guest only proves that programs load the library
and reach it.

### The caller can own the pixels

New decoders come in two calls: one that reads the size from the header
(`sx_<fmt>_info`) and one that decodes into a buffer the caller provides
(`sx_<fmt>_decode_into`). The desktop keeps its wallpaper in a section of
its own, not on the heap, and Paint decodes straight into its document's
bitmap; neither should pay for a copy. Pixels are top-down `0x00RRGGBB`,
the layout of `SX_PIXEL_FORMAT_BGRX8888`. Formats with alpha add an ARGB
variant, as PNG does (`sx_png_decode_argb`, straight alpha).

PNG gained the pair in I2 (`sx_png_info`, `sx_png_decode_into`). The PNG
calls that allocate (`sx_png_decode`, `_argb`) stay: the cursors and Paint
use them, and nothing gains from changing them.

### One entry point by content, with JPEG

I5 provides `sx_image_info`, `sx_image_decode_into` and `sx_image_format`
for BMP, PNG and sequential greyscale JPEG, detecting content rather than
extensions. The per-format calls remain for callers that only accept one
format or need straight alpha.
The shared contract for output limits, original/decoded dimensions, exact
and automatic scale, and error codes is documented in
[IMAGE_FORMATS.md](../../media/IMAGE_FORMATS.md).

### JPEG is ours, not vendored

Single-header decoders exist with clear licenses, but they bring every
format at once, a second inflate next to ours, and code the project cannot
test piece by piece. A baseline JPEG decoder is a bounded piece (Huffman,
dequantisation, an integer IDCT, upsampling, YCbCr to RGB), the host test can
build each part against known values, and it follows the rule the PNG
decoder already follows. If this proves wrong, the change is a decision
recorded here and in
[THIRD_PARTY_PROVENANCE.md](../../third-party/THIRD_PARTY_PROVENANCE.md),
not a quiet swap.

### How much memory an image may take

Decoded pixels cost 4 bytes each: 4096x4096 is 64 MiB. The native heap
grows in up to eight arenas, and an arena is a section the kernel zeroes
when it is created ([NATIVE_HEAP.md](../../userland/NATIVE_HEAP.md)), so a
picture that size costs its full size at once. The limits are therefore:

- **4096 pixels on a side** for every decoder (`SX_PNG_MAX_DIMENSION`,
  `SX_BMP_MAX_DIMENSION`). Callers keep their own smaller limits (Paint
  512x384, the wallpaper 2048).
- **JPEG decodes at a reduced scale instead of refusing.** The IDCT can
  produce 1/2, 1/4 or 1/8 of the size almost for free, so a larger photo is
  decoded at the first scale that fits, and the status bar says so.
- **The viewer keeps its pixels outside the heap**, in a section of their
  own as the wallpaper does, so freeing an image gives the memory back at
  once.

### Who opens an image by default

Files and the desktop resolve a double click through `file_assoc`
([SXE_FORMAT.md](../../userland/SXE_FORMAT.md)): the user's `assoc.ini`
first, then the first program that declares the extension, scanning `/bin`
before `/system`. Paint (in `/bin`) declares `.bmp` and `.png`, so with no
policy it would keep opening them. On Windows the viewer opens a picture and
Paint is "Edit". **Open question for I6**: whether the viewer wins by a
policy line the system ships, or by a tie-break rule in `file_assoc`. Either
way the decision goes into `SXE_FORMAT.md`; it is not solved inside the
viewer.

## Phases

### I1 — BMP moves into `libsximg` (done)

`sx_bmp_info` and `sx_bmp_decode_into` replace the two readers that Paint
and the desktop wallpaper each carried. The subset is the union of what they
read: `BI_RGB`, 24 or 32 bpp, `BITMAPINFOHEADER` or larger, bottom-up or
top-down. Paint now also opens 32 bpp BMPs, which only the wallpaper read
before. Host test: both depths, both row orders, row padding, and every
refusal (other depths, compression, planes, sizes, truncated rows, a small
buffer).

### I2 — PNG wallpapers (done)

The wallpaper tries each decoder's `info` call on the file's bytes and
decodes with the first that accepts it; Display Properties lists `.bmp` and
`.png` in `/wallpapers`, and Browse accepts both. The check that keeps the
list from showing something the desktop cannot draw is unchanged.
Transparent pixels come out white (see
[DISPLAY_PROPERTIES.md](../../desktop/DISPLAY_PROPERTIES.md)). JPEG joins
the decoder table when I5 lands. Verification: `tools/shoot.sh --scenario
wallpaperpng` picks `/paint-test.png` with Browse and reads the tiled pixels
off the desktop.

### I3 — The rest of BMP (done)

Palettes (1, 4 and 8 bpp), 16 bpp (5-5-5 and `BI_BITFIELDS`),
32 bpp with masks and alpha (`BITMAPV4HEADER`/`V5`), `BI_RLE4` and
`BI_RLE8` are shared by Paint and the wallpaper. The decoder follows the
[bitmap header](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapv4header)
and [RLE](https://learn.microsoft.com/en-us/windows/win32/gdi/bitmap-compression)
references. INFO has three external masks; larger headers embed them
(V2/V3 are accepted too). Masks must be contiguous, disjoint and within
the pixel depth. Components are scaled to 8 bits with nearest rounding.
`sx_bmp_decode_into` composites explicit alpha onto white;
`sx_bmp_decode_argb_into` retains straight alpha in caller-owned pixels.
BI_RGB's unused high bits and palette reserved bytes are ignored.

RLE accepts encoded and word-aligned absolute runs, EOL, EOB and delta;
short rows, skips and early EOB leave palette entry zero. Top-down RLE is
refused. A nonzero `biSizeImage` bounds the compressed stream and an explicit
EOB is required. `sx_bmp_info` validates metadata and pixel-data extent;
only decode validates indices and RLE instructions, so it may still fail.
Colour profiles and gamma conversion are not applied.

The host test checks exact pixels in both row orders, packed odd-width
palettes, 555/565 and wider masks, alpha endpoints and midpoints, every RLE
instruction and padding, plus malformed metadata, indices, masks, truncated
streams and out-of-bounds runs/deltas. Existing 24/32-bit checks remain.

### I4 — The rest of PNG (done)

The shared decoder accepts indexed colour with `PLTE` and optional `tRNS`,
greyscale, greyscale-alpha, RGB and RGBA at every legal PNG sample depth.
Adam7 reconstructs each pass independently, including packed rows and empty
passes, then scatters pixels into the caller's final image. The API and
4096-per-side bound are unchanged. Paint, wallpaper and PNG cursors share
these formats; CUR validates its bounded dimensions with `sx_png_info`
before allocation rather than keeping a separate PNG subset.

Samples are rescaled to 8 bits with nearest rounding. Colour keys are tested
at the original depth before rescaling; unused high bits in low-depth keys
are masked as specified by the
[PNG reference](https://www.w3.org/TR/2025/REC-png-3-20250624/).
The RGB functions composite onto white; `_argb` retains straight alpha.
Palette entries not covered by `tRNS` stay opaque. Gamma, profiles and
background-colour metadata are not applied. Chunk CRC checking stays off;
zlib headers and Adler-32 are checked. Invalid palette indices, malformed
or misplaced `PLTE`/`tRNS`, missing `IEND` and unknown critical chunks are
rejected. `sx_png_info` still needs only a complete IHDR; decode validates
the remaining file.

Host assertions cover every type/depth, all five filters, ordinary and Adam7
rows, narrow images with empty passes, 16-bit key collisions, palette alpha,
buffer guards and truncated/malformed input. Existing RGB/RGBA and cursor
pack checks remain; independent libpng-encoded indexed and greyscale-alpha
Adam7 fixtures also pass through CUR selection and retain exact alpha.

### I5 — JPEG baseline and the common entry point (in progress)

1. **Common API and contract (done):** BMP/PNG detection and decode via
   `sx_image_format`, `sx_image_info`, `sx_image_decode_into`; output limits,
   original/decoded dimensions, scale denominator and named errors. Existing
   format-specific functions remain unchanged. See
   [IMAGE_FORMATS.md](../../media/IMAGE_FORMATS.md) for the contract and tests.
2. **Greyscale JPEG end to end (done):** SOF0/SOF1, 8-bit one-component
   frames, tables, Huffman coefficients, restart markers, dequantisation and
   integer IDCT, connected to the common API at scale 1. Stage assertions
   follow T.81; reference fixtures compare within one intensity level.
   See [IMAGE_FORMATS.md](../../media/IMAGE_FORMATS.md#sequential-greyscale-jpeg).
3. **Complete colour and scan support:** SOF0 and SOF1 (8-bit, Huffman),
   one or three components, legal sampling factors of 1 or 2 (4:4:4, 4:2:2,
   4:2:0, 4:4:0), interleaved/separate scans, restart markers, JFIF and
   Adobe (`APP14`) colour transforms.
4. **Reduced decode:** denominators 1/2/4/8, planning and decode in agreement,
   without first building a full-resolution bitmap.
5. **Consumer integration and closure:** wallpaper uses the common API;
   Display Properties accepts `.jpg`/`.jpeg`, with visual and persistence
   verification. Paint keeps its current formats unless extended separately.

The JPEG calls are `sx_jpeg_info` and `sx_jpeg_decode_into`, alongside
the common entry points. The host test refuses, by name: progressive
(SOF2), arithmetic coding, 12-bit, lossless and CMYK/YCCK. Reference-encoded
fixtures are compared within a stated tolerance for IDCT rounding and matched
chroma reconstruction. JPEG errors and scale handling must be covered before
I5 closes; identifying a JPEG signature alone is not decoder support.

### I6 — Image Viewer

A SUN program, `system/imgview`:

- Opens the file named in `argv[1]`, or one from File > Open.
- **Fit to window** by default (never enlarging), **actual size**, zoom in
  and out by steps; scrollbars and dragging when the image is larger than
  the window. Nearest-neighbour from 200 % up, bilinear below.
- **Previous / next image in the same folder** (Left/Right, PageUp/PageDown),
  in Files' order.
- Status bar: name, size in pixels, format, zoom, and the reduced scale when
  a JPEG was decoded at one.
- F11 fullscreen, Esc to leave it.
- Declares `ext_open=.bmp,.png,.jpg,.jpeg` and the matching `mime_open`.
- An image that cannot be read shows why in the window ("Not a supported
  BMP: embedded JPEG"), not an empty frame.

Verification: an `imgview --selftest` scenario in `tools/smoke_catalog.py`
that opens one fixture of each format through the same functions the window
uses, and a `tools/shoot.sh` scenario that looks at the window. Document:
`docs/desktop/IMAGE_VIEWER.md`, at which point this roadmap keeps only the
pending phases.

### I7 — Progressive JPEG

SOF2: spectral selection and successive approximation, with the whole image
kept as coefficients until the last scan. Phone and web pictures are often
progressive, so this is the first thing users will miss after I6.

### Later, unordered

EXIF orientation; rotate the view; copy the image to the clipboard (it holds
named formats, [CLIPBOARD.md](../../desktop/CLIPBOARD.md)); "Set as desktop
background"; a slideshow; GIF. A PNG encoder, so that Paint can save PNG, is
a separate decision and not part of this plan.
