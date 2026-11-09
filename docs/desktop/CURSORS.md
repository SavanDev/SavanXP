# Desktop cursors

The compositor reads `.cur` files through SUN and caches decoded images.
Applications request `SAVANXP_CURSOR_*` roles through the window protocol;
they do not choose packs or upload images. **SavanXP Classic** is the default:
the eight original SavanXP cursors retain their exact pixels, native sizes
and hotspots, with two matching diagonal resize cursors added.

## Mouse Properties

Open **Mouse Properties...** from the desktop background's context menu,
**Mouse Properties** in Program Manager's System group, or run `mouse.sxe`.
The SUN/SxGUI panel follows a classic property sheet: a Pointers tab, Scheme
selector, native-size preview of the selected role, role list with thumbnails,
Use Default, License, OK, Cancel and Apply. Thumbnails alone are reduced to
fit the list; the desktop pointer and large preview are never rescaled.

Changing the scheme only previews it. Apply switches all ten roles immediately
and saves the choice; OK applies and closes; Cancel discards changes made
since the last Apply. Use Default selects SavanXP Classic for preview. A failed
save remains visible and Apply retries it. An incomplete pack cannot replace
the active one. License shows attribution, terms and source locations for the
selected pack; Retrosmart's GPLv3 notice is also identified below the list.
The panel selects complete packs; editing individual roles and animated ANI
playback are not part of this interface.

Included schemes:

| ID | Display name | License |
|---|---|---|
| `classic` | SavanXP Classic | MIT |
| `retrosmart` | Retrosmart (Win-ish Classic), v2.0.3 | GPL-3.0 |
| `hackneyed` | Hackneyed Standard, v0.9.3 | MIT/X11 |

Wine's cursor pack is no longer distributed. This does not change the Wine
font used elsewhere. Upstream attribution, pinned revisions, release hashes
and imports are in [third-party provenance](../third-party/THIRD_PARTY_PROVENANCE.md).
Retrosmart includes the full GPL, upstream notices, corresponding source
archive and import script alongside its images. Its static CURs and Hackneyed's
are original release bytes under role names; `wait.cur` is the first embedded
CUR from each upstream `wait.ani`, with no pixel conversion.

## Installing another pack

`/system/cursors/themes.list` contains up to 16 unique entries, one per line:

```text
classic|SavanXP Classic|MIT
retrosmart|Retrosmart (Win-ish Classic)|GPL-3.0
hackneyed|Hackneyed|MIT/X11
```

An ID has 1–23 lowercase ASCII letters, digits, `_` or `-`; display names have
at most 47 bytes and license labels 23. Place each pack in
`/system/cursors/<id>/`, with these filenames in role order:

```text
arrow.cur        wait.cur         text.cur          move.cur
resize_h.cur     resize_v.cur     unavailable.cur   link.cur
resize_nwse.cur  resize_nesw.cur
```

Include the pack's required licenses, authors and a short `NOTICE` (up to seven
lines) for the panel's license dialog. Reopen Mouse Properties after changing
the index. All ten files must decode before Apply succeeds. Pack changes are
loaded off-screen into a temporary catalog, then installed together. Reapply
an active pack through `sxncursor_select` to reload its files. Normal builds retain extra pack directories
and `/users/user/Settings/cursors.cfg`; the build index itself is managed by the
repository, so add bundled schemes to `assets/desktop/cursors/themes.list`.

The selected stable ID is saved in `/users/user/Settings/cursors.cfg` through
`sxn_write_file`, including a volume sync. On startup a missing setting means
Classic; an invalid or unavailable saved pack falls back to Classic. A storage
error can leave an incomplete preference, which the next startup rejects.
The current single-user session uses the existing `user` account (uid 1000)
and its Settings directory, created by init. There is no new account database.

## Format

The decoder accepts CUR directories with 1–256 images and files up to 1 MiB.
Images up to 64x64 are supported. The largest fitting 32x32 is preferred;
otherwise the smallest supported larger image is chosen. Bit depth and
original directory order break ties. Native hotspots must be inside the image.

