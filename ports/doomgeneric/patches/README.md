# Patch series

The patches are applied in filename order to the pristine upstream subtree:

1. `0001-savanxp-runtime-and-persistence.patch` — data paths, save handling,
   configuration parsing, and directory creation.
2. `0002-savanxp-iwad-compat.patch` — IWAD menu compatibility.
3. `0003-savanxp-platform-and-media.patch` — resolution, input, sound, and
   no-SSE media changes.

The platform adapter itself is kept in `../overlay/`; it is not hidden inside
an upstream patch.
