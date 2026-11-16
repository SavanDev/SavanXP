# Image decoding with libsximg

`libsximg.so.0.4`, declared in `savanxp/img.h`, is the memory-only image
library shared by SUN and POSIX. It opens no files and makes no system calls.
The caller supplies the file bytes and owns the destination pixels. Format
support and the remaining JPEG work are tracked in
[the Image Viewer roadmap](../roadmaps/open/IMAGE_VIEWER_ROADMAP.md).

## Common interface

- `sx_image_format(data, size)` identifies BMP (`BM`), PNG (its eight-byte
  signature) or JPEG (`FF D8`). It returns `SX_IMAGE_FORMAT_UNKNOWN` for
  unknown or insufficient input, including a null pointer. Detection does
  not validate the file. Greyscale sequential JPEG is decoded at full resolution.
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
an explicit denominator. Other values are argument errors. BMP, PNG and the
current greyscale JPEG decoder support only 1: automatic selection returns 1,
an explicit 2/4/8 returns
`SX_IMAGE_ERROR_SCALE`, and an image that does not fit is refused. There is
no implicit resampling, upscaling or fallback to a different exact scale.

The contract for the pending reduced JPEG decoder is: auto chooses the smallest
supported denominator in 1/2/4/8 that fits **all three** output limits;
explicit selection uses precisely that denominator. Output dimensions are
`ceil(source_width / scale_denom)` by `ceil(source_height / scale_denom)`.
If no permitted scale fits, the result is `SX_IMAGE_ERROR_LIMIT`. Currently
JPEG auto selects 1 and refuses output exceeding the limits; explicit 2/4/8
returns `SX_IMAGE_ERROR_SCALE`. Colour JPEG is not yet supported.

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
The colour error currently includes RGB/YCbCr as well as CMYK/YCCK: this
first JPEG delivery accepts one component only.

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
the straight-alpha calls used by cursors. Existing programs keep their current
format and file-association policies. JPEG integration into applications is
a later I5 delivery.

`tests/host/sximg_test.cpp` compares common output and metadata with the
existing BMP/PNG fixtures, including paletted, masked, RLE, transparent,
16-bit and Adam7 images. It also covers signatures, explicit/automatic scale,
exact and insufficient budgets, buffer guards, reset metadata, global limits,
late decoder errors and deterministic diagnostics.

## Sequential greyscale JPEG

`sx_jpeg_info` and `sx_jpeg_decode_into` use the same options, metadata and
buffer contract as the common interface. Both operate entirely on memory,
allocate nothing and use no floating point. The implementation is written
from the [T.81 specification](https://www.w3.org/Graphics/JPEG/itu-t81.pdf)
(`Reference`, no copied decoder code or runtime libjpeg dependency).

The current subset is SOF0/SOF1 with 8-bit samples, one component, sampling
factors 1 or 2, and one sequential scan (`Ss=0`, `Se=63`, `Ah=Al=0`). A
noninterleaved scan contains one block per MCU even when the frame's single
component uses larger sampling factors. Tables may be redefined before SOS;
selected tables must exist there. SOF0 uses 8-bit quantizers and Huffman
selectors 0/1; SOF1 also accepts 16-bit quantizers and selectors up to 3.
Height must be known in SOF; deferred DNL height, hierarchy and extra scans
are not supported. APP/COM metadata is skipped by bounded segment length.

The entropy reader handles MSB-first Huffman codes and `FF00` stuffing,
DC prediction, AC runs/EOB/ZRL, and restart intervals. It rejects all-ones
codes, oversubscribed/invalid tables, runs past coefficient 63, invalid
categories and DC values outside the 8-bit coefficient range. Restart
markers must appear at the declared intervals and in modulo-eight order;
predictors reset there. One-bit padding and EOI are required. Failure after
entropy decoding starts may leave partial pixels but never success metadata.

Blocks are dequantized in natural order and reconstructed with an integer,
separable Q14 IDCT. Wide intermediate sums stay below 2^61 for validated
coefficients and 16-bit quantizers. Final values are rounded to nearest
(ties away from zero), shifted by 128 and clamped to 0..255. Only one block
and a fixed set of tables are kept; original image-sized working buffers
are unnecessary. Partial right/bottom blocks are clipped to the image.

`sxjpeg-test` checks Huffman construction, sign extension, coefficient runs,
zig-zag placement, dequantization and the integer IDCT against an independent
floating-point definition. Six fixtures encoded/decoded with libjpeg-turbo
3.2.1 from locally generated samples cover constant images, odd sizes,
quality levels, optimized tables and restart intervals. Their declared pixel
tolerance is **one intensity level** against `JDCT_ISLOW`; the checked-in
bytes need no encoder or libjpeg at build time. The test also checks every
truncated prefix, buffer boundaries, named unsupported modes, SOF1 tables,
restart wraparound and deterministic mutations. Full colour/scan support
and reduced decoding remain separate I5 deliveries.
