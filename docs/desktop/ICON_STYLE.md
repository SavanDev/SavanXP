# Icon style

The house icon set is drawn to look like the shell icons of Windows 2000: small
solid objects with a dark outline, lit from the top left, in the colours of the
256-colour palette of the time. This document records the rules, so a new icon
joins the set instead of standing out from it.

## What is covered

- `assets/desktop/icons/16x16` and `32x32`: the system catalog (`icon=<name>`).
- The icons that travel next to a manifest through `icon_file=`: the media
  player (`subsystems/posix/userland/mediaplayer/icon*.png`) and the Doom and
  Celeste ports (`ports/*/overlay/icon*.png`).
- The media player's folder and speaker glyphs, which the taskbar tray also
  uses for the volume icon.

Not covered: `assets/desktop/icons/mime/`, the file-type catalog, which is
Tango art (see
[`THIRD_PARTY_PROVENANCE.md`](../third-party/THIRD_PARTY_PROVENANCE.md)), and the
flat black transport glyphs of the player and Paint's tool box, which follow
toolbar conventions (glyphs on a button face) rather than icon ones.

## Rules

1. **Hard edges.** Alpha is 0 or 255, as with an `.ico` and its 1-bit mask. No
   anti-aliasing against transparency: the same icon is drawn over the grey
   face, the blue selection and the desktop colour.
2. **Light from the top left.** Every solid part has an outline on its own
   border, a highlight on its inner top and left edges, a shade on its inner
   bottom and right edges, and its base colour inside. Things set into a
   surface (screens, displays) invert the bevel.
3. **Ramps, not loose colours.** Each material is a ramp of four tones
   (outline, shade, base, highlight). Outlines are near-black versions of the
   hue, as in the 256-colour icons of the era. Box-like objects take a lighter
   top face and a darker side face of the same ramp.
4. **Objects, not symbols.** A program is shown as the thing it works with: a
   computer, a calculator, a pad of paper, a globe. Boxes are seen slightly from
   above where that helps (keyboard, computer case).
5. **Each size is drawn on its own.** A 32x32 that is the 16x16 scaled by two
   reads as a blown-up small icon. The 16x16 keeps the silhouette and drops
   detail; where a shape does not survive 16 px (two gears, bevels on 3 px
   teeth), it is simplified rather than shrunk.

## Where it lives

The checked-in PNGs are the source of the art, and the player's `icons.inc` is
the embedded copy of its glyphs. The drawing code they came from is kept on the
maintainer's machine and is deliberately not part of the repository; the build
only packages the PNGs and never redraws them. A new or replaced icon is a
hand-drawn PNG that keeps the rules above.

`icon_file=` accepts one PNG per size (`icon_file=icon.png,icon-32.png`), so a
port can carry both drawings; see
[`SXE_FORMAT.md`](../userland/SXE_FORMAT.md).
