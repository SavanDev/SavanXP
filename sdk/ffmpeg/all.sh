#!/bin/bash
# Construye el port de FFmpeg de punta a punta.
#
# Necesita un entorno POSIX con GNU make, clang y ld.lld. En Linux o macOS se
# corre directo; en Windows, desde WSL o MSYS2. Ver README.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for step in fetch runtime configure build link-demo; do
    echo
    echo "############ $step ############"
    bash "$HERE/$step.sh"
done

echo
echo "############ listo ############"
echo "Falta generar el material y correrlo, desde el host del build:"
echo "  python sdk/ffmpeg/make-tone.py"
echo "  build.ps1 ffmpeg-smoke"
