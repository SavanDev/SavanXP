#!/bin/bash
# Compila el runtime de userland de SavanXP como libsavanxp.a dentro de WSL.
#
# Hace falta por dos motivos: el configure de FFmpeg LINKEA programas de prueba
# -- sin una libc contra la que linkear, casi todos sus checks fallan y el
# resultado no dice nada -- y el link final del port necesita exactamente lo
# mismo. Se construye con el clang de Arch, no con el de toolchain/: son objetos
# ELF x86-64 freestanding, asi que conviven sin problema con los que produce el
# pipeline de Windows.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${REPO:-$(cd "$HERE/../.." && pwd)}"
SDK="$REPO/subsystems/posix/sdk/v1"
OUT="${OUT:-$HOME/savanxp-ffmpeg/runtime}"

# Mismos flags que tools/UserAppCommon.ps1 Get-UserCompileFlags -Sse. FFmpeg usa
# punto flotante en todos lados, asi que es el camino con SSE si o si.
CFLAGS=(
    -target x86_64-unknown-none-elf
    -ffreestanding
    -fno-stack-protector
    -fno-pic
    -fno-pie
    -mno-red-zone
    -mcmodel=small
    -mno-mmx
    -msse
    -msse2
    -I "$SDK/include"
    -I "$REPO/include"
)

mkdir -p "$OUT"
cd "$OUT"

echo "== compilando el runtime de SavanXP con clang $(clang --version | head -1 | awk '{print $3}')"
clang -c -x assembler-with-cpp "$SDK/runtime/crt0.S" -o crt0.o "${CFLAGS[@]}"
for unit in libc posix gfx gfx2d math; do
    clang -c -x c "$SDK/runtime/$unit.c" -o "$unit.o" "${CFLAGS[@]}"
    echo "  ok $unit.o"
done

rm -f libsavanxp.a
llvm-ar rcs libsavanxp.a libc.o posix.o gfx.o gfx2d.o math.o
echo "== libsavanxp.a: $(du -h libsavanxp.a | cut -f1)"

# Prueba de humo del link: si esto no cierra, ningun check de configure va a
# dar un resultado que signifique algo.
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
clang -c -x c probe.c -o probe.o "${CFLAGS[@]}"
# Se linkea con el DRIVER de clang, igual que hace configure, y no llamando a
# ld.lld directo: es la unica forma de que un problema del driver -- por ejemplo
# el -pie que agrega por default, que con -fno-pic hace que lld rechace todo --
# aparezca aca y no adentro de configure, donde solo se ve como "la libc no
# tiene ninguna funcion".
clang -nostdlib -static -no-pie -fuse-ld=lld -target x86_64-unknown-none-elf \
    -Wl,-T,"$SDK/linker.ld" -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none \
    -o probe.elf crt0.o probe.o libsavanxp.a
echo "== link de prueba: ok ($(du -h probe.elf | cut -f1))"
llvm-readelf -h probe.elf | grep -E 'Type|Machine|Entry'
