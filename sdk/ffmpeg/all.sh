#!/bin/bash
# Construye el port de FFmpeg y el reproductor de punta a punta.
#
# En Windows se corre a traves de sdk/ffmpeg/build.ps1, que pone adelante en el
# PATH el LLVM horneado y el GNU make fijado. En Linux o macOS se puede correr
# directo con clang, ld.lld, llvm-ar y GNU make en el PATH. Ver README.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for step in fetch runtime configure build link; do
    echo
    echo "############ $step ############"
    bash "$HERE/$step.sh"
done
