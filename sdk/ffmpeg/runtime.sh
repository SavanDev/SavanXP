#!/bin/bash
# Compila el runtime de userland de SavanXP como bibliotecas estaticas.
#
# Hace falta por dos motivos: el configure de FFmpeg LINKEA programas de prueba
# -- sin una libc contra la que linkear, casi todos sus checks fallan y el
# resultado no dice nada -- y el link final del port necesita exactamente lo
# mismo. Salen objetos ELF x86-64 freestanding iguales a los del pipeline de
# build.ps1, aunque el clang sea otro.
#
#   libsavanxp.a  crt0 aparte + libc, posix, gfx, gfx2d, math, setjmp
#   libsxgui.a    el toolkit SXGUI, para el reproductor (lo que -Gui suma en
#                 tools/build-user.ps1, mas sxchrome)
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

echo "== sysroot en $SYSROOT"
rm -rf "$SYSROOT"
mkdir -p "$SYSROOT/include"
cp -R "$SDK/include/." "$SYSROOT/include/"
# include/ del repo: formatos compartidos con el host (sxe/sxe_format.h), que
# build-user.ps1 tambien expone a las apps externas.
cp -R "$REPO/include/." "$SYSROOT/include/"
cp "$SDK/linker.ld" "$SYSROOT/linker.ld"

mkdir -p "$RUNTIME"
cd "$RUNTIME"

echo "== compilando el runtime con $(clang --version | head -1)"
$SX_CC -c -x assembler-with-cpp "$SDK/runtime/crt0.S" -o crt0.o $SX_TARGET_CFLAGS
$SX_CC -c -x assembler-with-cpp "$SDK/runtime/setjmp.S" -o setjmp.o $SX_TARGET_CFLAGS
for unit in libc posix gfx gfx2d sxgui sxgui_app sxchrome; do
    $SX_CC -c -x c "$SDK/runtime/$unit.c" -o "$unit.o" $SX_TARGET_CFLAGS
done
# -fno-builtin por lo mismo que en build-user.ps1: que clang no reconozca el
# cuerpo de fabs y lo reemplace por una llamada a fabs.
$SX_CC -c -x c "$SDK/runtime/math.c" -o math.o -fno-builtin $SX_TARGET_CFLAGS

rm -f libsavanxp.a libsxgui.a
llvm-ar rcs libsavanxp.a libc.o posix.o gfx.o gfx2d.o math.o setjmp.o
llvm-ar rcs libsxgui.a sxgui.o sxgui_app.o sxchrome.o
echo "== libsavanxp.a: $(du -h libsavanxp.a | cut -f1)  libsxgui.a: $(du -h libsxgui.a | cut -f1)"

# Prueba de humo del link, por el DRIVER y con los mismos flags que va a usar
# configure: un problema del driver aparece aca con un error legible, y no
# adentro de configure, donde solo se ve como "la libc no tiene nada".
cat > probe.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
int main(void) {
    char buffer[64];
    double value = strtod("2.5", 0);
    snprintf(buffer, sizeof(buffer), "%s %.2f %.3f", "probe", value, sqrt(2.0));
    printf("%s\n", buffer);
    return 0;
}
EOF
$SX_CC -c -x c probe.c -o probe.o $SX_TARGET_CFLAGS
$SX_LD $SX_TARGET_LDFLAGS -o probe.elf crt0.o probe.o libsavanxp.a
echo "== link de prueba: ok ($(du -h probe.elf | cut -f1))"
llvm-readelf -h probe.elf | grep -E 'Type|Machine|Entry'
