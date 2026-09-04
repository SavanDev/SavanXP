"""Saca una captura de la pantalla por QMP.

Lo usa `build.ps1 ffmpeg-smoke`: el player deja el ultimo cuadro fijo y lo avisa
por serial, y en ese momento este script pide el screendump. Reutiliza la sesion
QMP de tools/shoot_session.py para no tener dos formas distintas de sacar una
foto.

Ademas de guardar el PNG comprueba que la pantalla no este en blanco: una
captura toda de un color es exactamente lo que se veria si el player decodifico
bien pero no llego a presentar nada, que es el caso que esto tiene que atrapar.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from shoot_session import Qmp, Session  # noqa: E402

MINIMUM_COLORS = 16


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--name", default="player")
    arguments = parser.parse_args()

    os.makedirs(arguments.out_dir, exist_ok=True)
    session = Session(Qmp(arguments.port), arguments.out_dir)
    image = session.shot(arguments.name)

    colors = image.getcolors(maxcolors=1 << 20)
    distinct = len(colors) if colors is not None else 1 << 20
    print(f"  colores distintos en pantalla: {distinct}")
    if distinct < MINIMUM_COLORS:
        print(f"  FALLA: la pantalla tiene {distinct} color(es), esperaba >= {MINIMUM_COLORS}")
        return 1
    return 0


sys.exit(main())
