#!/usr/bin/env python3
"""Emits one .sxicon blob per icon of the file-type catalog, so the image
carries them as loose files in /icons/.

Why files and not a set baked into filesapp: docs/userland/SXE_FORMAT.md decides
that the KiB of icons do NOT travel in a loadable segment (BSS is mapped
eagerly at exec, so every baked-in KiB is resident RAM per process). Loose
.sxicon files are read from the disk when needed and cached at run time, like
a program's own icon.

The blob's format is EXACTLY the one gen_sxe_resources.py produces for an
executable's .sxicon section -- its helpers are reused, not duplicated -- so the
same sxe_icons_open() parses both.

Usage:  python tools/gen_mime_icons.py --project-root DIR --output-dir DIR
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gen_sxe_resources import build_icon_blob, load_format, read_icon_png, write_if_changed

# Raiz del catalogo de tipos, separada del arte propio del repo justamente
# porque su procedencia es distinta (ver docs/third-party/THIRD_PARTY_PROVENANCE.md).
ICON_ROOT = ("assets", "desktop", "icons", "mime")

# Catalog names drawn by the project itself, for what the Tango set has no
# icon for. They are emitted into /icons next to the catalog but read from the
# house directories (assets/desktop/icons/<size>/), keeping "this is ours"
# verifiable from the tree. name -> PNG basename.
HOUSE_ICONS = {
    "application-x-linux": "app-linux",
    "drive-harddisk": "drive-harddisk",
    "drive-optical": "drive-optical",
    "drive-removable-media": "drive-removable-media",
}


def collect_sizes(fmt, project_root, icon_name, root=None):
    """Both sizes the system uses are required, for the same reason as in
    collect_icons(): an .sxicon with only one would force scaling at run time,
    and scaling an icon from 16 to 32 looks bad. `root` is the directory that
    holds the <size>x<size> folders; the type catalog's by default."""
    images = []
    if root is None:
        root = os.path.join(project_root, *ICON_ROOT)
    for size in (fmt["SXE_ICON_SIZE_SMALL"], fmt["SXE_ICON_SIZE_LARGE"]):
        path = os.path.join(root, f"{size}x{size}", f"{icon_name}.png")
        width, height, pixels = read_icon_png(path)
        if width != size or height != size:
            raise SystemExit(
                f"mime-icons: '{path}' mide {width}x{height}, se esperaba {size}x{size}.")
        images.append((width, height, pixels))
    return images


def discover_names(project_root):
    """El catalogo es el directorio: lo que este en 16x16 y en 32x32 se emite.
    No hay lista de nombres en este archivo a proposito -- agregar un tipo tiene
    que ser copiar dos PNG y tocar mimeicon.ini, no editar el generador."""
    root = os.path.join(project_root, *ICON_ROOT)
    small = os.path.join(root, "16x16")
    large = os.path.join(root, "32x32")
    if not os.path.isdir(small) or not os.path.isdir(large):
        raise SystemExit(f"mime-icons: falta el catalogo en '{root}'.")

    def names_in(directory):
        return {f[:-4] for f in os.listdir(directory) if f.endswith(".png")}

    have_small = names_in(small)
    have_large = names_in(large)
    incomplete = have_small ^ have_large
    if incomplete:
        raise SystemExit(
            "mime-icons: estos iconos no estan en los dos tamanos: " +
            ", ".join(sorted(incomplete)))
    return sorted(have_small)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    fmt = load_format(args.project_root)
    names = discover_names(args.project_root)
    written = 0
    for name in names:
        images = collect_sizes(fmt, args.project_root, name)
        blob = build_icon_blob(fmt, images, f"mime-icons: {name}")
        if write_if_changed(os.path.join(args.output_dir, name + ".sxicon"), blob):
            written += 1
    house_root = os.path.join(args.project_root, "assets", "desktop", "icons")
    for name, source in sorted(HOUSE_ICONS.items()):
        images = collect_sizes(fmt, args.project_root, source, house_root)
        blob = build_icon_blob(fmt, images, f"mime-icons: {name}")
        if write_if_changed(os.path.join(args.output_dir, name + ".sxicon"), blob):
            written += 1
        names.append(name)

    print(f"mime-icons: {len(names)} iconos, {written} escritos en {args.output_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
