# Image decoding with libsximg

`libsximg.so.0.4`, declared in `savanxp/img.h`, is the memory-only image
library shared by SUN and POSIX. It opens no files and makes no system calls.
The caller supplies the file bytes and owns the destination pixels. Format
support and its history are tracked in
[the Image Viewer roadmap](../roadmaps/open/IMAGE_VIEWER_ROADMAP.md).

## Common interface

- `sx_image_format(data, size)` identifies BMP (`BM`), PNG (its eight-byte
  signature) or JPEG (`FF D8`). It returns `SX_IMAGE_FORMAT_UNKNOWN` for
  unknown or insufficient input, including a null pointer. Detection does
  not validate the file. Sequential and progressive greyscale/RGB/YCbCr JPEG
  supports full resolution and decoder-level reduction by 2, 4 or 8.
- `sx_image_info(data, size, options, out)` validates supported metadata and
  plans the output. It allocates nothing. `out` is required and zeroed on
  error; on success it contains the format, original `source_width` and
  `source_height`, decoded `width` and `height`, and actual `scale_denom`.
- `sx_image_decode_into(data, size, options, pixels, pixel_count, out)` uses
  the same plan and writes top-down `0x00RRGGBB`, compositing transparency
  onto white. `pixel_count` counts 32-bit entries, not bytes. The optional
  metadata output is zeroed on error and filled on success.
- `sx_image_error_string(result)` returns a static English diagnostic, with
  a fallback for unknown result codes. The caller must not free it.

Use identical options for planning and decoding, and keep the input bytes
unchanged between calls. Data, options, output metadata and pixels must not
overlap. A successful plan does not prove the compressed pixels are valid:
PNG info needs only a complete IHDR; BMP also checks masks, palette and
pixel-data extent. JPEG info validates through the first SOS, including the
selected quantization and Huffman tables. Decode validates the remaining content.

## Limits and scaling

A null `options` pointer is the same as a zero-initialized
`struct sx_image_options`. Zero `max_width`, `max_height` or `max_pixels`
selects that global limit. A positive value lowers the corresponding limit;
a larger value cannot raise the global ceilings of 4096 per side and
16,777,216 output pixels. The pixel budget counts output pixels, not decoder
workspace or input bytes. Callers remain responsible for their own input
and memory budgets.

`scale_denom` is zero for automatic selection, or exactly 1, 2, 4 or 8 for
an explicit denominator. Other values are argument errors. BMP and PNG
support only 1: automatic selection returns 1, an explicit 2/4/8 returns
`SX_IMAGE_ERROR_SCALE`, and an image that does not fit is refused.

JPEG auto chooses the smallest denominator in 1/2/4/8 that fits **all three**
output limits. Explicit selection uses precisely that denominator: there is
no fallback to another scale, resampling after decode or upscaling. Output
is `ceil(source_width / scale_denom)` by `ceil(source_height / scale_denom)`.
If no permitted scale fits, the result is `SX_IMAGE_ERROR_LIMIT`. A null or
all-zero options structure can therefore return a reduced JPEG with its
actual denominator in the metadata.

