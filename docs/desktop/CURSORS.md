# Desktop cursors

The compositor reads existing regular files using SUN, loads the catalog once
at process startup, and keeps the decoded images in memory. Applications still
request `SAVANXP_CURSOR_*` shapes through the existing window protocol. No new
client API, theme selector or live reload is provided.

## Files and format

The build copies unmodified Wine files from `assets/desktop/cursors/wine/` to
`/system/cursors/`; there is no image conversion step. Restart the compositor
to read replacements. The fixed order is arrow (`ocr_normal.cur`), wait
(`ocr_wait.cur`), text (`ocr_ibeam.cur`), move (`ocr_sizeall.cur`), horizontal
resize (`ocr_sizewe.cur`), vertical resize (`ocr_sizens.cur`), unavailable
(`ocr_no.cur`) and link (`ocr_hand.cur`). Provenance, hashes and licensing are
in [THIRD_PARTY_PROVENANCE.md](../third-party/THIRD_PARTY_PROVENANCE.md).

The pure decoder accepts CUR directories with 1 to 256 images, in files no
larger than 1 MiB. Supported images use a 40-byte BITMAPINFOHEADER, positive
bottom-up dimensions, BI_RGB (uncompressed) 1/4/8/24/32-bit DIBs and the AND
mask. Width and height must agree with the directory; hotspots must lie inside
the image. Palette indices, strides and byte ranges are checked before use.
PNG payloads, compressed DIBs, other header variants and animated ANI files
are unsupported. Invalid candidates are skipped. The largest supported image
up to 64x64 wins, followed by bit depth and directory order. Images are neither
scaled nor cropped. A file with no supported candidate fails as a whole.

For a 32-bit image with any nonzero alpha, straight alpha supplies opacity.
Otherwise the decoder retains the classic `(background AND mask) XOR color`
operation, including inversion. The decoded image carries hardware-compatible
pixels too when it does not require the background.

## Rendering and failures

The hardware plane and software painter share one catalog and native hotspot.
If any loaded image needs background operations, the entire session uses
software cursors. Otherwise the hardware plane is preferred when available;
shape replacement hides it before uploading, so a failed upload can safely
enable software rendering even if the driver discarded the resource. A failed
movement also attempts to hide it before switching.
If the plane cannot be hidden, the compositor exits so its supervisor can
recover display ownership rather than leaving duplicate pointers.

Software drawing happens after restoring damaged layers and respects clipping,
including screen edges. Old and new cursor bounds are damaged on movement.
Each successfully loaded file is logged with dimensions, hotspot and background
requirements. Missing, unsupported or unreadable files log an error and use the
loaded arrow. If the arrow itself is unavailable, a small built-in emergency
arrow keeps the desktop usable. The previous PNG pack is not retained.

## Verification

`./build.sh test` includes `cursor-cur-test`: all eight upstream files, smaller
image selection, palette depths, native hotspots, alpha, all classic mask
operations and malformed input. `./build.sh smoke native-test` checks file
reads, EOF, duplicate offsets, query-only access and permission refusals.

`./build.sh smoke cursor-smoke` starts compositord's `--cursor-selftest` mode.
It renders all eight cursors through the scene's software painter on white and
black tiles and checks hardware uploads when a plane is available. The host
compares the scanout screenshot pixel by pixel with the original Wine DIBs,
and checks classic inversion, painter clipping and drawing at the screen edge.
The hardware run also simulates a refused upload to verify software fallback.
Run it also with `--virtio` to exercise the hardware upload path. Screenshots
are saved under `build/smoke-logs/*-callback/`. `cursor-repro` checks pointer
presentation and `sxgui-smoke` checks the desktop's actual toolkit drawing.

`cursor-fallback-smoke` removes the entire pack only on the disposable smoke
image and checks that every shape uses the emergency arrow without preventing
the compositor from presenting a frame.

The virtio cursor path publishes its loaded image before the first update,
retains visibility and position across shape changes, and uses UPDATE_CURSOR
with resource id 0 to hide. Cursor queue completion accepts either a full
OK_NODATA header or a zero-length used entry, as emitted by
[QEMU's cursor handler](https://github.com/qemu/qemu/blob/master/hw/display/virtio-gpu.c).
Short nonempty responses and timeouts remain errors.
