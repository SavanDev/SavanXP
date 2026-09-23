#!/bin/bash
# Entorno comun de los scripts del port. Se incluye con `. env.sh`, no se corre.
#
# Corre igual en Linux/macOS que en Git Bash sobre Windows. Las herramientas se
# toman del PATH: en Windows es sdk/ffmpeg/build.ps1 el que resuelve el LLVM
# horneado y el GNU make fijado por tools/Toolchain.ps1 y los pone adelante.
# Ningun script de aca adivina donde vive una herramienta.

FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.1}"
FFMPEG_SHA256="733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1"

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${REPO:-$(cd "$PORT/../.." && pwd)}"
SDK="$REPO/subsystems/posix/sdk/v1"

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) SX_WINDOWS=1 ;;
    *) SX_WINDOWS=0 ;;
esac

# El directorio de trabajo NO puede tener espacios, y no es una preferencia: el
# configure de FFmpeg expande CFLAGS/LDFLAGS sin comillas, asi que una ruta con
# espacios se parte en dos argumentos y los checks fallan en silencio. En
# Windows el perfil del usuario muy seguido los tiene ("C:\Users\Nombre
# Apellido"); ahi se usa la forma corta 8.3 de la misma ruta, que no los tiene.
WORK="${WORK:-$HOME/savanxp-ffmpeg}"
case "$WORK" in
    *[[:space:]]*)
        if [ "$SX_WINDOWS" = 1 ]; then
            mkdir -p "$WORK"
            WORK="$(cygpath -u "$(cygpath -m -s "$WORK")")"
        fi
        ;;
esac
case "$WORK" in
    *[[:space:]]*)
        echo "error: el directorio de trabajo '$WORK' tiene espacios." >&2
        echo "       Defini WORK=<ruta sin espacios> y volve a correr." >&2
        exit 1
        ;;
esac

SRC="$WORK/ffmpeg-${FFMPEG_VERSION}"
BUILD="$WORK/build"
RUNTIME="$WORK/runtime"
# Copia de los headers publicos y del linker script del SDK. Existe por el
# mismo motivo que el chequeo de arriba: la raiz del repo SI puede tener
# espacios, y esas rutas terminan adentro de CFLAGS.
SYSROOT="$WORK/sysroot"
OUT="$WORK/out"

# Mismos flags que tools/UserAppCommon.ps1 Get-UserCompileFlags -Sse, sin los
# warnings. FFmpeg usa punto flotante en todos lados, asi que es el camino con
# SSE si o si. Los headers del SDK entran con -isystem y no con -I: -MMD deja
# afuera de los .d a los headers de sistema, y asi las dependencias que ve make
# son solo rutas de FFmpeg.
SX_TARGET_CFLAGS="-ffreestanding -fstack-protector-strong -fno-pic -fno-pie -mno-red-zone"
SX_TARGET_CFLAGS="$SX_TARGET_CFLAGS -mcmodel=small -mno-mmx -msse -msse2"
SX_TARGET_CFLAGS="$SX_TARGET_CFLAGS -isystem $SYSROOT/include"

# El compilador apunta al target freestanding, pero el LINK va con el triple de
# Linux. No cambia el ELF que sale -- -nostdlib -static y el linker script del
# SDK deciden todo --, cambia a quien llama el driver: con x86_64-*-none-elf el
# driver de clang delega el link en un `gcc`, que en Windows no existe; con el
# triple de Linux llama a ld.lld directo en cualquier host.
SX_CC="clang -target x86_64-unknown-none-elf"
SX_LD="clang -target x86_64-unknown-linux-gnu"

# -no-pie: sin el, el driver le pasa -pie al linker y con -fno-pic lld rechaza
# CUALQUIER programa. -static: sin el, el ELF sale marcado como dinamico con un
# .interp que desalinea el segundo PT_LOAD. Ver README.md.
SX_TARGET_LDFLAGS="-nostdlib -static -no-pie -fuse-ld=lld -Wl,-T,$SYSROOT/linker.ld"
SX_TARGET_LDFLAGS="$SX_TARGET_LDFLAGS -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none"

sx_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        echo 4
    fi
}

# Ruta que entiende un programa nativo de Windows (C:/...). Identidad fuera de
# Windows.
sx_native_path() {
    if [ "$SX_WINDOWS" = 1 ]; then
        cygpath -m "$1"
    else
        printf '%s\n' "$1"
    fi
}
