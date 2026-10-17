#!/usr/bin/env bash
# Mapeo de arquitectura para los scripts que construyen fuera de CMake.
# Se sourcea; no se ejecuta.
#
# SAVANXP_ARCH elige el objetivo: x86_64 (por defecto, el sistema actual) o
# i386 (el objetivo legacy de docs/REAL_HARDWARE.md). CMakeLists.txt hace este
# mismo mapeo para la mitad del grafo que pasa por CMake; esto es la mitad de
# los scripts, y existe para que el triple no este escrito a mano en cada uno.
#
#   SX_TARGET_TRIPLE   triple freestanding, para compilar
#   SX_HOST_TRIPLE     triple Linux, para que clang deleogue el enlace en ld.lld
#   SX_LINK_EMULATION  emulacion de ld.lld cuando se lo invoca directo
#   SX_MCMODEL_SMALL   -mcmodel=small, o vacio en 32 bits
#   SX_MCMODEL_MEDIUM  -mcmodel=medium, o vacio en 32 bits
#
# -mcmodel no existe en i386: clang lo rechaza con "unsupported argument". Los
# valores vacios se expanden sin comillas para que desaparezcan del argv.

SAVANXP_ARCH="${SAVANXP_ARCH:-x86_64}"
case "$SAVANXP_ARCH" in
    x86_64)
        SX_TARGET_TRIPLE=x86_64-unknown-none-elf
        SX_HOST_TRIPLE=x86_64-unknown-linux-gnu
        SX_LINK_EMULATION=elf_x86_64
        SX_MCMODEL_SMALL=-mcmodel=small
        SX_MCMODEL_MEDIUM=-mcmodel=medium
        ;;
    i386)
        SX_TARGET_TRIPLE=i386-unknown-none-elf
        SX_HOST_TRIPLE=i386-unknown-linux-gnu
        SX_LINK_EMULATION=elf_i386
        SX_MCMODEL_SMALL=
        SX_MCMODEL_MEDIUM=
        ;;
    *)
        echo "arch-env.sh: SAVANXP_ARCH desconocida: '$SAVANXP_ARCH' (x86_64 o i386)" >&2
        return 2 2>/dev/null || exit 2
        ;;
esac
