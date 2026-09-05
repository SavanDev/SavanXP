#!/usr/bin/env python3
"""Emite un blob .sxicon por cada icono del catalogo de tipos, para que la
imagen los lleve como archivos sueltos en /disk/icons/.

Por que archivos y no un set horneado en filesapp: docs/SXE_FORMAT.md decide
que los KiB de iconos NO viajan en un segmento cargable (la BSS se mapea eager
al exec, asi que cada KiB horneado es RAM residente por proceso). Los .sxicon
sueltos se leen del disco cuando hacen falta y se cachean en runtime, igual que
el icono propio de un programa.

El formato del blob es EXACTAMENTE el mismo que produce gen_sxe_resources.py
para la seccion .sxicon de un ejecutable -- se reusan sus helpers, no se
duplican --, asi que el mismo sxe_icons_open() parsea los dos.

Uso:  python tools/gen_mime_icons.py --project-root DIR --output-dir DIR
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gen_sxe_resources import build_icon_blob, load_format, read_icon_png, write_if_changed

# Raiz del catalogo de tipos, separada del arte propio del repo justamente
# porque su procedencia es distinta (ver docs/THIRD_PARTY_PROVENANCE.md).
ICON_ROOT = ("assets", "desktop", "icons", "mime")


def collect_sizes(fmt, project_root, icon_name):
    """Los dos tamanos que el sistema usa son obligatorios, por el mismo motivo
    que en collect_icons(): un .sxicon con uno solo obligaria a escalar en
    runtime, y escalar un icono de 16 a 32 se ve mal."""
    images = []
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

    print(f"mime-icons: {len(names)} iconos, {written} escritos en {args.output_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