The 4096-side and 16,777,216-pixel ceilings apply to decoded output. JPEG SOF
source dimensions may exceed 4096 (up to the format's 16-bit fields) if an
allowed scale fits. For example, a 4097x1 JPEG auto selects 2 and produces
2049x1; exact scale 1 returns `SX_IMAGE_ERROR_LIMIT`. A 32768x32768 source
can be planned at scale 8 with global defaults; 32769 on either side cannot
fit even at scale 8. BMP/PNG retain their original source limits.

## Results and failures

`SX_IMAGE_OK` is zero; errors are distinct negative values. Existing values
are stable; later codecs may add more specific errors.

| Result | Meaning |
|---|---|
| `SX_IMAGE_ERROR_ARGUMENT` | Null required pointer or invalid scale denominator |
| `SX_IMAGE_ERROR_FORMAT` | Signature is unrecognized |
| `SX_IMAGE_ERROR_UNSUPPORTED` | Unsupported image encoding or JPEG process |
| `SX_IMAGE_ERROR_HEADER` | Invalid or unsupported metadata for a supported format |
| `SX_IMAGE_ERROR_LIMIT` | Global dimension limit or caller's output limits exceeded |
| `SX_IMAGE_ERROR_SCALE` | Valid denominator unsupported by the selected decoder |
| `SX_IMAGE_ERROR_BUFFER` | Destination has fewer than decoded width times height entries |
| `SX_IMAGE_ERROR_DECODE` | Pixel decoding failed: malformed/unsupported data or decoder resource failure |

JPEG also reports specific unsupported cases:
`SX_IMAGE_ERROR_JPEG_PROGRESSIVE`, `_ARITHMETIC`, `_PRECISION`, `_LOSSLESS`,
`_COLOR` and `_SAMPLING` (each with the full `SX_IMAGE_ERROR_JPEG_` prefix).
`_PROGRESSIVE` names differential progressive frames (SOF6, hierarchical);
ordinary progressive SOF2 is decoded.
The colour error covers component counts other than one or three (including
CMYK/YCCK), unsupported Adobe transforms and contradictory Adobe markers.

Argument, header, scale, limit and capacity failures precede pixel writes.
After `SX_IMAGE_ERROR_DECODE`, the destination may contain part of an image;
metadata is still zero. The existing BMP/PNG decoders report a generic
failure for corrupt data and failed workspace allocation, so the common
interface cannot truthfully distinguish those causes. These failures stay
`SX_IMAGE_ERROR_DECODE` rather than inventing a more specific diagnosis.

## Compatibility and verification

The format-specific PNG/BMP functions, their alpha variants and their error
conventions remain available. The common interface returns the same white
composite as `sx_png_decode_into` or `sx_bmp_decode_into`; it does not replace
the straight-alpha calls used by cursors. Desktop wallpapers and Display
Properties use the common API with a 2048-side output ceiling and automatic
JPEG reduction; see [DISPLAY_PROPERTIES.md](../desktop/DISPLAY_PROPERTIES.md).
Paint retains its BMP/PNG calls and file associations.

`tests/host/sximg_test.cpp` compares common output and metadata with the
existing BMP/PNG fixtures, including paletted, masked, RLE, transparent,
16-bit and Adam7 images. It also covers signatures, explicit/automatic scale,
exact and insufficient budgets, buffer guards, reset metadata, global limits,
late decoder errors and deterministic diagnostics.

## Sequential JPEG

`sx_jpeg_info` and `sx_jpeg_decode_into` use the same options, metadata and
buffer contract as the common interface. Both operate entirely on memory
and use no floating point. The implementation is written from the
[T.81 specification](https://www.w3.org/Graphics/JPEG/itu-t81.pdf)
(`Reference`, no copied decoder code or runtime libjpeg dependency).

The supported subset is SOF0/SOF1 with 8-bit samples, one or three components
and horizontal/vertical sampling factors 1 or 2. This includes 4:4:4, 4:2:2,
4:2:0, 4:4:0 and other combinations of those factors. Each component occurs
exactly once across sequential scans (`Ss=0`, `Se=63`, `Ah=Al=0`). A scan may
select one, two or three components. Interleaved selectors follow frame order
and carry at most ten blocks per MCU; separate scans may arrive in any order.
A noninterleaved MCU contains one block regardless of frame sampling factors.
Height must be known in SOF; deferred DNL height and hierarchy are unsupported.

Each scan validates its selected quantization and Huffman tables. Tables may
be redefined between scans; already reconstructed components keep their
samples. SOF0 uses 8-bit quantizers and Huffman selectors 0/1; SOF1 also accepts
16-bit quantizers and selectors up to 3. The entropy reader handles MSB-first
Huffman codes, `FF00` stuffing, DC prediction, AC runs/EOB/ZRL, and restart
intervals. It rejects all-ones codes, oversubscribed/invalid tables, coefficient
overruns, invalid categories and DC values outside the 8-bit coefficient range.

Restart intervals count MCUs, may change between scans and require markers in
modulo-eight order, starting at RST0 in each scan. All DC predictors reset at
scan start and at restarts. One-bit padding and EOI after all components are
required. Missing, duplicate or extra component scans fail decoding. Info
stops at the first SOS, so malformed later headers/metadata and incomplete
frames return `SX_IMAGE_ERROR_DECODE` with zero metadata during decode.

Colour interpretation follows file conventions in frame component order:

- [JFIF APP0](https://www.w3.org/Graphics/JPEG/jfif3.pdf) selects full-range
  CCIR 601 YCbCr for three components and takes precedence over Adobe APP14.
- Adobe APP14 transform 0 selects RGB and transform 1 selects YCbCr for
  three components. Other transforms and conflicting Adobe markers are refused.
- Without either marker, ordered `R`, `G`, `B` identifiers select RGB;
  other identifiers default to YCbCr. One component is always greyscale.

Recognized APP0/APP14 payloads are length-checked; JFIF thumbnail bytes must
fit the segment. Unrecognized APP/COM metadata is skipped by bounded length.
Colour hints may occur between scans; the final interpretation is applied
only after a complete frame. ICC profiles, Exif orientation and colour
management are not applied. YCbCr converts with the JFIF matrix using Q16
integer arithmetic, rounding and clamping to 0..255. Lower-resolution samples
are replicated by their integer horizontal/vertical ratios; no interpolation
filter is applied.

Blocks are dequantized in natural order and reconstructed with an integer,
separable Q14 IDCT. Wide intermediate sums stay below 2^61 for validated
coefficients and 16-bit quantizers. Final values are rounded to nearest
(ties away from zero), shifted by 128 and clamped to 0..255. Partial blocks
are clipped to each component's true dimensions; interleaved padding blocks
are still consumed. Greyscale writes directly and needs no heap allocation.
Colour decode allocates one byte per reduced sample in three component planes,
at most `3*width*height` bytes using decoded dimensions (48 MiB at the global ceiling),
plus fixed tables and block storage. No image-sized coefficient buffer is
kept. Workspace is freed on every exit; allocation failure returns
`SX_IMAGE_ERROR_DECODE`. Info allocates nothing. Output budgets do not include
workspace. Sequential decode never allocates source-size pixel or coefficient
buffers; progressive decode must (see below).

At scale 2 or 4, the separable IDCT basis averages each group of 2 or 4
adjacent spatial samples directly from the coefficients. It emits only 4x4
or 2x2 values per original 8x8 block, rounding and clamping **after** averaging.
At scale 8 only the DC coefficient contributes, divided by 8 with level shift
and clamping. This differs from reducing an already rounded/clipped bitmap
or simply discarding all high-frequency coefficients. Scale 1 retains its
existing transform and pixels. All components use the same denominator;
subsampled components retain the same replication ratios at reduced size.
Partial bins at the image edge use the full encoded block's reconstruction
and are clipped to the reduced component dimensions.

Reduction does not skip entropy validation: every original MCU, AC run,
stuffed byte, restart, scan boundary, padding and EOI is still checked, even
when only DC contributes to output. Work therefore also depends on source
size and input bytes, not just output size. Each component's plane measures
`ceil(component_source_width / scale_denom)` by
`ceil(component_source_height / scale_denom)`. For a 17x19 4:2:0 image the
three planes require 503/140/43/17 bytes at denominators 1/2/4/8 respectively.

`sxjpeg-test` checks Huffman construction, sign extension, coefficient runs,
zig-zag placement, dequantization and the integer IDCT against an independent
floating-point definition. Six greyscale fixtures encoded/decoded with
libjpeg-turbo 3.2.1 from locally generated samples retain their tolerance of
**one intensity level** against `JDCT_ISLOW`. Fourteen colour fixtures cover
sampling layouts, JFIF/Adobe/markerless interpretation, scans and restarts,
with `do_fancy_upsampling=FALSE`. Their declared tolerance is **three RGB
levels per channel**: a one-level component IDCT difference propagated through
the YCbCr matrix can change an RGB channel by three levels. The checked-in
bytes need no encoder or libjpeg at build time.

An independent constant-block encoder checks all 64 combinations of sampling
factors 1/2, exact component placement, mixed/separate scan order, partial MCUs,
table redefinitions and changing restart intervals. Tests also check every
truncated reference prefix, buffer guards, allocation failures and release,
named unsupported modes, SOF1, restart wraparound, malformed/duplicate/missing
scans and deterministic mutations. `wallpaper-smoke` exercises the common
API in both desktop consumers, including reduced placement and reboot persistence.

The same twenty JPEG fixtures also have sixty reduced references at 2/4/8,
with the same one-level greyscale and three-level RGB tolerances. For the
reference only, libjpeg-turbo losslessly transcodes each component into a
one-component JPEG with unchanged coefficients and quantizers, then decodes
it using `JDCT_ISLOW` at the requested denominator. This avoids libjpeg's
adaptive chroma IDCT sizes; all reference components use the same reduction.
A double-precision JFIF matrix combines the planes with sample replication.
Only the expected pixels are stored; no new runtime or build dependency is
introduced. Reduced stages also compare directly with averaged floating-point
cosine bases, including AC cancellation, clipping and wide intermediate sums.

Tests assert the smallest automatic fit for each output limit and their
intersection, exact scale failures without fallback, rounded odd dimensions,
large source metadata and decode, reduced buffer bounds, memory allocation
sizes and failure, every truncated prefix at every scale, malformed discarded
AC data, later scan errors, and mutations with explicit/automatic scales.
All 64 sampling combinations and scan/table/restart cases run at 1/2/4/8.

## Progressive JPEG

SOF2 (8-bit, Huffman) is decoded with the same options, metadata, limits,
colour rules, sampling factors and reduced IDCT as the sequential subset;
only the entropy stage differs ([T.81](https://www.w3.org/Graphics/JPEG/itu-t81.pdf)
Annex G). SOF2 accepts 16-bit quantizers and Huffman selectors 0..3, as SOF1
does. Arithmetic progressive (SOF10), differential progressive (SOF6) and
12-bit precision remain refused by name.

Scans are validated against the progression of every coefficient:

- A DC scan has `Ss=Se=0` and may interleave components in frame order (at
  most ten blocks per MCU). An AC scan has `1<=Ss<=Se<=63` and exactly one
  component, and may come only after that component's first DC scan.
- A first scan of a coefficient has `Ah=0`; a refinement has `Ah` equal to
  the previous scan's `Al` and `Al=Ah-1`. `Al` is at most 13. Coding a band
  twice, refining one never coded, or skipping a bit position is invalid.
- Each component needs its first DC scan before EOI. AC bands may be left
  uncoded (they decode as zero), as may later refinement bits.
- The quantizer is latched at a component's first DC scan; a DQT after that
  does not change it.

The entropy decoder implements DC prediction on point-transformed values, DC
refinement bits, AC first scans with ZRL and EOB runs (`EOB0`..`EOB14`), and AC
refinement with correction bits for already nonzero coefficients. Huffman
tables may hold EOB-run symbols; a sequential scan refuses such a table. An EOB
run may not cross a restart marker or extend past the last block of its scan.
Restart intervals, marker order, predictor resets, padding and EOI follow the
sequential rules. Stored values are restored to full scale and must stay in
the 8-bit coefficient range (DC -1024..1023, AC -1023..1023), so a
coefficient made too large by its point transform is a decode error.

Every coefficient of the frame is kept until EOI as `int16_t` in natural
order, in MCU-padded blocks (interleaved DC scans also code the padding
blocks), and only then dequantized, transformed at the chosen scale and
stored as a sequential block would be. The store costs 128 bytes per padded
block whatever the output scale, so it bounds the source: at most
50,331,648 coefficients (96 MiB), three 4096x4096 planes. That admits a
4:2:0 source of about 33 megapixels or a 4:4:4 one of 16.7; for greyscale,
8192x6144 fits and 8192x6152 does not. A larger frame returns
`SX_IMAGE_ERROR_LIMIT` from `sx_jpeg_info` and `sx_image_info`, before any
allocation. Decode takes one workspace: the zeroed coefficient store followed
by the colour planes described above. Greyscale progressive therefore
allocates where sequential greyscale does not.

`sxjpeg-test` checks the stages with hand-built tables: EOB-run lengths,
blocks inside a run reading no bits, correction bits on positive and negative
coefficients, placement after zero-history coefficients, point transforms,
and refusals for ZRL past the band, coefficients past `Se` and values out of
range. Hand-made streams cover an EOB run that covers the scan exactly, one
that crosses a restart or the scan end, DC-only frames, restart numbering, the
coefficient-store boundary and nine scan-header violations.

Thirty-six fixtures in `sxjpeg_progressive_fixtures.h` are lossless
`jpegtran` (libjpeg-turbo 3.2.1) transcodes of the twenty sequential
references with the default script, a deep successive-approximation script
(separate and interleaved DC, two DC refinements, split AC bands, `Al` from 3
to 0), a spectral-selection-only script, and restarts at every MCU. Their
coefficients equal the sources', so each must decode **identically** to its
sequential source at 1/2/4/8, through both APIs, with exact buffer guards,
one released workspace, every truncated prefix refused and allocation failure
before any write. A 64x48 own-sample 4:2:0 image at quality 50 (long EOB runs)
is also compared with libjpeg-turbo's own decode of the progressive file
(`JDCT_ISLOW`, no fancy upsampling) within three RGB levels. Random mutations
of a deep-script stream stay bounded and release their workspace.