DIB images use a 40-byte BITMAPINFOHEADER, positive bottom-up dimensions,
BI_RGB, 1/4/8/24/32-bit pixels and an AND mask. Palette indices, strides,
hotspots and ranges are validated. Nonzero 32-bit alpha uses straight alpha;
otherwise `(background AND mask) XOR color` is retained, including inversion.

Embedded PNG uses the system PNG decoder's noninterlaced 8-bit RGB/RGBA
subset, with straight alpha retained by `sx_png_decode_argb`. Dimensions are
bounded and checked against the CUR directory before allocation. Zlib headers
and Adler-32 are checked; PNG chunk CRCs are not. Palette/grayscale PNG,
compression in DIBs, other DIB headers and ANI playback are unsupported.
Malformed candidates fail validation; a corrupted selected PNG fails the file.

## Session control and rendering

`<sxn/sxncursor.h>` offers `sxncursor_query` and `sxncursor_select`. The
compositor's `cursor-settings` port accepts one bounded request per connection,
uses nonblocking I/O and expires partial requests after two seconds. Up to four
clients can be pending. Only the session user and the system may use it, and
clients check that the port creator acts as the system. Replies distinguish
apply failure from save failure. Alternate test compositors do not claim the
session's settings port. The WM compositor protocol remains unchanged.

Hardware and software share one catalog and its hotspots. Any pack requiring
background operations uses software rendering. A live switch hides the old
hardware plane, restores the whole scene to remove old software bounds, then
uploads the new shape or uses software if uploading fails. Changes in size and
hotspot therefore leave no trails. Switching packs can recover hardware use.
An upload hides the old plane first; if a failed move cannot hide it, the
compositor exits so sessiond can recover display ownership safely.

At startup missing Classic roles use its loaded arrow. If even that arrow is
unavailable, a built-in 12x18 emergency arrow keeps the desktop usable.
During a resize, windowd keeps the grabbed edge/corner role even off the frame.
The diagonal roles are appended to the public enum, preserving prior values.

## Verification

- `./build.sh test`: all 30 bundled images, size selection, hotspots, palette
  depths, alpha, classic mask operations, malformed DIB/PNG and PNG alpha API.
- `./build.sh smoke cursor-smoke` (also `--virtio`): exact scanout comparison
  for all three packs on white/black, original Classic PNG pixel equivalence,
  clipping, inversion and hardware upload/fallback. Captures are saved under
  `build/smoke-logs/*-callback/`.
- `./build.sh smoke cursor-fallback-smoke`: removes Classic and its selection
  on a disposable image and checks every emergency role.
- `./build.sh smoke cursor-settings-smoke`: native control requests including
  an idle peer, invalid IDs and operations; actual panel previews, licensing,
  Apply, Cancel, defaults and preference retention through a guest reboot.
- `./build.sh smoke native-test`: SUN whole-file saving, replacement, empty
  contents, validation before mutation, durable writes and permission denial.
- `cursor-repro` and `sxgui-smoke`: pointer presentation and toolkit painting.

The virtio cursor queue accepts a full OK_NODATA reply or the zero-length
completion emitted by [QEMU's cursor handler](https://github.com/qemu/qemu/blob/master/hw/display/virtio-gpu.c).
Short nonempty completions and timeouts remain errors.

## Optional pointer shadow

Display Properties → Settings → Effects controls an additional system-generated
shadow independently of the selected pack. The original pixels and classic
AND/XOR operations are preserved; disabling the option removes only this extra
shadow, not shadows already drawn into a pack.

The catalog caches a black, alpha-80 triangular falloff of radius 3, offset
(1, 2), for each shape. Software composition draws the mask before the original
pointer and includes both in old/new damage bounds. Hardware composition packs
the mask and cursor into one straight-alpha ARGB image and adjusts the hotspot
by the three-pixel margin. Packs whose expanded images cannot fit the 64×64
plane use software composition; disabling the shadow can recover the plane.
