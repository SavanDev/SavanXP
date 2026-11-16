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
  not validate the file. JPEG is identified but not decoded yet.
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
pixel-data extent. Decode still validates the remaining content.

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
`SX_IMAGE_ERROR_SCALE`, and an image that does not fit is refused. There is
no implicit resampling, upscaling or fallback to a different exact scale.

The contract for the pending JPEG decoder is: auto chooses the smallest
supported denominator in 1/2/4/8 that fits **all three** output limits;
explicit selection uses precisely that denominator. Output dimensions are
`ceil(source_width / scale_denom)` by `ceil(source_height / scale_denom)`.
If no permitted scale fits, the result is `SX_IMAGE_ERROR_LIMIT`. Until JPEG
lands, any recognized JPEG returns `SX_IMAGE_ERROR_UNSUPPORTED`; no JPEG
header, size or scale is advertised as validated.

## Results and failures

`SX_IMAGE_OK` is zero; errors are distinct negative values. Existing values
are stable; later codecs may add more specific errors.

| Result | Meaning |
|---|---|
| `SX_IMAGE_ERROR_ARGUMENT` | Null required pointer or invalid scale denominator |
| `SX_IMAGE_ERROR_FORMAT` | Signature is unrecognized |
| `SX_IMAGE_ERROR_UNSUPPORTED` | Format is recognized but its decoder is unavailable (currently JPEG) |
| `SX_IMAGE_ERROR_HEADER` | Invalid or unsupported metadata for a supported format |
| `SX_IMAGE_ERROR_LIMIT` | Global dimension limit or caller's output limits exceeded |
| `SX_IMAGE_ERROR_SCALE` | Valid denominator unsupported by the selected decoder |
| `SX_IMAGE_ERROR_BUFFER` | Destination has fewer than decoded width times height entries |
| `SX_IMAGE_ERROR_DECODE` | Pixel decoding failed: malformed/unsupported data or decoder resource failure |

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
format and file-association policies. JPEG integration is a later I5 delivery.

`tests/host/sximg_test.cpp` compares common output and metadata with the
existing BMP/PNG fixtures, including paletted, masked, RLE, transparent,
16-bit and Adam7 images. It also covers signatures, explicit/automatic scale,
exact and insufficient budgets, buffer guards, reset metadata, global limits,
late decoder errors and deterministic diagnostics.
